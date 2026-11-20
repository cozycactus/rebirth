'use strict';

// The Twisted MIDI songs laid out as the device's clips (sim/web/midi_clips.js):
// the save image is the one the firmware writes, LOAD takes every song, the
// artist's moves load and start one, and the device then plays every note
// start of the laid-out song at its step on its channel.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const Core = require('../web/core.js');
const { Device } = require('../web/device.js');
const Artist = require('../web/artist.js');
const MidiSongs = require('../web/midi_songs.js');
const MidiClips = require('../web/midi_clips.js');

const LIBRARY = path.join(__dirname, '../web/songs/twisted');
const songs = fs.readdirSync(path.join(LIBRARY, 'Midi Files')).filter(f => f.endsWith('.mid')).sort()
  .map(f => MidiSongs.read(new Uint8Array(fs.readFileSync(path.join(LIBRARY, 'Midi Files', f))), f));

function crc32(crc, bytes) {
  for (const byte of bytes) {
    crc ^= byte;
    for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return crc >>> 0;
}
const store = core => core.HEAPU8.slice(core._sim_flash_store(), core._sim_flash_store() + core._sim_flash_store_size());

(async () => {
  const core = await Core();
  const device = new Device(core);
  const advance = us => device.runUntil(device.now() + us);
  const tap = (key, held = 80000) => { key(true); advance(held); key(false); advance(90000); };
  const selector = () => tap(down => device.encoderSwitch(1, down), 700000);
  const L = MidiClips.layout;

  // 1. The layout: a save the firmware writes itself reads back with these
  //    offsets, and its checksum is the one computed over them.
  advance(250000);
  selector();
  tap(down => device.step(8, down));          // SAVE
  const saved = store(core), view = new DataView(saved.buffer);
  assert.equal(view.getUint32(0, true), 0x52425448, 'the firmware saved');
  assert.equal(saved[4], 7, 'version 7');
  const PAT_BYTES = L.PATTERN_BYTES * 32;
  let crc = crc32(0xffffffff, saved.subarray(L.PAT_OFFSET, L.PAT_OFFSET + PAT_BYTES));
  crc = crc32(crc, saved.subarray(L.SONG_OFFSET, L.SONG_OFFSET + L.SONG_BYTES));
  crc = ~crc32(crc, saved.subarray(L.KIT_OFFSET, L.KIT_OFFSET + 15 * 64)) >>> 0;
  assert.equal(crc, view.getUint32(8, true), 'pattern, song and kit records where midi_clips.js puts them');
  for (let t = 0; t < 64; t++) {
    const at = L.PAT_OFFSET + 3072 + t * 16;
    assert.equal(saved[at + 4], device.trackConfig(t).channel, 'track ' + (t + 1) + ' channel at its offset');
    assert.equal(saved[at + 13], device.trackConfig(t).gate, 'track ' + (t + 1) + ' gate at its offset');
  }
  assert.equal(saved[L.PAT_OFFSET + 4096], 16, 'pattern length at its offset');

  // 2. LOAD takes every song's image; its clips and routing are what the plan says.
  const plans = songs.map(song => MidiClips.plan(song));
  plans.forEach((plan, i) => {
    assert.ok(plan.tracks.length <= 64, plan.title + ': 64 tracks at most');
    device.boot(false);
    advance(250000);
    device.writeStore(MidiClips.store(plan));
    selector();
    tap(down => device.step(9, down));        // LOAD
    plan.tracks.forEach((track, t) => {
      const config = device.trackConfig(t);
      assert.equal(config.channel, track.channel, plan.title + ' track ' + (t + 1) + ' channel');
      assert.equal(config.type, track.type, plan.title + ' track ' + (t + 1) + ' type');
      track.clips.forEach((clip, location) => clip.forEach((cell, step) => {
        const got = device.stepData(location, t, step);
        assert.equal(got.active, !!cell, plan.title + ' ' + track.name + ' clip ' + (location + 1) + ' step ' + (step + 1));
        if (cell) assert.equal(got.note, cell.note);
      }));
    });
    assert.equal(device.inspect().fatal, null);
    console.log('LOAD ' + songs[i].title + ': ' + plan.tracks.length + ' tracks, ' + Math.min(256, plan.entries.length) +
      ' chain rows' + (plan.lost.entries ? ' (' + plan.entries.length + ' needed: plays ' + plan.played + ' of ' + plan.bars + ' bars)' : ''));
  });

  // 3. The artist loads and starts two songs, one that fits the chain and one
  //    that does not; every note start of the bars it plays sounds on time.
  for (const index of [0, 4]) {
    const plan = plans[index];
    device.boot(false);
    device.writeStore(MidiClips.store(plan));
    const run = MidiClips.show(plan);
    Artist.schedule(run, device, null, null);
    device.runUntil(run.playAt - 1000);
    device.drain();
    const stepUs = Math.floor(60000000 / (plan.bpm * 96)) * 24;
    const notes = [];
    let start = null;
    const until = run.playAt + (plan.played + 2) * 16 * stepUs;
    while (device.now() < until) {
      device.runUntil(device.now() + 500000);
      for (const e of device.drain()) {
        if (e.type !== 'midi') continue;
        if (e.status === 0xFA) start = e.t;
        if ((e.status & 0xf0) === 0x90 && e.d2) notes.push(e);
      }
    }
    assert.notEqual(start, null, plan.title + ': PLAY started the chain');
    assert.equal(Math.round(device.inspect().bpm), Math.round(plan.bpm), plan.title + ': the tempo the artist set');
    assert.equal(device.inspect().state, 0, plan.title + ': the chain stops at its end');
    const want = new Map();
    for (const track of plan.tracks) track.cells.forEach((cell, g) => {
      if (!cell || g >= plan.played * 16) return;
      const prior = track.cells[g - 1];
      if (prior && prior.note === cell.note && (prior.flags & 8)) return;     // a tie, not a start
      const key = g + '/' + track.channel + '/' + cell.note;
      want.set(key, (want.get(key) || 0) + 1);
    });
    const got = new Map();
    for (const e of notes) {
      // A message is stamped at its last byte: up to a few ms after its step.
      const g = Math.floor((e.t - start) / stepUs + 0.4);
      const key = g + '/' + (e.status & 15) + '/' + e.d1;
      got.set(key, (got.get(key) || 0) + 1);
    }
    let missing = 0, extra = 0;
    for (const [key, n] of want) missing += Math.max(0, n - (got.get(key) || 0));
    for (const [key, n] of got) extra += Math.max(0, n - (want.get(key) || 0));
    assert.equal(missing, 0, plan.title + ': every note start sounds');
    assert.equal(extra, 0, plan.title + ': nothing else sounds');
    const info = device.inspect();
    assert.equal(info.overruns, 0);
    assert.equal(info.midiOverflow, 0);
    assert.equal(info.fatal, null);
    console.log('PLAY ' + plan.title + ': ' + notes.length + ' note starts at their steps over ' + plan.played + ' bars');
  }

  // 4. After the chain the clips are the pads': back in Pattern mode (step 3
  //    in the song editor), a pad picks a part's clip and PLAY plays it on
  //    that part's channel.
  {
    const plan = plans[4];
    const long = enc => tap(down => device.encoderSwitch(enc, down), 700000);
    long(1);                                   // VALUE long: SELECT PATTERN
    long(0);                                   // TRACK long: the song editor
    tap(down => device.step(2, down));         // step 3: Pattern mode
    tap(down => device.encoderSwitch(1, down));   // back to SELECT PATTERN
    long(1);                                   // and to the Session view
    // a drum on the first page whose sound no other track plays (the first
    // page of a Twisted song is its drums)
    const t = plan.tracks.findIndex((tr, i) => i < 8 && tr.type === 1 && tr.clips[0]?.some(Boolean) &&
      plan.tracks.every((o, j) => j === i || o.drum !== tr.drum));
    assert.ok(t >= 0, plan.title + ': a part to play from its pad');
    const track = plan.tracks[t];
    for (let other = 0; other < 8; other++) {
      // the others on their empty clip, so only the part pressed sounds
      if (other !== t) tap(down => device.pad(other, plan.tracks[other].silent, down));
    }
    tap(down => device.pad(t, 0, down));
    assert.equal(device.current(t), 0, track.name + ': the pad picked its first clip');
    device.drain();
    tap(down => device.play(down));
    advance(2 * 16 * Math.floor(60000000 / (plan.bpm * 96)) * 24);
    const hits = track.clips[0].filter(c => c && !(c.flags & 8)).length;
    const heard = device.drain().filter(e => e.type === 'midi' && (e.status & 0xf0) === 0x90 && e.d2 &&
      (e.status & 15) === track.channel && e.d1 === track.drum);
    assert.ok(heard.length >= hits, track.name + ': its clip sounds from its pad (' + heard.length + ' of ' + hits + ' a bar)');
    tap(down => device.play(down));
    console.log('PADS ' + plan.title + ': ' + track.name + ' played from its pad, ' + heard.length + ' notes on channel ' + (track.channel + 1));
  }

  // 5. parts.json names songs and parts the library has.
  const sounds = JSON.parse(fs.readFileSync(path.join(LIBRARY, 'parts.json'), 'utf8'));
  for (const [title, parts] of Object.entries(sounds)) {
    const song = songs.find(s => s.title === title);
    assert.ok(song, 'parts.json: a song "' + title + '"');
    for (const name of Object.keys(parts)) assert.ok(song.tracks.some(t => t.name === name), 'parts.json: ' + title + ' has "' + name + '"');
  }
  console.log('PASS MIDI songs as clips: the save layout, LOAD of all ' + songs.length + ' songs, two played note for note, parts.json');
})().catch(error => { console.error(error); process.exitCode = 1; });
