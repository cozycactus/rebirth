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

  // Where this script came from: acid.js and dsp.js sit beside it, and are
  // asked for with the same query (the public page adds the build's commit,
  // so a browser never mixes files of two builds).
  const SRC = typeof document !== 'undefined' && document.currentScript && document.currentScript.src || '';
  const HERE = SRC.replace(/\?.*$/, '').replace(/[^/]*$/, '');
  const QUERY = (SRC.match(/\?.*$/) || [''])[0];
  const SH101 = typeof window === 'undefined' ? require('./sh101.js') : window.SH101;

  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
  const num = (v, d) => (typeof v === 'number' && isFinite(v) ? v : d);
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
      // The shared Effects board (the Bench page's delay and reverb): the
      // returns, the delay's division and feedback, the reverb's tail.
      k('delay.level', 'Delay', 'Effects', 0.5), k('delay.time', 'Time', 'Effects', 0.4),
      k('delay.feedback', 'Feedback', 'Effects', 0.45, 0, 0.85),
      k('reverb.level', 'Space', 'Effects', 0.35), k('reverb.size', 'Size', 'Effects', 0.4),
      k('reverb.tone', 'Tone', 'Effects', 0.5),
    ];
    for (const [x, g] of [['acid1', 'Acid 1'], ['acid2', 'Acid 2']]) {
      out.push(
        // The seven knobs of the model and the two of its distortion, named
        // and set as the Bench page has them.
        k(x + '.tune', 'Tune', g, 0.5), k(x + '.cutoff', 'Cut off freq', g, 0.35),
        k(x + '.resonance', 'Resonance', g, 0.85), k(x + '.envmod', 'Env mod', g, 0.6),
        k(x + '.decay', 'Decay', g, 0.35), k(x + '.accent', 'Accent', g, 0.8),
        k(x + '.volume', 'Volume', g, 0.75),
        k(x + '.drive', 'Drive', g, 0), k(x + '.color', 'Color', g, 0.5),
        k(x + '.level', 'Level', g, 0.7));
    }
    out.push(
      k('bass.level', 'Level', 'Bass', 0.8), k('lead.level', 'Level', 'Lead', 0.6),
      k('arp.level', 'Level', 'Arp', 0.5),
      k('drums.level', 'Kit level', 'Drums', 0.85), k('kick.level', 'Kick level', 'Drums', 0.9),
      k('pad.level', 'Level', 'Pad', 0.55),
      k('fx.level', 'Level', 'FX', 0.6), k('fx.riserBars', 'Riser length', 'FX', 8, 1, 16, 'bars'));
    if (SH101) {
      for (const role of ['bass', 'lead', 'arp', 'fx', 'pad']) {
        const patch = SH101.defaults(role);
        for (const def of SH101.controls) {
          out.push(k(role + '.' + def.id, def.label, role.toUpperCase(), patch[def.id], 0, SH101.max(def)));
        }
      }
    }
    // The mixer strips' pan (0.5 is the centre) and their sends to the
    // shared delay and reverb, set where the mix had them fixed before. The
    // 303s had an echo and a tail of their own: their delay sends give the
    // same echo, their reverb sends a drier tail than Space 0.5 gave them.
    for (const [x, g, pan, delay, reverb] of [
      ['kick', 'Kick', 0.5, 0, 0], ['drums', 'Drums', 0.5, 0, 0.18], ['bass', 'Bass', 0.5, 0, 0],
      ['acid1', 'Acid 1', 0.375, 0.35, 0.5], ['acid2', 'Acid 2', 0.625, 0.35, 0.5],
      ['lead', 'Lead', 0.55, 0.6, 0.25], ['arp', 'Arp', 0.45, 0.5, 0.2], ['fx', 'FX', 0.5, 0.3, 0.5],
      ['pad', 'Pad', 0.54, 0.28, 0.68]]) {
      out.push(k(x + '.pan', 'Pan', g, pan));
      if (delay != null) out.push(k(x + '.delay', 'Delay send', g, delay), k(x + '.reverb', 'Reverb send', g, reverb));
    }
    return out;
  }

  const SCORE_CUTOFF = { 'bass.cutoff': [60, 4000], 'lead.cutoff': [150, 12000] };

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
  // kick. (7 when each 303 had its own reverb, whose Space 0.5 left the dry
  // signal at 0.825: 7 x 0.825.)
  const ACID_GAIN = 5.8;

  // ---- The effects of the Bench page --------------------------------------
  // Ported from x0xb0x-vcf-bench/index.html (buildFx and what it calls), with
  // the rig's master in place of its own limiter. Each 303 keeps the
  // distortion, in its channel before the fader: a clean path and a
  // WaveShaper path (tanh with a little asymmetry, 4x oversampled, then a
  // Color low-pass and a DC block), crossfaded by Drive, so Drive 0 is a true
  // bypass. The delay and the reverb are one shared pair that every mixer
  // strip sends to.
  const DIVS = [[1, '1/16'], [2, '1/8'], [3, '3/16'], [4, '1/4'], [6, '3/8'], [8, '1/2']];   // sixteenths
  const divOf = (v) => DIVS[Math.round(clamp(v, 0, 1) * (DIVS.length - 1))];
  const rt60 = (size) => 1.2 * Math.pow(10, size);                  // 1.2 s to 12 s

  class Distortion {
    constructor(rig, id, src) {
      const c = this.c = rig.ctx;
      this.rig = rig;
      this.id = id;
      const inp = c.createGain(), clean = c.createGain(), shaper = c.createWaveShaper(), color = c.createBiquadFilter();
      const dcb = c.createBiquadFilter(), dist = c.createGain(), post = this.out = c.createGain();
      shaper.oversample = '4x';
      color.type = 'lowpass'; color.Q.value = 0.7;
      dcb.type = 'highpass'; dcb.frequency.value = 25;
      inp.connect(clean); clean.connect(post);
      inp.connect(shaper); shaper.connect(color); color.connect(dcb); dcb.connect(dist); dist.connect(post);
      src.connect(inp);
      Object.assign(this, { clean, shaper, color, dist, driveDrawn: -1 });
      this.setDrive(true);
      // Drive redraws a curve, so it acts when it is turned.
      for (const name of ['drive', 'color']) rig.watch(id + '.' + name, () => this.setDrive(false));
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
  }

  // The Bench page's delay and reverb as the rig's send effects: inputs
  // `delayIn` and `reverbIn`, wet only, returns into the mix. The delay is a
  // ping-pong pair synced to the device's tempo, each repeat through a 250 Hz
  // to 3.5 kHz band; its echoes also reach the reverb. The reverb is
  // high-passed, delayed 35 ms and convolved with a generated stereo tail.
  // The returns and the feedback are AudioParams, so a move the artist
  // stretches over bars glides; Time, Size and Tone act when they are turned.
  const ECHO_TO_REVERB = 0.3;
  // The tail has unit energy per channel. The rig's reverb before it was a
  // 2.2 s tail that the ConvolverNode normalised to about 0.38 in energy
  // terms, with a return of 1.5 x Space; 1.5 x 0.38 keeps a send as wet.
  const REVERB_GAIN = 0.57;
  const MAX_DELAY = 6.5;   // a 1/2 note at the device's slowest tempo, 20 BPM, is 6 s

  class SendFx {
    constructor(rig) {
      const c = this.c = rig.ctx;
      this.rig = rig;
      // the delay
      const din = this.delayIn = rig.gain(1), dhp = c.createBiquadFilter();
      const dl = c.createDelay(MAX_DELAY), dr = c.createDelay(MAX_DELAY);
      const fbLR = c.createGain(), fbRL = c.createGain(), merge = c.createChannelMerger(2), dret = c.createGain();
      dhp.type = 'highpass'; dhp.frequency.value = 120;
      const band = (from, to) => {
        const h = c.createBiquadFilter(), l = c.createBiquadFilter();
        h.type = 'highpass'; h.frequency.value = 250; l.type = 'lowpass'; l.frequency.value = 3500;
        from.connect(h); h.connect(l); l.connect(to);
      };
      din.connect(dhp); dhp.connect(dl);
      band(dl, fbLR); fbLR.connect(dr);          // left echo feeds the right
      band(dr, fbRL); fbRL.connect(dl);          // and the right feeds the left
      dl.connect(merge, 0, 0); dr.connect(merge, 0, 1);
      merge.connect(dret); dret.connect(rig.mix);
      rig.bind('delay.level', dret.gain, (x) => 1.2 * x);
      rig.bind('delay.feedback', fbLR.gain, (x) => x);
      rig.bind('delay.feedback', fbRL.gain, (x) => x);
      this.times = [dl.delayTime, dr.delayTime];
      const T = this.delayAt(c.currentTime);
      for (const p of this.times) { p.value = T; p.setValueAtTime(T, c.currentTime); }
      rig.watch('delay.time', () => this.turnTime());
      // the reverb
      const rin = this.reverbIn = rig.gain(1), hp = c.createBiquadFilter(), pre = this.pre = c.createDelay(0.2);
      const rret = this.rret = c.createGain();
      hp.type = 'highpass'; hp.frequency.value = 160; pre.delayTime.value = 0.035;
      rin.connect(hp); hp.connect(pre);
      rret.connect(rig.mix);
      rig.bind('reverb.level', rret.gain, (x) => REVERB_GAIN * x);
      chain(dret, rig.gain(ECHO_TO_REVERB), rin);
      this.conv = null; this.tail = null; this.timer = 0;
      // its own noise: the rig's generator also picks where the hats' loop
      // starts, and a new tail would move that on
      this.rand = prng(0x7A11);
      this.swapTail();
      // a new tail is costly to make: wait until the knob rests
      for (const name of ['size', 'tone']) {
        rig.watch('reverb.' + name, () => { clearTimeout(this.timer); this.timer = setTimeout(() => this.swapTail(), 150); });
      }
    }

    // The delay time the Time knob asks for at the tempo of time t.
    delayAt(t) {
      return clamp(divOf(this.rig.getParam('delay.time'))[0] * 60 / this.rig.tempo.at(t) / 4, 0.001, MAX_DELAY);
    }

    // A tempo change at T, which may be ahead of now: only the delay times
    // move, gliding like tape. A knob is never touched here, so a change the
    // page scheduled ahead cannot undo a turn made in the meantime.
    retime(T) {
      const d = this.delayAt(T);
      for (const p of this.times) { p.cancelScheduledValues(T); p.setTargetAtTime(d, T, 0.05); }
    }

    // Time turned now: the new division from now on, and again at every
    // tempo change already scheduled after now.
    turnTime() {
      const now = this.c.currentTime, d = this.delayAt(now);
      for (const p of this.times) { p.cancelScheduledValues(now); p.setTargetAtTime(d, now, 0.05); }
      for (const s of this.rig.tempo.segs) if (s.t0 > now) this.retime(s.t0);
    }

    makeTail() {
      const c = this.c, rand = this.rand;
      const size = this.rig.getParam('reverb.size'), tone = this.rig.getParam('reverb.tone');
      const sr = c.sampleRate, T = rt60(size), n = Math.ceil(Math.min(T * 1.15, 14) * sr);
      const buf = c.createBuffer(2, n, sr);
      const fc0 = 2500 * Math.pow(6, tone), fc1 = fc0 / 6;   // the tail darkens as it decays
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
      this.pre.connect(conv); conv.connect(tail); tail.connect(this.rret);
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
    constructor(rig, id, wave) {
      this.rig = rig;
      this.id = id;
      this.wave = wave;
      this.note = -1;      // sounding note, by event order
      this.skip = 0;       // note offs to ignore after a same-pitch legato on
      this.lastT = 0;
      this.sink = null;    // takes the line changes once the model runs
      this.ua = 0;         // ladder current, peak over the last 30 ms or so, for a meter
      this.out = rig.gain(1);
      // The Bench page's distortion in its channel; its delay and reverb are
      // the shared ones, through the strip's sends.
      this.dist = new Distortion(rig, id, this.out);
      rig.strip(this.dist.out, { id, level: id + '.level', k: ACID_GAIN, panned: true });
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

  const SH101_STRIP_GAIN = { bass: 3.4, lead: 5.7, arp: 4.5, pad: 3.3 };

  class SH101Voice {
    constructor(rig, role, effectBus) {
      this.rig = rig;
      this.role = role;
      this.effectBus = effectBus;
      this.powered = true;
      this.input = 'midi';
      this.ready = SH101 ? this.start() : Promise.resolve('none');
    }

    async start() {
      const rig = this.rig, ctx = rig.ctx, role = this.role;
      const effects = role === 'fx';
      const parameters = SH101.controls.map(({ id }) => id);
      let node, send, mode;
      try {
        if (await rig.sh101Module) {
          node = new AudioWorkletNode(ctx, 'rebirth-sh101', {
            numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [1],
            processorOptions: { role },
          });
          for (const id of parameters) rig.bind(role + '.' + id, node.parameters.get(id), (value) => value);
          send = (event) => node.port.postMessage(event);
          mode = 'worklet';
        }
      } catch (error) {
        node = null;  // Loaded from disk, or this browser cannot run the processor.
      }
      if (!node && ctx.createScriptProcessor) {
        const voice = new SH101.Voice(ctx.sampleRate, effects), settings = SH101.defaults(role);
        node = ctx.createScriptProcessor(1024, 0, 1);
        node.onaudioprocess = (event) => {
          const output = event.outputBuffer.getChannelData(0);
          const start = event.playbackTime ?? ctx.currentTime;
          for (let i = 0; i < output.length; i += 128) {
            const time = start + i / ctx.sampleRate;
            for (const id of parameters) settings[id] = rig.knobAt(role + '.' + id, time);
            voice.render(output.subarray(i, i + 128), time, settings);
          }
        };
        send = (event) => voice.enqueue(event);
        mode = 'inline';
      }
      if (!node) return 'none';
      if (effects) chain(node, rig.gain(3.3), this.effectBus);
      else rig.strip(node, { id: role, level: role + '.level', k: SH101_STRIP_GAIN[role], panned: role !== 'bass' });
      this.node = node;
      this.send = send;
      return mode;
    }

    get useROM() { return this.input === 'rom'; }
    get external() { return this.powered && this.input === 'midi'; }
    setInput(input) {
      if (this.input === input) return;
      this.input = input;
      this.send?.({ type: 'source', rom: this.useROM, cv: input === 'cv', t: this.rig.ctx.currentTime });
    }
    setPower(powered) {
      this.powered = powered;
      this.send?.({ type: 'kill', t: this.rig.ctx.currentTime });
    }
    on(note, _velocity, t) { if (this.external) this.send?.({ type: 'note', note, down: true, t }); }
    off(note, t) { if (this.external) this.send?.({ type: 'note', note, down: false, t }); }
    bend(value, t) { if (this.external) this.send?.({ type: 'bend', value, t }); }
    cvGate(volts, gate, t) {
      if (this.powered && this.input === 'cv') this.send?.({ type: 'cv', volts, gate, t });
    }
    kill(t, includeCV = true) {
      if (this.external || (includeCV && this.input === 'cv')) this.send?.({ type: 'kill', t });
    }
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
      rig.strip(this.kickBus, { id: 'kick', level: 'kick.level', k: 1.25 });
      // kit: dry bus (hats, toms, rim, ride) and an ambient one (snare, clap, crash)
      this.kit = rig.gain(1);
      this.amb = rig.gain(1);
      // One strip on the mixer for both buses; its reverb send reaches the
      // ambient bus only, as the dry one never had any.
      rig.strip(this.kit, { id: 'drums', level: 'drums.level', k: 0.9, revK: 0 });
      rig.strip(this.amb, { id: 'drums', level: 'drums.level', k: 0.9 });
    }

    on(note, vel, t, trim = 1) {
      const lvl = Math.pow(vel / 127, 1.5) * trim;
      switch (note) {
        case 35: case 36: this.kick(lvl, t); break;
        case 38: this.snare(lvl, t); break;
        case 39: this.clap(lvl, t); break;
        case 37: this.rim(lvl, t); break;
        case 45: this.tom(95, 0.1, lvl, t); break;
        case 47: this.tom(135, 0.085, lvl, t); break;
        case 50: this.tom(190, 0.07, lvl, t); break;
        case 42: case 44: this.hat(false, lvl, t); break;
        case 46: this.hat(true, lvl, t); break;
        case 49: this.crash(lvl, t); break;
        case 51: this.ride(lvl, t); break;
        case 54: this.tambourine(lvl, t); break;
        case 57: this.reverseCymbal(lvl, t); break;
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
    tambourine(lvl, t) {
      const h = this.hit(this.amb, 0.34 * lvl, t, 0.3);
      chain(this.metal(h), this.filt(h, 'bandpass', 7900, 2.2), this.perc(h, 1, 0.001, 0.038), h.out);
      this.go(h);
    }
    reverseCymbal(lvl, t) {
      const h = this.hit(this.amb, 0.55 * lvl, t, 0.57);
      const g = this.gain(h, 0);
      g.gain.setValueAtTime(0, t);
      g.gain.linearRampToValueAtTime(1, t + 0.53);
      g.gain.linearRampToValueAtTime(0, t + 0.56);
      chain(this.noise(h), this.filt(h, 'highpass', 3800, 0), g, h.out);
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
      rig.strip(this.bus, { id: 'fx', level: 'fx.level', k: 1.4 });
      this.sh101 = new SH101Voice(rig, 'fx', this.bus);
      this.useSH101 = false;
    }
    setSH101(enabled) {
      if (!this.sh101?.send || enabled === this.useSH101) return;
      this.kill(this.ctx.currentTime);
      this.useSH101 = enabled;
    }
    on(note, vel, t) {
      if (!this.sh101.external) return;
      if (this.useSH101) {
        const rise = vel >= 120;
        this.sh101.send({ type: 'sweep', rise, duration: rise ? this.riserDuration(t) : 0.75, t });
        return;
      }
      if (this.riser) { this.choke(this.riser, Math.max(t, this.riser.t0)); this.riser = null; }
      if (vel >= 120) this.rise(t); else { this.zap(t); this.crash(t); }
    }
    off() { /* one-shots */ }
    kill(t, includeCV) { super.kill(t); this.riser = null; this.sh101?.kill(t, includeCV); }

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
    riserDuration(t) {
      const rig = this.rig;
      return clamp((rig.knobAt('fx.riserBars', t) * 240) / rig.tempo.at(t), 0.25, 20);
    }

    rise(t) {
      const rig = this.rig, dur = this.riserDuration(t);
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
  const CH_INDEX = [-1, -1, 1, 2, 3, 4, 5, 6, 7, 0, -1, -1, -1, -1, -1, -1];
  const LAMPS = 4;   // remembered triggers per instrument (events arrive ahead of time)
  // Imported patches carry oscillator and envelope levels from TAL. Bring
  // their quieter synth strips up to the kit without changing those controls.
  const MIDI_PART_GAIN = { bass: 2.6, lead: 1.85, arp: 2.5, pad: 1.4, fx: 1.25 };

  // Check the patch at a representative pitch before playback. A bounded
  // trim raises unusually quiet patches while preserving note velocities,
  // envelope shapes and expression automation from the file.
  function midiPartTrim(track, settings) {
    const notes = track.events
      .filter(e => (e.status & 0xf0) === 0x90 && e.d2 > 0)
      .sort((a, b) => a.d1 - b.d1);
    const note = notes[Math.floor(notes.length / 2)];
    const rate = 24000, block = new Float32Array(128);
    const probe = new SH101.MidiPart(rate, { ...settings, poly: 1 });
    probe.midi({ ...note, t: 0 });
    let power = 0, peak = 0;
    for (let i = 0; i < 80; i++) {
      probe.render(block, i * block.length / rate);
      if (i < 4) continue;
      for (const sample of block) {
        power += sample * sample;
        peak = Math.max(peak, Math.abs(sample));
      }
    }
    const level = Math.max(Math.sqrt(power / (76 * block.length)), peak * 0.18);
    return level > 0.0001 ? Math.round(clamp(20 * Math.log10(0.016 / level), 0, 12)) : 0;
  }

  class Rig {
    constructor(ctx) {
      this.ctx = ctx;
      this.rand = prng(0x5EED);
      this.tempo = new Line(145);
      this.params = knobDefs();
      this.knobs = new Map();
      this.stripInputs = new Map();
      for (const d of this.params) this.knobs.set(d.id, { desc: d, line: new Line(d.value), binds: [], watchers: [] });
      this.lampT = new Float64Array(8 * LAMPS).fill(-1e9);
      this.lampV = new Float64Array(8 * LAMPS);

      this.noise = ctx.createBuffer(1, Math.floor(ctx.sampleRate * 2), ctx.sampleRate);
      const nd = this.noise.getChannelData(0);
      for (let i = 0; i < nd.length; i++) nd[i] = this.rand() * 2 - 1;

      this.buildMaster();
      this.sendFx = new SendFx(this);
      this.delayIn = this.sendFx.delayIn;
      this.reverbIn = this.sendFx.reverbIn;

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

      this.sh101Module = SH101 && ctx.audioWorklet && typeof AudioWorkletNode !== 'undefined'
        ? Promise.resolve().then(() => ctx.audioWorklet.addModule(HERE + 'sh101.js' + QUERY)).then(() => true, () => false)
        : Promise.resolve(false);
      this.sh101 = Object.fromEntries(['bass', 'lead', 'arp', 'pad'].map(role => [role, new SH101Voice(this, role)]));
      this.fx = new Fx(this);
      this.sh101.fx = this.fx.sh101;
      this.inst = [
        new Drums(this), this.sh101.bass,
        new AcidVoice(this, 'acid1', 'saw'), new AcidVoice(this, 'acid2', 'square'),
        this.sh101.lead, this.sh101.arp, this.fx, this.sh101.pad,
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
        .then(() => ctx.audioWorklet.addModule(HERE + 'acid.js' + QUERY))
        .then(() => ctx.audioWorklet.addModule(HERE + 'dsp.js' + QUERY))
        .then(() => { for (const a of acids) a.worklet(); return 'worklet'; })
        .catch(inline);
    }

    // -- construction helpers
    gain(v) { const g = this.ctx.createGain(); g.gain.value = v; return g; }
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

    // Channel strip: level knob (square taper) -> pan -> mix, plus sends
    // taken after the level, before the pan. A strip with an `id` takes its
    // pan and sends from the knobs `<id>.pan`, `<id>.delay`, `<id>.reverb`
    // (`sends: false` for none; `revK`, `delayK` scale a send, 0 for none).
    // One without an id is fixed: `rev` and `delay` are send gains.
    strip(src, o) {
      const lvl = this.gain(o.k);
      if (o.level) this.bind(o.level, lvl.gain, (x) => o.k * x * x);
      src.connect(lvl);
      if (!o.id) {
        lvl.connect(this.mix);
        if (o.delay) chain(lvl, this.gain(o.delay), this.delayIn);
        if (o.rev) chain(lvl, this.gain(o.rev), this.reverbIn);
        return;
      }
      this.stripInputs.set(o.id, lvl);
      // A mono source panned to the centre comes out 3 dB down on each side.
      // A strip that had no panner before gets that back, so the mix stays
      // where it was calibrated; one that had (`panned`) was set with it.
      const pan = this.ctx.createStereoPanner();
      this.bind(o.id + '.pan', pan.pan, (x) => 2 * x - 1);
      if (o.panned) chain(lvl, pan, this.mix);
      else chain(lvl, this.gain(Math.SQRT2), pan, this.mix);
      if (o.sends === false) return;
      for (const [name, bus, scale] of [['delay', this.delayIn, o.delayK], ['reverb', this.reverbIn, o.revK]]) {
        if (scale === 0) continue;
        const s = this.gain(0), k = scale == null ? 1 : scale;
        this.bind(o.id + '.' + name, s.gain, (x) => k * x);
        chain(lvl, s, bus);
      }
    }

    async midiParts(song) {
      const ctx = this.ctx, worklet = await this.sh101Module;
      const hasGate = song.tracks.some(part => part.role === 'gate');
      const drum = this.inst[0];
      const sources = new Map();
      const gates = [];
      let gateCount = 0;
      for (const track of song.tracks) {
        if (!track.notes || track.drum || track.role === 'gate') continue;
        const settings = { role: track.role, track: track.track,
          patch: { ...track.patch?.settings, ...track.controls }, poly: track.poly };
        track.levelDb ??= midiPartTrim(track, settings);
        let node, send;
        if (worklet) {
          node = new AudioWorkletNode(ctx, 'rebirth-midi-part', {
            numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [1], processorOptions: settings,
          });
          send = e => node.port.postMessage(e);
        } else {
          const part = new SH101.MidiPart(ctx.sampleRate, settings);
          node = ctx.createScriptProcessor(1024, 0, 1);
          node.onaudioprocess = e => {
            const out = e.outputBuffer.getChannelData(0);
            for (let i = 0; i < out.length; i += 128) part.render(out.subarray(i, i + 128), e.playbackTime + i / ctx.sampleRate);
          };
          send = e => {
            if (e.type === 'control') part.control(e.id, e.value);
            else part.midi(e);
          };
        }
        const trim = MIDI_PART_GAIN[track.role];
        const level = this.gain(trim * 10 ** ((track.levelDb ?? 0) / 20));
        const gate = this.gain(1);
        if (track.gated && hasGate) gates.push(gate.gain);
        chain(node, level, gate, this.stripInputs.get(track.role));
        sources.set(track.track, { node, level: level.gain, send, trim, db: track.levelDb ?? 0, volume: 1, expression: 1 });
      }
      const updateLevel = (source, t) => {
        source.level.setTargetAtTime(source.trim * 10 ** (source.db / 20) * source.volume * source.expression, t, 0.003);
      };
      return {
        control(track, id, value) {
          sources.get(track)?.send({ type: 'control', id, value });
        },
        level(track, db) {
          const source = sources.get(track);
          if (!source) return;
          source.db = db;
          updateLevel(source, ctx.currentTime);
        },
        midi(e, t) {
          const part = song.tracks[e.track];
          const type = e.status & 0xf0, on = type === 0x90 && e.d2 > 0;
          if (part.role === 'gate') {
            if (on) gateCount++;
            else if ((type === 0x80 || type === 0x90) && gateCount) gateCount--;
            for (const gate of gates) gate.setTargetAtTime(gateCount ? 1 : 0.06, t, 0.004);
          } else if (part.drum) {
            if (!on) return;
            const note = part.drum === 42 && [42, 44, 46].includes(e.d1) ? e.d1 : part.drum;
            drum.on(note, e.d2, t, part.role === 'kick' ? 0.62 : 0.82);
          } else {
            const source = sources.get(e.track);
            if (!source) return;
            if (type === 0xb0 && (e.d1 === 7 || e.d1 === 11)) {
              if (e.d1 === 7) source.volume = e.d2 / 127;
              else source.expression = e.d2 / 127;
              updateLevel(source, t);
            } else source.send({ ...e, t });
          }
        },
        stop(t) {
          drum.kill(t);
          for (const { node } of sources.values()) {
            node.disconnect();
            node.port?.close();
            if ('onaudioprocess' in node) node.onaudioprocess = null;
          }
          sources.clear();
        },
      };
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
      chain(this.mix, satIn, sat, comp, vol, clip, ctx.destination);
      // One analyser per channel: an analyser reads its input mixed down to mono,
      // which would halve a peak that is on one side only.
      const split = ctx.createChannelSplitter(2);
      clip.connect(split);
      this.analysers = [0, 1].map((ch) => {
        const a = ctx.createAnalyser();
        a.fftSize = 2048;
        split.connect(a, ch);
        return a;
      });
    }

    // -- public interface
    midi(status, d1, d2, time) {
      try {
        status |= 0;
        if (status < 0x80 || status > 0xEF) return;
        const type = status & 0xF0, channel = status & 15;
        if (type !== 0x80 && type !== 0x90 && type !== 0xB0 && type !== 0xE0) return;
        const now = this.ctx.currentTime, t = clamp(num(time, now), now, now + 1e5);
        const a = d1 & 127, b = d2 & 127, i = CH_INDEX[channel], inst = this.inst[i];
        if (!inst) return;
        if (type === 0xE0) inst.bend?.(((b << 7) + a - 8192) / 8192, t);
        else if (type === 0xB0) {
          if (a === 120 || a === 123) inst.kill(t, false);
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
        this.sendFx.retime(T);
      } catch (e) { /* ignore */ }
    }

    allOff(time, includeCV = true) {
      try {
        const now = this.ctx.currentTime, t = clamp(num(time, now), now, now + 1e5);
        for (const inst of this.inst) inst.kill(t, includeCV);
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
  Rig.scoreValue = (id, x) => {
    const range = SCORE_CUTOFF[id];
    return range && SH101 ? SH101.cutoffPosition(range[0] * (range[1] / range[0]) ** x) : x;
  };
  if (typeof window !== 'undefined') window.Rig = Rig;
  if (typeof module !== 'undefined' && module.exports) module.exports = Rig;
})();
