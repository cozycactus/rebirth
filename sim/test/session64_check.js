'use strict';

const assert = require('node:assert/strict');
const Core = require('../web/core.js');
const { Device } = require('../web/device.js');

(async () => {
  const device = new Device(await Core());
  const advance = us => device.runUntil(device.now() + us);
  function tap(key, held = 80000) {
    key(true);
    advance(held);
    key(false);
    advance(90000);
  }
  function nextPage() {
    // Move focus to the last column, then turn TRACK one detent.
    tap(down => device.pad(7, 0, down));
    const start = device.now();
    for (let edge = 0; edge < 4; edge++) device.encoderEdge(0, 1, start + 1000 + edge * 4000);
    advance(90000);
  }
  const selector = () => tap(down => device.encoderSwitch(1, down), 700000);
  advance(250000);
  for (let page = 1; page < 8; page++) nextPage();
  tap(down => device.pad(7, 5, down));
  assert.equal(device.current(63), 5, 'the last column on page eight launches T64');
  for (let t = 0; t < 63; t++) assert.equal(device.current(t), 0);
  tap(down => device.step(4, down));
  assert.ok(device.stepData(5, 63, 4).active, 'step keys edit the focused slot on T64');
  assert.ok(!device.stepData(5, 7, 4).active, 'T8 has its own clip');
  tap(down => device.scene(2, down));
  for (let t = 0; t < 64; t++) assert.equal(device.current(t), 2, 'SCENE includes track ' + (t + 1));
  selector();
  tap(down => device.step(8, down));
  device.boot(true);
  advance(250000);
  selector();
  tap(down => device.step(9, down));
  assert.ok(device.stepData(5, 63, 4).active, 'T64 step survives SAVE, reboot and LOAD');
  assert.ok(!device.stepData(5, 7, 4).active);
  assert.equal(device.inspect().fatal, null);
  console.log('PASS native 64-track Session panel, global SCENE and flash');
})().catch(error => { console.error(error); process.exitCode = 1; });
