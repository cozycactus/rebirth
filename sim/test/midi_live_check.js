'use strict';

// The Twisted songs laid out as clips and performed live with MUTE
// ("Клипы на падах · артист играет MUTE", sim/web/midi_clips.js mutes() and
// show(plan, start, opts)), as docs/superpowers/specs/2026-11-25-sham-live-mutes-design.md
// has it:
//   1. the time signatures MidiSongs.read reports;
//   2. the mute script: Sham's moves as the spec's table, the rules of a
//      move on all eight songs, and mutes() leaving the plan as it was;
//   3. the plain clips mode unchanged: show(plan, start) and store(plan);
//   4. on the wasm core (skipped without quantised MUTE), every song from
//      boot: the actions of the performance, every change on its downbeat,
//      the mutes in every bar, every note start of the plan less the muted
//      cells and nothing else, the chain stopping on its bar: each song
//      whole in the core's 384 rows, and Sham as a 256-row core lays it out,
//      cut on its last phrase line (a version 7 image, the fallback loads it);
//   5. app.js's wiring of the mode (read from its source, as hands_unit.js
//      reads its panel: when it fails, it names the edit to make).

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const Core = require('../web/core.js');
const { Device } = require('../web/device.js');
const Artist = require('../web/artist.js');
const MidiSongs = require('../web/midi_songs.js');
const MidiClips = require('../web/midi_clips.js');

const LIBRARY = path.join(__dirname, '../web/songs/twisted/Midi Files');
const songs = fs.readdirSync(LIBRARY).filter(f => f.endsWith('.mid')).sort()
  .map(f => MidiSongs.read(new Uint8Array(fs.readFileSync(path.join(LIBRARY, f))), f));
const byNumber = n => songs.find(s => s.title.startsWith(n));

// Sham's script, 0-based chain bars: muted from `start`, back on `end`. In
// the 384-row chain the whole song (361 bars) plays and the set runs to its
// end; in 256 rows the chain stops after bar 262 and the set on bar 257.
const SHAM_256 = [
  ['kick', 40, 41, [1]], ['kick', 56, 57, [1]], ['hats', 77, 81, [5, 6]], ['drop', 87, 89, [1, 10]],
  ['kick', 96, 97, [1]], ['snare', 135, 137, [2]], ['snare', 151, 153, [2]], ['kick', 208, 209, [1]],
  ['hats', 237, 241, [5]], ['drop', 247, 249, [1, 10]],
];
const SHAM = [...SHAM_256,
  ['kick', 264, 265, [1]], ['hats', 277, 281, [5, 6]], ['hats', 317, 321, [5, 6]], ['hats', 333, 337, [5, 6]],
];

// The groups as the spec defines them, built here apart from mutes():
// every track of the role or drum note, the gate left out.
function specGroups(plan) {
  const all = test => plan.tracks.map((t, i) => i).filter(i => plan.tracks[i].role !== 'gate' && test(plan.tracks[i]));
  return {
    kick: all(t => t.role === 'kick'),
    hats: all(t => [42, 44, 46].includes(t.drum)),
    snare: all(t => [38, 40].includes(t.drum) && !/fill/i.test(t.name)),
    bass: all(t => t.role === 'bass'),
  };
}

// A one-track SMF: the given meta events at tick 0, then one note.
function smf(metas) {
  const body = [];
  for (const m of metas) body.push(0, 0xff, ...m);
  body.push(0, 0x90, 60, 100, 0x83, 0x60, 0x80, 60, 0, 0, 0xff, 0x2f, 0);
  const len = body.length;
  return new Uint8Array([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 1, 0xe0,
    0x4d, 0x54, 0x72, 0x6b, len >>> 24, (len >>> 16) & 255, (len >>> 8) & 255, len & 255, ...body]);
}

(async () => {
  const core = await Core();
  const device = new Device(core);
  const ROWS = device.songEntries();
  assert.equal(ROWS, 384, 'the core holds 384 chain rows');

  // 1. Time signatures: seven songs in 4/4 throughout, Are You Sponged with
  //    two 6/4 bars (chain bars of 4 beats: at 119 and 126.5); one that
  //    makes no bar (numerator 0, denominator 2^200) is left out.
  for (const song of songs) {
    assert.ok(song.meters.length >= 1 && song.meters[0].tick === 0, song.title + ': a time signature at the start');
    const odd = song.meters.filter(m => m.num !== 4 || m.den !== 4);
    if (song.title.startsWith('07')) {
      const bar = song.ppqn * 4;
      assert.deepEqual(odd.map(m => [m.num, m.den, m.tick / bar]), [[6, 4, 119], [6, 4, 126.5]], 'Are You Sponged: its 6/4 bars');
    } else assert.deepEqual(odd, [], song.title + ': 4/4 throughout');
  }
  const bad = MidiSongs.read(smf([[0x58, 4, 0, 2, 24, 8], [0x58, 4, 3, 200, 24, 8], [0x58, 4, 4, 2, 24, 8]]), 'meters.mid');
  assert.deepEqual(bad.meters, [{ tick: 0, num: 4, den: 4 }], 'a time signature that makes no bar is left out');

  // 2. The mute script. The plain images are taken before mutes() runs.
  const plans = songs.map(song => MidiClips.plan(song, { rows: ROWS }));
  const images = plans.map(plan => MidiClips.store(plan));
  const lives = plans.map(plan => {
    const before = JSON.stringify(plan);
    const live = MidiClips.mutes(plan);
    assert.equal(JSON.stringify(plan), before, plan.title + ': mutes() leaves the plan as it was');
    return live;
  });
  const sham = songs.indexOf(byNumber('05'));
  plans.forEach((plan, i) => {
    assert.equal(plan.played, plan.bars, plan.title + ': the whole song is in the chain');
    assert.equal(lives[i].setEnd, plan.played, plan.title + ': the set runs to the end of the song');
  });
  assert.deepEqual(lives[sham].moves.map(m => [m.kind, m.start, m.end, m.tracks]), SHAM, 'Sham: the spec\'s table at 384 rows');
  // The cut path: Sham in 256 rows.
  const sham256 = MidiClips.plan(songs[sham], { rows: 256 }), image256 = MidiClips.store(sham256);
  const live256 = MidiClips.mutes(sham256);
  assert.equal(sham256.played, 262, 'Sham in 256 rows: the chain plays 262 bars');
  assert.equal(live256.setEnd, 257, 'Sham in 256 rows: the set ends on chain bar 257, the last phrase line in the chain');
  assert.deepEqual(live256.moves.map(m => [m.kind, m.start, m.end, m.tracks]), SHAM_256, 'Sham in 256 rows: the spec\'s table');
  assert.deepEqual(specGroups(plans[sham]), { kick: [1], hats: [5, 6], snare: [2], bass: [10] }, 'Sham: the spec\'s groups');
  const checkScript = (plan, live) => {
    const on = (t, b) => b >= 0 && b < plan.played && plan.tracks[t].cells.slice(b * 16, b * 16 + 16).some(c => c && (c.flags & 1));
    const change = b => plan.tracks.reduce((n, t, k) => n + (t.role !== 'gate' && on(k, b) !== on(k, b - 1) ? 1 : 0), 0);
    const groups = specGroups(plan);
    let prior = -Infinity;
    for (const m of live.moves) {
      const what = plan.title + ' ' + m.kind + ' back on ' + (m.end + 1);
      assert.equal(m.press, m.start - 1, what + ': pressed in the bar before');
      assert.ok(m.end - prior >= (m.kind === 'hats' || m.kind === 'snare' ? 16 : 8), what + ': spacing');
      assert.ok(m.end <= live.setEnd - 8, what + ': 8 bars before the end at least');
      assert.ok(live.stretches.some(([a, e]) => m.press >= a && m.end < e), what + ': inside one 4/4 stretch');
      for (const t of m.tracks) {
        assert.ok(t >= 0 && t < 16 && plan.tracks[t].role !== 'gate', what + ': track ' + t + ' on pages 1-2, not the gate');
        for (let b = m.press; b <= m.end; b++) assert.ok(on(t, b), what + ': track ' + t + ' plays in bar ' + (b + 1));
        for (const b of [m.start, m.end]) {
          const c = plan.tracks[t].cells[b * 16], p = plan.tracks[t].cells[b * 16 - 1];
          assert.ok(!(c && p && p.note === c.note && (p.flags & 8)), what + ': no tie into bar ' + (b + 1));
        }
      }
      // Never half a group: every track of it that plays in the span is muted.
      for (const g of m.kind === 'drop' ? ['kick', 'bass'] : [m.kind]) {
        for (const t of groups[g]) {
          let any = false;
          for (let b = m.press; b <= m.end; b++) any = any || on(t, b);
          assert.equal(m.tracks.includes(t), any, what + ': ' + g + ' track ' + t + (any ? ' plays and is muted' : ' is silent and left alone'));
        }
      }
      for (let b = m.start; b < m.end; b++) assert.ok(change(b) <= 2, what + ': no section change in bar ' + (b + 1));
      prior = m.end;
    }
  };
  plans.forEach((plan, i) => checkScript(plan, lives[i]));
  checkScript(sham256, live256);
  const sponged = songs.indexOf(byNumber('07'));
  assert.deepEqual(lives[sponged].stretches, [[0, 119], [128, 255]], 'Are You Sponged: nothing across its 6/4 bars');

  // Made-up songs from Sham's plan: meters that make no bar (as 4/4), a
  // long song in 5/4 from bar 1 (no moves; in 256 rows the chain is not cut
  // to nothing),
  // a hats continuation on page 2 (muted with the hats, a page turn) and
  // on page 3 (no hats move while it plays).
  {
    const base = plans[sham];
    const broken = MidiClips.mutes({ ...base, meters: [{ tick: 0, num: 0, den: 4 }, { tick: 0, num: 3, den: 2 ** 200 }] });
    assert.deepEqual(broken.moves, lives[sham].moves, 'meters that make no bar count as 4/4');
    for (const from of [base, sham256]) {
      const odd = { ...from, meters: [{ tick: 0, num: 4, den: 4 }, { tick: 1920, num: 5, den: 4 }] };
      const oddLive = MidiClips.mutes(odd);
      const what = '5/4 from bar 1 in ' + from.rows + ' rows: ';
      assert.equal(oddLive.moves.length, 0, what + 'no moves');
      assert.equal(oddLive.setEnd, from.played, what + 'the chain is not cut');
      const oddImage = MidiClips.store(odd, { bars: oddLive.setEnd }), L = MidiClips.layoutFor(from.rows);
      assert.equal(new DataView(oddImage.buffer).getUint16(L.SONG_OFFSET + L.TRAILER, true), Math.min(from.rows, from.entries.length),
        what + 'every row stored');
    }
    for (const at of [8, 20]) {
      const tracks = base.tracks.slice();
      tracks[at] = { ...tracks[5], name: 'Hats CH +2' };
      const plan = { ...base, tracks, title: 'Sham with Hats CH +2 on track ' + at };
      const live = MidiClips.mutes(plan);
      checkScript(plan, live);
      const hats = live.moves.filter(m => m.kind === 'hats');
      if (at === 8) {
        assert.ok(hats.length && hats.every(m => m.tracks.includes(5) && m.tracks.includes(8)), 'a page-2 hats continuation is muted with the hats');
        const run = MidiClips.show(plan, 600000, { moves: live.moves, bars: live.setEnd });
        assert.deepEqual(run.moves, live.moves, 'with the continuation every move fits, page turns and all');
      } else assert.ok(hats.every(m => !m.tracks.includes(5)), 'no hats move while a page-3 continuation plays');
    }
  }
  console.log('SCRIPT ' + plans.map((p, i) => p.title.replace(/^\d+ /, '') + ' ' + lives[i].moves.length).join(', ') +
    '; Sham in 256 rows ' + live256.moves.length);

  // 3. The plain clips mode: today's show and store; the performed show's
  //    loading the same but for the chain it announces, and every move kept.
  const all = [...plans.map((plan, i) => [plan, lives[i], images[i]]), [sham256, live256, image256]];
  for (const [plan, live, image] of all) {
    const plain = MidiClips.show(plan, 600000);
    assert.deepEqual(Object.keys(plain).sort(), ['actions', 'playAt'], plan.title + ': the plain show has no t0 or end');
    const performed = MidiClips.show(plan, 600000, { moves: live.moves, bars: live.setEnd });
    const loading = performed.actions.slice(0, plain.actions.length);
    const chainSay = a => a.op === 'say' && a.text.startsWith('TRACK долго');
    assert.deepEqual(loading.map(a => chainSay(a) ? { ...a, text: '' } : a), plain.actions.map(a => chainSay(a) ? { ...a, text: '' } : a),
      plan.title + ': the loading as in the plain show');
    assert.equal(performed.playAt, plain.playAt);
    assert.deepEqual(performed.moves, live.moves, plan.title + ': every move of the script fits');
    assert.deepEqual(MidiClips.store(plan), image, plan.title + ': store(plan) as before mutes() ran');
    const cut = MidiClips.store(plan, { bars: live.setEnd }), view = new DataView(cut.buffer);
    const L = MidiClips.layoutFor(plan.rows), rows = view.getUint16(L.SONG_OFFSET + L.TRAILER, true);
    assert.equal(cut[4], L.VERSION, plan.title + ': a version ' + L.VERSION + ' image for ' + plan.rows + ' rows');
    let bars = 0;
    for (let r = 0; r < rows; r++) bars += cut[L.SONG_OFFSET + r * L.ROW_BYTES + 64];
    assert.equal(bars, Math.min(live.setEnd, plan.played), plan.title + ': the stored chain holds the set\'s bars');
    assert.ok(loading.some(a => chainSay(a) && a.text.includes('(' + rows + ' строк, ' + bars + ' тактов)')), plan.title + ': the chain announced as stored');
  }

  // 4. On the core.
  const onCore = !!device.features().quantMutes;
  if (!onCore) {
    console.log('SKIP the performance on the core: this firmware\'s MUTE does not wait for the launch point');
  } else {
    for (let i = 0; i < plans.length; i++) performOnCore(device, plans[i], lives[i]);
    performOnCore(device, sham256, live256);
  }

  // 5. app.js's wiring.
  const src = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
  const has = (re, what) => assert.ok(re.test(src), 'app.js: ' + what);
  has(/const handsShown = [\s\S]*?!\(padsRun && !padsRun\.performed\);/, 'handsShown hides the hands only for a clips run that is not performed');
  has(/function planHands\(\)[\s\S]*?\|\| \(padsRun && !padsRun\.performed\)\) return;/, 'planHands plans a performed clips run');
  has(/const buildable = [\s\S]*?\(phase === 'midi-loading' && buildDuringLoad === launchSerial\)/, 'buildable allows the build while a performed song loads');
  has(/liveOption\.disabled = !can\.quantMutes;/, 'the performed mode offered only when MUTE waits for the bar line');
  has(/if \(loadingPlayer\) \{ loadingPlayer\.stop\(ctx\.currentTime\); loadingPlayer = null; \}/, 'endShow stops the voices of a start that waits');
  const start = src.indexOf('async function startMidiSong'), end = src.indexOf('function channelSong', start);
  assert.ok(start > 0 && end > start, 'app.js: startMidiSong, then channelSong');
  const body = src.slice(start, end), at = s => body.indexOf(s);
  const order = ["const performed = $('midi-song-mode').value === 'live'", 'if (performed) buildDuringLoad = request',
    'await rig.midiParts(channelSong(plan))', 'loadingPlayer = player',
    'if (performed && handsWanted() && !handLayer && request === launchSerial)',
    'Promise.race([loadHands(), new Promise(resolve => setTimeout(resolve, 4000))])',
    'if (buildDuringLoad === request) buildDuringLoad = 0', 'if (request !== launchSerial)', 'player.stop(ctx.currentTime)',
    'Artist.schedule(show', 'performed,', 'loadingPlayer = null', "phase = 'prep'", 'if (performed) {', 'generation++', 'planHands()',
    '} catch (error) {', 'if (buildDuringLoad === request) buildDuringLoad = 0;\n      if (request === launchSerial) endShow'];
  let from = 0;
  for (const s of order) {
    const k = body.indexOf(s, from);
    assert.ok(k > 0, 'app.js startMidiSong: no "' + s + '" after the previous step');
    from = k + s.length;
  }
  assert.ok(/t0: run\.t0/.test(body) && /end: run\.end/.test(body), 'app.js startMidiSong: the show\'s t0 and end from MidiClips.show');
  console.log('PASS live MUTE: time signatures, the script on all ' + songs.length + ' songs and Sham\'s table, made-up meters and ' +
    'continuations, the plain mode unchanged, ' + (onCore ? 'the performance on the core' : 'NOT the performance on the core (skipped)') +
    ', the page\'s wiring');
})().catch(error => { console.error(error); process.exitCode = 1; });

// One song performed from boot. The checks read the actions and the device,
// never the show's own bookkeeping beyond the moves it kept.
function performOnCore(device, plan, live) {
  device.boot(false);
  device.runUntil(device.now() + 250000);
  device.writeStore(MidiClips.store(plan, { bars: live.setEnd }));
  const run = MidiClips.show(plan, device.now() + 300000, { moves: live.moves, bars: live.setEnd });
  const stepUs = Math.floor(60000000 / (plan.bpm * 96)) * 24, barUs = stepUs * 16;
  const bars = Math.min(live.setEnd, plan.played);
  const title = plan.title;

  // The actions after PLAY: captions, MUTE columns, whole TRACK detents.
  const after = run.actions.filter(a => a.t > run.playAt + 80000);   // after PLAY's release
  for (const a of after) {
    assert.ok(['say', 'mute', 'edge'].includes(a.op), title + ': only MUTE, TRACK and captions after PLAY, not ' + a.op);
    if (a.op === 'mute') assert.ok(a.track >= 0 && a.track < 8, title + ': a MUTE column');
    if (a.op === 'edge') assert.equal(a.enc, 0, title + ': only TRACK turns');
  }
  const edges = after.filter(a => a.op === 'edge');
  assert.equal(edges.length % 32, 0, title + ': TRACK turned by whole pages');
  assert.equal(edges.reduce((s, a) => s + a.dir, 0), 0, title + ': the set ends on page 1');
  const downs = after.filter(a => a.op === 'mute' && a.down);
  for (const d of downs) {
    const near = downs.filter(x => x.t >= d.t && x.t < d.t + 100000);
    assert.ok(near.length <= 2 && Math.max(...near.map(x => x.track)) - Math.min(...near.map(x => x.track)) <= 2,
      title + ': at most two MUTE keys at once, two columns apart');
  }
  assert.equal(run.end, run.t0 + bars * barUs + 1500000, title + ': the show ends 1.5 s after the chain');

  Artist.schedule(run, device, null, null);
  let startT = null, stop = null, late = 0;
  const got = new Map();
  const pump = until => {
    device.runUntil(until);
    for (const e of device.drain()) {
      if (e.type !== 'midi') continue;
      if (e.status === 0xFA) startT = e.t;
      if (e.status === 0xFC && startT !== null && stop === null) stop = e.t;
      if ((e.status & 0xf0) === 0x90 && e.d2 && startT !== null) {
        const g = Math.floor((e.t - startT) / stepUs + 0.4);
        if (g >= bars * 16) { late++; continue; }
        const key = g + '/' + (e.status & 15) + '/' + e.d1;
        got.set(key, (got.get(key) || 0) + 1);
      }
    }
  };
  pump(run.playAt + 100000);
  assert.notEqual(startT, null, title + ': PLAY started the chain');
  const downbeat = b => startT + b * barUs;

  // The presses as the device takes them: the page from the TRACK turns,
  // each change landing on the next bar line; no key pressed again before
  // its change has landed.
  let focus = 0, edgeCount = 0;
  const landing = new Map(), presses = [];
  for (const a of after) {
    if (a.op === 'edge') { if (++edgeCount % 4 === 0) focus = Math.max(0, Math.min(63, focus + a.dir)); continue; }
    if (a.op !== 'mute' || !a.down) continue;
    const track = Math.floor(focus / 8) * 8 + a.track, bar = Math.ceil((a.t - startT) / barUs);
    assert.ok(!(landing.get(track) > a.t), title + ': track ' + track + ' pressed while its change waits');
    landing.set(track, downbeat(bar));
    assert.ok(a.t <= downbeat(bar) - 60000, title + ': track ' + track + ' pressed 60 ms before its downbeat at least');
    presses.push({ track, bar });
  }
  // The script's mutes, bar by bar, from the kept moves.
  const moves = run.moves, maskAt = new Array(bars + 2).fill(0), muted = new Map();
  for (const m of moves) {
    for (let b = m.start; b < m.end; b++) for (const t of m.tracks) {
      maskAt[b] |= 1 << t;
      if (!muted.has(t)) muted.set(t, new Set());
      muted.get(t).add(b);
    }
  }
  const pressedFor = presses.map(p => p.track + '@' + p.bar).sort();
  const wantPresses = moves.flatMap(m => m.tracks.flatMap(t => [t + '@' + m.start, t + '@' + m.end])).sort();
  assert.deepEqual(pressedFor, wantPresses, title + ': each muted track pressed once for its mute and once for its return');
  const landings = new Set(moves.flatMap(m => [m.start, m.end]));
  for (let b = 0; b < bars; b++) {
    if (landings.has(b)) {
      pump(downbeat(b) - 25000);
      assert.equal(device.inspect().mutes >>> 0, maskAt[b - 1] >>> 0, title + ': 25 ms before bar ' + (b + 1));
      pump(downbeat(b) + 40000);
      assert.equal(device.inspect().mutes >>> 0, maskAt[b] >>> 0, title + ': 40 ms into bar ' + (b + 1));
    }
    pump(downbeat(b) + barUs / 2);
    assert.equal(device.inspect().mutes >>> 0, maskAt[b] >>> 0, title + ': the mutes in bar ' + (b + 1));
  }
  pump(downbeat(bars + 2));

  const want = new Map();
  plan.tracks.forEach((track, t) => track.cells.forEach((cell, g) => {
    if (!cell || g >= bars * 16) return;
    const prior = track.cells[g - 1];
    if (prior && prior.note === cell.note && (prior.flags & 8)) return;
    if (muted.get(t)?.has(Math.floor(g / 16))) return;
    const key = g + '/' + track.channel + '/' + cell.note;
    want.set(key, (want.get(key) || 0) + 1);
  }));
  let missing = 0, extra = 0;
  for (const [key, n] of want) missing += Math.max(0, n - (got.get(key) || 0));
  for (const [key, n] of got) extra += Math.max(0, n - (want.get(key) || 0));
  assert.equal(missing, 0, title + ': every note start of the plan outside the muted cells sounds');
  assert.equal(extra, 0, title + ': nothing else sounds');
  assert.equal(late, 0, title + ': nothing from the stop\'s downbeat on');
  const info = device.inspect();
  assert.equal(info.state, 0, title + ': stopped');
  assert.ok(stop !== null && Math.abs(stop - downbeat(bars)) < stepUs / 2, title + ': the chain stops on bar ' + (bars + 1) + '\'s downbeat');
  assert.equal(info.mutes, 0, title + ': nothing muted at the end');
  assert.equal(info.overruns, 0);
  assert.equal(info.midiOverflow, 0);
  assert.equal(info.fatal, null);
  console.log('LIVE ' + title + ': ' + moves.length + ' moves, ' + presses.length + ' MUTE presses, ' + edges.length / 32 +
    ' page turns, stops after bar ' + bars + (bars < plan.played ? ' (the chain would play ' + plan.played + ')' : '') +
    ' (' + plan.rows + ' rows)');
}
