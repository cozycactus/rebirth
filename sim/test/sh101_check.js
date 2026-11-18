// Offline checks of the SH-101 model. Reference data are the component
// values and controls in Roland's manuals, not bench recordings.
'use strict';

const assert = require('node:assert/strict');
const { Filter, Voice, defaults } = require('../web/sh101.js');

const fs = 48000, rate = 2 * fs;
const rms = (x) => Math.sqrt(x.reduce((sum, sample) => sum + sample * sample, 0) / x.length);
const frequency = (x, sampleRate) => {
  const mean = x.reduce((sum, sample) => sum + sample, 0) / x.length;
  let crossings = 0;
  for (let i = 1; i < x.length; i++) if (x[i - 1] < mean && x[i] >= mean) crossings++;
  return crossings * sampleRate / x.length;
};

function filterTail(cutoff, resonance, input, diodes = 0) {
  const filter = new Filter(rate, diodes), tail = new Float64Array(rate / 2);
  for (let i = 0; i < rate + tail.length; i++) {
    const y = filter.step(input(i), cutoff, resonance);
    if (i >= rate) tail[i - rate] = y;
  }
  return tail;
}

{
  const impulse = i => i === 0 ? 0.01 : 0;
  const roland = filterTail(600, 1, impulse), revA = filterTail(600, 1, impulse, 1);
  const ratio = rms(revA) / rms(roland);
  assert.ok(ratio > 0.975 && ratio < 0.985, '1N4148 limits oscillation about 2% earlier');
  assert.ok(Math.abs(frequency(revA, rate) - frequency(roland, rate)) <= 2, 'diode choice preserves filter tracking');
}

// Four independent C/OTA poles have -12.04 dB at their common pole
// frequency. Compare against the continuous-time, small-signal transfer.
for (const hz of [100, 600, 1200, 2400, 4800]) {
  const y = filterTail(600, 0, (i) => 0.1 * Math.sin(2 * Math.PI * hz * i / rate));
  const expected = 0.1 / Math.sqrt(2) / (1 + (hz / 600) ** 2) ** 2;
  assert.ok(Math.abs(20 * Math.log10(rms(y) / expected)) < 0.35, 'four-pole response at ' + hz + ' Hz');
}

// Once excited, the closed loop sustains at the cutoff with RES at max.
// The two diodes bound the signal without any hard output clip.
for (const cutoff of [110, 440, 1760]) {
  const impulse = (i) => i === 0 ? 0.01 : 0;
  assert.ok(rms(filterTail(cutoff, 0.65, impulse)) < 1e-6, 'subcritical feedback dies away');
  const y = filterTail(cutoff, 1, impulse);
  assert.ok(rms(y) > 0.15 && rms(y) < 1, 'bounded self-oscillation');
  assert.ok(Math.abs(frequency(y, rate) - cutoff) <= 2, 'self-oscillation follows cutoff');
}

for (const [source, subMode, expected] of [['saw', 0, 440], ['sub', 0, 220], ['sub', 1, 110], ['sub', 2, 110]]) {
  const v = new Voice(fs), samples = new Float32Array(fs);
  const p = { ...defaults(), pulse: 0, saw: 0, sub: 0, [source]: 1, subMode, resonance: 0,
    vca: 1, cutoff: 1, envMod: 0, keyFollow: 0 };
  v.enqueue({ type: 'note', note: 69, down: true, t: 0 });
  v.render(samples, 0, p);
  assert.ok(Math.abs(frequency(samples.subarray(fs / 2), fs) - expected) <= 2, source + ' tracks the oscillator');
}

for (const trigger of [0, 1, 2]) {
  const v = new Voice(fs), quantum = new Float32Array(128);
  const p = { ...defaults(), trigger, attack: 0, sustain: 0.5 };
  v.enqueue({ type: 'note', note: 60, down: true, t: 0 });
  v.render(quantum, 0, p);
  assert.equal(v.envStage, 'decay', 'minimum attack reaches peak by 128 samples');
  const t = quantum.length / fs;
  v.enqueue({ type: 'note', note: 67, down: true, t });
  v.render(quantum.subarray(0, 1), t, p);
  assert.equal(v.note, trigger === 0 ? 67 : 60, 'key priority follows gate mode');
  if (trigger !== 0) assert.equal(v.envStage, 'decay', 'legato does not restart a gate envelope');
}

for (const sampleRate of [44100, 48000, 96000]) {
  const v = new Voice(sampleRate), out = new Float32Array(2 * sampleRate);
  v.enqueue({ type: 'audition', note: 48, t: 0.01 });
  v.enqueue({ type: 'note', note: 40, down: true, t: 0.9 });
  v.enqueue({ type: 'kill', t: 0.75 });
  v.render(out, 0, defaults());
  assert.ok(out.every(Number.isFinite), 'finite browser audio samples');
  assert.ok(rms(out.subarray(0, sampleRate / 2)) > 0.005, 'the test note sounds');
  assert.ok(rms(out.subarray(sampleRate)) < 1e-9, 'panic clears queued notes and releases audio');
}

for (const rise of [false, true]) {
  const fx = new Voice(fs, true), lead = new Voice(fs), p = defaults();
  const head = new Float32Array(fs / 4), tail = new Float32Array(3 * fs / 4);
  fx.enqueue({ type: 'sweep', t: 0, duration: 0.8, rise });
  lead.enqueue({ type: 'note', note: 69, down: true, t: 0 });
  fx.render(head, 0, p);
  const headPitch = fx.effectPitch, headCutoff = fx.effectCutoff;
  fx.render(head.subarray(0, 1), 0.25, { ...p, diodes: 1 });
  assert.equal(fx.filter.diodes, 1, 'FX uses its independent diode selector');
  fx.render(tail, 0.25, p);
  assert.ok(tail.every(Number.isFinite) && rms(head) > 1e-4, 'the SH-101 effect sounds');
  assert.equal(fx.effectPitch > headPitch, rise, 'pitch CV sweeps in the correct direction');
  assert.equal(fx.effectCutoff > headCutoff, rise, 'the VCF follows the sweep');
  assert.ok(tail.subarray(0.6 * fs).every(y => y === 0), 'one-shot gate ends on time');
  lead.render(tail, 0, p);
  assert.equal(lead.note, 69, 'the lead and effect have independent gates');
  assert.ok(rms(tail) > 0.005, 'the lead keeps sounding through the sweep');
}
console.log('SH-101: four-pole response, diode selection, bounded resonance, divider pitch, note priority, panic and FX sweeps OK');
