// Runs the scripted artist against the real firmware (the WebAssembly core)
// without a browser and checks that the device ended up where the script
// believes it is: the clips it programmed, the clips playing in each section,
// the mutes, and that the MIDI it produced is well formed. Every song is
// played, each on a device of its own.
//   node sim/test/artist_check.js                     every song in every arrangement
//   SONG=3 FORM=sunrise node sim/test/artist_check.js  one of each, by index or id
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

function playSong(core, song, form) {
  const device = new Device(core);
  const hands = new Hands(device);
  // FW2=1: the firmware under test starts new steps at the last note and copies clips.
  // KEYS=1 (with FW2=1): the firmware reads a MIDI keyboard and the artist enters pitches from one.
  // PADS=1: the pads are a keyboard while a step is held; the artist uses them.
  // QMUTE=1: a MUTE press waits for the launch point; the artist plays mute
  // groups, and the check also reads the mutes around each downbeat.
  const pads = process.env.PADS === '1';
  const keys = process.env.KEYS === '1' && !pads;
  const show = Artist.buildShow(undefined, { song, form, fw2: process.env.FW2 === '1' || keys || pads, keys, pads,
    quantMutes: process.env.QMUTE === '1' });
  const before = failures;
  console.log('── ' + show.song.id + ' / ' + show.form.id + ': «' + show.song.title + '», ' + show.song.style + ', ' + show.song.key + ', ' +
    show.bpm + ' BPM, аранжировка «' + show.form.title + '»');
  const said = [];
  const pad = [];
  const io = { say: (kind, text) => said.push({ t: device.now(), kind, text }), rig: () => {}, padMidi: e => pad.push(e) };
  Artist.schedule(show, device, hands, io);
  console.log('show: ' + show.actions.length + ' actions; preparation ' + (show.prepEnd / 1e6).toFixed(1) +
    ' s of device time, set ' + ((show.end - show.playAt) / 1e6).toFixed(1) + ' s');

  const midi = [], cv = [];
  const codes = [0, 0, 0, 0];
  let mask = 0;
  const collect = () => {
    for (const e of device.drain()) {
      if (e.type === 'midi') midi.push(e);
      else if (e.type === 'dac') {
        codes[e.channel] = e.code;
        if (mask & (1 << e.channel)) cv.push({ t: e.t, pair: e.channel, code: e.code, dac: true });
      } else if (e.type === 'gate') {
        for (let pair = 0; pair < codes.length; pair++) {
          if ((mask ^ e.mask) & (1 << pair)) cv.push({ t: e.t, pair, code: codes[pair], high: !!(e.mask & (1 << pair)) });
        }
        mask = e.mask;
      }
    }
  };
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
  if (show.song.cv) {
    for (const [track, pair] of Object.entries(show.model.cv)) {
      const cfg = device.trackConfig(+track);
      check(cfg.output === 1 && cfg.cvChannel === pair, 'track ' + (+track + 1) + ' must own CV/Gate ' + (pair + 1));
    }
    check(device.trackConfig(0).output === 0 && device.trackConfig(3).output === 0, 'kick and ACID 1 must release their CV pairs');
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
  if (keys || pads) {
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
  const startMutes = show.form.start.muted.reduce((bits, track) => bits | (1 << track), 0);
  check(device.inspect().mutes === startMutes, 'mutes before the set: ' + device.inspect().mutes.toString(2) + ', want ' + startMutes.toString(2));
  // The flash save stalls both cores for about 1.3 s, so the timer misses its
  // alarm once. Nothing may be late after that.
  const overrunsAfterSave = device.inspect().overruns;
  check(overrunsAfterSave <= 1, 'timer overruns during the preparation: ' + overrunsAfterSave);

  // ─── The set ──────────────────────────────────────────────────
  // A point is the rows and mutes in the middle of a bar or a beat; with
  // QMUTE also the mutes alone a few ms from a downbeat (`ms`), and the MUTE
  // lights of a group while its first key is held, at two moments a sixteenth
  // apart: bright white at both (each channel equal, as the LED budget may
  // scale a colour down equally on all three, and above the dim white
  // LED_LEVEL_DIM, 3, of a muted track in focus). One of the two moments is an
  // even sixteenth, where a change that only waits shows its normal light.
  const LED_LEVEL_DIM = 3;
  const where = point => 'bar ' + point.bar + (point.beat ? ' beat ' + point.beat : '') +
    (point.ms ? (point.ms > 0 ? ' +' : ' ') + point.ms + ' ms' : '') + (point.later ? ', a sixteenth later' : '');
  const LED_MUTE = track => 72 + track;
  for (const point of show.expectedPlaying) {
    run(point.t);
    if (point.lights) {
      const leds = device.leds();
      for (const track of point.lights) {
        const [r, g, b] = leds.subarray(LED_MUTE(track) * 3, LED_MUTE(track) * 3 + 3);
        check(r === g && g === b && r > LED_LEVEL_DIM, where(point) + ': MUTE ' + Artist.TRACK_NAME[track] + ' light is ' +
          [r, g, b].join(',') + ', want bright white (in the group)');
      }
      continue;
    }
    if (!point.mutesOnly) {
      for (let track = 0; track < 8; track++) {
        check(device.current(track) === point.map[track] - 1,
          where(point) + ': track ' + (track + 1) + ' plays row ' + (device.current(track) + 1) + ', want ' + point.map[track]);
      }
    }
    check(device.inspect().mutes === point.map.muted,
      where(point) + ': mutes ' + device.inspect().mutes.toString(2) + ', want ' + point.map.muted.toString(2));
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
  if (show.song.padNotes) {
    check(pad.length === 24, 'PAD must play three phrases');
    for (let i = 0; i < pad.length; i += 2) {
      const on = pad[i], off = pad[i + 1];
      check(on.status === 0x98 && off?.status === 0x88 && off.d1 === on.d1 && off.t - on.t > show.barUs,
        'PAD must play and release a sustained note on MIDI 9');
    }
  }

  // ─── The MIDI stream ──────────────────────────────────────────
  const sounding = new Map();
  const perChannel = {};
  const perTrack = new Array(Artist.TRACK_NAME.length).fill(0);
  let clocks = 0, firstNote = null, starts = 0, stops = 0;
  for (const m of midi) {
    if (m.status === 0xF8) { clocks++; continue; }
    if (m.status === 0xFA) { starts++; continue; }
    if (m.status === 0xFC) { stops++; continue; }
    const kind = m.status & 0xF0, channel = m.status & 0x0F, key = channel + ':' + m.d1;
    if (kind === 0x90 && m.d2 > 0) {
      if (firstNote === null) firstNote = m.t;
      perChannel[channel] = (perChannel[channel] || 0) + 1;
      if (channel === 9) perTrack[m.d1 === 35 || m.d1 === 36 ? 0 : 1]++;
      else if (channel >= 2 && channel <= 7) perTrack[channel]++;
      sounding.set(key, (sounding.get(key) || 0) + 1);
    } else if (kind === 0x80 || (kind === 0x90 && m.d2 === 0)) {
      if (sounding.get(key)) sounding.set(key, sounding.get(key) - 1);
    }
  }
  // What is heard is what was programmed: in a bar where a clip plays whole,
  // the Note Ons on its channel are the clip's notes in step order, accents at
  // velocity 127. (A synth track's channel is its index.)
  // Bars are counted from the first note on the wire, whose three bytes end
  // 960 us after its tick: PLAY's debounce ends on a pass of the main loop, so
  // the set starts a millisecond or two off the moment the script assumes.
  const cvRises = show.song.cv ? cv.filter(e => e.high) : [];
  const cvFirst = cvRises[0]?.t ?? Infinity;
  const origin = Math.min(firstNote === null ? Infinity : firstNote - 960, cvFirst);
  if (show.song.cv) {
    for (const [pair, track] of Artist.CV_TRACKS.entries()) {
      check(!perChannel[track], Artist.TRACK_NAME[track] + ' leaked to MIDI');
      perTrack[track] += cvRises.filter(e => e.pair === pair).length;
    }
    check(mask === 0, 'native gates must drop after stop');
    check(cvRises.every(e => device.dacVolts(e.code) <= 2), 'native note outside 0–2 V');
    const fx = cv.filter(e => e.pair === 3);
    check(fx.some(e => e.high && e.code === 0) && fx.some(e => e.high && device.dacVolts(e.code) === 2),
      'FX must start both the rising and falling CV contours');
    check(fx.some(e => e.dac && e.code > 0 && device.dacVolts(e.code) < 2), 'FX must slew pitch while holding Gate');
  }
  const barStart = bar => origin + (bar - 1) * show.barUs;
  const notesIn = (bar, channel) => midi.filter(m => (m.status & 0xF0) === 0x90 && m.d2 > 0 && (m.status & 15) === channel &&
    m.t >= barStart(bar) - 500 && m.t < barStart(bar + 1) - 500);
  let melodic = 0;
  const heardAs = notes => notes.slice().sort((x, y) => x[0] - y[0]).map(n => n[1] + '/' + (n[2].includes('a') ? 127 : 100));
  for (const { bar, track, row } of show.heard) {
    const clip = show.clips.synth.find(c => c.track === track && c.row === row);
    const pair = show.song.cv ? Artist.CV_TRACKS.indexOf(track) : -1;
    const want = pair < 0 ? heardAs(clip.notes) : clip.notes.map(note => note[1]);
    const got = pair < 0 ? notesIn(bar, track).map(m => m.d1 + '/' + m.d2)
      : cvRises.filter(e => e.pair === pair && e.t >= barStart(bar) - 500 && e.t < barStart(bar + 1) - 500)
        .map(e => Math.round(24 + 12 * device.dacVolts(e.code)));
    check(got.join(' ') === want.join(' '), 'bar ' + bar + ' ' + Artist.TRACK_NAME[track] + ' row ' + row +
      ': heard ' + got.join(' ') + ', programmed ' + want.join(' '));
    melodic += got.length;
  }
  // The notes changed live are on the wire: one turn of the edited clip,
  // wherever in its loop the bar starts, holds its notes in step order.
  if (show.edited) {
    const { bar, track, row } = show.edited, location = row - 1;
    const length = show.model.len[location + '/' + track] || 16, notes = [];
    for (let step = 0; step < length; step++) {
      const m = show.model.steps[location + '/' + track + '/' + step];
      if (m && m.active) notes.push([step, m.note, m.accent ? 'a' : '']);
    }
    const want = heardAs(notes), from = barStart(bar) - 500, to = from + length * show.barUs / 16;
    const got = midi.filter(m => (m.status & 0xF0) === 0x90 && m.d2 > 0 && (m.status & 15) === track && m.t >= from && m.t < to)
      .map(m => m.d1 + '/' + m.d2);
    let turned = false;
    for (let i = 0; i < want.length && !turned; i++) turned = want.slice(i).concat(want.slice(0, i)).join(' ') === got.join(' ');
    check(turned, 'bar ' + bar + ' ' + Artist.TRACK_NAME[track] + ' row ' + row + ' after the live edits: heard ' + got.join(' ') +
      ', the clip holds ' + want.join(' '));
    melodic += got.length;
  }
  // A track muted for good is silent from the downbeat it leaves on to the
  // end of the set. The kick and the kit share channel 10: the kick is note 36 there.
  for (const { bar, track } of show.silent) {
    const pair = show.song.cv ? Artist.CV_TRACKS.indexOf(track) : -1;
    if (pair >= 0) {
      check(!cvRises.some(e => e.pair === pair && e.t >= barStart(bar) - 500),
        'bar ' + bar + ': native ' + Artist.TRACK_NAME[track] + ' played after its mute');
      continue;
    }
    const channel = track < 2 ? 9 : track;
    const late = midi.filter(m => (m.status & 0xF0) === 0x90 && m.d2 > 0 && (m.status & 15) === channel && m.t >= barStart(bar) - 500)
      .filter(m => track >= 2 || (m.d1 === 36) === (track === 0));
    check(late.length === 0, 'bar ' + bar + ': ' + Artist.TRACK_NAME[track] + ' still played ' + late.length + ' notes after its mute');
  }
  // A kick fill is on the wire: steps 15 and 16 in the bar it is written in
  // and the next, gone again in the bar after.
  const kickSteps = bar => notesIn(bar, 9).filter(m => m.d1 === 36)
    .map(m => Math.round((m.t - barStart(bar)) * 16 / show.barUs) + 1);
  for (const { bar } of show.fills) {
    for (const [b, want] of [[bar, true], [bar + 1, true], [bar + 2, false]]) {
      const steps = kickSteps(b), has = steps.includes(15) && steps.includes(16);
      check(has === want, 'bar ' + b + ': kick fill ' + (want ? 'missing' : 'still there') + ', kick on steps ' + steps.join(' '));
    }
  }
  console.log('heard: ' + melodic + ' notes in ' + (show.heard.length + (show.edited ? 1 : 0)) + ' clip-bars match the score; ' +
    show.silent.length + ' final mutes land before their downbeat');

  const hanging = [...sounding].filter(([, n]) => n > 0);
  for (const [track, hits] of perTrack.entries()) check(hits > 0, Artist.TRACK_NAME[track] + ' never played');
  check(hanging.length === 0, 'notes left sounding: ' + JSON.stringify(hanging));
  check(starts === 1 && stops === 1, 'Start/Stop: ' + starts + '/' + stops);
  const bars = (show.end - 1500000 - show.t0) / show.barUs;
  check(Math.abs(clocks - bars * 96) < 30, 'clocks ' + clocks + ' for ' + bars.toFixed(1) + ' bars');
  check(Math.abs(origin - show.t0) < 6000,
    'first note ' + ((origin - show.playAt) / 1000).toFixed(1) + ' ms after PLAY went down; the script assumes ' +
    ((show.t0 - show.playAt) / 1000).toFixed(1));
  console.log('set: ' + midi.length + ' MIDI messages, ' + clocks + ' clocks, notes per channel ' +
    JSON.stringify(perChannel));
  console.log('tracks: ' + Artist.TRACK_NAME.map((name, i) => name + ' ' + perTrack[i]).join(' | '));
  console.log('first note ' + ((firstNote - show.playAt) / 1000).toFixed(2) + ' ms after PLAY went down');
  console.log(said.filter(x => x.kind === 'info').map(x => (x.t / 1e6).toFixed(1) + ' s  ' + x.text).join('\n'));
  // The MUTE moves of the set as the captions name them (one MUTE, two
  // fingers, a group held).
  console.log(said.filter(x => x.kind === 'device' && x.t >= show.playAt && /^(Отпускает )?MUTE /.test(x.text))
    .map(x => (x.t / 1e6).toFixed(1) + ' s  ' + x.text).join('\n'));
  if (failures > before) console.log('FAILED ' + show.song.id + ': ' + (failures - before) + ' checks');
  // The mute groups of the score, each checked above.
  return show.expectedPlaying.filter(point => point.lights && !point.later).length;
}

(async () => {
  const one = process.env.SONG, oneForm = process.env.FORM;
  const songs = one === undefined ? Artist.SONGS.map((s, i) => i) : [/^\d+$/.test(one) ? +one : one];
  // buildShow falls back to the first song: a mistyped SONG must not pass as that one.
  if (one !== undefined && !(Artist.SONGS[songs[0]] || Artist.SONGS.some(s => s.id === one))) {
    console.error('unknown SONG ' + one + '; there are ' + Artist.SONGS.length + ': ' + Artist.SONGS.map(s => s.id).join(', '));
    process.exit(2);
  }
  if (oneForm !== undefined && !Artist.FORMS.some(f => f.id === oneForm)) {
    console.error('unknown FORM ' + oneForm + '; there are: ' + Artist.FORMS.map(f => f.id).join(', '));
    process.exit(2);
  }
  // Every song in every arrangement, unless one is named.
  const forms = oneForm === undefined ? Artist.FORMS.map(f => f.id) : [oneForm];
  let shows = 0;
  const groups = {};
  for (const song of songs) {
    for (const form of forms) {
      groups[form] = (groups[form] || 0) + playSong(await RebirthCore(), song, form);
      shows++;
    }
  }
  // With QMUTE every arrangement has to hold a mute group somewhere: a score
  // that found no room for one would otherwise pass on two-finger pairs alone.
  if (process.env.QMUTE === '1') {
    for (const form of forms) check(groups[form] > 0, 'arrangement ' + form + ': no show played a mute group');
    console.log('mute groups: ' + forms.map(form => form + ' ' + groups[form]).join(', '));
  }
  console.log(failures ? 'FAILED: ' + failures + ' checks' : 'PASS artist (' + shows + (shows === 1 ? ' show)' : ' shows)'));
  process.exit(failures ? 1 : 0);
})();
