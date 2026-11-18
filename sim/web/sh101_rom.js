// The 80C49 controller drives the SH-101 audio model via the virtual board.
(function (root) {
  'use strict';

  const HZ = 400000;  // 6 MHz crystal, 15 oscillator periods per machine cycle
  const ROM_SHA256 = '5cf1b2c18903cf9e7e55cb424d0b796a6d2f114849b4353ce7ae1ba0c608c4a8';
  const BUTTON = { load: 1, play: 2, down: 4, upDown: 8, up: 16, hold: 32, transpose: 128 };
  const MODE_LED = [null, 'load', 'play', 'down', 'upDown', 'up'];
  const P2 = { bypass: 0x08, reset: 0x10, hold: 0x20, transpose: 0x40, gate: 0x80 };
  const SOUND_BITS = 0x3ffff | ((P2.gate | P2.reset | P2.bypass) << 18);
  const LOOKAHEAD = 0.032;
  const TRIGGER_SWITCH = [0x08, 0x18, 0x10];
  const OCTAVE_SWITCH = [0x20, 0, 0x40];

  function decode(word) {
    const p2 = word >>> 18;
    return {
      pitch: word & 63, range: (word >>> 6) & 63, random: (word >>> 12) & 63,
      gate: !(p2 & P2.gate), glide: !(p2 & P2.bypass), reset: !(p2 & P2.reset),
      hold: !(p2 & P2.hold), transpose: !!(p2 & P2.transpose), mode: MODE_LED[p2 & 7] ?? null,
    };
  }

  class Board {
    static async load(createCore, image) {
      if (image.byteLength !== 2048) throw new Error('ROM должен занимать 2048 байт.');
      const digest = await crypto.subtle.digest('SHA-256', image);
      const hash = Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, '0')).join('');
      if (hash !== ROM_SHA256) throw new Error('Этот файл не совпадает с ROM Roland 80C49-6-7301.');
      return new Board(await createCore(), new Uint8Array(image));
    }

    constructor(core, bytes) {
      this.core = core;
      core.HEAPU8.set(bytes, core._sh101_rom_data());
      core._sh101_reset();
      this.keys = this.buttons = 0;
      this.range = this.octave = 1;
      this.trigger = 0;
      this.rate = 0;
      this.pedal = this.external = this.clockHigh = false;
    }

    get cycles() { return this.core._sh101_cycles(); }
    get phase() { return this.core._sh101_phase(); }
    get state() { return this.core._sh101_state(); }

    powerOn() {
      this.core._sh101_power_on();
      this.keys = this.buttons = 0;
      this.pedal = this.clockHigh = false;
    }

    advanceTo(target, onChange) {
      const c = this.core;
      // Raw P1 bits seen when DB4 is low. The ROM decodes range and mode.
      const switches = 0x84 | this.range | TRIGGER_SWITCH[this.trigger] | OCTAVE_SWITCH[this.octave];
      c._sh101_inputs(this.keys, this.buttons, switches, this.pedal, this.clockHigh);
      c._sh101_clock(this.rate, this.external);
      for (let start = this.cycles; start < target; start = this.cycles) {
        const n = c._sh101_run(Math.min(40000, target - start));
        if (!onChange) continue;
        const words = c.HEAPU32, base = c._sh101_events() / 4;
        for (let i = base; i < base + 2 * n; i += 2) onChange(start + words[i], words[i + 1]);
      }
    }
  }

  class Player {
    constructor(board, send) {
      this.board = board;
      this.send = send;
      this.offset = null;
    }

    tick(wallTime, audioTime) {
      const board = this.board, running = audioTime !== null;
      const now = audioTime ?? wallTime;
      const head = board.cycles / HZ;
      const rebase = this.offset === null || running !== this.running ||
        head + this.offset < now - (running ? 0 : 0.06);
      if (rebase) this.offset = now - head + (running ? 0.012 : 0);
      this.running = running;
      const time = head + this.offset;
      const target = Math.floor((now + (running ? LOOKAHEAD : 0) - this.offset) * HZ);

      let last = board.state & SOUND_BITS;
      if (running) {
        if (rebase) this.send({ type: 'kill', t: now });
        if (rebase || this.rate !== board.rate) {
          this.send({ type: 'romClock', hz: board.rate, phase: board.phase, t: time });
          this.rate = board.rate;
        }
        if (rebase) this.send({ ...decode(board.state), type: 'rom', t: time });
      }
      board.advanceTo(target, running ? (cycle, word) => {
        const state = word & SOUND_BITS;
        if (state === last) return;
        last = state;
        this.send({ ...decode(word), type: 'rom', t: this.offset + cycle / HZ });
      } : undefined);
      return decode(board.state);
    }
  }

  const api = { Board, Player, BUTTON, HZ, decode };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.SH101ROM = api;
})(globalThis);
