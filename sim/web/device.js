// The device as the outside world meets it: pins, keys, lights, a MIDI wire.
// Wraps the WebAssembly core (the real firmware on a virtual board). Loaded as
// a classic script in the page and with require() in Node for the checks.
(function (root) {
  'use strict';

  const PIN = {
    PLAY: 28,
    ENC: [{ a: 18, b: 19, sw: 20 }, { a: 21, b: 22, sw: 26 }],
    TEMPO_LED: 25,
    GATES: [14, 15, 16, 17],
  };
  const COL_SCENE = 8;
  const COL_MUTE = 9;
  const EV_MIDI = 0, EV_GATE = 1, EV_DAC = 2;
  const IN_KEY = 0, IN_STEP = 1, IN_GPIO = 2, IN_MIDI = 3;
  const MIDI_BYTE_US = 320;       // 10 bits at 31,250 baud

  class Device {
    constructor(core) {
      this.core = core;
      this.dacVoltsPerCode = (core._sim_dac_full_scale_mv?.() ?? 2048) / 4096000;
      this.status = 0;          // running status of the MIDI byte stream
      this.data = [];
      this.boot(false);
    }

    boot(keepFlash) {
      this.core._sim_boot(keepFlash ? 1 : 0);
      this.status = 0;
      this.data = [];
      this.encPhase = [0, 0];   // position in the quadrature cycle, 0 = detent (A and B high)
      this.midiInFreeAt = 0;
    }

    now() { return this.core._sim_now(); }
    // Whole microseconds: the core truncates, and a caller waiting for now() to
    // reach a fractional time would wait forever.
    runUntil(us) { this.core._sim_run_until(Math.ceil(us)); }

    // ─── Physical inputs ──────────────────────────────────────
    // Each takes an optional device time `at` (microseconds). Without it the
    // input happens now. With it the core applies the input when device time
    // gets there, also in the middle of a stall such as a flash save.
    input(kind, a, b, level, at) {
      this.core._sim_input_at(at === undefined ? 0 : Math.round(at), kind, a, b, level);
    }
    pad(track, row, down, at) { this.input(IN_KEY, row, track, down ? 1 : 0, at); }
    scene(row, down, at) { this.input(IN_KEY, row, COL_SCENE, down ? 1 : 0, at); }
    mute(track, down, at) { this.input(IN_KEY, track, COL_MUTE, down ? 1 : 0, at); }
    step(index, down, at) { this.input(IN_STEP, index, 0, down ? 1 : 0, at); }
    play(down, at) { this.input(IN_GPIO, PIN.PLAY, 0, down ? 0 : 1, at); }
    encoderSwitch(enc, down, at) { this.input(IN_GPIO, PIN.ENC[enc].sw, 0, down ? 0 : 1, at); }

    // One quadrature edge, a quarter of a detent. Clockwise: A falls, B falls,
    // A rises, B rises. Edges must be given in time order.
    encoderEdge(enc, dir, at) {
      const phase = (this.encPhase[enc] + (dir > 0 ? 1 : 3)) % 4;
      this.encPhase[enc] = phase;
      this.input(IN_GPIO, PIN.ENC[enc].a, 0, phase === 1 || phase === 2 ? 0 : 1, at);
      this.input(IN_GPIO, PIN.ENC[enc].b, 0, phase === 2 || phase === 3 ? 0 : 1, at);
    }
    // What the firmware inside can do (sim/build.sh reads it from the source):
    // {lastNote, midiIn, padKeys}. An older core reports nothing.
    features() {
      const bits = this.core._sim_features ? this.core._sim_features() : 0;
      return { lastNote: !!(bits & 1), midiIn: !!(bits & 2), padKeys: !!(bits & 4) };
    }
    // Whether this firmware reads its DIN input at all (older builds do not).
    midiInListening() {
      return !!(this.core._sim_midi_in_listening && this.core._sim_midi_in_listening());
    }
    // Raw bytes on the DIN input, one every 320 us, the first starting at
    // `at` (now when omitted) or when the wire is free. Returns when the last
    // one has arrived.
    midiIn(bytes, at) {
      let t = Math.max(at === undefined ? this.now() : at, this.midiInFreeAt || 0);
      for (const byte of bytes) {
        t += MIDI_BYTE_US;
        this.input(IN_MIDI, byte & 0xFF, 0, 0, t);
      }
      this.midiInFreeAt = t;
      return t;
    }

    // Drops every input still scheduled and reads the encoder phases back
    // from the pins, which may have been left between detents.
    clearInputs() {
      this.core._sim_inputs_clear();
      this.midiInFreeAt = 0;
      const levels = this.core._sim_gpio_levels();
      this.encPhase = PIN.ENC.map(pins => {
        const a = (levels >>> pins.a) & 1, b = (levels >>> pins.b) & 1;
        return a && b ? 0 : !a && b ? 1 : !a && !b ? 2 : 3;
      });
    }

    // ─── Outputs ──────────────────────────────────────────────
    oled() { return this.core.HEAPU8.subarray(this.core._sim_oled(), this.core._sim_oled() + 1024); }
    oledSeq() { return this.core._sim_oled_seq(); }
    leds() { return this.core.HEAPU8.subarray(this.core._sim_leds(), this.core._sim_leds() + 82 * 3); }
    ledsSeq() { return this.core._sim_leds_seq(); }
    stepLeds() { return this.core._sim_step_leds(); }
    tempoLed() { return (this.core._sim_gpio_levels() >>> PIN.TEMPO_LED) & 1; }
    gates() { return (this.core._sim_gpio_levels() >>> PIN.GATES[0]) & 15; }
    dac(channel) { return this.core.HEAPU16[(this.core._sim_dac() >> 1) + channel]; }
    dacVolts(code) { return code * this.dacVoltsPerCode; }
    openMidiSong(bytes) {
      const c = this.core;
      if (!c._sim_midi_song_slot || bytes.length > c._sim_midi_song_capacity() || this.inspect().state !== 0) return false;
      c.HEAPU8.set(bytes, c._sim_midi_song_slot());
      return c._sim_midi_song_open() !== 0;
    }

    // Everything that left the device since the last call, in the order the
    // firmware produced it (time stamps can be up to 640 us out of order):
    // {t, type:'midi', status, d1, d2} | {t, type:'gate', mask} | {t, type:'dac', channel, code}.
    // t is device microseconds; a MIDI message carries the time its last byte ended.
    drain() {
      const core = this.core;
      const count = core._sim_events_count();
      const out = [];
      if (!count) return out;
      const base = core._sim_events();
      for (let i = 0; i < count; i++) {
        const at = base + i * 16;
        const t = core.HEAPF64[at >> 3];
        const kind = core.HEAPU32[(at + 8) >> 2];
        const value = core.HEAPU32[(at + 12) >> 2];
        if (kind === EV_MIDI) {
          if (value >= 0xF8) {
            out.push({ t, type: 'midi', status: value, d1: 0, d2: 0 });
          } else if (value & 0x80) {
            this.status = value;
            this.data.length = 0;
          } else if (this.status) {
            this.data.push(value);
            const need = (this.status & 0xE0) === 0xC0 ? 1 : 2;
            if (this.data.length === need) {
              out.push({ t, type: 'midi', status: this.status, d1: this.data[0], d2: need === 2 ? this.data[1] : 0 });
              this.data.length = 0;
            }
          }
        } else if (kind === EV_GATE) {
          out.push({ t, type: 'gate', mask: value });
        } else if (kind === EV_DAC) {
          out.push({ t, type: 'dac', channel: value >>> 16, code: value & 0xFFFF });
        }
      }
      core._sim_events_clear();
      return out;
    }

    // ─── Faults ───────────────────────────────────────────────
    i2cFault(addr, mode) { this.core._sim_i2c_fault(addr, mode); }
    chipReset(addr) { this.core._sim_chip_reset(addr); }

    // ─── Inspection (checks and meters; never the artist) ─────
    inspect() {
      const c = this.core;
      const fatal = c._sim_dbg_fatal();
      return {
        state: c._sim_dbg_transport_state(),
        bpm: c._sim_dbg_bpm(),
        pulse: c._sim_dbg_pulse(),
        mutes: c._sim_dbg_mutes(),
        overruns: c._sim_dbg_overruns(),
        midiOverflow: c._sim_dbg_midi_overflow(),
        lockErrors: c._sim_dbg_lock_errors(),
        ledsSkipped: c._sim_leds_skipped(),
        eventsDropped: c._sim_events_dropped(),
        midiInOverruns: c._sim_dbg_midi_in_overruns ? c._sim_dbg_midi_in_overruns() : 0,   // absent in an older core
        fatal: fatal ? c.UTF8ToString(fatal) : null,
      };
    }
    current(track) { return this.core._sim_dbg_current(track); }
    queued(track) { return this.core._sim_dbg_queued(track); }
    stepData(location, track, step) {
      const v = this.core._sim_dbg_step(location, track, step);
      const flags = v >>> 16;
      return { note: v & 0xFF, velocity: (v >>> 8) & 0xFF, active: !!(flags & 1), accent: !!(flags & 2),
               slide: !!(flags & 4), tie: !!(flags & 8) };
    }
    kitStep(location, track, step) { return this.core._sim_dbg_kit_step(location, track, step) >>> 0; }
    trackLength(location, track) { return this.core._sim_dbg_track_length(location, track); }
    trackConfig(track) {
      const v = this.core._sim_dbg_track_config(track);
      return { type: v & 0xFF, channel: (v >>> 8) & 0xFF, gate: (v >>> 16) & 0xFF,
               output: (v >>> 24) & 3, cvChannel: (v >>> 26) & 3 };
    }
  }

  // Things that happen beside the device at device times: the light under the
  // artist's finger, a caption, a knob on a synth. The device's own inputs are
  // not here: they are scheduled inside the core (Device.input with a time).
  class Hands {
    constructor(device) {
      this.device = device;
      this.queue = [];          // sorted by t, stable
      this.encFreeAt = [0, 0];
    }
    clear() {
      this.queue.length = 0;
      this.encFreeAt = [0, 0];
      this.device.clearInputs();
    }
    get pending() { return this.queue.length; }

    at(t, fn) {
      const q = this.queue;
      let i = q.length;
      while (i > 0 && q[i - 1].t > t) i--;
      q.splice(i, 0, { t, fn });
    }

    // Schedules a turn of whole detents starting no earlier than `from`;
    // returns when it ends. Edges are edgeUs apart so every core 0 pass sees
    // each one.
    turn(enc, detents, from, edgeUs) {
      edgeUs = edgeUs || 2000;
      let t = Math.max(from, this.encFreeAt[enc]);
      const dir = detents > 0 ? 1 : -1;
      for (let i = 0; i < Math.abs(detents) * 4; i++) {
        t += edgeUs;
        this.device.encoderEdge(enc, dir, t);
      }
      this.encFreeAt[enc] = t;
      return t;
    }
    // Leaves no encoder between detents (after a cleared queue).
    settleEncoders() {
      for (let enc = 0; enc < 2; enc++) {
        const left = (4 - this.device.encPhase[enc]) % 4;
        let t = this.device.now();
        for (let i = 0; i < left; i++) {
          t += 2000;
          this.device.encoderEdge(enc, 1, t);
        }
        this.encFreeAt[enc] = t;
      }
    }

    // Runs the device to `target`, doing every queued side effect at its time.
    advance(target) {
      const q = this.queue;
      while (q.length && q[0].t <= target) {
        const action = q.shift();
        if (action.t > this.device.now()) this.device.runUntil(action.t);
        action.fn();
      }
      this.device.runUntil(target);
    }
  }

  const api = { Device, Hands, PIN };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else Object.assign(root, api);
})(typeof window !== 'undefined' ? window : globalThis);
