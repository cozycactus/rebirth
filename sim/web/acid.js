// acid.js: plugs the x0xb0x model of dsp.js into the rig as its two 303s.
//
// dsp.js is the x0xb0x VCF Bench model, copied unchanged from
// https://github.com/cozycactus/x0xb0x-vcf-bench (see sim/README.md). It
// carries its own sequencer; here the sequencer is the device, so this file
// only turns timed line changes (gate, accent, slide, CV) into the events the
// model's voice takes, and moves its knobs the way the Bench page does.
//
// The file is loaded three ways: as an AudioWorklet module (before dsp.js),
// as a classic script of the page (the fallback that renders on the main
// thread when a worklet cannot be loaded, as from a file:// page), and by
// Node for offline checks.
(function (root) {
  'use strict';

  const inWorklet = typeof registerProcessor === 'function';
  // A worklet module's declarations stay inside the module, but dsp.js hands
  // its classes to `module.exports` when there is one: give it one.
  if (inWorklet && typeof root.module === 'undefined') root.module = { exports: {} };

  function model() {
    if (typeof X0xVoice === 'function') return { X0xVoice, Sequencer };     // page: dsp.js is a classic script
    if (typeof module !== 'undefined' && module.exports && module.exports.X0xVoice) return module.exports;   // worklet
    return require('./dsp.js');                                             // Node
  }

  // The seven knobs of the voice, at the Bench page's own positions
  // (index.html, KNOBS).
  const KNOBS = ['cut', 'res', 'env', 'decay', 'acc', 'tune', 'vol'];
  const DEFAULTS = { cut: 0.35, res: 0.85, env: 0.6, decay: 0.35, acc: 0.8, tune: 0.5, vol: 0.75 };
  const BLOCK = 128;

  class AcidEngine {
    constructor(fs, wave) {
      const { X0xVoice } = model();
      this.fs = fs;
      this.voice = new X0xVoice(fs);
      this.voice.knob.wave = wave === 'square' ? 'square' : 'saw';
      Object.assign(this.voice.knob, DEFAULTS);
      this.queue = [];            // { t, type, value }, by context time
      this.peakUa = 0;            // ladder current, peak since takeUa(), for the page's meter
      this.last = 0;              // the voice's sample of the change handed to it last
    }

    // Line changes at context time t: [[type, value], ...] in the order given.
    lines(t, list) {
      const q = this.queue;
      let i = q.length;
      while (i > 0 && q[i - 1].t > t) i--;
      q.splice(i, 0, ...list.map(([type, value]) => ({ t, type, value })));
    }

    wave(w) { this.voice.knob.wave = w === 'square' ? 'square' : 'saw'; }

    // The ladder current's peak since the last call, in microamps.
    takeUa() { const ua = this.peakUa; this.peakUa = 0; return ua; }

    // Everything low at t, whatever was due later. The cut is made in block(),
    // which knows the sample it falls on: a gate that has already been handed
    // to the voice for a later sample must go too.
    kill(t) {
      this.queue = this.queue.filter(e => e.t < t);
      this.lines(t, [['kill', 0]]);
    }

    // One block of at most 128 samples starting at context time t0. `knobs`
    // holds the seven knob positions for this block; the voice follows them
    // over about 10 ms, as on the Bench page.
    block(out, n, t0, knobs) {
      const v = this.voice, q = this.queue, end = t0 + n / this.fs;
      for (const k of KNOBS) v.knob[k] += (knobs[k] - v.knob[k]) * 0.25;
      while (q.length && q[0].t < end) {
        const e = q.shift();
        // Never before the change handed over last: the voice takes its events
        // from the front, and a gate delayed by a sample must not be overtaken
        // by the note off that follows it on the wire.
        let at = Math.max(v.sampleIndex + Math.max(0, Math.round((e.t - t0) * this.fs)), this.last);
        if (e.type === 'kill') {
          v.events = v.events.filter(x => x.n < at);
          for (const type of ['gate', 'slide', 'accent']) v.events.push({ n: at, type, value: 0 });
        } else {
          if (e.type === 'gate' && e.value) at++;   // the gate rises a sample after the CV latches (dsp.js, Sequencer.plan)
          v.events.push({ n: at, type: e.type, value: e.value });
        }
        this.last = at;
      }
      v.process(out, n);
      const ua = v.IL * 1e6;
      if (ua > this.peakUa) this.peakUa = ua;
    }

    // Any number of samples, in blocks; knobAt(name, time) gives a knob.
    render(out, t0, knobAt) {
      const knobs = {};
      for (let i = 0; i < out.length; i += BLOCK) {
        const n = Math.min(BLOCK, out.length - i), t = t0 + i / this.fs;
        for (const k of KNOBS) knobs[k] = knobAt(k, t);
        this.block(n === out.length ? out : out.subarray(i, i + n), n, t, knobs);
      }
    }
  }

  // Runs every path of the model once so that it is compiled before the first
  // real block (what dsp.js does for its own processor; its warmUp is private).
  function warmUp(fs) {
    const { Sequencer } = model();
    const e = new AcidEngine(fs, 'saw'), v = e.voice, s = new Sequencer(v);
    s.steps = [{ note: 0, oct: 0, accent: true, slide: false, rest: false }, { note: 12, oct: 0, accent: false, slide: true, rest: false },
      { note: 3, oct: 1, accent: true, slide: true, rest: false }, { note: 0, oct: -1, accent: false, slide: false, rest: true }];
    s.bpm = 240;
    s.start(0);
    const buf = new Float32Array(BLOCK), n = Math.ceil(0.6 * fs / BLOCK);
    for (let b = 0; b < n; b++) {
      if (b === n >> 1) v.knob.wave = 'square';
      v.knob.res = 0.5 + 0.5 * Math.sin(b / 9);
      v.knob.cut = 0.5 + 0.5 * Math.sin(b / 7);
      v.knob.env = 0.5 + 0.5 * Math.sin(b / 5);
      s.plan(b * BLOCK, BLOCK);
      v.process(buf, BLOCK);
    }
  }

  if (inWorklet) {
    let warm = false;
    class AcidProcessor extends AudioWorkletProcessor {
      static get parameterDescriptors() {
        return KNOBS.map(name => ({ name, defaultValue: DEFAULTS[name], minValue: 0, maxValue: 1, automationRate: 'k-rate' }));
      }
      constructor(options) {
        super();
        if (!warm) { warm = true; warmUp(sampleRate); }
        this.engine = new AcidEngine(sampleRate, options.processorOptions && options.processorOptions.wave);
        this.knobs = {};
        this.meterEvery = Math.round(sampleRate / 30);   // the Bench page's meter rate
        this.meterCount = 0;
        this.port.onmessage = (e) => {
          if (e.data.kill) this.engine.kill(e.data.t);
          else if (e.data.wave) this.engine.wave(e.data.wave);
          else this.engine.lines(e.data.t, e.data.lines);
        };
      }
      process(inputs, outputs, parameters) {
        const out = outputs[0][0];
        for (const k of KNOBS) this.knobs[k] = parameters[k][0];
        this.engine.block(out, out.length, currentTime, this.knobs);
        this.meterCount += out.length;
        if (this.meterCount >= this.meterEvery) {
          this.meterCount = 0;
          this.port.postMessage({ ua: this.engine.takeUa() });
        }
        return true;
      }
    }
    registerProcessor('rebirth-acid', AcidProcessor);
  }

  const api = { AcidEngine, KNOBS, DEFAULTS, warmUp };
  if (inWorklet) return;
  if (typeof window !== 'undefined') window.Acid = api;
  else if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(globalThis);
