// Checks of the device model against the firmware, without a browser:
// slide ordering on the MIDI wire, the flash save, and recovery after a chip
// reset.   node sim/test/device_check.js
'use strict';
const path = require('path');
const web = path.join(__dirname, '..', 'web');
const RebirthCore = require(path.join(web, 'core.js'));
const { Device, Hands } = require(path.join(web, 'device.js'));
const Artist = require(path.join(web, 'artist.js'));

let failures = 0;
const check = (ok, message) => { if (!ok) { failures++; console.log('FAIL ' + message); } };

RebirthCore().then(core => {
  const device = new Device(core);
  const hands = new Hands(device);
  const show = Artist.buildShow(undefined, { fw2: process.env.FW2 === '1' || process.env.KEYS === '1', keys: process.env.KEYS === '1' });
  Artist.schedule(show, device, hands, null);
  const midi = [];
  const run = until => {
    while (device.now() < until) {
      hands.advance(Math.min(until, device.now() + 500000));
      for (const e of device.drain()) if (e.type === 'midi') midi.push(e);
    }
  };

  // ─── Flash save at the end of the preparation ─────────────────
  run(show.prepEnd);
  const store = core.HEAPU8.subarray(core._sim_flash_store(), core._sim_flash_store() + 16);
  const magic = (store[0] | store[1] << 8 | store[2] << 16 | store[3] << 24) >>> 0;
  check(magic === 0x52425448, 'flash header magic is ' + magic.toString(16));
  console.log('flash: header magic ' + magic.toString(16) + ', version ' + store[4]);

  // ─── Slide: the next Note On leaves before the old Note Off ───
  // ACID 1 (channel 4 on the wire, index 3) plays row 2 from bar 21: step 3 is
  // A3 with slide, step 4 is A2.
  run(show.t0 + 22 * show.barUs);
  const from = show.t0 + 20 * show.barUs;
  const acid = midi.filter(m => m.t >= from && (m.status & 15) === 3);
  const text = acid.slice(0, 12).map(m => ((m.t - from) / 1000).toFixed(1) + (m.status >> 4 === 9 && m.d2 ? ' on ' : ' off ') + m.d1);
  console.log('acid line, ms from bar 21: ' + text.join(' | '));
  const onA2 = acid.findIndex((m, i) => i > 0 && m.status >> 4 === 9 && m.d2 > 0 && m.d1 === 45 && acid[i - 1].d1 === 57);
  check(onA2 > 0, 'no slide A3 -> A2 found');
  if (onA2 > 0) {
    const off = acid.findIndex((m, i) => i > onA2 && m.d1 === 57 && (m.status >> 4 === 8 || m.d2 === 0));
    check(off === onA2 + 1, 'the slid-from note should be released right after the new Note On');
    check(acid[off].t - acid[onA2].t < 2500, 'release ' + (acid[off].t - acid[onA2].t) + ' us after the new note');
    console.log('slide: Note On 45 at ' + ((acid[onA2].t - from) / 1000).toFixed(2) + ' ms, Note Off 57 ' +
      (acid[off].t - acid[onA2].t) + ' us later');
  }

  // ─── A chip that resets while the set plays ───────────────────
  const snapshot = () => JSON.stringify({ cur: [0, 1, 2, 3, 4, 5, 6, 7].map(t => device.current(t)),
    q: [0, 1, 2, 3, 4, 5, 6, 7].map(t => device.queued(t)), mutes: device.inspect().mutes,
    steps: [0, 1, 2, 3].map(l => [0, 2, 3].map(t => { let m = 0; for (let s = 0; s < 16; s++) if (device.stepData(l, t, s).active) m |= 1 << s; return m; })) });
  hands.clear();
  for (const [addr, name] of [[0x34, 'TCA8418'], [0x20, 'MCP23017 buttons'], [0x21, 'MCP23017 step LEDs']]) {
    const before = snapshot();
    device.chipReset(addr);
    run(device.now() + 2500000);
    check(snapshot() === before, name + ' reset changed the device state');
    check(device.inspect().state === 1, name + ' reset stopped the transport');
  }
  // The controls work again afterwards.
  const tap = (down, up) => { down(); run(device.now() + 90000); up(); run(device.now() + 90000); };
  const mutes = device.inspect().mutes;
  tap(() => device.mute(7, true), () => device.mute(7, false));
  check(device.inspect().mutes === (mutes ^ 0x80), 'MUTE does not work after the resets');
  tap(() => device.pad(0, 0, true), () => device.pad(0, 0, false));     // focus: KICK, row 1
  const kick = device.stepData(0, 0, 1).active;
  tap(() => device.step(1, true), () => device.step(1, false));
  check(device.stepData(0, 0, 1).active === !kick, 'step buttons do not work after the resets');
  check(device.stepLeds() !== 0, 'step LEDs stay dark after the reset');
  check(device.inspect().fatal === null, 'fatal: ' + device.inspect().fatal);
  console.log('chip resets: state kept, controls and step LEDs back');

  // ─── A bus that hangs must not cost the timer its pulses ──────
  // Every transfer to the key scanner takes its whole 2 ms timeout; core 1 and
  // its 96 PPQN timer keep running on the real board.
  {
    const count = () => { let n = 0; for (const e of device.drain()) if (e.type === 'midi' && e.status === 0xF8) n++; return n; };
    count();
    const overruns = device.inspect().overruns;
    device.i2cFault(0x34, 2);
    const from = device.now();
    device.runUntil(from + 2000000);
    const clocks = count();
    device.i2cFault(0x34, 0);
    const want = 2000000 / (Math.floor(60000000 / (145 * 96)) * 4);
    check(Math.abs(clocks - want) <= 1, 'hung I2C: ' + clocks + ' clocks in 2 s, want ' + want.toFixed(1));
    check(device.inspect().overruns === overruns, 'hung I2C: timer overruns ' + (device.inspect().overruns - overruns));
    console.log('hung I2C bus: ' + clocks + ' MIDI clocks in 2 s (' + want.toFixed(1) + ' expected), no timer overrun');
  }

  // ─── A key pressed while the flash save stalls the device ─────
  // PLAY goes down in the middle of the save and stays down: the firmware
  // resyncs its inputs after the stall and must not take it as a command.
  {
    const fresh = new Device(core);         // power cycle
    const settle = ms => fresh.runUntil(fresh.now() + ms * 1000);
    settle(300);
    fresh.encoderSwitch(1, true); settle(620); fresh.encoderSwitch(1, false); settle(150);    // selector
    const saveAt = fresh.now();
    fresh.step(8, true);                                // step 9: save
    fresh.play(true, saveAt + 400000);                  // during the erase
    fresh.step(8, false, saveAt + 500000);
    settle(2500);
    const store = core.HEAPU8.subarray(core._sim_flash_store(), core._sim_flash_store() + 4);
    check(store[0] === 0x48 && store[3] === 0x52, 'the save did not happen');
    check(fresh.now() - saveAt >= 2500000 && fresh.inspect().state === 0, 'PLAY pressed during the save started the transport');
    fresh.play(false); settle(100);
    check(fresh.inspect().state === 0, 'releasing PLAY after the save started the transport');
    fresh.play(true); settle(80); fresh.play(false); settle(80);
    check(fresh.inspect().state === 1, 'PLAY does not work after the save');
    console.log('flash save: PLAY held through the stall is ignored, the next press works');
  }

  console.log(failures ? 'FAILED: ' + failures + ' checks' : 'PASS device');
  process.exit(failures ? 1 : 0);
});
