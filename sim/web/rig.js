// rig.js: the virtual instruments plugged into the simulated sequencer.
// Classic script, one global (window.Rig). The two 303s are the x0xb0x model
// of dsp.js (see acid.js); everything else is synthesized with native Web
// Audio nodes. Nothing is touched at load time, so the file also loads under
// Node for syntax checks.
//
// Scheduling rules that keep the automation sane:
//  - every AudioParam has exactly one writer (a knob, a voice envelope or a
//    one-shot), so no cancelScheduledValues can wipe somebody else's events;
//  - persistent voices only use cancelScheduledValues + setTargetAtTime chains,
//    which continue from the value the param has at that moment (cancelling a
//    linear/exponential ramp half way makes the value snap back);
//  - knobs keep a JS copy of their timeline (Line) so a truncated ramp can be
//    rebuilt and so voices can sample a knob at a note's event time.
(function () {
  'use strict';

  // Where this script came from: acid.js and dsp.js sit beside it.
  const HERE = typeof document !== 'undefined' && document.currentScript && document.currentScript.src
    ? document.currentScript.src.replace(/[^/]*$/, '') : '';

  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
  const num = (v, d) => (typeof v === 'number' && isFinite(v) ? v : d);
  const cents = (hi, lo) => 1200 * Math.log2(hi / lo);
  const chain = (...n) => { for (let i = 1; i < n.length; i++) n[i - 1].connect(n[i]); return n[n.length - 1]; };

  // Seeded noise: offline renders must be reproducible.
  function prng(seed) {
    let a = seed >>> 0;
    return function () {
      a = (a + 0x6D2B79F5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  function curve(fn) {
    const n = 2049, c = new Float32Array(n);
    for (let i = 0; i < n; i++) c[i] = fn((i / (n - 1)) * 2 - 1);
    return c;
  }

  // Piecewise linear timeline of one automated value.
  class Line {
    constructor(v) { this.segs = [{ t0: -Infinity, t1: -Infinity, v0: v, v1: v }]; }
    at(t) {
      const s = this.segs;
      let i = s.length - 1;
      while (i > 0 && s[i].t0 > t) i--;
      const g = s[i];
      if (t >= g.t1) return g.v1;
      return t <= g.t0 ? g.v0 : g.v0 + ((g.v1 - g.v0) * (t - g.t0)) / (g.t1 - g.t0);
    }
    // Ramp to v over `ramp` seconds from T, superseding everything from T on.
    // Returns the value the line had at T.
    to(v, T, ramp, now) {
      const s = this.segs, vT = this.at(T);
      while (s.length > 1 && s[s.length - 1].t0 >= T) s.pop();
      const last = s[s.length - 1];
      if (last.t1 > T) { last.t1 = T; last.v1 = vT; }
      s.push({ t0: T, t1: T + ramp, v0: vT, v1: v });
      while (s.length > 1 && s[1].t0 <= now) s.shift();
      return vT;
    }
  }

  function knobDefs() {
    const k = (id, label, group, value, min, max, unit) =>
      ({ id, label, group, min: min || 0, max: max == null ? 1 : max, value, unit: unit || '' });
    const out = [
      k('master.volume', 'Volume', 'Master', 0.8),
      k('delay.feedback', 'Delay feedback', 'Master', 0.45, 0, 0.85),
      k('delay.level', 'Delay level', 'Master', 0.5),
      k('reverb.level', 'Reverb level', 'Master', 0.35),
    ];
    for (const [x, g] of [['acid1', 'Acid 1'], ['acid2', 'Acid 2']]) {
      out.push(
        // The seven knobs of the model and the eight of its effects, named and
        // set as the Bench page has them.
        k(x + '.tune', 'Tune', g, 0.5), k(x + '.cutoff', 'Cut off freq', g, 0.35),
        k(x + '.resonance', 'Resonance', g, 0.85), k(x + '.envmod', 'Env mod', g, 0.6),
        k(x + '.decay', 'Decay', g, 0.35), k(x + '.accent', 'Accent', g, 0.8),
        k(x + '.volume', 'Volume', g, 0.75),
        k(x + '.drive', 'Drive', g, 0), k(x + '.color', 'Color', g, 0.5),
        k(x + '.delay', 'Delay', g, 0.3), k(x + '.time', 'Time', g, 0.4), k(x + '.fdbk', 'Feedback', g, 0.4),
        k(x + '.space', 'Space', g, 0.5), k(x + '.size', 'Size', g, 0.55), k(x + '.tone', 'Tone', g, 0.5),
        k(x + '.level', 'Level', g, 0.7));
    }
    out.push(
      k('bass.cutoff', 'Cutoff', 'Bass', 0.45), k('bass.level', 'Level', 'Bass', 0.8),
      k('lead.cutoff', 'Cutoff', 'Lead', 0.6), k('lead.level', 'Level', 'Lead', 0.6),
      k('lead.delay', 'Delay send', 'Lead', 0.6),
      k('arp.level', 'Level', 'Arp', 0.5), k('arp.delay', 'Delay send', 'Arp', 0.5),
      k('drums.level', 'Kit level', 'Drums', 0.85), k('kick.level', 'Kick level', 'Drums', 0.9),
      k('fx.level', 'Level', 'FX', 0.6), k('fx.riserBars', 'Riser length', 'FX', 8, 1, 16, 'bars'));
    return out;
  }

  // ---- Mono synth voice: a persistent graph driven by automation ----------
  // oscillators -> lowpass stage(s) -> vca. Pitch and filter envelope are
  // ConstantSourceNodes feeding the oscillators' and filters' detune (cents),
  // so one param carries the pitch of every oscillator and the knob that
  // owns filter.detune never meets the envelope.
  const GLIDE_TC = 0.02;   // ~60 ms to arrive

  class MonoVoice {
    constructor(rig, o) {
      const ctx = rig.ctx;
      this.o = o;
      this.note = -1;      // sounding note, by event order
      this.skip = 0;       // note offs to ignore after a same-pitch legato on
      this.lastT = 0;
      this.rel = 0.01;
      this.pitch = rig.constant(0);
      this.env = rig.constant(0);
      this.envAmt = rig.gain(o.envCents || 0);
      this.env.connect(this.envAmt);
      this.vca = rig.gain(0);
      const sum = rig.gain(1);
      for (const d of o.oscs) {
        const osc = ctx.createOscillator();
        osc.type = d.type;
        osc.frequency.value = 440 * (d.ratio || 1);
        osc.detune.value = d.cents || 0;
        this.pitch.connect(osc.detune);
        chain(osc, rig.gain(d.gain), sum);
        osc.start(0);
      }
      this.filters = o.q.map((q) => {
        const f = ctx.createBiquadFilter();
        f.type = 'lowpass';
        f.frequency.value = o.baseHz;
        f.Q.value = q;
        this.envAmt.connect(f.detune);
        return f;
      });
      chain(sum, ...this.filters, this.vca);
      this.out = this.vca;
    }

    on(note, vel, t) {
      // Events of one voice are taken in arrival (wire) order; a stray earlier
      // timestamp must not cancel automation that is already scheduled later.
      t = this.lastT = Math.max(t, this.lastT);
      const p = this.pitch.offset, c = (note - 69) * 100;
      if (this.note >= 0) {
        // Legato (slide): glide, keep the envelopes running.
        if (note === this.note) { this.skip++; return; }
        this.note = note;
        this.skip = 0;
        p.cancelScheduledValues(t);
        p.setTargetAtTime(c, t, GLIDE_TC);
        return;
      }
      this.note = note;
      this.skip = 0;
      p.cancelScheduledValues(t);
      p.setValueAtTime(c, t);
      const e = this.o.trig(vel, t), g = this.vca.gain, f = this.env.offset;
      g.cancelScheduledValues(t);
      g.setTargetAtTime(e.peak, t, e.atk);
      g.setTargetAtTime(e.peak * e.sus, t + e.hold, e.dec);
      f.cancelScheduledValues(t);
      f.setTargetAtTime(e.fPeak, t, 0.0015);
      f.setTargetAtTime(0, t + 0.006, e.fDec);
      this.rel = e.rel;
    }

    off(note, t) {
      if (note !== this.note) return;   // the slid-from note: already replaced
      if (this.skip > 0) { this.skip--; return; }
      t = this.lastT = Math.max(t, this.lastT);
      this.note = -1;
      this.release(t, this.rel);
    }

    // Panic: unlike on/off this cuts at t even if later events are scheduled.
    kill(t) {
      this.lastT = t;
      this.note = -1;
      this.skip = 0;
      this.release(t, 0.005);
    }

    release(t, tc) {
      const g = this.vca.gain;
      g.cancelScheduledValues(t);
      g.setTargetAtTime(0, t, tc);
    }
  }

  const velGain = (vel) => 0.5 + 0.5 * Math.min(vel, 100) / 100;

  const CUT_BASS = cents(4000, 60), CUT_LEAD = cents(12000, 150);

  // ---- The two 303s: the x0xb0x model of dsp.js, run by acid.js -----------
  // MIDI becomes the four lines a x0xb0x's firmware drives. A note struck
  // while another is held is a slide: SLIDE goes high and the CV moves, but
  // the gate stays up, so the envelopes are not struck again. Accent is a
  // velocity of 120 or more. The knobs are the model's own, 0 to 1 of a turn.
  const ACID_KNOBS = { cut: 'cutoff', res: 'resonance', env: 'envmod', decay: 'decay', acc: 'accent', tune: 'tune', vol: 'volume' };
  const cvOf = (note) => 2 + (note - 36) / 12;   // the model's CV: 2 V is C2, a volt an octave
  // The model's line output at its Volume 7.5 is quiet beside the rest of the
  // rig (accents peak near 0.3, about 0.03 RMS): the strip brings it up, so
  // that at the master an accent with its echoes peaks about as high as the
  // kick.
  const ACID_GAIN = 7;

  // ---- The effects of the Bench page, one set for each 303 -----------------
  // Ported from x0xb0x-vcf-bench/index.html (buildFx and what it calls), with
  // the rig's master in place of its own limiter. The voice first meets the
  // distortion: a clean path and a WaveShaper path (tanh with a little
  // asymmetry, 4x oversampled, then a Color low-pass and a DC block),
  // crossfaded by Drive, so Drive 0 is a true bypass. Then dry, a delay send
  // and a reverb send. The delay is a ping-pong pair synced to the tempo, each
  // repeat through a 250 Hz to 3.5 kHz band, and its echoes go to the reverb
  // too. The reverb send is high-passed, delayed 35 ms and convolved with a
  // generated stereo tail. These knobs act when they are turned: a move
  // scheduled ahead or stretched over time is taken at once.
  const DIVS = [[1, '1/16'], [2, '1/8'], [3, '3/16'], [4, '1/4'], [6, '3/8'], [8, '1/2']];   // sixteenths
  const divOf = (v) => DIVS[Math.round(clamp(v, 0, 1) * (DIVS.length - 1))];
  const rt60 = (size) => 1.2 * Math.pow(10, size);                  // 1.2 s to 12 s

  class BenchFx {
    constructor(rig, id, src) {
      const c = this.c = rig.ctx;
      this.rig = rig;
      this.id = id;
      const out = this.out = rig.gain(1);
      const dry = c.createGain(), send = c.createGain(), hp = c.createBiquadFilter(), pre = c.createDelay(0.2);
      hp.type = 'highpass'; hp.frequency.value = 160; pre.delayTime.value = 0.035;
      dry.connect(out); send.connect(hp); hp.connect(pre);
      const dsend = c.createGain(), dhp = c.createBiquadFilter(), dl = c.createDelay(2.5), dr = c.createDelay(2.5);
      const fbLR = c.createGain(), fbRL = c.createGain(), merge = c.createChannelMerger(2), dout = c.createGain();
      dhp.type = 'highpass'; dhp.frequency.value = 120;
      const band = (from, to) => {
        const h = c.createBiquadFilter(), l = c.createBiquadFilter();
        h.type = 'highpass'; h.frequency.value = 250; l.type = 'lowpass'; l.frequency.value = 3500;
        from.connect(h); h.connect(l); l.connect(to);
      };
      dsend.connect(dhp); dhp.connect(dl);
      band(dl, fbLR); fbLR.connect(dr);          // left echo feeds the right
      band(dr, fbRL); fbRL.connect(dl);          // and the right feeds the left
      dl.connect(merge, 0, 0); dr.connect(merge, 0, 1);
      merge.connect(dout); dout.connect(out); dout.connect(send);
      const inp = c.createGain(), clean = c.createGain(), shaper = c.createWaveShaper(), color = c.createBiquadFilter();
      const dcb = c.createBiquadFilter(), dist = c.createGain(), post = c.createGain();
      shaper.oversample = '4x';
      color.type = 'lowpass'; color.Q.value = 0.7;
      dcb.type = 'highpass'; dcb.frequency.value = 25;
      inp.connect(clean); clean.connect(post);
      inp.connect(shaper); shaper.connect(color); color.connect(dcb); dcb.connect(dist); dist.connect(post);
      post.connect(dry); post.connect(send); post.connect(dsend);
      src.connect(inp);
      Object.assign(this, { dry, send, pre, conv: null, tail: null, timer: 0, dsend, dl, dr, fbLR, fbRL, clean, shaper, color, dist, driveDrawn: -1 });
      this.setDrive(true);
      this.swapTail();
      this.setWet(true);
      this.setDelay(true);
      for (const name of ['drive', 'color']) rig.watch(id + '.' + name, () => this.setDrive(false));
      for (const name of ['delay', 'time', 'fdbk']) rig.watch(id + '.' + name, () => this.setDelay(false));
      rig.watch(id + '.space', () => this.setWet(false));
      // a new tail is costly to make: wait until the knob rests
      for (const name of ['size', 'tone']) {
        rig.watch(id + '.' + name, () => { clearTimeout(this.timer); this.timer = setTimeout(() => this.swapTail(), 150); });
      }
    }

    k(name) { return this.rig.getParam(this.id + '.' + name); }

    setDrive(now) {
      const d = this.k('drive'), t = this.c.currentTime;
      const g = 1 + 40 * d * d;   // gain into the curve
      const shape = (x) => Math.tanh(g * (x + 0.12 * x * x));
      if (Math.abs(d - this.driveDrawn) > 0.002) {
        const n = 2048, curve = new Float32Array(n);
        for (let i = 0; i < n; i++) curve[i] = shape(2 * i / (n - 1) - 1);
        this.shaper.curve = curve;
        this.driveDrawn = d;
      }
      // the two paths differ in phase (Color, DC block), so cross over within the first 5 % of Drive
      const w = Math.min(1, d * 20), makeup = Math.pow(g, -0.7);
      const set = (prm, v) => { if (now) prm.value = v; else prm.setTargetAtTime(v, t, 0.02); };
      set(this.clean.gain, 1 - w);
      set(this.dist.gain, w * makeup);
      set(this.color.frequency, this.rig.hz(1500 * Math.pow(12, this.k('color'))));
    }

    setDelay(now, at) {
      const t = at === undefined ? this.c.currentTime : at;
      const T = divOf(this.k('time'))[0] * 60 / this.rig.tempo.at(t) / 4;
      const mix = 0.9 * Math.pow(this.k('delay'), 1.2), fb = 0.85 * this.k('fdbk');
      const set = (prm, v, tc) => { if (now) prm.value = v; else prm.setTargetAtTime(v, t, tc); };
      set(this.dl.delayTime, T, 0.05); set(this.dr.delayTime, T, 0.05);   // a tempo change glides, like tape
      set(this.dsend.gain, mix, 0.02); set(this.fbLR.gain, fb, 0.02); set(this.fbRL.gain, fb, 0.02);
    }

    setWet(now) {
      const sp = this.k('space'), t = this.c.currentTime;
      const wet = 1.2 * Math.pow(sp, 1.5), dry = 1 - 0.35 * sp;
      if (now) { this.send.gain.value = wet; this.dry.gain.value = dry; return; }
      this.send.gain.setTargetAtTime(wet, t, 0.02);
      this.dry.gain.setTargetAtTime(dry, t, 0.02);
    }

    makeTail() {
      const c = this.c, rand = this.rig.rand;
      const sr = c.sampleRate, T = rt60(this.k('size')), n = Math.ceil(Math.min(T * 1.15, 14) * sr);
      const buf = c.createBuffer(2, n, sr);
      const fc0 = 2500 * Math.pow(6, this.k('tone')), fc1 = fc0 / 6;   // the tail darkens as it decays
      for (let ch = 0; ch < 2; ch++) {
        const d = buf.getChannelData(ch);
        let lp = 0, a = 0, e2 = 0;
        for (let i = 0; i < n; i++) {
          const t = i / sr;
          if ((i & 63) === 0) a = Math.exp(-2 * Math.PI * fc0 * Math.pow(fc1 / fc0, Math.min(t / T, 1)) / sr);
          lp = (1 - a) * (rand() * 2 - 1) + a * lp;
          const v = lp * (1 - Math.exp(-t / 0.05)) * Math.exp(-6.91 * t / T);
          d[i] = v;
          e2 += v * v;
        }
        const k = 1 / Math.sqrt(e2 || 1);
        for (let i = 0; i < n; i++) d[i] *= k;
      }
      return buf;
    }

    swapTail() {
      const c = this.c, t = c.currentTime;
      const conv = c.createConvolver(), tail = c.createGain();
      conv.normalize = false;
      conv.buffer = this.makeTail();
      tail.gain.value = this.conv ? 0 : 1;
      this.pre.connect(conv); conv.connect(tail); tail.connect(this.out);
      if (this.conv) {   // fade the new tail in and the old one out, so a turn of Size does not cut the sound
        const oldConv = this.conv, oldTail = this.tail;
        tail.gain.setTargetAtTime(1, t, 0.05);
        oldTail.gain.setTargetAtTime(0, t, 0.05);
        setTimeout(() => { this.pre.disconnect(oldConv); oldConv.disconnect(); oldTail.disconnect(); }, 400);
      }
      this.conv = conv; this.tail = tail;
    }
  }

  class AcidVoice {
    constructor(rig, id, wave, pan) {
      this.rig = rig;
      this.id = id;
      this.wave = wave;
      this.note = -1;      // sounding note, by event order
      this.skip = 0;       // note offs to ignore after a same-pitch legato on
      this.lastT = 0;
      this.sink = null;    // takes the line changes once the model runs
      this.ua = 0;         // ladder current, peak over the last 30 ms or so, for a meter
      this.out = rig.gain(1);
      // Its own delay and reverb, as on the Bench page: no sends to the rig's.
      this.fx = new BenchFx(rig, id, this.out);
      rig.strip(this.fx.out, { level: id + '.level', k: ACID_GAIN, pan });
    }

    send(t, lines) { if (this.sink) this.sink({ t, lines }); }

    // 'saw' or 'square', as the switch on the panel.
    setWave(wave) {
      this.wave = wave === 'square' ? 'square' : 'saw';
      if (this.sink) this.sink({ wave: this.wave });
    }

    on(note, vel, t) {
      // Until the model runs a note is dropped whole: remembering it as held
      // would turn the first note that does sound into a slide with no gate.
      if (!this.sink) return;
      // Events of one voice are taken in arrival (wire) order.
      t = this.lastT = Math.max(t, this.lastT);
      const accent = vel >= 120 ? 1 : 0;
      if (this.note >= 0) {
        if (note === this.note) {      // tied to itself: nothing moves but the accent line
          this.skip++;
          this.send(t, [['slide', 1], ['accent', accent]]);
          return;
        }
        this.note = note;
        this.skip = 0;
        this.send(t, [['slide', 1], ['accent', accent], ['cv', cvOf(note)]]);
        return;
      }
      this.note = note;
      this.skip = 0;
      this.send(t, [['slide', 0], ['accent', accent], ['cv', cvOf(note)], ['gate', 1]]);
    }

    off(note, t) {
      if (!this.sink || note !== this.note) return;   // not this voice's note: the slid-from one, already replaced
      if (this.skip > 0) { this.skip--; return; }
      t = this.lastT = Math.max(t, this.lastT);
      this.note = -1;
      this.send(t, [['gate', 0]]);
    }

    // Panic: unlike on/off this cuts at t even if later events are scheduled.
    kill(t) {
      this.lastT = t;
      this.note = -1;
      this.skip = 0;
      if (this.sink) this.sink({ t, kill: true });
    }

    // The model in an AudioWorklet: its knobs are the node's AudioParams.
    worklet() {
      const rig = this.rig;
      const node = new AudioWorkletNode(rig.ctx, 'rebirth-acid', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [1], processorOptions: { wave: this.wave } });
      for (const k of Object.keys(ACID_KNOBS)) rig.bind(this.id + '.' + ACID_KNOBS[k], node.parameters.get(k), (x) => x);
      node.connect(this.out);
      node.port.onmessage = (e) => { if (e.data.ua !== undefined) this.ua = e.data.ua; };
      this.node = node;
      this.sink = (m) => node.port.postMessage(m);
    }

    // The same model on the main thread, for a page that cannot load a
    // worklet module (one opened from disk). Costs more and can crackle.
    inline() {
      const rig = this.rig, ctx = rig.ctx;
      const engine = new window.Acid.AcidEngine(ctx.sampleRate, this.wave);
      const node = ctx.createScriptProcessor(1024, 0, 1);
      const knob = (k, t) => rig.knobAt(this.id + '.' + ACID_KNOBS[k], t);
      node.onaudioprocess = (e) => {
        engine.render(e.outputBuffer.getChannelData(0), e.playbackTime === undefined ? ctx.currentTime : e.playbackTime, knob);
        this.ua = engine.takeUa();
      };
      node.connect(this.out);
      this.node = node;
      this.sink = (m) => { if (m.kill) engine.kill(m.t); else if (m.wave) engine.wave(m.wave); else engine.lines(m.t, m.lines); };
    }
  }

  function makeBass(rig) {
    const v = new MonoVoice(rig, {
      oscs: [{ type: 'sawtooth', gain: 0.6 }, { type: 'square', gain: 0.25 }, { type: 'sine', gain: 0.5 }],
      baseHz: 60, q: [3, -3], envCents: 2600,
      trig(vel) {
        const acc = vel >= 120 ? 1 : 0;
        return { peak: 0.8 * velGain(vel) * (1 + 0.25 * acc), atk: 0.002, hold: 0.008, sus: 0.3, dec: 0.045, rel: 0.012,
                 fPeak: 1 + 0.3 * acc, fDec: 0.035 };
      },
    });
    for (const f of v.filters) rig.bind('bass.cutoff', f.detune, (x) => x * CUT_BASS);
    rig.strip(v.out, { level: 'bass.level', k: 1.0 });
    return v;
  }

  function makeLead(rig) {
    const v = new MonoVoice(rig, {
      oscs: [{ type: 'sawtooth', cents: -9, gain: 0.5 }, { type: 'sawtooth', cents: 9, gain: 0.5 },
             { type: 'sawtooth', ratio: 2, cents: 3, gain: 0.22 }],
      baseHz: 150, q: [7, -3], envCents: 1200,
      trig(vel) {
        return { peak: 0.8 * (0.6 + 0.4 * vel / 127), atk: 0.004, hold: 0.02, sus: 0.75, dec: 0.15, rel: 0.07,
                 fPeak: 1, fDec: 0.12 };
      },
    });
    for (const f of v.filters) rig.bind('lead.cutoff', f.detune, (x) => x * CUT_LEAD);
    rig.strip(v.out, { level: 'lead.level', k: 1.78, pan: 0.1, delay: 'lead.delay', rev: 0.25 });
    return v;
  }

  function makeArp(rig) {
    const v = new MonoVoice(rig, {
      oscs: [{ type: 'square', gain: 0.45 }, { type: 'sawtooth', gain: 0.4 }],
      baseHz: 900, q: [5], envCents: 3000,
      trig(vel) {
        return { peak: 0.7 * velGain(vel), atk: 0.0015, hold: 0.006, sus: 0.12, dec: 0.04, rel: 0.03,
                 fPeak: 1, fDec: 0.02 };
      },
    });
    rig.strip(v.out, { level: 'arp.level', k: 2.2, pan: -0.1, delay: 'arp.delay', rev: 0.2 });
    return v;
  }

  function makePad(rig) {
    const v = new MonoVoice(rig, {
      oscs: [{ type: 'sawtooth', cents: -7, gain: 0.5 }, { type: 'sawtooth', cents: 7, gain: 0.5 }],
      baseHz: 1100, q: [-3],
      trig(vel) {
        return { peak: 0.5 * velGain(vel), atk: 0.03, hold: 0.12, sus: 0.8, dec: 0.3, rel: 0.2, fPeak: 0, fDec: 0.1 };
      },
    });
    rig.strip(v.out, { k: 0.4, rev: 0.4 });
    return v;
  }

  // ---- One-shot voices: fresh nodes per hit, freed when the source ends ----
  // Each hit ends in its own `out` gain whose param carries no automation, so
  // choking or panicking is one setTargetAtTime on it and never a cancel.
  class OneShots {
    constructor(rig) { this.rig = rig; this.ctx = rig.ctx; this.live = new Set(); }

    hit(dest, level, t, dur) {
      const out = this.ctx.createGain();
      out.gain.value = level;
      out.connect(dest);
      const h = { out, t0: t, end: t + dur, nodes: [out], srcs: [], dead: false, open: false };
      this.live.add(h);
      return h;
    }
    gain(h, v) {
      const g = this.ctx.createGain();
      g.gain.value = v;
      h.nodes.push(g);
      return g;
    }
    filt(h, type, hz, q) {
      const f = this.ctx.createBiquadFilter();
      f.type = type;
      f.frequency.value = this.rig.hz(hz);
      f.Q.value = q;
      h.nodes.push(f);
      return f;
    }
    // Oscillator falling (or rising) from f0 towards f1 with time constant tc.
    tone(h, type, f0, f1, tc) {
      const o = this.ctx.createOscillator();
      o.type = type;
      o.frequency.value = f0;
      if (f1 !== undefined) { o.frequency.setValueAtTime(f0, h.t0); o.frequency.setTargetAtTime(f1, h.t0, tc); }
      o.start(h.t0);
      h.srcs.push(o);
      return o;
    }
    // Looping buffer source: the shared noise or the metallic bank. Sources
    // are silent (and cost nothing) until their start time, so hits scheduled
    // far ahead in an offline render stay cheap.
    loop(h, buffer) {
      const s = this.ctx.createBufferSource();
      s.buffer = buffer;
      s.loop = true;
      s.start(h.t0, this.rig.rand() * 0.5);
      h.srcs.push(s);
      return s;
    }
    noise(h) { return this.loop(h, this.rig.noise); }
    metal(h) { return this.loop(h, this.rig.metal); }
    // Percussive envelope gain: 0 -> peak in atk, optional hold, exponential fall.
    perc(h, peak, atk, tc, hold) {
      const g = this.gain(h, 0), p = g.gain, t = h.t0;
      p.setValueAtTime(0, t);
      p.linearRampToValueAtTime(peak, t + atk);
      p.setTargetAtTime(0, t + atk + (hold || 0), tc);
      return g;
    }
    go(h) {
      for (const s of h.srcs) s.stop(h.end);
      h.srcs[0].onended = () => this.free(h);
    }
    choke(h, t) {
      if (h.dead || t >= h.end) return;
      h.out.gain.setTargetAtTime(0, t, 0.004);
      const e = Math.max(t + 0.05, h.t0 + 0.01);
      if (e < h.end) {
        h.end = e;
        for (const s of h.srcs) { try { s.stop(e); } catch (err) { /* already stopped */ } }
      }
    }
    kill(t) { for (const h of this.live) this.choke(h, t); }
    free(h) {
      if (h.dead) return;
      h.dead = true;
      this.live.delete(h);
      for (const n of h.srcs) { n.onended = null; try { n.disconnect(); } catch (err) { /* gone */ } }
      for (const n of h.nodes) { try { n.disconnect(); } catch (err) { /* gone */ } }
    }
  }

  class Drums extends OneShots {
    constructor(rig) {
      super(rig);
      this.kickBus = rig.gain(1);
      this.punch = curve((x) => Math.tanh(1.6 * x) / Math.tanh(1.6));
      rig.strip(this.kickBus, { level: 'kick.level', k: 1.25 });
      // kit: dry bus (hats, toms, rim, ride) and an ambient one (snare, clap, crash)
      this.kit = rig.gain(1);
      this.amb = rig.gain(1);
      rig.strip(this.kit, { level: 'drums.level', k: 0.9 });
      rig.strip(this.amb, { level: 'drums.level', k: 0.9, rev: 0.18 });
    }

    on(note, vel, t) {
      const lvl = Math.pow(vel / 127, 1.5);
      switch (note) {
        case 35: case 36: this.kick(lvl, t); break;
        case 38: this.snare(lvl, t); break;
        case 39: this.clap(lvl, t); break;
        case 37: this.rim(lvl, t); break;
        case 45: this.tom(95, 0.1, lvl, t); break;
        case 47: this.tom(135, 0.085, lvl, t); break;
        case 50: this.tom(190, 0.07, lvl, t); break;
        case 42: this.hat(false, lvl, t); break;
        case 46: this.hat(true, lvl, t); break;
        case 49: this.crash(lvl, t); break;
        case 51: this.ride(lvl, t); break;
        default: break;
      }
    }
    off() { /* one-shots */ }

    kick(lvl, t) {
      const h = this.hit(this.kickBus, lvl, t, 0.5);
      // Saturate the body before the velocity gain, so an accent stays louder.
      const body = this.ctx.createWaveShaper();
      body.curve = this.punch;
      h.nodes.push(body);
      chain(this.tone(h, 'sine', 170, 48, 0.024), this.perc(h, 1, 0.0015, 0.06, 0.05), body, h.out);
      chain(this.noise(h), this.filt(h, 'bandpass', 3000, 0.7), this.perc(h, 0.35, 0.0005, 0.003), h.out);
      this.go(h);
    }
    snare(lvl, t) {
      const h = this.hit(this.amb, 0.8 * lvl, t, 0.5);
      chain(this.tone(h, 'triangle', 280, 185, 0.008), this.perc(h, 0.6, 0.001, 0.04), h.out);
      chain(this.tone(h, 'sine', 500, 330, 0.008), this.perc(h, 0.3, 0.001, 0.025), h.out);
      chain(this.noise(h), this.filt(h, 'highpass', 1400, 0), this.perc(h, 0.7, 0.001, 0.065), h.out);
      this.go(h);
    }
    clap(lvl, t) {
      const h = this.hit(this.amb, 2 * lvl, t, 0.5);
      const g = this.gain(h, 0), p = g.gain;
      for (let i = 0; i < 3; i++) {       // three quick bursts, then the tail
        p.setValueAtTime(1, t + i * 0.011);
        p.exponentialRampToValueAtTime(0.12, t + i * 0.011 + 0.009);
      }
      p.setValueAtTime(0.8, t + 0.033);
      p.setTargetAtTime(0, t + 0.033, 0.06);
      chain(this.noise(h), this.filt(h, 'bandpass', 1200, 1.3), this.filt(h, 'highpass', 600, 0), g, h.out);
      this.go(h);
    }
    rim(lvl, t) {
      const h = this.hit(this.kit, 0.6 * lvl, t, 0.1);
      const hp = this.filt(h, 'highpass', 400, 0);
      chain(this.tone(h, 'triangle', 1700), this.perc(h, 0.7, 0.0005, 0.007), hp);
      chain(this.tone(h, 'sine', 470), this.perc(h, 0.5, 0.0005, 0.01), hp);
      hp.connect(h.out);
      this.go(h);
    }
    tom(hz, tc, lvl, t) {
      const h = this.hit(this.kit, 0.8 * lvl, t, 0.7);
      chain(this.tone(h, 'sine', hz * 1.7, hz, 0.03), this.perc(h, 0.9, 0.0015, tc, 0.02), h.out);
      chain(this.noise(h), this.filt(h, 'bandpass', hz * 6, 1), this.perc(h, 0.15, 0.001, 0.02), h.out);
      this.go(h);
    }
    hat(open, lvl, t) {
      // Any hat chokes an open hat that started before it.
      for (const o of this.live) if (o.open && o.t0 < t - 1e-4) this.choke(o, t);
      const h = this.hit(this.kit, 2.5 * lvl, t, open ? 0.8 : 0.12);
      h.open = open;
      chain(this.loop(h, this.rig.sizzle), this.perc(h, 1, 0.001, open ? 0.11 : 0.013), h.out);
      this.go(h);
    }
    crash(lvl, t) {
      const h = this.hit(this.amb, 0.5 * lvl, t, 2.4);
      const hp = this.filt(h, 'highpass', 4500, 0);
      this.metal(h).connect(hp);
      chain(this.noise(h), this.gain(h, 0.6), hp);
      chain(hp, this.perc(h, 1, 0.002, 0.38), h.out);
      this.go(h);
    }
    ride(lvl, t) {
      const h = this.hit(this.kit, 0.45 * lvl, t, 1.6);
      const bp = this.filt(h, 'bandpass', 9000, 1.5);
      this.metal(h).connect(bp);
      chain(this.noise(h), this.gain(h, 0.1), bp);
      chain(bp, this.perc(h, 1, 0.001, 0.26), h.out);
      this.go(h);
    }
  }

  class Fx extends OneShots {
    constructor(rig) {
      super(rig);
      this.bus = rig.gain(1);
      this.riser = null;
      rig.strip(this.bus, { level: 'fx.level', k: 1.4, delay: 0.3, rev: 0.5 });
    }
    on(note, vel, t) {
      if (this.riser) { this.choke(this.riser, Math.max(t, this.riser.t0)); this.riser = null; }
      if (vel >= 120) this.rise(t); else { this.zap(t); this.crash(t); }
    }
    off() { /* one-shots */ }
    kill(t) { super.kill(t); this.riser = null; }

    zap(t) {
      const h = this.hit(this.bus, 0.5, t, 0.9);
      const o = this.tone(h, 'sawtooth', 2400), f = this.filt(h, 'lowpass', 7000, 14);
      o.frequency.setValueAtTime(2400, t);
      o.frequency.exponentialRampToValueAtTime(70, t + 0.4);
      f.frequency.setValueAtTime(this.rig.hz(7000), t);
      f.frequency.exponentialRampToValueAtTime(180, t + 0.4);
      chain(o, f, this.perc(h, 0.5, 0.002, 0.13), h.out);
      this.go(h);
    }
    crash(t) {
      const h = this.hit(this.bus, 0.4, t, 1.9);
      const hp = this.filt(h, 'highpass', 3000, 0);
      this.metal(h).connect(hp);
      chain(this.noise(h), this.gain(h, 0.7), hp);
      chain(hp, this.perc(h, 1, 0.002, 0.3), h.out);
      this.go(h);
    }
    rise(t) {
      const rig = this.rig;
      const dur = clamp((rig.knobAt('fx.riserBars', t) * 240) / rig.tempo.at(t), 0.25, 20);
      const h = this.hit(this.bus, 1.4, t, dur + 0.06);
      const bp = this.filt(h, 'bandpass', 300, 1.2), g = this.gain(h, 0);
      bp.frequency.setValueAtTime(300, t);
      bp.frequency.exponentialRampToValueAtTime(rig.hz(9000), t + dur);
      g.gain.setValueAtTime(0, t);
      g.gain.linearRampToValueAtTime(0.03, t + 0.03);
      g.gain.exponentialRampToValueAtTime(1, t + dur);
      const o = this.tone(h, 'sawtooth', 110);
      o.frequency.setValueAtTime(110, t);
      o.frequency.exponentialRampToValueAtTime(1760, t + dur);
      chain(this.noise(h), bp);
      chain(o, this.gain(h, 0.12), bp);
      chain(bp, g, h.out);
      h.out.gain.setTargetAtTime(0, t + dur, 0.004);   // the cut at the end
      this.go(h);
      this.riser = h;
    }
  }

  // ---- The rig -------------------------------------------------------------
  // Calibrated so a full mix peaks around -2.5 dBFS (0.76) at master.volume 0.8.
  const MASTER_TRIM = 1.08;
  const REVERB_GAIN = 1.5;
  const CH_INDEX = [7, 7, 1, 2, 3, 4, 5, 6, 7, 0, 7, 7, 7, 7, 7, 7];
  const LAMPS = 4;   // remembered triggers per instrument (events arrive ahead of time)

  class Rig {
    constructor(ctx) {
      this.ctx = ctx;
      this.rand = prng(0x5EED);
      this.tempo = new Line(145);
      this.params = knobDefs();
      this.knobs = new Map();
      for (const d of this.params) this.knobs.set(d.id, { desc: d, line: new Line(d.value), binds: [], watchers: [] });
      this.lampT = new Float64Array(8 * LAMPS).fill(-1e9);
      this.lampV = new Float64Array(8 * LAMPS);

      this.noise = ctx.createBuffer(1, Math.floor(ctx.sampleRate * 2), ctx.sampleRate);
      const nd = this.noise.getChannelData(0);
      for (let i = 0; i < nd.length; i++) nd[i] = this.rand() * 2 - 1;

      this.buildMaster();
      this.buildSends();

      // Six detuned squares (the 808 cymbal bank), rendered once with polyBLEP
      // edges; hats and cymbals loop it through their own filters.
      this.metal = ctx.createBuffer(1, Math.floor(ctx.sampleRate * 3), ctx.sampleRate);
      const md = this.metal.getChannelData(0);
      const blep = (x, dt) => {
        if (x < dt) { x /= dt; return x + x - x * x - 1; }
        if (x > 1 - dt) { x = (x - 1) / dt; return x * x + x + x + 1; }
        return 0;
      };
      for (const f of [205.3, 304.4, 369.6, 522.7, 540, 800]) {
        const dt = f / ctx.sampleRate;
        let ph = this.rand();
        for (let i = 0; i < md.length; i++) {
          md[i] += 0.5 * ((ph < 0.5 ? 1 : -1) + blep(ph, dt) - blep((ph + 0.5) % 1, dt));
          ph += dt;
          if (ph >= 1) ph -= 1;
        }
      }
      // Hats are by far the most frequent hit, so their source is prepared:
      // the bank plus a little noise, already high-passed at 7 kHz (two
      // one-pole stages). A hat is then three nodes instead of six.
      this.sizzle = ctx.createBuffer(1, md.length, ctx.sampleRate);
      const sd = this.sizzle.getChannelData(0), hk = 1 - Math.exp((-2 * Math.PI * 7000) / ctx.sampleRate);
      for (let i = 0, a = 0, b = 0; i < sd.length; i++) {
        const x = md[i] + 0.25 * nd[i % nd.length];
        a += hk * (x - a);
        b += hk * (x - a - b);
        sd[i] = x - a - b;
      }

      this.inst = [
        new Drums(this), makeBass(this),
        new AcidVoice(this, 'acid1', 'saw', -0.25), new AcidVoice(this, 'acid2', 'square', 0.25),
        makeLead(this), makeArp(this), new Fx(this), makePad(this),
      ];
      // Resolves once the 303s can sound; until then their notes are dropped.
      this.ready = this.startAcids([this.inst[2], this.inst[3]]);
    }

    startAcids(acids) {
      const ctx = this.ctx;
      const silent = (why) => {
        console.warn('rig: the 303 model is not loaded, ACID 1 and ACID 2 stay silent', why);
        return 'none';
      };
      const inline = (why) => {
        if (typeof window === 'undefined' || !window.Acid || !ctx.createScriptProcessor) return silent(why);
        try {
          for (const a of acids) if (!a.sink) a.inline();
          return 'inline';
        } catch (e) { return silent(e); }
      };
      if (!ctx.audioWorklet || typeof AudioWorkletNode === 'undefined') return Promise.resolve(inline());
      // acid.js first: it gives dsp.js the `module` to export its classes through.
      // Inside a promise chain, so that an addModule that throws instead of
      // rejecting still ends in the fallback.
      return Promise.resolve()
        .then(() => ctx.audioWorklet.addModule(HERE + 'acid.js'))
        .then(() => ctx.audioWorklet.addModule(HERE + 'dsp.js'))
        .then(() => { for (const a of acids) a.worklet(); return 'worklet'; })
        .catch(inline);
    }

    // -- construction helpers
    gain(v) { const g = this.ctx.createGain(); g.gain.value = v; return g; }
    constant(v) { const c = this.ctx.createConstantSource(); c.offset.value = v; c.start(0); return c; }
    hz(f) { return Math.min(f, this.ctx.sampleRate * 0.45); }

    // Attach an AudioParam to a knob. Each binding keeps its own timeline in
    // the param's units, so a ramp cut short can be reproduced exactly.
    // A param bound late (the 303s, once their model has loaded) joins the
    // knob where it is now and takes over what is left of a move in progress.
    bind(id, param, map) {
      const k = this.knobs.get(id), now = this.ctx.currentTime;
      const v = map(k.line.at(now));
      param.value = v;
      param.setValueAtTime(v, now);
      const line = new Line(v);
      for (const s of k.line.segs) {
        if (s.t1 <= now) continue;
        const T = Math.max(now, s.t0), target = map(s.v1);
        param.setValueAtTime(map(k.line.at(T)), T);
        param.linearRampToValueAtTime(target, s.t1);
        line.to(target, T, s.t1 - T, now);
      }
      k.binds.push({ param, map, line });
    }
    knobAt(id, t) { return this.knobs.get(id).line.at(t); }
    // For what a knob moves that is not an AudioParam (a curve, an impulse
    // response): fn runs when the knob is set, after its value has changed.
    watch(id, fn) { this.knobs.get(id).watchers.push(fn); }

    // Channel strip: level knob (square taper) -> pan -> mix, plus sends.
    strip(src, o) {
      const lvl = this.gain(o.k);
      if (o.level) this.bind(o.level, lvl.gain, (x) => o.k * x * x);
      src.connect(lvl);
      let out = lvl;
      if (o.pan) {
        out = this.ctx.createStereoPanner();
        out.pan.value = o.pan;
        lvl.connect(out);
      }
      out.connect(this.mix);
      if (o.delay != null) {
        const s = this.gain(typeof o.delay === 'number' ? o.delay : 0);
        if (typeof o.delay === 'string') this.bind(o.delay, s.gain, (x) => x);
        chain(lvl, s, this.delayIn);
      }
      if (o.rev) chain(lvl, this.gain(o.rev), this.reverbIn);
    }

    buildMaster() {
      const ctx = this.ctx;
      this.mix = this.gain(1);
      // Gentle saturation: tanh(0.6 * mix), bounded below 1 (a shaper only
      // reads its curve for inputs in -1..1, hence the gain in front).
      const satIn = this.gain(0.3), sat = ctx.createWaveShaper();
      sat.curve = curve((x) => Math.tanh(2 * x));
      const comp = this.comp = ctx.createDynamicsCompressor();   // .reduction can feed a meter
      comp.threshold.value = -6;
      comp.knee.value = 6;
      comp.ratio.value = 6;
      comp.attack.value = 0.002;
      comp.release.value = 0.12;
      // The compressor adds make-up gain and overshoots on attacks, so it is
      // no ceiling: trim after it and round off whatever still gets near 1.
      const vol = this.gain(1), clip = ctx.createWaveShaper();
      this.bind('master.volume', vol.gain, (x) => MASTER_TRIM * x);
      clip.curve = curve((x) => {
        const a = Math.abs(x);
        return a <= 0.85 ? x : Math.sign(x) * (0.85 + 0.13 * Math.tanh((a - 0.85) / 0.13));
      });
      this.analyser = ctx.createAnalyser();
      this.analyser.fftSize = 2048;
      chain(this.mix, satIn, sat, comp, vol, clip, this.analyser, ctx.destination);
    }

    buildSends() {
      const ctx = this.ctx;
      const flat = (type, hz) => {
        const f = ctx.createBiquadFilter();
        f.type = type;
        f.frequency.value = this.hz(hz);
        f.Q.value = -3;   // dB: Butterworth, no peak inside the feedback loop
        return f;
      };
      // Ping-pong delay, 3/16 note per bounce: in -> L -> R -> back to L.
      this.delayIn = this.gain(1);
      const hp = flat('highpass', 220), lp = flat('lowpass', 4200);
      const dl = ctx.createDelay(2), dr = ctx.createDelay(2), toR = this.gain(0), toL = this.gain(0);
      const panL = ctx.createStereoPanner(), panR = ctx.createStereoPanner(), ret = this.gain(0);
      panL.pan.value = -0.75;
      panR.pan.value = 0.75;
      this.delays = [dl.delayTime, dr.delayTime];
      for (const p of this.delays) { p.value = 45 / 145; p.setValueAtTime(45 / 145, 0); }
      this.bind('delay.feedback', toR.gain, (x) => x);
      this.bind('delay.feedback', toL.gain, (x) => x);
      this.bind('delay.level', ret.gain, (x) => 1.2 * x);
      chain(this.delayIn, hp, lp, dl, toR, dr, toL, hp);
      chain(dl, panL, ret);
      chain(dr, panR, ret);
      ret.connect(this.mix);

      // Reverb: exponentially decaying noise, darker towards the tail.
      this.reverbIn = this.gain(1);
      const conv = ctx.createConvolver(), sr = ctx.sampleRate, n = Math.floor(sr * 2.2);
      const ir = ctx.createBuffer(2, n, sr);
      for (let c = 0; c < 2; c++) {
        const d = ir.getChannelData(c);
        let y = 0;
        for (let i = 0; i < n; i++) {
          const x = i / n;
          y += (0.85 - 0.6 * x) * (this.rand() * 2 - 1 - y);
          d[i] = y * Math.exp(-6.9 * x) * Math.min(1, i / (0.003 * sr));
        }
      }
      conv.buffer = ir;
      const rret = this.gain(0);
      this.bind('reverb.level', rret.gain, (x) => REVERB_GAIN * x);
      chain(this.reverbIn, flat('highpass', 300), conv, rret, this.mix);
    }

    // -- public interface
    midi(status, d1, d2, time) {
      try {
        status |= 0;
        if (status < 0x80 || status > 0xEF) return;
        const type = status & 0xF0;
        if (type !== 0x80 && type !== 0x90 && type !== 0xB0) return;
        const now = this.ctx.currentTime, t = clamp(num(time, now), now, now + 1e5);
        const a = d1 & 127, b = d2 & 127, i = CH_INDEX[status & 15], inst = this.inst[i];
        if (type === 0xB0) {
          if (a === 120 || a === 123) inst.kill(t);
        } else if (type === 0x90 && b > 0) {
          this.lamp(i, b / 127, t);
          inst.on(a, b, t);
        } else {
          inst.off(a, t);
        }
      } catch (e) { /* never throw into the transport */ }
    }

    setParam(id, value, time, rampSeconds) {
      try {
        const k = this.knobs.get(id);
        if (!k || typeof value !== 'number' || !isFinite(value)) return;
        const now = this.ctx.currentTime;
        const v = clamp(value, k.desc.min, k.desc.max);
        const T = clamp(num(time, now), now, now + 1e5);
        const ramp = clamp(num(rampSeconds, 0.02), 0.005, 3600);
        k.desc.value = v;
        k.line.to(v, T, ramp, now);
        for (const b of k.binds) {
          const p = b.param, target = b.map(v), vT = b.line.to(target, T, ramp, now);
          // Up to T the line is flat or one linear ramp, so a ramp point at
          // (T, vT) changes nothing that is already scheduled. With that
          // anchor in place everything after T can be cancelled without the
          // value ever leaving the line, whenever the audio thread looks.
          // (cancelAndHoldAtTime is not used: in an offline probe in Chromium
          // 156 a cancelAndHoldAtTime + setValueAtTime sequence lost the ramp
          // when a later call superseded an earlier one from an earlier time.)
          p.linearRampToValueAtTime(vT, T);
          p.cancelScheduledValues(T + 1e-7);
          p.linearRampToValueAtTime(target, T + ramp);
        }
        for (const fn of k.watchers) fn(v);
      } catch (e) { /* ignore */ }
    }

    getParam(id) {
      const k = this.knobs.get(id);
      return k ? k.desc.value : undefined;
    }

    setTempo(bpm, time) {
      try {
        if (typeof bpm !== 'number' || !isFinite(bpm)) return;
        bpm = clamp(bpm, 20, 400);
        const now = this.ctx.currentTime, T = clamp(num(time, now), now, now + 1e5);
        this.tempo.to(bpm, T, 0, now);
        const d = clamp(45 / bpm, 0.02, 1.95);   // 3/16 note
        for (const p of this.delays) {
          p.cancelScheduledValues(T);
          p.setTargetAtTime(d, T, 0.03);
        }
        for (const inst of this.inst) if (inst.fx) inst.fx.setDelay(false, T);
      } catch (e) { /* ignore */ }
    }

    allOff(time) {
      try {
        const now = this.ctx.currentTime, t = clamp(num(time, now), now, now + 1e5);
        for (const inst of this.inst) inst.kill(t);
      } catch (e) { /* ignore */ }
    }

    lamp(i, v, t) {
      const T = this.lampT, V = this.lampV, o = i * LAMPS;
      let old = o;
      for (let k = 0; k < LAMPS; k++) {
        if (T[o + k] === t) { if (v > V[o + k]) V[o + k] = v; return; }
        if (T[o + k] < T[old]) old = o + k;
      }
      T[old] = t;
      V[old] = v;
    }

    channelLevel(index) {
      const i = index | 0;
      if (i < 0 || i > 7) return 0;
      const now = this.ctx.currentTime, T = this.lampT, o = i * LAMPS;
      let best = -1;
      for (let k = 0; k < LAMPS; k++) {
        if (T[o + k] <= now && (best < 0 || T[o + k] > T[best])) best = o + k;
      }
      return best < 0 ? 0 : this.lampV[best] * Math.exp((T[best] - now) * 6);
    }
  }

  // How the page should print the two effect knobs that are not 0 to 10.
  Rig.format = {
    time: (v) => divOf(v)[1],
    size: (v) => rt60(v).toFixed(1) + ' s',
    drive: (v) => (v < 0.005 ? 'off' : (v * 10).toFixed(1)),
  };
  if (typeof window !== 'undefined') window.Rig = Rig;
  if (typeof module !== 'undefined' && module.exports) module.exports = Rig;
})();
