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
  // The chain length and save layout this core has (384 rows, version 8).
  assert.equal(device.songEntries(), 384, 'the core reports its chain length');
  // Device's fallback only (no older wasm core is run here): a core without
  // the export is taken for 256 rows.
  assert.equal(Device.prototype.songEntries.call({ core: {} }), 256, 'Device reads a missing export as 256 rows');
  assert.throws(() => MidiClips.layoutFor(512), 'no other chain length');
  assert.throws(() => MidiClips.layoutFor('256'), 'a number, not a string');
  assert.throws(() => MidiClips.layoutFor('toString'), 'no inherited key');
  assert.equal(MidiClips.plan(songs[0]).rows, 384, 'plan() lays out for 384 rows unless told');
  const L = MidiClips.layoutFor(device.songEntries());

  // 1. The layout: a save the firmware writes itself reads back with these
  //    offsets, and its checksum is the one computed over them.
  advance(250000);
  selector();
  tap(down => device.step(8, down));          // SAVE
  const saved = store(core), view = new DataView(saved.buffer);
  assert.equal(view.getUint32(0, true), 0x52425448, 'the firmware saved');
  assert.equal(saved[4], L.VERSION, 'version ' + L.VERSION);
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

  // The plans: the tempo kept longest, the bars to the end of the file, and
  // an image whose patterns and song record sit where the firmware's do.
  const plans = songs.map(song => MidiClips.plan(song, { rows: L.ROWS }));
  plans.forEach(plan => {
    assert.equal(plan.lost.entries, 0, plan.title + ': the whole chain fits ' + L.ROWS + ' rows');
    assert.equal(plan.played, plan.bars, plan.title + ': every bar plays');
  });
  assert.equal(plans[5].bpm, 147, 'Snarled Up: the tempo it keeps longest, not its first');
  songs.forEach((song, i) => assert.equal(Math.round(song.bpm), Math.round(plans[i].bpm),
    song.title + ': the song list shows the tempo the set plays'));
  assert.equal(plans[0].bars, 235, 'Contains LSD: 235 bars, its file ending on a bar line');
  {
    const image = MidiClips.store(plans[0]), iv = new DataView(image.buffer);
    for (let loc = 0; loc < 32; loc++) {
      const at = L.PAT_OFFSET + loc * L.PATTERN_BYTES;
      assert.equal(image[at + 4161], saved[L.PAT_OFFSET + 4161], 'swing as the firmware saves it (50, straight)');
      assert.equal(image[at + 4096], 16, 'pattern length');
    }
    assert.equal(iv.getUint16(L.SONG_OFFSET + L.TRAILER, true), plans[0].entries.length, 'song length');
    assert.equal(iv.getUint32(L.SONG_OFFSET + L.TRAILER + 4, true), 1, 'song ends with Stop');
    assert.equal(image[L.SONG_OFFSET + 64], plans[0].entries[0].repeats, 'repeats at byte 64 of a row');
  }

  // 2. LOAD takes every song's image; its clips and routing are what the plan says.
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
      assert.equal(config.gate, track.gate, plan.title + ' track ' + (t + 1) + ' gate');
      track.clips.forEach((clip, location) => clip.forEach((cell, step) => {
        const got = device.stepData(location, t, step);
        assert.equal(got.active, !!cell, plan.title + ' ' + track.name + ' clip ' + (location + 1) + ' step ' + (step + 1));
        if (!cell) return;
        assert.equal(got.note, cell.note);
        assert.equal(got.velocity, cell.vel);
        assert.equal(got.tie, !!(cell.flags & 8));
      }));
      for (let step = 0; step < 16; step++) {
        assert.ok(!device.stepData(track.silent, t, step).active, plan.title + ' ' + track.name + ': its silent clip is empty');
      }
    });
    assert.equal(device.inspect().fatal, null);
    assert.equal(device.inspect().songLength, plan.entries.length, plan.title + ': LOAD gave the whole chain');
    if (i === 4) {
      // SAVE after the load writes the chain back as the image had it.
      const image = MidiClips.store(plan);
      tap(down => device.step(8, down));      // SAVE
      const back = store(core);
      assert.equal(back[4], L.VERSION, plan.title + ': saved again as version ' + L.VERSION);
      assert.deepEqual(back.subarray(L.SONG_OFFSET, L.SONG_OFFSET + L.SONG_BYTES),
        image.subarray(L.SONG_OFFSET, L.SONG_OFFSET + L.SONG_BYTES), plan.title + ': SAVE writes the loaded chain');
    }
    console.log('LOAD ' + songs[i].title + ': ' + plan.tracks.length + ' tracks, ' + Math.min(plan.rows, plan.entries.length) +
      ' chain rows' + (plan.lost.entries ? ' (' + plan.entries.length + ' needed: plays ' + plan.played + ' of ' + plan.bars + ' bars)' : ''));
  });

  // 3. The artist loads and starts songs; every note start of the bars it
  //    plays sounds on time, as the plan has it and as the MIDI file has it:
  //    Contains LSD, then Sham whole, then Sham as a 256-row core lays it out,
  //    cut short, as a version 7 image that this core loads through its
  //    fallback.
  const sham256 = MidiClips.plan(songs[4], { rows: 256 });
  assert.ok(sham256.lost.entries > 0 && sham256.played === 262, sham256.title + ': 256 rows play 262 bars');
  assert.equal(MidiClips.store(sham256)[4], 7, 'a 256-row plan is a version 7 image');
  assert.equal(plans[4].played, 361, plans[4].title + ': 384 rows play all 361 bars');
  for (const [index, plan] of [[0, plans[0]], [4, plans[4]], [4, sham256]]) {
    device.boot(false);
    device.writeStore(MidiClips.store(plan));
    const run = MidiClips.show(plan);
    Artist.schedule(run, device, null, null);
    device.runUntil(run.playAt - 1000);
    device.drain();
    const stepUs = Math.floor(60000000 / (plan.bpm * 96)) * 24;
    const notes = [];
    let start = null, stop = null, lastRow = 0;
    const until = run.playAt + (plan.played + 2) * 16 * stepUs;
    while (device.now() < until) {
      device.runUntil(device.now() + 500000);
      if (device.inspect().state !== 0) lastRow = Math.max(lastRow, device.inspect().songPosition);
      for (const e of device.drain()) {
        if (e.type !== 'midi') continue;
        if (e.status === 0xFA) start = e.t;
        if (e.status === 0xFC && stop === null) stop = e.t;
        if ((e.status & 0xf0) === 0x90 && e.d2) notes.push(e);
      }
    }
    assert.notEqual(start, null, plan.title + ': PLAY started the chain');
    assert.ok(Math.abs(device.inspect().bpm - plan.bpm) < 0.06, plan.title + ': the tempo the artist set (' + device.inspect().bpm.toFixed(2) + ')');
    assert.equal(device.inspect().state, 0, plan.title + ': the chain stops at its end');
    assert.equal(lastRow, Math.min(plan.rows, plan.entries.length) - 1, plan.title + ': it played up to its last row (' + lastRow + ')');
    assert.ok(stop !== null && Math.abs(stop - start - plan.played * 16 * stepUs) < stepUs,
      plan.title + ': it stops after ' + plan.played + ' bars');
    const want = new Map();
    for (const track of plan.tracks) track.cells.forEach((cell, g) => {
      if (!cell || g >= plan.played * 16) return;
      const prior = track.cells[g - 1];
      if (prior && prior.note === cell.note && (prior.flags & 8)) return;     // a tie, not a start
      const key = g + '/' + track.channel + '/' + cell.note;
      want.set(key, (want.get(key) || 0) + 1);
    });
    const got = new Map(), heard = new Set();
    let latest = 0;
    for (const e of notes) {
      // A message is stamped at its last byte, a few ms after its step.
      const g = Math.floor((e.t - start) / stepUs + 0.4);
      latest = Math.max(latest, e.t - start - g * stepUs);
      const key = g + '/' + (e.status & 15) + '/' + e.d1;
      got.set(key, (got.get(key) || 0) + 1);
      heard.add(g + '/' + e.d1);
    }
    assert.ok(latest < 40000, plan.title + ': every note within 40 ms of its step (' + Math.round(latest / 1000) + ' ms)');
    let missing = 0, extra = 0;
    for (const [key, n] of want) missing += Math.max(0, n - (got.get(key) || 0));
    for (const [key, n] of got) extra += Math.max(0, n - (want.get(key) || 0));
    assert.equal(missing, 0, plan.title + ': every note start of the plan sounds');
    assert.equal(extra, 0, plan.title + ': nothing else sounds');
    // The MIDI file itself, read apart from the plan: each part's notes at
    // each step (the drums as the GM notes the full arrangement plays). Some
    // note of the part sounds at every such step (two notes of one voice on
    // one step keep one), and nothing sounds that the file does not start
    // there: no lost step, no note struck again in the middle of a tie.
    const song = songs[index], unit = song.ppqn / 4, onsets = new Set();
    let steps = 0, silent = [];
    for (const raw of song.tracks) {
      const part = MidiSongs.tag(raw), byStep = new Map();
      for (const e of part.events) {
        if (!MidiSongs.noteOn(e)) continue;
        const g = Math.round(e.tick / unit);
        if (g >= plan.played * 16) continue;
        const note = !part.drum ? e.d1 : part.drum === 42 && [42, 44, 46].includes(e.d1) ? e.d1 : part.drum;
        if (!byStep.has(g)) byStep.set(g, new Set());
        byStep.get(g).add(note);
        onsets.add(g + '/' + note);
      }
      for (const [g, set] of byStep) {
        steps++;
        if (![...set].some(note => heard.has(g + '/' + note))) silent.push(part.name + ' step ' + g);
      }
    }
    assert.deepEqual(silent, [], plan.title + ': a note of the file at each of its steps');
    const stray = [...heard].filter(key => !onsets.has(key));
    assert.deepEqual(stray, [], plan.title + ': no note the file does not start at that step');
    const info = device.inspect();
    assert.equal(info.overruns, 0);
    assert.equal(info.midiOverflow, 0);
    assert.equal(info.fatal, null);
    console.log('PLAY ' + plan.title + ': ' + notes.length + ' note starts at their steps over ' + plan.played +
      ' bars; all ' + steps + ' part-steps of the file heard, nothing else');
  }

  // 4. After the chain the clips are the pads': back in Pattern mode (step 3
  //    in the song editor), a pad picks a part's clip and PLAY plays it on
  //    that part's channel.
  {
    const plan = sham256;                      // the plan section 3 loaded last
    const long = enc => tap(down => device.encoderSwitch(enc, down), 700000);
    long(1);                                   // VALUE long: SELECT PATTERN
    long(0);                                   // TRACK long: the song editor
    tap(down => device.step(2, down));         // step 3: Pattern mode
    tap(down => device.encoderSwitch(1, down));   // back to SELECT PATTERN
    long(1);                                   // and to the Session view
    // a drum on the first page whose sound no other track plays (the first
    // page of a Twisted song is its drums), and a clip of it in bank A that
    // is not the one it plays now
    const t = plan.tracks.findIndex((tr, i) => i < 8 && tr.type === 1 && tr.clips.length > 1 &&
      plan.tracks.every((o, j) => j === i || o.drum !== tr.drum));
    assert.ok(t >= 0, plan.title + ': a part to play from its pad');
    const track = plan.tracks[t];
    const before = device.current(t);
    const row = [...Array(Math.min(8, track.clips.length)).keys()].find(r => r !== before);
    tap(down => device.pad(t, row, down));
    assert.equal(device.current(t), row, track.name + ': the pad picked clip ' + (row + 1) + ' (was ' + (before + 1) + ')');
    device.drain();
    tap(down => device.play(down));
    const unitUs = Math.floor(60000000 / (plan.bpm * 96)) * 24;
    advance(20 * unitUs);
    const out = device.drain().filter(e => e.type === 'midi');
    const go = out.find(e => e.status === 0xFA);
    assert.ok(go, 'PLAY started the pattern');
    const clip = track.clips[row];
    const hits = clip.filter((c, i) => c && !(i > 0 && clip[i - 1] && clip[i - 1].note === c.note && (clip[i - 1].flags & 8))).length;
    const heard = out.filter(e => (e.status & 0xf0) === 0x90 && e.d2 && (e.status & 15) === track.channel &&
      e.d1 === track.drum && e.t < go.t + 16 * unitUs);
    assert.equal(heard.length, hits, track.name + ': its clip ' + (row + 1) + ' sounds from its pad, one bar of it');
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
  console.log('PASS MIDI songs as clips: the save layout, LOAD of all ' + songs.length + ' songs, SAVE after a load, ' +
    'two songs played note for note against the plan and the file (one whole at 384 rows and cut at 256 from a ' +
    'version 7 image), a clip launched from its pad, parts.json');
})().catch(error => { console.error(error); process.exitCode = 1; });
