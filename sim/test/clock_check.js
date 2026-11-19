// The device following an external MIDI clock, without a browser: the clock
// source set from the panel, the measured tempo, Start, one Clock out per Clock
// in, steps on their Clocks, Stop, a lost clock in EXT and in AUTO, and Continue
// after a Song Position Pointer.   node sim/test/clock_check.js
'use strict';
const path = require('path');
const web = path.join(__dirname, '..', 'web');
const RebirthCore = require(path.join(web, 'core.js'));
const { Device } = require(path.join(web, 'device.js'));

let failures = 0;
const check = (ok, message) => { if (!ok) { failures++; console.log('FAIL ' + message); } };

const T145 = 60e6 / (145 * 24);           // a Clock interval at 145 BPM, us
const BYTE_US = 320;

RebirthCore().then(core => {
  const device = new Device(core);
  if (!device.features().extClock) {
    console.log('this firmware does not follow an external clock: nothing to check');
    process.exit(0);
  }
  const out = [];
  const settle = ms => {
    device.runUntil(device.now() + ms * 1000);
    for (const e of device.drain()) if (e.type === 'midi') out.push(e);
  };
  const tapStep = i => { device.step(i, true); settle(90); device.step(i, false); settle(90); };
  const longValue = () => { device.encoderSwitch(1, true); settle(620); device.encoderSwitch(1, false); settle(150); };
  const state = () => device.inspect();

  // Bytes on the DIN input; `at` is when the first one starts. Returns when each
  // one has arrived (its stop bit ended).
  const send = (bytes, at) => bytes.map((b, i) => {
    const start = at + i * BYTE_US;
    device.input(3, b & 0xFF, 0, 0, start + BYTE_US);
    return start + BYTE_US;
  });
  // `n` Clocks from `from`, `interval` apart: their arrival times.
  const clocks = (from, n, interval) => {
    const times = [];
    for (let i = 0; i < n; i++) times.push(send([0xF8], from + i * (interval || T145))[0]);
    return times;
  };
  const midiAfter = t => out.filter(e => e.t > t);

  settle(300);

  // ─── The source from the panel: Pattern Select, step 15 ──────
  longValue();                                       // Session -> Pattern Select
  tapStep(14);                                       // INT -> EXT
  check(state().clockSource === 1, 'step 15 did not set the clock source to EXT: ' + state().clockSource);
  check((device.stepLeds() >>> 14) & 1, 'step 15 is not lit with EXT');
  longValue();                                       // back to the Session view
  check(state().state === 0, 'the transport started by itself');
  // The power-up demo plays: a kick on every beat and a kit's closed hat on every step,
  // both on channel 10.
  console.log('clock source EXT set from Pattern Select step 15');

  // ─── Clocks while stopped: the tempo is measured, nothing plays ──
  let t = device.now() + 1000;
  let arrived = clocks(t, 120);
  device.runUntil(arrived[arrived.length - 1] + 1000);
  settle(1);
  check(Math.abs(state().extBpm - 145) <= 0.1, 'measured tempo ' + state().extBpm + ', want 145.0');
  check(state().state === 0, 'Clocks alone started the transport');
  check(!out.some(e => e.status === 0xF8), 'a Clock went out while stopped');
  console.log('stopped, 120 Clocks at 145 BPM: shown tempo ' + state().extBpm.toFixed(1) + ', nothing sent');

  // ─── Start: the first Clock is beat 1; one Clock out per Clock in ──
  t = arrived[arrived.length - 1] + T145 - BYTE_US;
  const startAt = send([0xFA], t - 2000)[0];
  arrived = clocks(t, 6 * 16 * 2);                    // two bars
  device.runUntil(arrived[arrived.length - 1] + T145 - 300);   // the last Clock's pulses ran
  settle(0);
  const run = midiAfter(startAt - 1);
  const clocksOut = run.filter(e => e.status === 0xF8);
  const kicks = run.filter(e => e.status === 0x99 && e.d1 === 36 && e.d2 > 0);
  const hats = run.filter(e => e.status === 0x99 && e.d1 === 42 && e.d2 > 0);
  check(state().state === 1 && state().following, 'not playing on the Clock after Start');
  check(run.length && run[0].status === 0xFA, 'MIDI Start did not go out first');
  check(clocksOut.length === arrived.length, clocksOut.length + ' Clocks out for ' + arrived.length + ' in');
  check(state().pulse === 4 * arrived.length, 'pulse ' + state().pulse + ' after ' + arrived.length + ' Clocks');
  check(kicks.length === 8, kicks.length + ' kicks in two bars, want 8');
  check(hats.length === 32, hats.length + ' hats in two bars, want 32');
  let worst = 0;
  kicks.forEach((n, i) => { worst = Math.max(worst, Math.abs(n.t - arrived[i * 24])); });  // a beat: 24 Clocks
  hats.forEach((n, i) => { worst = Math.max(worst, Math.abs(n.t - arrived[i * 6])); });    // a step: 6
  check(worst < 3000, 'a note left ' + worst + ' us from its Clock');
  const clockLag = Math.max(...clocksOut.map((c, i) => c.t - arrived[i]));
  check(clockLag < 1700, 'a Clock went out ' + clockLag + ' us after it came in');
  console.log('Start: ' + clocksOut.length + ' Clocks out for ' + arrived.length + ' in (at most ' +
    Math.round(clockLag) + ' us later), every kick and hat within ' + Math.round(worst) + ' us of its Clock');

  // ─── Stop from the wire ───────────────────────────────────────
  const stopAt = send([0xFC], device.now() + 10)[0];
  device.runUntil(stopAt + 1000);
  settle(1);
  check(state().state === 0 && !state().following, 'Stop did not stop');
  check(device.gates() === 0, 'a gate is high after Stop');
  check(midiAfter(stopAt - 1).some(e => e.status === 0xFC), 'no MIDI Stop out');
  console.log('Stop: stopped, gates low, Stop sent');

  // ─── Continue after a Song Position Pointer (bar 2) ───────────
  t = device.now() + 5000;
  send([0xF2, 16, 0], t);                           // SPP 16: the second bar
  send([0xFB], t + 4 * BYTE_US);
  arrived = clocks(t + 3000, 1);
  device.runUntil(arrived[0] + 500);
  settle(1);
  check(state().state === 1, 'Continue after SPP did not start on the Clock');
  check(state().pulse === 16 * 24 + 1, 'SPP 16: pulse ' + state().pulse + ', want ' + (16 * 24 + 1));
  check(midiAfter(t).some(e => e.status === 0xFB), 'no MIDI Continue out');
  console.log('SPP 16 then Continue: plays from bar 2 on the first Clock');

  // ─── The clock is lost in EXT: stop, notes and gates off ──────
  arrived = clocks(arrived[0] + T145 - BYTE_US, 30);
  const last = arrived[arrived.length - 1];
  device.runUntil(last + 2 * T145);
  check(state().state === 1, 'stopped before the loss timeout');
  device.runUntil(last + 3 * T145 + 2000);
  settle(1);
  check(state().state === 0, 'still playing ' + Math.round((device.now() - last) / 1000) + ' ms after the last Clock');
  check(device.gates() === 0, 'a gate is high after the clock was lost');
  check(midiAfter(last).some(e => e.status === 0xFC), 'no MIDI Stop after the clock was lost');
  check(state().extLosses === 1, 'losses ' + state().extLosses);
  console.log('EXT, Clocks stop: stopped ' + Math.round(3 * T145 / 1000) + ' ms after the last one, gates low, Stop sent');

  // ─── AUTO: plays on through a loss, then follows again ────────
  longValue();
  tapStep(14);                                       // EXT -> AUTO
  check(state().clockSource === 2, 'step 15 did not move on to AUTO');
  longValue();
  t = device.now() + 1000;
  send([0xFA], t);
  arrived = clocks(t + 2000, 96);
  device.runUntil(arrived[arrived.length - 1] + 500);
  check(state().state === 1 && state().following, 'AUTO is not following the Clock after Start');
  device.runUntil(arrived[arrived.length - 1] + 1000000);   // a second with no Clock
  check(state().state === 1 && !state().following, 'AUTO did not play on through the loss');
  check(Math.abs(state().bpm - 145) < 0.1, 'AUTO plays on at ' + state().bpm + ' BPM, want 145');
  const p = state().pulse;
  device.runUntil(device.now() + 1000000);
  const perSecond = state().pulse - p;
  check(Math.abs(perSecond - 145 * 96 / 60) <= 2, perSecond + ' pulses a second on its own timer');
  arrived = clocks(device.now() + 3000, 12);
  device.runUntil(arrived[arrived.length - 1] + 500);
  check(state().state === 1 && state().following, 'AUTO did not follow the returning Clock');
  check(state().pulse % 4 === 1, 'not on a Clock boundary after the relock: pulse ' + state().pulse);
  console.log('AUTO: played on for a second at ' + state().bpm.toFixed(1) + ' BPM, followed the Clock again on its return');

  const s = state();
  check(s.fatal === null, 'fatal: ' + s.fatal);
  check(!s.midiOverflow, 'MIDI output overflowed');
  check(s.lockErrors === 0, 'lock errors: ' + s.lockErrors);
  console.log(failures ? 'FAILED: ' + failures + ' checks' : 'PASS clock');
  process.exit(failures ? 1 : 0);
});
