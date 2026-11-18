'use strict';

const assert = require('node:assert/strict');
const Core = require('../web/core.js');
const { Device } = require('../web/device.js');
const MidiSongs = require('../web/midi_songs.js');

function chunk(tag, data) {
  const header = Buffer.alloc(8);
  header.write(tag, 0);
  header.writeUInt32BE(data.length, 4);
  return Buffer.concat([header, Buffer.from(data)]);
}

function fixture() {
  const tracks = Array.from({ length: 64 }, () => []);
  tracks[0] = [0, 0xff, 0x51, 3, 7, 0xa1, 0x20, 0x83, 0x60, 0xff, 0x51, 3, 6, 0x1a, 0x80];
  for (let track = 1; track <= 4; track++) {
    tracks[track] = [0, 0x90, 24 + track * 6, 100, 0x81, 0x70, 24 + track * 6, 0];
  }
  tracks[63] = [
    0, 0xc8, 17,          // a two-byte program change
    0, 0x98, 60, 86,
    0, 64, 80,           // running status, second pitch on the same track
    0x83, 0x60, 0xb8, 11, 71,
    0x78, 0xe8, 0, 80,
    0x78, 0x88, 60, 0,
    0, 64, 0,
  ];
  return Buffer.concat([
    chunk('MThd', [0, 1, 0, tracks.length, 1, 0xe0]),
    ...tracks.map(data => chunk('MTrk', [...data, 0, 0xff, 0x2f, 0])),
  ]);
}

(async () => {
  const song = MidiSongs.read(fixture(), 'Song Test.mid');
  assert.equal(song.tracks.length, 64);
  assert.equal(song.tracks[63].poly, 2);
  assert.equal(song.timeAt(480), 500000);
  assert.equal(song.timeAt(600), 600000);
  assert.equal(song.duration, 700000);

  const routes = Object.fromEntries([0, 1, 2, 3].map(pair => [pair + 1, { cv: pair }]));
  assert.throws(() => MidiSongs.image(song, { 63: { cv: 0 } }), /монофоническую/);
  assert.throws(() => MidiSongs.image({ ...song, tracks: [...song.tracks, {}] }), /64 дорожки/);
  const image = MidiSongs.image(song, routes);
  const uf2 = MidiSongs.uf2(image);
  const header = new DataView(uf2.buffer);
  assert.equal(header.getUint32(12, true), 0x100e0000);
  assert.ok(header.getUint32(12, true) + uf2.length / 2 < 0x101e0000);

  const device = new Device(await Core());
  const bad = image.slice();
  bad[bad.length - 1] ^= 1;
  assert.equal(device.openMidiSong(bad), false);
  assert.ok(device.openMidiSong(image));
  const t = device.now();
  device.play(true);
  device.play(false, t + 80000);
  device.runUntil(t + 125000);
  const lit = device.leds();
  assert.ok(lit[63 * 3 + 1] >= 24, 'an active track lights pad 64');
  assert.deepEqual([...lit.subarray(0, 3)], [0, 0, 0], 'metadata track has no pad light');
  device.runUntil(t + 1e6);
  const all = device.drain();
  const up = all.find(e => e.type === 'gate' && e.mask === 15);
  assert.ok(up, 'Gate rises must be committed as a group');
  const down = all.find(e => e.type === 'gate' && e.mask === 0 && e.t > up.t);
  assert.equal(down.t - up.t, 250000);
  assert.deepEqual(all.filter(e => e.type === 'dac' && e.t === up.t).map(e => device.dacVolts(e.code)), [0.5, 1, 1.5, 2]);
  const midi = all.filter(e => e.type === 'midi' && [0xc8, 0x98, 0x88, 0xb8, 0xe8].includes(e.status) &&
    !(e.status === 0xb8 && e.d1 >= 120));
  assert.deepEqual(midi.map(e => [e.status, e.d1, e.d2]),
    song.tracks[63].events.map(e => [e.status, e.d1, e.d2]));
  const expression = midi.find(e => e.status === 0xb8 && e.d1 === 11);
  assert.ok(Math.abs(expression.t - up.t - 500000 - 960) < 1000);
  assert.equal(device.inspect().state, 0);
  assert.equal(device.inspect().midiOverflow, 0);
  assert.equal(device.gates(), 0);
  const idle = device.leds();
  assert.ok(idle[63 * 3 + 1] > 0 && idle[63 * 3 + 1] < 24, 'a loaded track stays visible after playback');
  assert.ok(device.openMidiSong(image));
  const restart = device.now();
  for (let edge = 0; edge < 28; edge++) device.encoderEdge(0, 1, restart + 1000 + edge * 3000);
  device.play(true, restart + 120000);
  device.play(false, restart + 200000);
  device.mute(7, true, restart + 300000);
  device.mute(7, false, restart + 380000);
  device.runUntil(restart + 1e6);
  const last = device.drain().filter(e => e.type === 'midi' && (e.status & 15) === 8);
  assert.equal(last.filter(e => e.status === 0x98 && e.d2 > 0).length, 2);
  assert.equal(last.filter(e => e.status === 0x88 && e.t < restart + 400000).length, 2, 'MUTE releases track 64 on page 8');
  assert.ok(!last.some(e => e.status === 0xe8), 'MUTE suppresses later messages on track 64');
  console.log('PASS MIDI Song (64 tracks and page-8 mute, tempo map, pitch bend, CC, four CV/Gates, CRC, RP2040 UF2)');
})().catch(error => { console.error(error); process.exitCode = 1; });
