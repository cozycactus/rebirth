// Reproducible traces at the 80C49 pins. Set SH101_ROM to run the original
// image scenarios. The synthetic program also runs in CI without the image.
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const createCore = require('../web/sh101_cpu.js');
const { Board, BUTTON, HZ, Player, decode } = require('../web/sh101_rom.js');
const { Voice, defaults } = require('../web/sh101.js');

const keys = (...notes) => notes.reduce((mask, note) => mask | (1 << (note - 53)), 0);

function bench(board, settings = {}) {
  Object.assign(board, { external: true, ...settings });
  const state = () => decode(board.state);
  const advance = (ms = 12) => {
    const trace = [], start = board.cycles;
    board.advanceTo(start + HZ * ms / 1000, (cycle, word) => trace.push({ ...decode(word), cycle: cycle - start }));
    return trace;
  };
  const tap = (name) => {
    board.buttons = BUTTON[name]; advance();
    board.buttons = 0; advance();
  };
  const notes = (...held) => { board.keys = keys(...held); return advance(); };
  const pulse = () => {
    board.clockHigh = true;
    const trace = advance();
    const high = state();
    board.clockHigh = false;
    assert.deepEqual(advance(), [], 'the falling clock edge leaves the outputs alone');
    assert.deepEqual(state(), high);
    return trace;
  };
  advance();
  return { board, state, advance, tap, notes, pulse };
}

async function testBus() {
  await assert.rejects(Board.load(createCore, new Uint8Array(2047)), /2048/);
  await assert.rejects(Board.load(createCore, new Uint8Array(2048)), /80C49-6-7301/);

  // OUTL BUS to each S&H channel, OUTL P2, then loop on JMP.
  const image = new Uint8Array(2048);
  image.set([0x23, 0x3e, 0x02, 0x23, 0x57, 0x02, 0x23, 0xa5, 0x02, 0x23, 0x37, 0x3a, 0x04, 0x0c]);
  const board = new Board(await createCore(), image);
  const events = [];
  board.advanceTo(2000, (cycle, state) => {
    const { pitch, range, random, gate } = decode(state);
    events.push([cycle, pitch, range, random, gate]);
  });
  assert.deepEqual(events, [
    [2, 1, 0, 0, false], [6, 1, 40, 0, false], [10, 1, 40, 26, false], [14, 1, 40, 26, true],
  ]);
}

async function testROM(image) {
  const setup = async (settings) => bench(await Board.load(createCore, image), settings);
  const play = await setup();
  assert.equal(play.state().gate, false);
  for (let note = 53; note <= 84; note++) {
    play.notes(note);
    assert.equal(play.state().pitch, 17 + note - 53, 'keyboard matrix at MIDI note ' + note);
    assert.equal(play.state().gate, true);
    play.notes();
    assert.equal(play.state().gate, false);
  }
  play.notes(53); play.notes(53, 60);
  assert.equal(play.state().pitch, 24, 'GATE + TRIG uses last priority');
  play.board.trigger = 1; play.advance();
  assert.equal(play.state().pitch, 17, 'GATE uses low priority');
  for (const octave of [0, 1, 2]) {
    play.board.octave = octave; play.advance();
    assert.equal(play.state().pitch, 5 + octave * 12);
  }
  for (const range of [0, 1, 2, 3]) {
    play.board.range = range; play.advance();
    assert.equal(play.state().range, 12 + range * 12);
  }
  play.board.range = 1; play.board.octave = 1; play.notes();
  const transposeWith = (note) => {
    play.board.buttons = BUTTON.transpose; play.advance(); play.notes(note);
    play.board.buttons = 0; play.notes();
  };
  transposeWith(53);
  assert.equal(play.state().range, 17, 'F3 shifts RANGE down seven semitones');
  assert.equal(play.state().transpose, true);
  transposeWith(60);
  assert.equal(play.state().range, 24, 'C4 restores normal RANGE');
  assert.equal(play.state().transpose, false);

  for (const [mode, pitches] of Object.entries({
    up: [21, 24, 17, 21, 24, 17], down: [24, 21, 17, 24, 21, 17], upDown: [21, 24, 21, 17, 21, 24],
  })) {
    const arp = await setup();
    arp.tap(mode); arp.notes(53, 57, 60);
    assert.equal(arp.state().mode, mode);
    assert.equal(arp.state().glide, false, 'ARP bypasses AUTO portamento');
    const observed = [];
    for (const pitch of pitches) {
      arp.pulse();
      assert.equal(arp.state().pitch, pitch);
      assert.equal(arp.state().gate, true);
      observed.push(arp.state().random);
    }
    assert.deepEqual(observed, [54, 25, 28, 40, 12, 57], 'ROM updates the random DAC on each rising edge');
    arp.tap('hold'); arp.notes(); arp.pulse();
    assert.equal(arp.state().hold, true);
    assert.equal(arp.state().gate, true);
    arp.tap('hold');
    assert.equal(arp.state().gate, false);
    arp.notes(53, 60); arp.board.pedal = true; arp.advance(); arp.notes(); arp.pulse();
    assert.equal(arp.state().hold, true, 'the T0 pedal holds the arpeggio');
    arp.board.pedal = false; arp.advance();
    assert.equal(arp.state().gate, false);
  }

  const seq = await setup({ trigger: 1 });
  seq.tap('load'); seq.notes(53); seq.notes();
  seq.board.buttons = BUTTON.hold; seq.advance(); seq.notes(60);
  seq.board.buttons = 0; seq.notes();
  seq.tap('transpose'); seq.notes(55); seq.notes(); seq.tap('load');
  seq.tap('play');
  assert.equal(seq.state().mode, 'play');
  assert.equal(seq.state().gate, false, 'PLAY waits for external clock');
  seq.pulse(); assert.equal(seq.state().pitch, 17);
  const legato = seq.pulse();
  assert.ok(legato.length && legato.every(e => e.gate), 'F3 to C4 is tied with HOLD depressed while entering C4');
  assert.equal(seq.state().pitch, 24);
  seq.pulse(); assert.equal(seq.state().gate, false, 'REST lowers Gate');
  seq.pulse(); assert.equal(seq.state().pitch, 19);
  const loop = seq.pulse();
  assert.deepEqual(loop.filter((e, i) => i === 0 || loop[i - 1].gate !== e.gate).map(e => e.gate), [true, false, true]);
  assert.equal(seq.state().pitch, 17, 'loop returns to F3 and re-triggers Gate');
  seq.tap('play'); seq.pulse();
  assert.equal(seq.state().mode, null);
  assert.equal(seq.state().gate, false);
  seq.tap('play'); seq.pulse();
  assert.equal(seq.state().gate, true);
  seq.board.powerOn();
  assert.equal(seq.board.cycles, 0);
  seq.advance();
  assert.equal(seq.state().mode, null, 'POWER restarts the ROM idle');
  assert.equal(seq.state().gate, false);
  seq.tap('play'); seq.pulse();
  assert.equal(seq.state().pitch, 17, 'battery-backed sequence survives POWER');
  seq.pulse();
  assert.equal(seq.state().pitch, 24);

  const internal = await setup({ external: false, rate: 4 });
  internal.advance(170); internal.tap('up');
  assert.ok(internal.board.phase > 0.5);
  const reset = internal.notes(53, 60).filter(e => e.reset);
  assert.ok(reset.length && internal.board.phase < 0.06, 'ARP key press asserts P24 and restarts the LFO');
  const trace = internal.advance(520);
  assert.equal(trace.filter(e => e.pitch === 17 && e.random === 25).length, 1);
  assert.equal(trace.at(-1).pitch, 24);

  // The same scheduler and DSP entry points used by the browser. Check the
  // selected oscillator octave through RANGE CV and check that Gate silences it.
  const board = await Board.load(createCore, image);
  board.range = 2;
  const audio = new Voice(48000, false, true);
  const player = new Player(board, e => audio.enqueue(e));
  const patch = { ...defaults('rom'), saw: 1, pulse: 0, sub: 0, cutoff: 1, keyFollow: 0, envMod: 0,
    portaMode: 0, vca: 1, resonance: 0 };
  const samples = new Float32Array(480);
  let energy = 0, crossings = 0, previous = 0;
  for (let frame = 0; frame < 55; frame++) {
    const now = frame / 100;
    board.keys = frame < 45 ? keys(57) : 0;
    player.tick(now, now);
    audio.render(samples, now, patch);
    if (frame >= 10 && frame < 40) for (const y of samples) {
      energy += y * y;
      if (previous < 0 && y >= 0) crossings++;
      previous = y;
    }
  }
  assert.ok(energy > 1 && samples.every(y => Math.abs(y) < 1e-8), 'audio follows ROM Gate and decays to silence');
  assert.ok(Math.abs(crossings / 0.3 - 440) < 4, 'A3 with 4′ produces A4');
}

async function main() {
  await testBus();
  const fixture = process.env.SH101_ROM || path.join(__dirname, '../web/sh101.rom');
  if (process.env.SH101_ROM || fs.existsSync(fixture)) {
    await testROM(fs.readFileSync(fixture));
    console.log('SH-101 ROM: bus/DAC, 32 keys, priority, transpose, ARP/HOLD, SEQ/LEGATO/REST, POWER, clocks and gated audio OK');
  } else {
    console.log('SH-101 ROM: bus/DAC OK; original ROM scenarios require SH101_ROM=/path/to/sh101.rom');
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
