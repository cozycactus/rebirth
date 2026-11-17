// Runs the scripted artist against the real firmware (the WebAssembly core)
// without a browser and checks that the device ended up where the script
// believes it is: the clips it programmed, the clips playing in each section,
// the mutes, and that the MIDI it produced is well formed.
//   node sim/test/artist_check.js
'use strict';
const path = require('path');
const web = path.join(__dirname, '..', 'web');
const RebirthCore = require(path.join(web, 'core.js'));
const { Device, Hands } = require(path.join(web, 'device.js'));
const Artist = require(path.join(web, 'artist.js'));

let failures = 0;
function check(ok, message) {
  if (!ok) {
    failures++;
    if (failures <= 40) console.log('FAIL ' + message);
  }
}

RebirthCore().then(core => {
  const device = new Device(core);
  const hands = new Hands(device);
  // FW2=1: the firmware under test starts new steps at the last note and copies clips.
  // KEYS=1 (with FW2=1): the firmware reads a MIDI keyboard and the artist enters pitches from one.
  const keys = process.env.KEYS === '1';
  const show = Artist.buildShow(undefined, { fw2: process.env.FW2 === '1' || keys, keys });
  const said = [];
  const io = { say: (kind, text) => said.push({ t: device.now(), kind, text }), rig: () => {} };
  Artist.schedule(show, device, hands, io);
  console.log('show: ' + show.actions.length + ' actions; preparation ' + (show.prepEnd / 1e6).toFixed(1) +
    ' s of device time, set ' + ((show.end - show.playAt) / 1e6).toFixed(1) + ' s');

  const midi = [];
  const collect = () => { for (const e of device.drain()) if (e.type === 'midi') midi.push(e); };
  const run = until => {
    // In slices, so the event buffer never fills.
    while (device.now() < until) {
      hands.advance(Math.min(until, device.now() + 500000));
      collect();
    }
  };

  // ─── After the preparation ────────────────────────────────────
  run(show.prepEnd);
  const info = device.inspect();
  check(info.fatal === null, 'fatal: ' + info.fatal);
  check(Math.abs(info.bpm - show.bpm) < 0.05, 'tempo is ' + info.bpm);
  check(info.state === 0, 'transport should be stopped after the preparation');
  for (const [track, type] of Object.entries(show.model.types)) {
    check(device.trackConfig(+track).type === type, 'track ' + (+track + 1) + ' type ' + device.trackConfig(+track).type);
  }
  for (const [track, gate] of Object.entries(show.model.gate)) {
    check(device.trackConfig(+track).gate === gate, 'track ' + (+track + 1) + ' gate ' + device.trackConfig(+track).gate);
  }
  const verifyPatterns = (label, model) => {
    let checked = 0;
    for (let location = 0; location < 8; location++) {
      for (let track = 0; track < 8; track++) {
        if (track === 1) continue;
        if (location === 0 && track === 0) continue;      // the factory kick
        const length = model.len[location + '/' + track] || 16;
        check(device.trackLength(location, track) === length,
          label + ': length of row ' + (location + 1) + ' track ' + (track + 1) + ' is ' + device.trackLength(location, track));
        for (let step = 0; step < 16; step++) {
          const want = model.steps[location + '/' + track + '/' + step] || { active: false };
          const got = device.stepData(location, track, step);
          const where = label + ': row ' + (location + 1) + ' track ' + (track + 1) + ' step ' + (step + 1);
          check(got.active === !!want.active, where + ' active ' + got.active);
          if (want.active) {
            checked++;
            if (want.note) check(got.note === want.note, where + ' note ' + got.note + ' want ' + want.note);
            check(got.accent === !!want.accent, where + ' accent ' + got.accent);
            check(got.slide === !!want.slide, where + ' slide ' + got.slide);
          }
        }
      }
      if (location === 0) continue;                       // the factory kit pattern
      for (let step = 0; step < 16; step++) {
        const want = model.kit[location + '/' + step] || new Array(12).fill(0);
        const word = device.kitStep(location, 1, step);
        for (let lane = 0; lane < 12; lane++) {
          const got = (word >>> (2 * lane)) & 3;
          check(got === want[lane], label + ': kit row ' + (location + 1) + ' step ' + (step + 1) + ' lane ' +
            Artist.LANE_NAME[lane] + ' is ' + got + ' want ' + want[lane]);
          if (want[lane]) checked++;
        }
      }
    }
    return checked;
  };
  console.log('after preparation: ' + verifyPatterns('prep', show.prepModel) + ' programmed steps match; tempo ' + info.bpm.toFixed(1));
  if (keys) {
    // The keys are passed through to the synths while the artist programs:
    // every one of them must have been released again.
    const down = new Map();
    for (const m of midi) {
      const key = (m.status & 15) + ':' + m.d1;
      if ((m.status & 0xF0) === 0x90 && m.d2 > 0) down.set(key, (down.get(key) || 0) + 1);
      else if ((m.status & 0xF0) === 0x80 || (m.status & 0xF0) === 0x90) down.set(key, (down.get(key) || 0) - 1);
    }
    const stuck = [...down].filter(([, n]) => n !== 0);
    check(stuck.length === 0, 'keys left sounding after the preparation: ' + JSON.stringify(stuck));
    check(midi.length > 0, 'no key was passed through');
    check(device.inspect().midiInOverruns === 0, 'bytes lost at the MIDI input: ' + device.inspect().midiInOverruns);
    console.log('keyboard: ' + midi.filter(m => (m.status & 0xF0) === 0x90 && m.d2 > 0).length + ' keys passed through, all released');
    midi.length = 0;
  } else {
    check(midi.length === 0, 'MIDI was sent while stopped: ' + midi.length);
  }
  check(device.inspect().mutes === 0b1110, 'mutes before the set: ' + device.inspect().mutes.toString(2));
  // The flash save stalls both cores for about 1.3 s, so the timer misses its
  // alarm once. Nothing may be late after that.
  const overrunsAfterSave = device.inspect().overruns;
  check(overrunsAfterSave <= 1, 'timer overruns during the preparation: ' + overrunsAfterSave);

  // ─── The set ──────────────────────────────────────────────────
  for (const point of show.expectedPlaying) {
    run(point.t);
    for (let track = 0; track < 8; track++) {
      check(device.current(track) === point.map[track] - 1,
        'bar ' + point.bar + ': track ' + (track + 1) + ' plays row ' + (device.current(track) + 1) + ', want ' + point.map[track]);
    }
    check(device.inspect().mutes === point.map.muted,
      'bar ' + point.bar + ': mutes ' + device.inspect().mutes.toString(2) + ', want ' + point.map.muted.toString(2));
  }
  run(show.end + 500000);
  verifyPatterns('end', show.model);

  const end = device.inspect();
  check(end.state === 0, 'transport should be stopped at the end');
  check(end.fatal === null, 'fatal: ' + end.fatal);
  check(end.overruns === overrunsAfterSave, 'timer overruns during the set: ' + (end.overruns - overrunsAfterSave));
  check(!end.midiOverflow, 'MIDI queue overflowed');
  check(end.lockErrors === 0, 'lock errors: ' + end.lockErrors);
  check(end.eventsDropped === 0, 'events dropped: ' + end.eventsDropped);

  // ─── The MIDI stream ──────────────────────────────────────────
  const sounding = new Map();
  const perChannel = {};
  let clocks = 0, firstNote = null, starts = 0, stops = 0;
  for (const m of midi) {
    if (m.status === 0xF8) { clocks++; continue; }
    if (m.status === 0xFA) { starts++; continue; }
    if (m.status === 0xFC) { stops++; continue; }
    const kind = m.status & 0xF0, channel = m.status & 0x0F, key = channel + ':' + m.d1;
    if (kind === 0x90 && m.d2 > 0) {
      if (firstNote === null) firstNote = m.t;
      perChannel[channel] = (perChannel[channel] || 0) + 1;
      sounding.set(key, (sounding.get(key) || 0) + 1);
    } else if (kind === 0x80 || (kind === 0x90 && m.d2 === 0)) {
      if (sounding.get(key)) sounding.set(key, sounding.get(key) - 1);
    }
  }
  // What is heard is what was programmed: in a bar where a clip plays whole,
  // the Note Ons on its channel are the clip's notes in step order, accents at
  // velocity 127. (A synth track's channel is its index.)
  const barStart = bar => show.t0 + (bar - 1) * show.barUs;
  const notesIn = (bar, channel) => midi.filter(m => (m.status & 0xF0) === 0x90 && m.d2 > 0 && (m.status & 15) === channel &&
    m.t >= barStart(bar) - 500 && m.t < barStart(bar + 1) - 500);
  let melodic = 0;
  for (const [bar, track, row] of [[22, 2, 2], [22, 3, 2], [30, 4, 2], [30, 6, 2], [50, 2, 4], [50, 3, 4], [50, 4, 4],
                                   [50, 5, 4], [68, 5, 5], [68, 3, 5]]) {
    const clip = show.clips.synth.find(c => c.track === track && c.row === row);
    const want = clip.notes.slice().sort((x, y) => x[0] - y[0]).map(n => n[1] + '/' + (n[2].includes('a') ? 127 : 100));
    const got = notesIn(bar, track).map(m => m.d1 + '/' + m.d2);
    check(got.join(' ') === want.join(' '), 'bar ' + bar + ' ' + Artist.TRACK_NAME[track] + ' row ' + row +
      ': heard ' + got.join(' ') + ', programmed ' + want.join(' '));
    melodic += got.length;
  }
  // The tracks muted for the outro are silent from the downbeat they leave on.
  for (const [bar, channel] of [[89, 5], [89, 6], [93, 4], [97, 3], [97, 2], [101, 9]]) {
    const late = notesIn(bar, channel).filter(m => channel !== 9 || m.d1 !== 36);
    check(late.length === 0, 'bar ' + bar + ': channel ' + (channel + 1) + ' still played ' + late.length + ' notes after its mute');
  }
  console.log('heard: ' + melodic + ' notes in 10 clip-bars match the score; outro mutes land before their downbeat');

  const hanging = [...sounding].filter(([, n]) => n > 0);
  check(hanging.length === 0, 'notes left sounding: ' + JSON.stringify(hanging));
  check(starts === 1 && stops === 1, 'Start/Stop: ' + starts + '/' + stops);
  const bars = (show.end - 1500000 - show.t0) / show.barUs;
  check(Math.abs(clocks - bars * 96) < 30, 'clocks ' + clocks + ' for ' + bars.toFixed(1) + ' bars');
  check(Math.abs(firstNote - 960 - show.t0) < 6000,
    'first note ' + ((firstNote - show.playAt) / 1000).toFixed(1) + ' ms after PLAY went down; the script assumes ' +
    ((show.t0 - show.playAt) / 1000).toFixed(1));
  console.log('set: ' + midi.length + ' MIDI messages, ' + clocks + ' clocks, notes per channel ' +
    JSON.stringify(perChannel));
  console.log('first note ' + ((firstNote - show.playAt) / 1000).toFixed(2) + ' ms after PLAY went down');
  console.log(said.filter(x => x.kind === 'info').map(x => (x.t / 1e6).toFixed(1) + ' s  ' + x.text).join('\n'));
  console.log(failures ? 'FAILED: ' + failures + ' checks' : 'PASS artist');
  process.exit(failures ? 1 : 0);
});
