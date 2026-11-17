// The scripted Goa trance artist. It is a score of finger movements: every
// entry presses or releases a control, turns an encoder by one quadrature
// edge, or turns a knob on the synths (which are not part of the device). It
// never reads or writes the device's memory. It is open loop, like a rehearsed
// set: what it expects to have programmed is recorded in `expected`, and
// sim/test/artist_check.js compares that with the device afterwards.
(function (root) {
  'use strict';

  const T = { KICK: 0, KIT: 1, BASS: 2, ACID1: 3, ACID2: 4, LEAD: 5, ARP: 6, FX: 7 };
  const TRACK_NAME = ['KICK', '909', 'BASS', 'ACID 1', 'ACID 2', 'LEAD', 'ARP', 'FX'];
  const LANE = { AC: 0, BD: 1, SD: 2, LT: 3, MT: 4, HT: 5, RS: 6, CP: 7, CH: 8, OH: 9, CC: 10, RC: 11 };
  const LANE_NAME = ['AC', 'BD', 'SD', 'LT', 'MT', 'HT', 'RS', 'CP', 'CH', 'OH', 'CC', 'RC'];

  // A Phrygian dominant: A Bb C# D E F G.
  const N = { A1: 33, Bb1: 34, C2: 36, E2: 40, G2: 43, A2: 45, Bb2: 46, Cs3: 49, D3: 50, E3: 52, F3: 53,
              G3: 55, A3: 57, Bb3: 58, G4: 67, A4: 69, Bb4: 70, Cs5: 73, D5: 74, E5: 76, F5: 77, G5: 79,
              A5: 81, Bb5: 82 };

  const BASS_STEPS = [2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15, 16];   // the kick owns 1, 5, 9, 13
  const bass = (root_, changes) => BASS_STEPS.map(s => [s, (changes && changes[s]) || root_, '']);

  // Melodic clips: [step 1-16, note, flags] with 'a' accent, 's' slide into the next step.
  // Rows are 1-8 as printed on the panel.
  const SYNTH_CLIPS = [
    { track: T.BASS, row: 1, name: 'катящийся бас на ля', notes: bass(N.A1) },
    { track: T.BASS, row: 2, from: 1, name: 'бас с октавой', notes: bass(N.A1, { 8: N.A2, 16: N.E2 }) },
    { track: T.BASS, row: 3, from: 1, name: 'бас на си-бемоль (напряжение)', notes: bass(N.Bb1) },
    { track: T.BASS, row: 4, from: 1, name: 'бас для пика', notes: bass(N.A1, { 4: N.A2, 12: N.A2, 16: N.C2 }) },

    { track: T.ACID1, row: 1, name: 'кислотный намёк', notes: [
      [1, N.A2, ''], [4, N.A2, ''], [7, N.A2, 'a'], [11, N.A2, ''], [14, N.Bb2, ''] ] },
    { track: T.ACID1, row: 2, name: 'кислотная линия A', notes: [
      [1, N.A2, 'a'], [2, N.A2, ''], [3, N.A3, 's'], [4, N.A2, ''], [5, N.Bb2, 'a'], [6, N.A2, ''],
      [7, N.E3, ''], [9, N.A2, 'a'], [10, N.Cs3, 's'], [11, N.D3, ''], [12, N.A2, ''], [13, N.G3, 'a'],
      [14, N.F3, 's'], [15, N.E3, ''], [16, N.Bb2, ''] ] },
    { track: T.ACID1, row: 3, len: 12, name: 'верхняя линия, 12 шагов против 16', notes: [
      [1, N.A3, 'a'], [2, N.A2, ''], [3, N.E3, ''], [4, N.A3, 's'], [5, N.Bb3, 'a'], [6, N.A2, ''],
      [7, N.G3, ''], [8, N.A3, ''], [10, N.D3, 's'], [11, N.E3, ''], [12, N.Cs3, ''] ] },
    { track: T.ACID1, row: 4, name: 'плотная линия для пика', notes: [
      [1, N.A2, 'a'], [2, N.A3, ''], [3, N.A2, ''], [4, N.Bb3, 'as'], [5, N.A3, ''], [6, N.A2, ''],
      [7, N.G3, 'a'], [8, N.E3, ''], [9, N.A2, ''], [10, N.A3, 'a'], [11, N.F3, 's'], [12, N.E3, ''],
      [13, N.Cs3, 'a'], [14, N.D3, ''], [15, N.E3, 's'], [16, N.A3, 'a'] ] },
    { track: T.ACID1, row: 5, name: 'редкая линия для брейка', notes: [
      [1, N.A2, 'a'], [4, N.E3, ''], [7, N.Bb2, 's'], [8, N.A2, ''], [11, N.G2, ''], [13, N.A2, 'a'],
      [15, N.Cs3, ''] ] },

    { track: T.ACID2, row: 2, name: 'вторая кислота, ответы', notes: [
      [3, N.E3, ''], [4, N.E3, ''], [7, N.D3, 'a'], [11, N.E3, ''], [12, N.F3, 's'], [13, N.E3, ''],
      [15, N.Cs3, 'a'] ] },
    { track: T.ACID2, row: 3, name: 'вторая кислота, спуск', notes: [
      [1, N.A3, 'a'], [3, N.G3, ''], [5, N.F3, 'as'], [6, N.E3, ''], [9, N.D3, ''], [11, N.Cs3, 'a'],
      [13, N.Bb2, 's'], [14, N.A2, ''], [16, N.E3, ''] ] },
    { track: T.ACID2, row: 4, name: 'вторая кислота, верх', notes: [
      [2, N.A3, ''], [4, N.A3, 'a'], [6, N.Bb3, 's'], [7, N.A3, ''], [10, N.G3, ''], [12, N.A3, 'a'],
      [14, N.E3, ''], [16, N.G3, 's'] ] },

    { track: T.LEAD, row: 3, name: 'мелодия A', notes: [
      [1, N.A4, ''], [3, N.Cs5, ''], [5, N.E5, ''], [7, N.D5, ''], [9, N.Bb4, 'a'], [11, N.A4, ''],
      [13, N.G4, ''], [15, N.Bb4, ''] ] },
    { track: T.LEAD, row: 4, name: 'мелодия B, шестнадцатыми', notes: [
      [1, N.A4, 'a'], [2, N.E5, ''], [3, N.A5, ''], [4, N.E5, ''], [5, N.Bb4, 'a'], [6, N.F5, ''],
      [7, N.Bb5, ''], [8, N.F5, ''], [9, N.Cs5, 'a'], [10, N.E5, ''], [11, N.A5, ''], [12, N.G5, ''],
      [13, N.F5, 'a'], [14, N.E5, ''], [15, N.D5, ''], [16, N.Cs5, ''] ] },
    { track: T.LEAD, row: 5, name: 'медленная мелодия для брейка', notes: [
      [1, N.A4, ''], [5, N.Bb4, ''], [9, N.Cs5, ''], [13, N.Bb4, ''] ] },

    { track: T.ARP, row: 2, name: 'арпеджио', notes: [
      [1, N.A5, ''], [2, N.E5, ''], [3, N.A5, ''], [4, N.E5, ''], [5, N.Bb5, ''], [6, N.E5, ''],
      [7, N.Bb5, ''], [8, N.E5, ''], [9, N.A5, ''], [10, N.E5, ''], [11, N.A5, ''], [12, N.E5, ''],
      [13, N.G5, ''], [14, N.E5, ''], [15, N.F5, ''], [16, N.E5, ''] ] },
    { track: T.ARP, row: 4, len: 6, name: 'арпеджио из 6 шагов против 16', notes: [
      [1, N.A5, ''], [2, N.E5, ''], [3, N.Bb5, ''], [4, N.E5, ''], [5, N.A5, ''], [6, N.G5, ''] ] },
  ];

  // Drum-type clips: steps only; 'a' needs the Pattern view.
  const DRUM_CLIPS = [
    { track: T.KICK, row: 4, from: 1, name: 'бочка для пика', steps: [[1, ''], [5, ''], [9, ''], [13, '']] },
    { track: T.FX, row: 3, name: 'удар с тарелкой на сильную долю', steps: [[1, '']] },
    { track: T.FX, row: 5, name: 'подъём (райзер) для брейка', steps: [[1, 'a']] },
  ];

  // Kit clips: lanes in the order the artist walks them, steps per lane; a
  // step listed twice is tapped twice (off -> note -> accent).
  const OFFBEATS = [3, 7, 11, 15];
  const CLOSED = [1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, 16];
  const BEATS = [1, 5, 9, 13];
  const KIT_CLIPS = [
    { row: 2, name: 'хэты с открытым на слабую долю и клэп', lanes: [
      [LANE.CP, [5, 13]], [LANE.CH, CLOSED], [LANE.OH, OFFBEATS] ] },
    { row: 3, from: 2, name: 'то же с дробью малого в конце такта', lanes: [
      [LANE.OH, OFFBEATS], [LANE.CH, CLOSED], [LANE.CP, [5, 13]], [LANE.SD, [13, 14, 15, 15, 16, 16]] ],
      added: [[LANE.SD, [13, 14, 15, 15, 16, 16]]] },
    { row: 4, from: 2, name: 'полный набор для пика: акцент, райд', lanes: [
      [LANE.AC, BEATS], [LANE.CP, [5, 13]], [LANE.CH, CLOSED], [LANE.OH, OFFBEATS], [LANE.RC, BEATS] ],
      added: [[LANE.AC, BEATS], [LANE.RC, BEATS]] },
  ];

  // ─── Songs ─────────────────────────────────────────────────────
  // Every song fills the same clip slots, because the set is played the same
  // way each time: BASS rows 1-4 (2-4 are row 1 with changes), ACID 1 rows 1-5
  // (row 3 may be shorter than 16), ACID 2 rows 2-4, LEAD rows 3-5, ARP rows 2
  // and 4 (row 4 may be shorter), the kit on rows 2-4 (3 and 4 are row 2 with
  // lanes added). `edits` are the three notes changed live in ACID 1 row 3.
  // `sound` is what the artist sets on the synths, which the device knows
  // nothing about.
  const SOUND = {
    acid1: { cut: 0.22, open: 0.5, top: 0.92, peak: 0.62, dip: 0.4, back: 0.66, out: 0.3,
             res: 0.72, resHi: 0.88, env: 0.6, decay: 0.35, acc: 0.8 },
    acid2: { cut: 0.3, open: 0.6, out: 0.25, res: 0.85, env: 0.6, decay: 0.35, acc: 0.8 },
    lead: { cut: 0.45, open: 0.75, top: 0.85 },
    delay: 0.45, delayHi: 0.68, reverb: 0.3, reverbHi: 0.6,
  };
  function sound(changes) {
    const out = {};
    for (const k of Object.keys(SOUND)) {
      out[k] = typeof SOUND[k] === 'object' ? Object.assign({}, SOUND[k], changes && changes[k]) : (changes && k in changes ? changes[k] : SOUND[k]);
    }
    return out;
  }

  const SONGS = [{
    id: 'set', title: 'Первый сет', style: 'гоа-транс', bpm: 145, key: 'ля фригийский доминантовый',
    synth: SYNTH_CLIPS, drum: DRUM_CLIPS, kit: KIT_CLIPS,
    edits: [[0, N.A3 + 12], [6, N.E3], [7, N.G3]],
    sound: sound(),
  }];

  // The other songs are written in scale degrees: a clip is a row of steps,
  // each `-` (rest) or a degree 1-7 with `'` (an octave up) or `,` (down), `b`
  // or `#` in front for a semitone outside the scale, `a` for an accent and `s`
  // for a slide into the next step. Fewer than 16 steps make a shorter clip.
  const SCALES = {
    phrygian: [0, 1, 3, 5, 7, 8, 10], dominant: [0, 1, 4, 5, 7, 8, 10], double: [0, 1, 4, 5, 7, 8, 11],
    harmonic: [0, 2, 3, 5, 7, 8, 11], minor: [0, 2, 3, 5, 7, 8, 10],
  };
  // The bass octave of each root.
  const ROOTS = { E: 28, F: 29, 'F#': 30, G: 31, 'G#': 32, A: 33, Bb: 34, B: 35, C: 36, 'C#': 37, D: 38 };
  const OCTAVE = { [T.BASS]: 0, [T.ACID1]: 1, [T.ACID2]: 1, [T.LEAD]: 3, [T.ARP]: 4 };
  const CLIP_NAMES = {
    [T.BASS]: ['', 'катящийся бас', 'бас с вариацией', 'бас уходит с тоники (напряжение)', 'бас для пика'],
    [T.ACID1]: ['', 'кислотный намёк', 'кислотная линия A', 'верхняя линия', 'плотная линия для пика', 'редкая линия для брейка'],
    [T.ACID2]: ['', '', 'вторая кислота, ответы', 'вторая кислота, спуск', 'вторая кислота, верх'],
    [T.LEAD]: ['', '', '', 'мелодия A', 'мелодия B, шестнадцатыми', 'медленная мелодия для брейка'],
    [T.ARP]: ['', '', 'арпеджио', '', 'короткое арпеджио'],
  };
  const KIT_NAMES = ['', '', 'основной рисунок', 'то же со сбивкой в конце такта', 'полный набор для пика'];

  function pitch(root, scale, octave, token) {
    const m = /^([b#]?)([1-7])([',]*)([as]*)$/.exec(token);
    if (!m) throw new Error('artist: bad step "' + token + '"');
    let note = root + 12 * octave + scale[+m[2] - 1] + (m[1] === 'b' ? -1 : m[1] === '#' ? 1 : 0);
    for (const c of m[3]) note += c === "'" ? 12 : -12;
    if (!Number.isInteger(note) || note < 0 || note > 127) throw new Error('artist: step "' + token + '" is note ' + note);
    return { note, flags: m[4] };
  }

  function compose(d) {
    const root = ROOTS[d.root], scale = SCALES[d.scale];
    if (root === undefined || !scale) throw new Error('artist: song "' + d.id + '": unknown root or scale');
    const clip = (track, row, text) => {
      const tokens = text.trim().split(/\s+/), notes = [];
      tokens.forEach((token, i) => {
        if (token === '-') return;
        const p = pitch(root, scale, OCTAVE[track], token);
        notes.push([i + 1, p.note, p.flags]);
      });
      const c = { track, row, name: CLIP_NAMES[track][row], notes };
      if (tokens.length !== 16) {
        c.len = tokens.length;
        c.name += ', ' + c.len + ' шагов против 16';
      }
      if (track === T.BASS && row > 1) c.from = 1;
      return c;
    };
    const lanes = list => list.map(([name, steps]) => [LANE[name], steps]);
    const [base, fill, full] = d.kit;
    return {
      id: d.id, title: d.title, style: d.style, bpm: d.bpm, key: d.key,
      synth: [
        ...d.bass.map((text, i) => clip(T.BASS, i + 1, text)),
        ...d.acid1.map((text, i) => clip(T.ACID1, i + 1, text)),
        ...d.acid2.map((text, i) => clip(T.ACID2, i + 2, text)),
        ...d.lead.map((text, i) => clip(T.LEAD, i + 3, text)),
        clip(T.ARP, 2, d.arp[0]), clip(T.ARP, 4, d.arp[1]),
      ],
      drum: DRUM_CLIPS,
      kit: [
        { row: 2, name: KIT_NAMES[2], lanes: lanes(base) },
        { row: 3, from: 2, name: KIT_NAMES[3], lanes: lanes(base.concat(fill)), added: lanes(fill) },
        { row: 4, from: 2, name: KIT_NAMES[4], lanes: lanes(base.concat(full)), added: lanes(full) },
      ],
      edits: d.edits.map(([step, token]) => [step - 1, pitch(root, scale, OCTAVE[T.ACID1], token).note]),
      sound: sound(d.sound),
    };
  }

  // The synth settings of the four styles.
  const TWISTED = { acid1: { cut: 0.2, open: 0.55, top: 0.95, res: 0.85, resHi: 0.97, env: 0.8, decay: 0.3, acc: 0.9 },
                    acid2: { res: 0.9, env: 0.75 }, delay: 0.5, delayHi: 0.72 };
  const MELODIC = { acid1: { res: 0.65, resHi: 0.8, env: 0.5, decay: 0.45, acc: 0.7 },
                    acid2: { cut: 0.35, open: 0.65, res: 0.7, env: 0.45 },
                    lead: { cut: 0.55, open: 0.85, top: 0.95 }, delay: 0.5, reverb: 0.4, reverbHi: 0.7 };
  const DARK = { acid1: { cut: 0.18, open: 0.42, peak: 0.55, dip: 0.34, back: 0.58, out: 0.24, res: 0.8, resHi: 0.92, env: 0.55, decay: 0.6, acc: 0.85 },
                 acid2: { cut: 0.25, open: 0.5, out: 0.2, res: 0.88, decay: 0.55 },
                 lead: { cut: 0.4, open: 0.65, top: 0.8 }, reverb: 0.35 };
  const HYPNOTIC = { acid1: { cut: 0.25, open: 0.55, res: 0.85, resHi: 0.93, env: 0.65, decay: 0.45 },
                     acid2: { res: 0.8 }, delay: 0.4, delayHi: 0.6, reverb: 0.2, reverbHi: 0.45 };

  const SNARE_ROLL = ['SD', [13, 14, 15, 15, 16, 16]];
  const HATS = [['CH', CLOSED], ['OH', OFFBEATS]];

  SONGS.push(...[
    { id: 'garden', title: 'Кислотный сад', style: 'в духе Hallucinogen', root: 'F#', scale: 'phrygian', key: 'фа-диез фригийский', bpm: 146,
      bass: ["- 1 1 1 - 1 1 1 - 1 1 1 - 1 1 1",
             "- 1 1 1' - 1 1 1 - 1 1 1' - 1 1 7,",
             "- 2 2 2 - 2 2 2 - 2 2 2 - 2 2 2",
             "- 1 1' 1 - 1 1' 1 - 1 1' 1 - 1 5 7"],
      acid1: ["1 - - 1 - - 1a - - - 1 - - 2 - -",
              "1a 1' 1 1's 7 1 #4a 5 1 1' 2s 1 7,a 1 3s 2",
              "1'a 1 5 1' 2's 1' 7 5a 1 #4s 5 3 1' 2",
              "1a 1' 1 2'as 1' 1 7a 5 1 1'a 6s 5 3a 4 5s 1'a",
              "1a - - 5 - - 2s 1 - - 7, - 1a - 3 -"],
      acid2: ["- - 5 5 - - 4a - - - 5 6s 5 - 3a -",
              "1'a - 7 - 6as 5 - - 4 - 3a - 2s 1 - 5",
              "- 1' - 1'a - 2's 1' - - 7 - 1'a - 5 - 7s"],
      lead: ["1 - 3 - 5 - 4 - 2a - 1 - 7, - 2 -",
             "1a 5 1' 5 2a 6 2' 6 3a 5 1' 7 6a 5 4 3",
             "1 - - - 2 - - - 3 - - - 2 - - -"],
      arp: ["1 5, 1 5, 2 5, 2 5, 1 5, 1 5, 7, 5, 6, 5,", "1 5, 2 5, 1 7,"],
      kit: [[['CP', [5, 13]], ...HATS], [SNARE_ROLL], [['AC', BEATS], ['RC', BEATS]]],
      edits: [[1, "1''"], [7, "5"], [8, "7"]], sound: TWISTED },

    { id: 'spiral', title: 'Спираль', style: 'в духе Hallucinogen', root: 'D', scale: 'dominant', key: 'ре фригийский доминантовый', bpm: 142,
      bass: ["- 1 1 1' - 1 1 1 - 1 1 1' - 1 1 1",
             "- 1 1 1' - 1 1 5 - 1 1 1' - 1 7 1'",
             "- 2 2 2' - 2 2 2 - 2 2 2' - 2 2 2",
             "- 1 1' 1' - 1 1 1' - 1 1' 1' - 3 2 1"],
      acid1: ["- - 1 - - 1a - - 1 - - 2 - - 1 -",
              "1a - 1 3s 4 1 - 5a 4s 3 1 - 2a 1 7, 1",
              "1a 3 4 5s 6 5 4a 3 1' 7s 5 2",
              "1a 3 1 4a 1 5 1 6as 5 4 3a 1 2 3s 4 5a",
              "1a - - - 3s 4 - - 5a - - 6s 5 - 2 -"],
      acid2: ["- 5 - - 5a - 4 - - 5 - 6s 5 - 3 -",
              "5a - 6 5 - 4a - 3 - 2s 1 - 3a - 4 -",
              "1' - 1'a 7 - 1' 2'as 1' - 5 - 1'a - 7 6s 5"],
      lead: ["5 - - 6 5 - 4 - 3a - - 4 3 - 2 -",
             "1'a 5 3 5 1' 5 3 5 2'a 6 4 6 2' 6 4 6",
             "5 - - - - - 6 - 5 - - - - - 3 -"],
      arp: ["1 3 5 1' 5 3 1 3 2 4 6 2' 6 4 2 4", "1 5 1' 5 3"],
      kit: [[['SD', [5, 13]], ...HATS, ['RS', [4, 7, 12, 15]]], [['LT', [14]], ['MT', [15]], ['HT', [16]]],
            [['AC', BEATS], ['RC', OFFBEATS]]],
      edits: [[1, "1'"], [4, "7"], [9, "3'"]],
      sound: Object.assign({}, TWISTED, { acid1: Object.assign({}, TWISTED.acid1, { decay: 0.42, resHi: 0.94 }) }) },

    { id: 'mercury', title: 'Ртуть', style: 'в духе Hallucinogen', root: 'G#', scale: 'double', key: 'соль-диез двойной гармонический', bpm: 148,
      bass: ["- 1 1 1 - 1 1 1 - 1 1 1 - 1 2 1",
             "- 1 1 1 - 1 1' 1 - 1 1 1 - 1 2 3",
             "- 4 4 4 - 4 4 4 - 4 4 4 - 4 3 2",
             "- 1 1' 1 - 1 1' 1 - 1 1' 1 - 2 2' 2"],
      acid1: ["1 - 1 - - - 2a - 1 - 1 - - - 7, -",
              "1a 2 1 7,s 1 - 3a 2 1 2s 3 4a - 3 2s 1",
              "1a 1' 7 1' 5a 6s 5 3 2a 1",
              "1a 2 3 4a 5 6s 5 4 3a 2 1 1'a 7s 1' 2' 1'a",
              "1 - - 2s 1 - - - 5a - - 6s 5 - - 7,"],
      acid2: ["- - 3 - 3a - 2 - - - 3 4s 3 - 1a -",
              "5a 6s 5 - 4 - 3a - 2 - 1 - 7,a 1s 2 -",
              "1' - 7 1'a - 2' 1' - 7as 1' - 5 6 - 5a -"],
      lead: ["1 2 3 - 4 3 2 - 1a - 7, - 1 - 3 -",
             "1a 3 5 7 1' 7 5 3 2a 4 6 1' 2' 1' 6 4",
             "3 - - - 2 - - - 1 - - - 7, - - -"],
      arp: ["1 1' 5 1' 1 1' 6 1' 1 1' 5 1' 1 1' 4 1'", "1 3 5 7 1' 7 5"],
      kit: [[['SD', [5, 13]], ['CP', [5, 13]], ...HATS], [['HT', [11, 12]], ['MT', [13, 14]], ['LT', [15, 16]]],
            [['AC', BEATS], ['CC', [1]], ['RC', OFFBEATS]]],
      edits: [[1, "1'"], [4, "3'"], [8, "4"]],
      sound: Object.assign({}, TWISTED, { acid1: Object.assign({}, TWISTED.acid1, { cut: 0.24, env: 0.9, decay: 0.25 }) }) },

    { id: 'transit', title: 'Утренний транзит', style: 'в духе Astral Projection', root: 'B', scale: 'harmonic', key: 'си гармонический минор', bpm: 140,
      bass: ["- 1 1 1 - 1 1 1 - 1 1 1 - 1 1 1",
             "- 1 1 1 - 1 1 1 - 1 1 1 - 1 1 5,",
             "- 6, 6, 6, - 6, 6, 6, - 6, 6, 6, - b7, b7, b7,",
             "- 1 1 1' - 1 1 1' - 1 1 1' - 1 5 1'"],
      acid1: ["1 - - - 1 - - 1 - - 1a - - - 1 -",
              "1a 1 5 1 1 3a 1 5 1 1 4a 1 5 1 3 2",
              "1'a 5 3 1' 5 3 2'a 5 4 2' 5 4",
              "1a 5 1' 5 3'a 1' 5 1' 4'a 1' 5 1' 5'as 4' 3' 2'",
              "1 - - - - - 3s 2 - - - - 1a - 5, -"],
      acid2: ["- - 1' - - 7 - 1'a - - 3' - 2' - 1' -",
              "5 - - 6s 5 - 3a - 4 - - 5s 4 - 2a -",
              "1' - 5 - 1'a - 5 - 2' - 6 - 2'a - 7s 1'"],
      lead: ["1 - 2 3 - 5 - 3 4a - 3 2 - 3 - 7,",
             "5a 1' 3' 1' 5 1' 3' 1' 6a 1' 4' 1' 7a 2' 5' 2'",
             "5 - - - - - 6 - 5 - - - 3 - 2 -"],
      arp: ["1 3 5 3 1' 5 3 5 1 3 6 3 1' 6 3 6", "1 5 1' 3' 1' 5"],
      kit: [[['CP', [5, 13]], ...HATS], [['SD', [13, 15, 16, 16]]], [['AC', [1, 9]], ['RC', BEATS], ['HT', [16]]]],
      edits: [[1, "3'"], [7, "1'"], [10, "3'"]], sound: MELODIC },

    { id: 'beacon', title: 'Маяк', style: 'в духе Astral Projection', root: 'E', scale: 'minor', key: 'ми минор', bpm: 138,
      bass: ["- 1 1' 1 - 1 1' 1 - 1 1' 1 - 1 1' 1",
             "- 1 1' 1 - 1 1' 1 - 1 1' 1 - 7, 7 7,",
             "- 6, 6 6, - 6, 6 6, - 6, 6 6, - 6, 6 6,",
             "- 1 1' 1 - 1 1' 1 - 3 3' 3 - 4 4' 5"],
      acid1: ["- 1 - - - 1 - 1a - 1 - - - 1 - 7,",
              "1a - 1 1' - 1 7a - 1 1' - 1 5a - 3 4",
              "1a 1' 5 7 1' 5a 3 5 7s 1' 2' 7",
              "1a 1' 1 7 1a 1' 1 5 1a 1' 1 3' 2'a 1' 7 5s",
              "1 - - 5 - - - 3s 2 - - 1a - - 7, -"],
      acid2: ["5 - - 5a - - 4 - 5 - - 7s 5 - 3 -",
              "- 1' - 7 - 5a - 4 - 3 - 4s 5 - 7a -",
              "3'a - 2' - 1' - 7a - 1' - 2' - 3'as 2' 1' 7"],
      lead: ["1 - - 3 - - 5 - 4a - - 3 - - 2 -",
             "1'a 7 5 3 1' 7 5 3 6a 5 3 1 7a 5 4 2",
             "1 - - - 3 - - - 2 - - - 7, - - -"],
      arp: ["1 5 1' 5 1 5 1' 5 6, 3 6 3 7, 4 7 4", "1 5 3 1' 5 3"],
      kit: [[['SD', [5, 13]], ...HATS], [['CP', [13, 15, 16]]], [['AC', BEATS], ['RC', BEATS], ['CP', [5, 13]]]],
      edits: [[1, "1'"], [6, "7"], [9, "5"]],
      sound: Object.assign({}, MELODIC, { acid1: Object.assign({}, MELODIC.acid1, { cut: 0.26, open: 0.54 }), reverb: 0.45 }) },

    { id: 'desert', title: 'Сон о пустыне', style: 'в духе Astral Projection', root: 'C#', scale: 'dominant', key: 'до-диез фригийский доминантовый', bpm: 143,
      bass: ["- 1 1 1 - 1 1 2 - 1 1 1 - 1 1 7,",
             "- 1 1 1' - 1 1 2 - 1 1 1' - 1 2 7,",
             "- 2 2 2 - 2 2 3 - 2 2 2 - 2 2 1",
             "- 1 1' 1 - 1 1' 2 - 1 1' 1 - 4 3 2"],
      acid1: ["1 - - 1 - 2 - - 1a - - 1 - 3 - -",
              "1a 1 3s 4 1 1 5a 4 1 1 3s 2 1 7,a 1 2",
              "1'a 7 6 5s 6 5 4 3a 4 5 6s 7 1' 2'",
              "1a 3 4 5a 4 3 1 1'a 7 6 5a 6s 7 1' 2'a 1'",
              "1a - - - 2 - - 3s 4 - - - 3a - 2 -"],
      acid2: ["- 5 - 5 - - 6a - - 5 - 4 - - 3a -",
              "1' - - 7s 6 - 5a - - 4s 3 - 2a - 1 -",
              "5a - 6 - 7 - 1'a - 2' - 1' - 7as 6 5 4"],
      lead: ["1 2 3 - 2 1 7, - 1a 2 3 4 3 - 2 -",
             "1a 2 3 4 5 4 3 2 3a 4 5 6 7 6 5 4",
             "1 - - 2 3 - - - 2 - - 1 7, - - -"],
      arp: ["1 5 1' 5 2 5 1' 5 3 5 1' 5 2 5 1' 5", "1 2 3 5 3 2"],
      kit: [[['CP', [5, 13]], ...HATS, ['HT', [4]], ['MT', [12]]], [['SD', [14, 15, 16, 16]]], [['AC', BEATS], ['RC', BEATS]]],
      edits: [[1, "3'"], [7, "2"], [8, "1"]],
      sound: Object.assign({}, MELODIC, { acid1: Object.assign({}, MELODIC.acid1, { res: 0.75, resHi: 0.88, env: 0.6 }) }) },

    { id: 'penumbra', title: 'Полутень', style: 'в духе Total Eclipse', root: 'C', scale: 'phrygian', key: 'до фригийский', bpm: 145,
      bass: ["- 1 1 1 - 1 1 1 - 1 1 1 - 1 1 2",
             "- 1 1 1 - 1 1 3 - 1 1 1 - 1 1 2",
             "- 1 1 1 - 2 2 2 - 1 1 1 - 2 2 2",
             "- 1 1' 1 - 1 1 3 - 1 1' 1 - 2 2' 2"],
      acid1: ["- - 1 - - - 1 - - 1a - - - - 2 -",
              "1 1a 2 1 1 3a 1 2 1 1a 4 3 2 1a 7, 2",
              "1a 2 3 1 2a 3 4 2 3a 4 5s 3",
              "1a 1 2 3a 1 1 4 5a 1 1 6s 5 4a 3 2s 1",
              "1 - 2s 1 - - - - 3a - 2s 1 - - 7, -"],
      acid2: ["1 - - 1a - - 1 - 2 - - 1a - - 7, -",
              "5a - 4 - 3 - 2as 1 - 1 - 2 3a - 2 -",
              "1'a 7 - 1' 6a - 5 - 4a 5 - 6 7as 1' - 5"],
      lead: ["5 - - - 6 - 5 - - 4a - - 3 - 2 -",
             "1'a 1' 7 1' 6a 6 5 6 4a 4 3 4 2a 3 4 5",
             "1 - - - - - 2 - 1 - - - - - 7, -"],
      arp: ["1 3 5 3 1 3 6 3 1 3 5 3 2 4 6 4", "1 3 5 1' 6"],
      kit: [[['CP', [13]], ['SD', [5]], ...HATS], [['HT', [14]], ['LT', [15, 16]]], [['AC', BEATS], ['RC', [1, 3, 5, 7, 9, 11, 13, 15]]]],
      edits: [[1, "1'"], [5, "4"], [11, "6"]], sound: DARK },

    { id: 'farside', title: 'Обратная сторона', style: 'в духе Total Eclipse', root: 'F', scale: 'harmonic', key: 'фа гармонический минор', bpm: 147,
      bass: ["- - 1 1 - 1 1 1 - - 1 1 - 1 1 1",
             "- - 1 1 - 1 1 1' - - 1 1 - 1 5 1'",
             "- - 4 4 - 4 4 4 - - 5 5 - 5 5 5",
             "- - 1 1' - 1 1 1' - - 1 1' - 7 1' 2'"],
      acid1: ["1 - - - - 1a - - 1 - - - - 2 - 1",
              "1a - 1' 1 - 7a 1' - 5 6s 5 - 3a 2 - 1",
              "1'a 7 1' 5 6a 5 3 4s 5 7",
              "1'a 7 1' 5a 6 5 3a 4 5 1'a 2' 1' 7a 1's 2' 3'a",
              "1a - - - - 7,s 1 - - - 3a - 2 - - -"],
      acid2: ["- 5 5 - - 5a - 6 - 5 5 - - 7a - 1'",
              "3'a - 2' 1' - 7a - 1' 6 - 5a - 4s 5 - 7",
              "1' 1' - 7a - 1' 2' - 3'a 2' - 1' 7as 1' - 5"],
      lead: ["1 - 3 - 2 - 7, - 1a - 5 - 4 - 3 2",
             "1'a 3' 1' 5 1' 3' 1' 5 7a 2' 7 5 7 2' 7 5",
             "5 - - 6 - - 5 - - 4 - - 3 - 2 -"],
      arp: ["1 3 5 1' 3' 1' 5 3 7, 2 5 7 2' 7 5 2", "1 1' 5 1' 3 1'"],
      kit: [[['CP', [5, 13]], ...HATS], [['SD', [11, 12, 14, 15, 16, 16]]], [['AC', BEATS], ['RC', BEATS], ['CC', [1]]]],
      edits: [[1, "3'"], [5, "4"], [9, "7"]],
      sound: Object.assign({}, DARK, { acid1: Object.assign({}, DARK.acid1, { cut: 0.22, open: 0.48, decay: 0.5, res: 0.84 }) }) },

    { id: 'tape', title: 'Бесконечная лента', style: 'в духе The Infinity Project', root: 'G', scale: 'phrygian', key: 'соль фригийский', bpm: 139,
      bass: ["- - 1 1 - - 1 1 - - 1 1 - - 1 1",
             "- - 1 1 - - 1 1' - - 1 1 - - 1 7,",
             "- - 2 2 - - 2 2 - - 2 2 - - 2 2",
             "- - 1 1' - - 1 1' - - 1 1' - - 3 2"],
      acid1: ["1 - 1 - 1 - 1a - 1 - 1 - 1 - 2 -",
              "1a 1 1 1' 1 1 2a 1 1 1' 1 1 3as 2 1 1'",
              "1 1'a 1 1 7 1 1 1'a 1 2s 3 1",
              "1a 1' 1 1' 2a 1' 1 1' 3a 1' 1 1' 2as 3 2 1'",
              "1 - - - 1' - - - 2as 1 - - 1' - - -"],
      acid2: ["- - 1' - - 1'a - - 1' - - 7 - - 1'a -",
              "5 5 - 5a 5 - 4 4 - 4a 4 - 3 3s 2 -",
              "1'a - 1' 2' - 1' 1'a - 7 1' - 5 6as 5 - 7"],
      lead: ["1 - - - - - 2 - 1 - - - - - 7, -",
             "1 - 1' - 5 - 1' - 2 - 2' - 6 - 2' -",
             "5 - - - - - - - 6 - - - 5 - - -"],
      arp: ["1 1 1' 1 5 5 1' 5 1 1 1' 1 6 6 1' 6", "1 5 1'"],
      kit: [[['OH', OFFBEATS], ['CP', [5, 13]], ['RS', [2, 8, 10, 16]]], [['CH', CLOSED]], [['AC', BEATS], ['CH', CLOSED], ['RC', BEATS]]],
      edits: [[2, "3'"], [5, "5"], [11, "4"]], sound: HYPNOTIC },

    { id: 'loop', title: 'Петля', style: 'в духе The Infinity Project', root: 'Bb', scale: 'minor', key: 'си-бемоль минор', bpm: 137,
      bass: ["1 - 1 1 1 - 1 1 1 - 1 1 1 - 1 1",
             "1 - 1 1 1 - 1 1' 1 - 1 1 1 - 7, 7,",
             "6, - 6, 6, 6, - 6, 6, 7, - 7, 7, 7, - 7, 7,",
             "1 - 1 1' 1 - 1 1' 1 - 1 1' 3 - 2 7,"],
      acid1: ["- - - 1 - - 1 - - - 1a - - 1 - -",
              "1 - 1a 1 - 1 3 - 1a 1 - 1 7, - 2as 1",
              "1a - 1' 1 - 7 1a - 5 1 - 3s 4 2",
              "1a 1 1' 1 3a 1 1' 1 4a 1 1' 1 5as 4 3 2",
              "1a - - - - - 5s 4 - - 3a - - - 2 -"],
      acid2: ["5 - - - 5a - - 4 5 - - - 7a - 5 -",
              "- 1' 1' - 7a 7 - 5 5a - 4 4 - 3as 2 -",
              "1' - 3'a - 2' - 1' 7a - 1' - 5 7as 1' - 2'"],
      lead: ["1 - 3 - - 2 - 1 7,a - 1 - - 3 - 2",
             "1 3 5 3 7 5 3 5 6, 1 3 1 7, 2 4 2",
             "3 - - - - 2 - - 1 - - - - 7, - -"],
      arp: ["1 5 3 5 1' 5 3 5 6, 3 1 3 6 3 1 3", "1 3 5 7 5"],
      kit: [[['SD', [5, 13]], ...HATS], [['CP', [5, 13]], ['LT', [16]]], [['AC', BEATS], ['RC', BEATS], ['CP', [5, 13]]]],
      edits: [[3, "3'"], [6, "5"], [9, "7"]],
      sound: Object.assign({}, HYPNOTIC, { acid1: Object.assign({}, HYPNOTIC.acid1, { env: 0.55, decay: 0.55, res: 0.8 }) }) },
  ].map(compose));

  // A song the set cannot play is a mistake in this file: say so at once.
  const SLOTS = [[T.BASS, 1], [T.BASS, 2], [T.BASS, 3], [T.BASS, 4], [T.ACID1, 1], [T.ACID1, 2], [T.ACID1, 3], [T.ACID1, 4],
                 [T.ACID1, 5], [T.ACID2, 2], [T.ACID2, 3], [T.ACID2, 4], [T.LEAD, 3], [T.LEAD, 4], [T.LEAD, 5], [T.ARP, 2], [T.ARP, 4]];
  for (const song of SONGS) {
    const bad = message => { throw new Error('artist: song "' + song.id + '": ' + message); };
    const find = (track, row) => song.synth.find(c => c.track === track && c.row === row);
    if (song.synth.length !== SLOTS.length) bad('wants ' + SLOTS.length + ' melodic clips, has ' + song.synth.length);
    for (const [track, row] of SLOTS) {
      const clip = find(track, row), where = TRACK_NAME[track] + ' row ' + row;
      if (!clip) bad('no clip for ' + where);
      const len = clip.len || 16;
      // Only these two are launched where a shorter loop is the point.
      if (clip.len && !(track === T.ACID1 && row === 3) && !(track === T.ARP && row === 4)) bad(where + ' must have 16 steps');
      const at = new Map(clip.notes.map(n => [n[0], n]));
      if (at.size !== clip.notes.length) bad(where + ' has a step twice');
      for (const [step, note, flags] of clip.notes) {
        if (step < 1 || step > len) bad(where + ' step ' + step + ' is outside the clip');
        const next = at.get(step % len + 1);
        if (flags.includes('s') && next && next[1] === note) bad(where + ' step ' + step + ' slides into its own pitch');
      }
      if (clip.from) {
        const source = find(track, clip.from).notes.map(n => n[0]).join();
        if (clip.notes.map(n => n[0]).join() !== source) bad(where + ' must use the steps of row ' + clip.from);
      }
    }
    const edited = find(T.ACID1, 3);
    if (song.edits.length !== 3) bad('wants three live edits');
    for (const [step0, note] of song.edits) {
      const now = edited.notes.find(n => n[0] === step0 + 1);
      if (!now) bad('live edit of step ' + (step0 + 1) + ', which is empty');
      // An edit is one short turn of the encoder between two beats.
      if (now[1] === note || Math.abs(now[1] - note) > 12) bad('live edit of step ' + (step0 + 1) + ' moves ' + (note - now[1]) + ' semitones');
    }
  }

  const loc = row => row - 1;                 // bank A: location = row index

  // What the artist believes the device now holds.
  class Model {
    constructor() {
      this.steps = {};      // "loc/track/step0" -> {note, active, accent, slide}
      this.kit = {};        // "loc/step0" -> [12 lane values] for the kit track
      this.len = {};        // "loc/track" -> length
      this.types = {};      // track -> 0 synth, 1 drum, 2 kit
      this.gate = {};
      this.last = {};       // track -> the pitch last entered (newer firmware)
    }
    // What a clip copy does: one track's steps, kit steps and length.
    copy(track, from, to, kit) {
      for (let st = 0; st < 16; st++) {
        const src = this.steps[from + '/' + track + '/' + st];
        if (src) this.steps[to + '/' + track + '/' + st] = Object.assign({}, src);
        else delete this.steps[to + '/' + track + '/' + st];
        if (kit) {
          const word = this.kit[from + '/' + st];
          if (word) this.kit[to + '/' + st] = word.slice(); else delete this.kit[to + '/' + st];
        }
      }
      if (this.len[from + '/' + track]) this.len[to + '/' + track] = this.len[from + '/' + track];
      else delete this.len[to + '/' + track];
    }
    step(location, track, step0) {
      const key = location + '/' + track + '/' + step0;
      return this.steps[key] || (this.steps[key] = { note: 0, active: false, accent: false, slide: false });
    }
    kitStep(location, step0) {
      const key = location + '/' + step0;
      return this.kit[key] || (this.kit[key] = new Array(12).fill(0));
    }
  }

  // What n fast clicks in a row are worth to a number (include/encoder.h).
  function burstWorth(n) {
    if (n <= 4) return n;
    if (n <= 7) return 4 + 4 * (n - 4);
    return 16 + 8 * (n - 7);
  }
  function burstFor(amount) {
    let n = 1;
    while (burstWorth(n + 1) <= amount) n++;
    return n;
  }

  class Script {
    constructor(seed, options) {
      // fw2: the firmware starts a new step at the track's last note and can
      // copy a clip (hold a pad, press another pad of the track).
      this.fw2 = !!(options && options.fw2);
      // keys: the firmware also reads a MIDI keyboard, and the artist has one:
      // a held step takes its pitch from a key instead of an encoder turn.
      this.keys = this.fw2 && !!(options && options.keys);
      // pads: while a step is held the pads are a four-octave keyboard.
      this.pads = this.fw2 && !!(options && options.pads);
      // song: an index into SONGS or a song's id.
      const pick = options && options.song;
      this.song = SONGS.find(x => x.id === pick) || SONGS[pick | 0] || SONGS[0];
      // form: the arrangement, by id; the song's own unless one is asked for.
      const want = options && options.form;
      this.form = FORMS.find(f => f.id === want) || FORMS.find(f => f.id === this.song.form);
      this.t = 0;                 // cursor, microseconds of device time
      this.actions = [];
      this.model = new Model();
      this.seed = seed >>> 0 || 1;
      this.lane = LANE.BD;        // the kit's remembered lane after power-up
    }
    rnd() {
      let x = this.seed;
      x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0;
      this.seed = x || 1;
      return x / 4294967296;
    }
    human(ms) { return ms * (1 + 0.25 * this.rnd()); }
    wait(ms) { this.t += Math.round(ms * 1000); }
    push(action) { action.t = this.t; this.actions.push(action); }
    say(kind, text) { this.push({ op: 'say', kind, text }); }
    rig(id, value, ramp) { this.push({ op: 'rig', id, value, ramp: ramp || 0 }); }

    down(ctl) { this.push(Object.assign({ down: true }, ctl)); }
    up(ctl) { this.push(Object.assign({ down: false }, ctl)); }
    tap(ctl, hold, gap) {
      this.down(ctl);
      this.wait(this.human(hold || 70));
      this.up(ctl);
      this.wait(this.human(gap || 90));
    }
    // A press held past the 500 ms long-press time.
    long(ctl) {
      this.down(ctl);
      this.wait(620);
      this.up(ctl);
      this.wait(this.human(140));
    }
    // One key of the MIDI keyboard, pressed and let go.
    key(note) {
      this.push({ op: 'key', note, velocity: 100, down: true });
      this.wait(this.human(90));
      this.push({ op: 'key', note, velocity: 0, down: false });
      this.wait(this.human(50));
    }
    click(enc, dir) {
      for (let e = 0; e < 4; e++) {
        this.push({ op: 'edge', enc, dir });
        this.wait(3);
      }
    }
    // A choice (a lane, a list row, a type): one per click, however fast.
    turnChoice(enc, detents) {
      const dir = detents > 0 ? 1 : -1;
      for (let i = 0; i < Math.abs(detents); i++) {
        this.click(enc, dir);
        this.wait(this.human(45));
      }
      this.wait(70);
    }
    // A number: bursts of fast clicks are worth more (x1, then x4, then x8).
    turnNumber(enc, delta) {
      const dir = delta > 0 ? 1 : -1;
      let left = Math.abs(delta);
      while (left > 0) {
        this.wait(110);             // a pause: the next click counts 1 again
        const n = burstFor(left);
        for (let i = 0; i < n; i++) {
          this.click(enc, dir);
          this.wait(13);            // 25 ms a click: under the 60 ms that makes it fast
        }
        left -= burstWorth(n);
      }
      this.wait(80);
    }
  }

  const PAD = (track, row) => ({ op: 'pad', track, row: row - 1 });
  const SCENE = row => ({ op: 'scene', row: row - 1 });
  const MUTE = track => ({ op: 'mute', track });
  const STEP = step => ({ op: 'step', index: step - 1 });
  const PLAY = { op: 'play' };
  const TRACK_SW = { op: 'encsw', enc: 0 };
  const VALUE_SW = { op: 'encsw', enc: 1 };
  const TRACK_ENC = 0, VALUE_ENC = 1;

  // ─── Programming, with the transport stopped ───────────────────

  function setTempo(s) {
    s.say('device', 'VALUE: темп 120 → ' + s.song.bpm + ' BPM (быстрый поворот ускоряет счёт: ×1, ×4, ×8)');
    s.turnNumber(VALUE_ENC, Math.round((s.song.bpm - 120) * 10));
    s.wait(300);
  }

  // Tracks 5-7 come up as DRUM; the selector's long press opens their config.
  function configureTracks(s) {
    s.say('device', 'VALUE долго → селектор; удержание кнопки шага 5 → настройка дорожки 5');
    s.long(VALUE_SW);
    s.long(STEP(5));
    for (const track of [T.ACID2, T.LEAD, T.ARP]) {
      s.say('device', 'Дорожка ' + (track + 1) + ' (' + TRACK_NAME[track] + '): TYPE DRUM → SYNTH');
      s.turnChoice(VALUE_ENC, -1);
      s.model.types[track] = 0;
      if (track === T.LEAD) {
        s.say('device', 'LEAD: GATE 50 → 80 %, чтобы ноты мелодии были длиннее');
        s.turnChoice(TRACK_ENC, 3);
        s.turnNumber(VALUE_ENC, 30);
        s.model.gate[track] = 80;
      }
      if (track !== T.ARP) s.tap(TRACK_SW, 80, 160);      // TRACK press: next track, first row
    }
    s.tap(VALUE_SW, 80, 200);                             // back to the selector
    s.long(VALUE_SW);                                     // back to the Session view
  }

  // Holds up to four steps with one hand and edits them with the other.
  function holdEdit(s, location, track, steps0, target, wantAccent, wantSlide) {
    const first = s.model.step(location, track, steps0[0]);
    const drum = s.model.types[track] === 1;
    const fresh = drum ? 0 : s.fw2 ? (s.model.last[track] === undefined ? 60 : s.model.last[track]) : 60;
    const current = first.note || fresh;
    const delta = target === null ? 0 : target - current;
    const flipAccent = first.accent !== wantAccent;
    const flipSlide = first.slide !== wantSlide;
    if (!delta && !flipAccent && !flipSlide) {
      // Nothing to turn: a plain tap switches the step on.
      for (const st of steps0) {
        if (!s.model.step(location, track, st).active) s.tap(STEP(st + 1), 60, 70);
      }
    } else {
      for (const st of steps0) { s.down(STEP(st + 1)); s.wait(30); }
      s.wait(80);
      // The pad keyboard's window is set by the lowest held step's note, or
      // the track's last note for a step that never had one.
      const pad = delta && s.pads && !drum ? padFor(target, current) : null;
      if (delta && s.keys && !drum) s.key(target);
      else if (pad) s.tap(PAD(pad.column, pad.row + 1), 70, 60);
      else if (delta) s.turnNumber(TRACK_ENC, delta);
      if (flipAccent) s.tap(TRACK_SW, 90, 90);
      if (flipSlide) s.long(TRACK_SW);
      for (const st of steps0) { s.up(STEP(st + 1)); s.wait(25); }
      s.wait(s.human(90));
    }
    for (const st of steps0) {
      const m = s.model.step(location, track, st);
      m.active = true;
      if (target !== null) m.note = target;
      m.accent = wantAccent;
      m.slide = wantSlide;
    }
    if (s.fw2 && delta && !drum) s.model.last[track] = target;
  }

  // Hold the source pad past the long-press time, press the target pad.
  function copyClip(s, track, fromRow, toRow) {
    s.say('device', 'Копия клипа: держать пэд ряда ' + fromRow + ' полсекунды и нажать пэд ряда ' + toRow +
      ' той же дорожки (' + TRACK_NAME[track] + ')');
    s.down(PAD(track, fromRow));
    s.wait(620);
    s.down(PAD(track, toRow));
    s.wait(s.human(70));
    s.up(PAD(track, toRow));
    s.wait(30);
    s.up(PAD(track, fromRow));
    s.wait(s.human(160));
    s.model.copy(track, loc(fromRow), loc(toRow), track === T.KIT);
  }

  // The pad that plays `note` on the keyboard the firmware shows while a step
  // whose reference note is `reference` is held, or null when the note is
  // outside its four octaves (the artist then turns the encoder).
  const WHITE = [0, 2, 4, 5, 7, 9, 11, 12], BLACK = [-1, 1, 3, -1, 6, 8, 10, -1];
  function padFor(note, reference) {
    const base = Math.min(72, Math.max(0, 12 * (Math.floor(reference / 12) - 1)));
    for (let row = 7; row >= 0; row--) {
      const octave = (7 - row) >> 1, semis = row & 1 ? WHITE : BLACK;
      for (let column = 0; column < 8; column++) {
        if (semis[column] >= 0 && base + 12 * octave + semis[column] === note) return { column, row };
      }
    }
    return null;
  }

  // Splits steps that get the same edit into handfuls of neighbouring buttons.
  function handfuls(steps0) {
    const out = [];
    let hand = [];
    for (const st of steps0) {
      if (hand.length === 4 || (hand.length && st - hand[0] > 6)) { out.push(hand); hand = []; }
      hand.push(st);
    }
    if (hand.length) out.push(hand);
    return out;
  }

  function setLength(s, clip) {
    if (!clip.len) return;
    s.say('device', 'VALUE долго → селектор; кнопка 11 укорачивает клип: 16 → ' + clip.len + ' шагов');
    s.long(VALUE_SW);
    for (let i = 16; i > clip.len; i--) s.tap(STEP(11), 60, 80);
    s.long(VALUE_SW);
    s.model.len[loc(clip.row) + '/' + clip.track] = clip.len;
  }

  function programSynth(s, clip) {
    const location = loc(clip.row);
    const copied = s.fw2 && clip.from;
    if (copied) {
      copyClip(s, clip.track, clip.from, clip.row);     // the focus is on the copy now
      s.say('device', TRACK_NAME[clip.track] + ', ряд ' + clip.row + ': ' + clip.name + ' — правятся только отличия');
    } else {
      s.say('device', TRACK_NAME[clip.track] + ', ряд ' + clip.row + ': ' + clip.name +
        ' — пэд, TRACK нажать (экран паттерна), затем держать шаги и крутить TRACK');
      s.tap(PAD(clip.track, clip.row), 70, 140);
    }
    setLength(s, clip);
    s.tap(TRACK_SW, 80, 180);                 // Pattern view on the focus
    // Steps that get the same edit from the same starting point go together.
    const groups = new Map();
    for (const [step, note, flags] of clip.notes) {
      const m = s.model.step(location, clip.track, step - 1);
      const accent = flags.includes('a'), slide = flags.includes('s');
      if (m.active && m.note === note && m.accent === accent && m.slide === slide) continue;
      const key = note + '/' + flags + '/' + m.note + '/' + m.accent + '/' + m.slide + '/' + m.active;
      if (!groups.has(key)) groups.set(key, { note, accent, slide, steps: [] });
      groups.get(key).steps.push(step - 1);
    }
    for (const g of groups.values()) {
      for (const hand of handfuls(g.steps)) {
        holdEdit(s, location, clip.track, hand, g.note, g.accent, g.slide);
      }
    }
    s.long(TRACK_SW);                         // back to the Session view
  }

  function programDrum(s, clip) {
    const location = loc(clip.row);
    if (s.fw2 && clip.from) {
      copyClip(s, clip.track, clip.from, clip.row);
      return;
    }
    s.say('device', TRACK_NAME[clip.track] + ', ряд ' + clip.row + ': ' + clip.name +
      ' — пэд, затем кнопки шагов');
    s.tap(PAD(clip.track, clip.row), 70, 140);
    const accented = clip.steps.filter(x => x[1].includes('a'));
    for (const [step] of clip.steps.filter(x => !x[1].includes('a'))) {
      s.tap(STEP(step), 60, 80);
      const m = s.model.step(location, clip.track, step - 1);
      m.active = true;
    }
    if (accented.length) {
      s.say('device', 'Акцент на барабанной дорожке ставится на экране паттерна: держать шаг и нажать TRACK');
      s.tap(TRACK_SW, 80, 180);
      for (const [step] of accented) holdEdit(s, location, clip.track, [step - 1], null, true, false);
      s.long(TRACK_SW);
    }
  }

  function programKit(s, clip) {
    const location = loc(clip.row);
    s.say('device', '909, ряд ' + clip.row + ': ' + clip.name +
      ' — TRACK идёт по дорожкам кита, кнопки шагов ставят удары');
    const copied = s.fw2 && clip.from;
    if (copied) copyClip(s, T.KIT, clip.from, clip.row);
    else s.tap(PAD(T.KIT, clip.row), 70, 140);
    for (const [lane, steps] of copied ? clip.added : clip.lanes) {
      s.turnChoice(TRACK_ENC, lane - s.lane);
      s.lane = lane;
      s.say('device', 'Дорожка кита ' + LANE_NAME[lane]);
      for (const step of steps) {
        s.tap(STEP(step), 55, 65);
        const word = s.model.kitStep(location, step - 1);
        if (lane === LANE.AC) word[lane] = word[lane] ? 0 : 1;
        else word[lane] = word[lane] === 0 ? 1 : word[lane] === 1 ? 2 : 0;
        if (lane === LANE.CH && word[lane]) word[LANE.OH] = 0;
        if (lane === LANE.OH && word[lane]) word[LANE.CH] = 0;
      }
    }
  }

  function saveToFlash(s) {
    s.say('device', 'VALUE долго → селектор; кнопка 9 сохраняет всё во флеш (прибор замирает на время записи)');
    s.long(VALUE_SW);
    s.tap(STEP(9), 70, 80);
    s.wait(2600);
    s.long(VALUE_SW);
  }

  function prepare(s) {
    const song = s.song, snd = song.sound;
    s.say('info', 'ПОДГОТОВКА к песне «' + song.title + '» (' + song.style + '). Прибор только что включён: в памяти заводская бочка и хэты. ' +
      (s.pads ? 'Пока удержан шаг, пэды становятся клавиатурой на четыре октавы: высоту нот артист берёт с них.'
        : s.keys ? 'К входу MIDI подключена клавиатура: высоту нот артист берёт с неё, держа шаг.'
        : s.fw2 ? 'Клипы копируются удержанием пэда, новый шаг берёт последнюю ноту дорожки.'
        : 'Копирования клипов на приборе нет, поэтому каждый клип набирается руками.'));
    s.say('rig', 'На синтезаторах: звук этой песни; фильтры кислот прикрыты, чтобы было что открывать');
    for (const id of ['acid1', 'acid2']) {
      s.rig(id + '.cutoff', snd[id].cut);
      s.rig(id + '.resonance', snd[id].res);
      s.rig(id + '.envmod', snd[id].env);
      s.rig(id + '.decay', snd[id].decay);
      s.rig(id + '.accent', snd[id].acc);
    }
    s.rig('lead.cutoff', snd.lead.cut);
    s.rig('delay.feedback', snd.delay);
    s.rig('reverb.level', snd.reverb);
    s.rig('fx.riserBars', 8);
    s.wait(400);
    s.model.types = { 0: 1, 1: 2, 2: 0, 3: 0, 4: 1, 5: 1, 6: 1, 7: 1 };
    for (const st of [0, 4, 8, 12]) Object.assign(s.model.step(0, T.KICK, st), { active: true, note: 36 });   // the factory kick
    setTempo(s);
    configureTracks(s);
    for (const clip of song.synth.filter(c => c.track === T.BASS)) programSynth(s, clip);
    for (const clip of song.drum.filter(c => c.track === T.KICK)) programDrum(s, clip);
    for (const clip of song.kit) programKit(s, clip);
    for (const clip of song.synth.filter(c => c.track !== T.BASS)) programSynth(s, clip);
    for (const clip of song.drum.filter(c => c.track === T.FX)) programDrum(s, clip);
    saveToFlash(s);

    const start = s.form.start;
    s.say('device', 'Исходная расстановка для аранжировки «' + s.form.title + '»: ' +
      start.rows.map((row, track) => TRACK_NAME[track] + ' — ряд ' + row).join(', ') + ' (ряд 8 пуст)');
    for (let track = 0; track < 8; track++) s.tap(PAD(track, start.rows[track]), 70, 110);
    s.say('device', 'MUTE на дорожках ' + start.muted.map(track => TRACK_NAME[track]).join(', ') + ': ' + start.text);
    for (const track of start.muted) s.tap(MUTE(track), 70, 110);
    s.wait(600);
  }

  // ─── The live set ──────────────────────────────────────────────

  const START_LATENCY_US = 23000;                       // PLAY debounce (20 ms) plus the next pass
  const barUs = bpm => Math.floor(60000000 / (bpm * 96)) * 24 * 16;   // from the firmware's integer timer period

  // What a form (an arrangement) plays with: the artist's moves on the device
  // and on the synths, and the notes of what the check should find afterwards.
  // Bars and beats count from 1. A form must have a note on the first step of
  // bar 1, press at most two pads after launchFor (four after launchEarly) and
  // end with stop().
  function stage(s, form) {
    const song = s.song, snd = song.sound;
    const BAR_US = barUs(song.bpm), STEP_US = BAR_US / 16;
    const playAt = s.t, t0 = playAt + START_LATENCY_US;
    const out = { playAt, t0, barUs: BAR_US, expected: [], heard: [], silent: [], fills: [], edited: null };
    const state = Object.assign({}, form.start.rows);    // row per track
    let muted = form.start.muted.reduce((bits, track) => bits | (1 << track), 0);
    const st = { s, song, snd };

    // Moves the cursor to a moment in the set.
    st.at = (bar, beat, offsetMs) => {
      const t = t0 + Math.round(((bar - 1) * 16 + (beat - 1) * 4) * STEP_US + (offsetMs || 0) * 1000);
      if (t < s.t) throw new Error('artist: ' + form.id + ': bar ' + bar + ' beat ' + beat + ' is in the past by ' + (s.t - t) / 1000 + ' ms');
      s.t = t;
    };
    // A launch pressed in the bar before `bar`, a little after its third beat
    // (launchEarly: its second, when more than two pads have to go down).
    st.launchFor = bar => st.at(bar - 1, 3, 40 + 120 * s.rnd());
    st.launchEarly = bar => st.at(bar - 1, 2, 40 + 120 * s.rnd());
    // A mute hit just ahead of the downbeat of `bar`.
    st.onDownbeat = bar => st.at(bar, 1, -70 - 20 * s.rnd());
    st.sweepBars = bars => bars * BAR_US / 1e6;
    st.clipName = (track, row) => track === T.KIT ? song.kit.find(c => c.row === row).name
      : song.synth.find(c => c.track === track && c.row === row).name;

    st.pad = (track, row, why) => {
      s.say('device', 'Пэд ' + TRACK_NAME[track] + ', ряд ' + row + ': ' + (why || st.clipName(track, row)));
      s.tap(PAD(track, row), 80, 120);
      state[track] = row;
    };
    st.scene = (row, why) => {
      s.say('device', 'SCENE ' + row + ': ' + why);
      s.tap(SCENE(row), 80, 120);
      for (let track = 0; track < 8; track++) state[track] = row;
    };
    st.mute = (track, why) => {
      s.say('device', 'MUTE ' + TRACK_NAME[track] + ': ' + why);
      s.tap(MUTE(track), 80, 100);
      muted ^= 1 << track;
    };
    // Two MUTE keys with two fingers, so both land ahead of the downbeat.
    st.mutePair = (a, b, why) => {
      s.say('device', 'MUTE ' + TRACK_NAME[a] + ' и ' + TRACK_NAME[b] + ' двумя пальцами: ' + why);
      s.down(MUTE(a)); s.wait(12); s.down(MUTE(b));
      s.wait(s.human(80));
      s.up(MUTE(a)); s.wait(10); s.up(MUTE(b));
      s.wait(s.human(100));
      muted ^= (1 << a) | (1 << b);
    };
    st.rig = (text, moves) => {
      if (text) s.say('rig', text);
      for (const [id, value, bars] of moves) s.rig(id, value, bars ? st.sweepBars(bars) : 0);
    };

    // For the check: the rows and mutes in the middle of `bar`; a clip that
    // plays whole in `bar`; a track that is silent from `bar` on; the clip
    // whose live edits should be on the wire from `bar` on.
    st.snapshot = bar => out.expected.push({ t: t0 + (bar - 1) * BAR_US + BAR_US / 2, bar, map: Object.assign({ muted }, state) });
    // The same half a beat into `beat` of `bar`, for a change on the beat.
    // Like snapshot, it takes the rows as the script has them when it is called.
    st.snapshotAt = (bar, beat) => out.expected.push({ t: t0 + ((bar - 1) * 16 + (beat - 1) * 4 + 2) * STEP_US, bar, beat,
      map: Object.assign({ muted }, state) });
    st.heard = (bar, ...clips) => { for (const [track, row] of clips) out.heard.push({ bar, track, row }); };
    st.silent = (bar, ...tracks) => { for (const track of tracks) out.silent.push({ bar, track }); };

    // Three notes of ACID 1 row 3 changed while it plays, from the Pattern
    // view, between the beats of bars `bar` and `bar + 1`. The view is opened
    // before (see the forms) and closed here. Only the pitch moves: the step
    // keeps its accent and slide.
    st.liveEdits = bar => {
      const edit = ([step0, note]) => {
        const now = s.model.step(loc(3), T.ACID1, step0);
        holdEdit(s, loc(3), T.ACID1, [step0], note, now.accent, now.slide);
      };
      st.at(bar, 1, 60);
      s.say('device', 'Держит шаг ' + (song.edits[0][0] + 1) + ' и меняет его ноту: слышно со следующего круга');
      edit(song.edits[0]);
      st.at(bar, 3);
      s.say('device', 'Шаг ' + (song.edits[1][0] + 1) + ', затем шаг ' + (song.edits[2][0] + 1) + ': ещё две ноты');
      edit(song.edits[1]);
      st.at(bar + 1, 1, 60);
      edit(song.edits[2]);
      st.at(bar + 1, 3);
      s.long(TRACK_SW);                         // back to the Session view
      out.edited = { bar: bar + 2, track: T.ACID1, row: 3 };
    };
    // Opens the Pattern view on ACID 1's playing clip by pressing its pad: a
    // pad that already plays only takes the focus.
    st.focusAcid = (bar) => {
      st.at(bar, 2);
      s.say('device', 'Правка на ходу: пэд играющего клипа ACID 1 (фокус), TRACK нажать (экран паттерна)');
      s.tap(PAD(T.ACID1, 3), 80, 120);
      s.tap(TRACK_SW, 80, 200);
    };
    // Two more kick hits written into the playing kick clip, and taken out
    // two bars later.
    st.kickFill = (bar, row) => {
      st.at(bar, 2);
      s.say('device', 'Сбивка: пэд бочки (фокус), кнопки шагов 15 и 16 добавляют два удара в играющий клип');
      s.tap(PAD(T.KICK, row), 80, 120);
      s.tap(STEP(15), 60, 80);
      s.tap(STEP(16), 60, 80);
      out.fills.push({ bar });       // heard in bars bar and bar + 1, gone in bar + 2
      st.at(bar + 2, 2);
      s.say('device', 'Те же кнопки убирают сбивку');
      s.tap(STEP(15), 60, 80);
      s.tap(STEP(16), 60, 80);
    };
    // The arp switched on the beat for two bars, then the quantisation put back.
    st.arpOnBeats = bar => {
      st.at(bar - 1, 3);
      s.say('device', 'VALUE нажать: квантование запуска 1 BAR → 1 BEAT');
      s.tap(VALUE_SW, 80, 150);
      // Each pad starts its clip on the next beat: a snapshot half a beat
      // after that, before the next pad's beat, sees it playing.
      st.at(bar, 1, 80); st.pad(T.ARP, 2, 'смена арпеджио по долям');
      st.snapshotAt(bar, 3);
      st.at(bar, 3, 80); st.pad(T.ARP, 4, 'и обратно');
      st.snapshotAt(bar + 1, 1);
      st.at(bar + 1, 1, 80); st.pad(T.ARP, 2, 'ещё раз');
      st.snapshotAt(bar + 1, 3);
      st.at(bar + 1, 3, 80); st.pad(T.ARP, 4, 'и обратно');
      st.at(bar + 2, 1, 100);
      s.say('device', 'VALUE нажать три раза: NEXT STEP, TRACK LOOP и снова 1 BAR');
      for (let i = 0; i < 3; i++) s.tap(VALUE_SW, 80, 260);
    };
    // One crash: the FX clip for a single bar, then the track is parked.
    st.crash = bar => {
      st.launchFor(bar); st.pad(T.FX, 3, 'удар с тарелкой на один такт');
      st.launchFor(bar + 1); st.pad(T.FX, 8, 'пустой клип: дорожка снова молчит');
    };
    st.stop = bar => {
      st.onDownbeat(bar);
      s.say('device', 'PLAY: стоп');
      s.tap(PLAY, 80, 100);
      s.wait(1500);
      s.say('info', 'Конец сета.');
    };
    st.out = out;
    return st;
  }

  // The first set: from the kick alone to a peak, a break with a riser, the
  // return, and the tracks leaving one or two at a time.
  function playClassic(st) {
    const { s, snd, at, launchFor, onDownbeat, pad, mute, mutePair, scene, rig, snapshot, heard, silent, clipName } = st;
    snapshot(2);

    onDownbeat(5); mute(T.KIT, 'входят хэты и малый');
    onDownbeat(9); mute(T.BASS, 'входит бас');
    onDownbeat(13); mute(T.ACID1, 'входит кислота, пока глухо');
    at(13, 2);
    rig('На синтезаторе: срез фильтра ACID 1 медленно открывается 8 тактов. Прибор этого не умеет: CC он не шлёт.',
      [['acid1.cutoff', snd.acid1.open, 8]]);

    launchFor(17); pad(T.KIT, 2);
    snapshot(18);
    launchFor(21); pad(T.ACID1, 2, 'полная кислотная линия'); pad(T.BASS, 2);
    snapshot(22);
    heard(22, [T.BASS, 2], [T.ACID1, 2]);
    launchFor(25); pad(T.ACID2, 2, 'вторая кислота');
    at(25, 2);
    rig('На синтезаторе: резонанс ACID 1 выше', [['acid1.resonance', snd.acid1.resHi, 4]]);
    launchFor(29); pad(T.ARP, 2, 'арпеджио');
    snapshot(30);
    heard(30, [T.ACID2, 2], [T.ARP, 2]);

    launchFor(33); pad(T.ACID1, 3, clipName(T.ACID1, 3) + ': рисунок смещается каждый такт');
    pad(T.KIT, 3);
    snapshot(34);
    launchFor(37); pad(T.LEAD, 3, 'мелодия');
    at(37, 2);
    rig('На синтезаторе: фильтр LEAD открывается', [['lead.cutoff', snd.lead.open, 4]]);
    launchFor(39); pad(T.BASS, 3);
    snapshot(40);
    launchFor(41); pad(T.BASS, 2, 'бас возвращается');

    // Live editing of the clip that is playing.
    at(41, 2);
    s.say('device', 'Правка на ходу: TRACK повернуть (фокус на ACID 1), TRACK нажать (экран паттерна)');
    s.turnChoice(TRACK_ENC, 1);               // focus: BASS -> ACID 1, on the clip it plays
    s.tap(TRACK_SW, 80, 200);
    st.liveEdits(42);
    launchFor(45); pad(T.ACID2, 3, 'вторая кислота, спуск');
    snapshot(46);

    // The drop.
    onDownbeat(47); mute(T.KICK, 'бочка уходит на два такта');
    snapshot(47); snapshot(48);
    at(47, 2);
    rig('На синтезаторе: срез ACID 1 вверх до упора за два такта', [['acid1.cutoff', snd.acid1.top, 1.8]]);
    launchFor(49); scene(4, 'весь ряд 4 на всех дорожках со следующего такта');
    onDownbeat(49); mute(T.KICK, 'бочка возвращается на сильную долю: пик');
    at(49, 2);
    rig('', [['acid1.cutoff', snd.acid1.peak, 2]]);
    snapshot(50);
    heard(50, [T.BASS, 4], [T.ACID1, 4], [T.ACID2, 4], [T.LEAD, 4]);

    // One crash: launch the FX clip for a single bar, then park the track.
    launchFor(53); pad(T.FX, 3, 'удар с тарелкой на один такт');
    snapshot(53);
    launchFor(54); pad(T.FX, 8, 'пустой клип: дорожка снова молчит');
    snapshot(54);
    st.kickFill(55, 4);
    st.arpOnBeats(58);
    snapshot(62);

    at(61, 2);
    rig('На синтезаторах: больше обратной связи в задержке, фильтр кислоты прикрывается к брейку',
      [['delay.feedback', snd.delayHi, 3], ['acid1.cutoff', snd.acid1.dip, 3.5]]);

    // The break.
    launchFor(65); scene(5, 'брейк. На ряду 5 у бочки, хэтов и баса пусто, они замолкают');
    at(65, 2);
    rig('На синтезаторах: больше реверберации; срез ACID 1 ползёт вверх 8 тактов вместе с райзером',
      [['reverb.level', snd.reverbHi, 1], ['acid1.cutoff', snd.acid1.top - 0.02, 7.5], ['lead.cutoff', snd.lead.top, 7.5]]);
    at(65, 3); pad(T.FX, 8, 'райзер запущен один раз, дорожка FX снова на пустом клипе');
    snapshot(67);
    heard(68, [T.LEAD, 5], [T.ACID1, 5]);

    launchFor(73); scene(4, 'возврат');
    at(73, 1, 150);
    rig('На синтезаторах: реверберация и задержка обратно',
      [['reverb.level', snd.reverb, 0.5], ['delay.feedback', snd.delay, 1], ['acid1.cutoff', snd.acid1.back, 1]]);
    snapshot(74);
    launchFor(77); pad(T.FX, 3, 'удар с тарелкой');
    launchFor(78); pad(T.FX, 8, 'пустой клип');
    launchFor(81); pad(T.ACID1, 2, 'кислотная линия A вместо плотной');
    pad(T.ARP, 2, 'длинное арпеджио');
    snapshot(82);
    at(81, 2);
    rig('На синтезаторе: фильтр ACID 2 открывается', [['acid2.cutoff', snd.acid2.open, 4]]);

    // Outro: tracks leave one or two at a time.
    onDownbeat(89); mutePair(T.LEAD, T.ARP, 'мелодия и арпеджио уходят');
    silent(89, T.LEAD, T.ARP);
    at(89, 2);
    rig('', [['acid1.cutoff', snd.acid1.out, 8], ['acid2.cutoff', snd.acid2.out, 8]]);
    onDownbeat(93); mute(T.ACID2, 'вторая кислота уходит');
    silent(93, T.ACID2);
    onDownbeat(97); mutePair(T.ACID1, T.BASS, 'кислота и бас уходят');
    silent(97, T.ACID1, T.BASS);
    onDownbeat(101); mute(T.KIT, 'хэты уходят, остаётся бочка');
    silent(101, T.KIT);
    st.stop(105);
  }

  // Dawn: the melody alone in reverb, a short riser, then the kick and the
  // bass together. The lead carries the set (two clips taken in turn make a
  // phrase of four bars), and it is the last to leave.
  function playSunrise(st) {
    const { s, snd, at, launchFor, launchEarly, onDownbeat, pad, mute, mutePair, scene, rig, snapshot, heard, silent } = st;
    at(1, 2);
    rig('На синтезаторах: мелодия одна, в большой реверберации; её фильтр открывается 8 тактов',
      [['reverb.level', snd.reverbHi, 0], ['lead.cutoff', snd.lead.open, 8], ['fx.riserBars', 4, 0]]);
    snapshot(2);
    heard(2, [T.LEAD, 5]);
    launchFor(5); pad(T.FX, 5, 'райзер на четыре такта');
    onDownbeat(5); mute(T.ACID1, 'кислота намёком, фильтр закрыт');
    at(5, 3); pad(T.FX, 8, 'райзер запущен один раз');

    launchFor(9); pad(T.LEAD, 8, 'мелодия уступает место ритму'); pad(T.KIT, 2, 'хэты готовы, пока под MUTE');
    onDownbeat(9); mutePair(T.KICK, T.BASS, 'бочка и бас входят вместе');
    at(9, 2);
    rig('На синтезаторах: реверберация обратно', [['reverb.level', snd.reverb, 1], ['fx.riserBars', 8, 0]]);
    snapshot(10);
    onDownbeat(13); mute(T.KIT, 'входят хэты');
    at(13, 2);
    rig('На синтезаторе: срез ACID 1 открывается 8 тактов', [['acid1.cutoff', snd.acid1.open, 8]]);

    launchFor(17); pad(T.ACID1, 2, 'полная кислотная линия'); pad(T.BASS, 2);
    snapshot(18);
    heard(18, [T.BASS, 2], [T.ACID1, 2]);
    launchFor(21); pad(T.ARP, 2, 'арпеджио');
    heard(22, [T.ARP, 2]);

    // The melody: A A B B, twice.
    launchFor(25); pad(T.LEAD, 3, 'мелодия A на два такта');
    launchFor(27); pad(T.LEAD, 4, 'мелодия B: вместе с A это фраза в четыре такта');
    launchFor(29); pad(T.LEAD, 3, 'снова A');
    launchFor(31); pad(T.LEAD, 4, 'снова B');
    heard(26, [T.LEAD, 3]);
    heard(28, [T.LEAD, 4]);
    snapshot(32);
    launchFor(33); pad(T.LEAD, 3, 'мелодия A остаётся'); pad(T.ACID2, 2, 'вторая кислота отвечает');
    launchFor(35); pad(T.KIT, 3);
    launchFor(37); pad(T.BASS, 3);
    launchFor(39); pad(T.BASS, 2, 'бас возвращается');
    launchFor(41); pad(T.ACID1, 3, st.clipName(T.ACID1, 3) + ': рисунок смещается каждый такт');
    snapshot(42);
    st.focusAcid(42);
    st.liveEdits(43);

    // The long break.
    at(45, 2);
    rig('На синтезаторах: больше обратной связи в задержке, фильтр кислоты прикрывается к брейку',
      [['delay.feedback', snd.delayHi, 3], ['acid1.cutoff', snd.acid1.dip, 3]]);
    launchFor(49); scene(5, 'брейк: остаются медленная мелодия, редкая кислота и райзер');
    at(49, 2);
    rig('На синтезаторах: больше реверберации; срезы ползут вверх 8 тактов вместе с райзером',
      [['reverb.level', snd.reverbHi, 1], ['acid1.cutoff', snd.acid1.top - 0.02, 7.5], ['lead.cutoff', snd.lead.top, 7.5]]);
    at(49, 3); pad(T.FX, 8, 'райзер запущен один раз');
    snapshot(51);
    heard(52, [T.LEAD, 5], [T.ACID1, 5]);

    launchFor(57); scene(4, 'пик: весь ряд 4');
    at(57, 1, 150);
    rig('На синтезаторах: реверберация и задержка обратно',
      [['reverb.level', snd.reverb, 0.5], ['delay.feedback', snd.delay, 1], ['acid1.cutoff', snd.acid1.back, 1]]);
    snapshot(58);
    heard(58, [T.BASS, 4], [T.ACID1, 4], [T.ACID2, 4], [T.LEAD, 4]);
    st.crash(61);
    launchFor(65); pad(T.LEAD, 3, 'мелодия A'); pad(T.ARP, 2, 'длинное арпеджио');
    launchFor(67); pad(T.LEAD, 4, 'мелодия B');
    launchFor(69); pad(T.LEAD, 3, 'мелодия A');
    launchFor(71); pad(T.LEAD, 4, 'мелодия B');
    snapshot(72);
    launchFor(73); pad(T.ACID1, 2, 'кислотная линия A вместо плотной'); pad(T.ACID2, 3);
    at(73, 2);
    rig('На синтезаторе: фильтр ACID 2 открывается', [['acid2.cutoff', snd.acid2.open, 4]]);
    heard(74, [T.ACID1, 2], [T.ACID2, 3]);

    // Outro: the rhythm leaves first, the melody last.
    launchFor(81); pad(T.LEAD, 5, 'медленная мелодия, как в начале');
    onDownbeat(81); mutePair(T.KICK, T.BASS, 'бочка и бас уходят первыми');
    silent(81, T.KICK, T.BASS);
    at(81, 2);
    rig('На синтезаторах: реверберация растёт, кислоты закрываются',
      [['reverb.level', snd.reverbHi, 8], ['acid1.cutoff', snd.acid1.out, 6], ['acid2.cutoff', snd.acid2.out, 6]]);
    snapshot(82);
    onDownbeat(85); mutePair(T.ACID1, T.ACID2, 'обе кислоты уходят');
    silent(85, T.ACID1, T.ACID2);
    onDownbeat(89); mutePair(T.KIT, T.ARP, 'хэты и арпеджио уходят: остаётся одна мелодия');
    silent(89, T.KIT, T.ARP);
    heard(90, [T.LEAD, 5]);
    st.stop(93);
    s.rig('reverb.level', snd.reverb);
  }

  // Two peaks: the kick and the bass from the first bar, a short break after
  // 28 bars, a first peak, then a long break and a second, fuller peak.
  function playTwoPeaks(st) {
    const { s, snd, at, launchFor, launchEarly, onDownbeat, pad, mute, mutePair, scene, rig, snapshot, heard, silent } = st;
    snapshot(2);
    onDownbeat(5); mute(T.KIT, 'входят хэты');
    onDownbeat(9); mute(T.ACID1, 'входит кислота, пока глухо');
    at(9, 2);
    rig('На синтезаторе: срез фильтра ACID 1 открывается 6 тактов', [['acid1.cutoff', snd.acid1.open, 6], ['fx.riserBars', 4, 0]]);
    launchFor(13); pad(T.KIT, 2); pad(T.ACID1, 2, 'полная кислотная линия');
    snapshot(14);
    heard(14, [T.ACID1, 2]);
    launchFor(17); pad(T.ACID2, 2, 'вторая кислота'); pad(T.BASS, 2);
    heard(18, [T.BASS, 2], [T.ACID2, 2]);
    launchFor(21); pad(T.ARP, 2, 'арпеджио');
    launchFor(23); pad(T.ACID1, 4, 'плотная линия на два такта');
    launchFor(25); pad(T.ACID1, 2, 'и обратно: две линии по очереди дают фразу длиннее 16 шагов');
    at(25, 2);
    rig('На синтезаторе: резонанс ACID 1 выше', [['acid1.resonance', snd.acid1.resHi, 4]]);
    snapshot(26);
    heard(24, [T.ACID1, 4]);

    // The first, short break.
    launchEarly(29); pad(T.KIT, 8, 'хэты замолкают'); pad(T.LEAD, 5, 'медленная мелодия'); pad(T.FX, 5, 'райзер на четыре такта');
    onDownbeat(29); mutePair(T.KICK, T.BASS, 'короткий брейк: бочка и бас уходят на четыре такта');
    at(29, 2);
    rig('На синтезаторах: срез ACID 1 вверх, больше реверберации',
      [['acid1.cutoff', snd.acid1.top, 3.5], ['reverb.level', snd.reverbHi, 1]]);
    at(29, 3); pad(T.FX, 8, 'райзер запущен один раз');
    snapshot(30);
    heard(31, [T.LEAD, 5]);

    // The first peak.
    launchEarly(33); pad(T.KIT, 3); pad(T.LEAD, 3, 'мелодия A'); pad(T.BASS, 4);
    onDownbeat(33); mutePair(T.KICK, T.BASS, 'первый пик: бочка и бас на сильную долю');
    at(33, 2);
    rig('На синтезаторах: реверберация обратно, срез на место',
      [['reverb.level', snd.reverb, 0.5], ['acid1.cutoff', snd.acid1.peak, 2], ['lead.cutoff', snd.lead.open, 4], ['fx.riserBars', 8, 0]]);
    snapshot(34);
    heard(34, [T.LEAD, 3], [T.BASS, 4]);
    launchFor(37); pad(T.ACID2, 3);
    launchFor(41); pad(T.ACID1, 3, st.clipName(T.ACID1, 3) + ': рисунок смещается каждый такт'); pad(T.BASS, 3);
    launchFor(43); pad(T.BASS, 2, 'бас возвращается');
    st.focusAcid(43);
    st.liveEdits(44);
    snapshot(47);
    launchFor(49); pad(T.LEAD, 4, 'мелодия B');
    launchFor(51); pad(T.LEAD, 3, 'мелодия A');
    heard(50, [T.LEAD, 4]);
    st.kickFill(53, 1);

    // The second, long break.
    at(57, 2);
    rig('На синтезаторах: больше обратной связи в задержке, фильтр кислоты прикрывается к брейку',
      [['delay.feedback', snd.delayHi, 3], ['acid1.cutoff', snd.acid1.dip, 3.5]]);
    launchFor(61); scene(5, 'второй брейк, длинный: ряд 5');
    at(61, 2);
    rig('На синтезаторах: больше реверберации; срезы ползут вверх 8 тактов вместе с райзером',
      [['reverb.level', snd.reverbHi, 1], ['acid1.cutoff', snd.acid1.top - 0.02, 7.5], ['lead.cutoff', snd.lead.top, 7.5]]);
    at(61, 3); pad(T.FX, 8, 'райзер запущен один раз');
    snapshot(63);
    heard(64, [T.LEAD, 5], [T.ACID1, 5]);

    // The second peak.
    launchFor(69); scene(4, 'второй пик: весь ряд 4');
    at(69, 1, 150);
    rig('На синтезаторах: реверберация и задержка обратно',
      [['reverb.level', snd.reverb, 0.5], ['delay.feedback', snd.delay, 1], ['acid1.cutoff', snd.acid1.back, 1]]);
    snapshot(70);
    heard(70, [T.BASS, 4], [T.ACID1, 4], [T.ACID2, 4], [T.LEAD, 4]);
    st.crash(73);
    st.arpOnBeats(78);
    snapshot(82);
    launchFor(85); pad(T.ACID1, 2, 'кислотная линия A вместо плотной'); pad(T.LEAD, 3, 'мелодия A');
    at(85, 2);
    rig('На синтезаторе: фильтр ACID 2 открывается', [['acid2.cutoff', snd.acid2.open, 4]]);
    heard(86, [T.ACID1, 2], [T.LEAD, 3]);

    // Outro.
    onDownbeat(89); mutePair(T.LEAD, T.ARP, 'мелодия и арпеджио уходят');
    silent(89, T.LEAD, T.ARP);
    at(89, 2);
    rig('', [['acid1.cutoff', snd.acid1.out, 8], ['acid2.cutoff', snd.acid2.out, 8]]);
    onDownbeat(93); mutePair(T.ACID2, T.KIT, 'вторая кислота и хэты уходят');
    silent(93, T.ACID2, T.KIT);
    onDownbeat(97); mute(T.ACID1, 'кислота уходит: остаются бочка и бас, как в начале');
    silent(97, T.ACID1);
    snapshot(98);
    st.stop(101);
  }

  // Hypnosis: no long break. The acid leads from the first bar, the kick
  // drops out for a bar now and then and once, with the bass, for two, and
  // the lines change slowly; a clip of 10 to 14 steps against 16 does the rest.
  function playHypnotic(st) {
    const { s, snd, at, launchFor, onDownbeat, pad, mute, mutePair, rig, snapshot, heard, silent } = st;
    at(1, 3);
    rig('На синтезаторе: срез ACID 1 открывается очень медленно, 16 тактов', [['acid1.cutoff', snd.acid1.open, 16]]);
    snapshot(2);
    onDownbeat(5); mute(T.BASS, 'входит бас');
    onDownbeat(9); mute(T.KIT, 'входят хэты');
    launchFor(13); pad(T.ACID1, 2, 'полная кислотная линия');
    snapshot(14);
    heard(14, [T.ACID1, 2]);
    onDownbeat(16); mute(T.KICK, 'бочка пропускает один такт');
    snapshot(16);
    launchFor(17); pad(T.KIT, 2);
    onDownbeat(17); mute(T.KICK, 'и возвращается');
    snapshot(18);
    launchFor(21); pad(T.ACID2, 2, 'вторая кислота');
    heard(22, [T.ACID2, 2]);
    launchFor(25); pad(T.ACID1, 3, st.clipName(T.ACID1, 3) + ': рисунок смещается каждый такт'); pad(T.BASS, 2);
    at(25, 2);
    rig('На синтезаторе: резонанс ACID 1 растёт 8 тактов', [['acid1.resonance', snd.acid1.resHi, 8]]);
    heard(26, [T.BASS, 2]);
    st.focusAcid(27);
    st.liveEdits(28);
    snapshot(31);
    onDownbeat(32); mute(T.KICK, 'бочка пропускает такт');
    snapshot(32);
    launchFor(33); pad(T.ARP, 4, st.clipName(T.ARP, 4)); pad(T.KIT, 3);
    onDownbeat(33); mute(T.KICK, 'и возвращается');
    launchFor(37); pad(T.ACID1, 4, 'плотная линия'); pad(T.ACID2, 3);
    snapshot(38);
    heard(38, [T.ACID1, 4], [T.ACID2, 3]);
    launchFor(41); pad(T.BASS, 3);
    launchFor(45); pad(T.BASS, 2, 'бас возвращается');

    // The one drop: two bars without the kick and the bass.
    onDownbeat(47); mutePair(T.KICK, T.BASS, 'бочка и бас уходят на два такта');
    snapshot(47); snapshot(48);
    at(47, 2);
    rig('На синтезаторе: срез ACID 1 вверх до упора за два такта', [['acid1.cutoff', snd.acid1.top, 1.8]]);
    launchFor(49); pad(T.LEAD, 3, 'мелодия, впервые за сет'); pad(T.KIT, 4);
    onDownbeat(49); mutePair(T.KICK, T.BASS, 'и возвращаются на сильную долю');
    at(49, 2);
    rig('На синтезаторах: срез на место, фильтр LEAD открывается',
      [['acid1.cutoff', snd.acid1.peak, 2], ['lead.cutoff', snd.lead.open, 4]]);
    snapshot(50);
    heard(50, [T.LEAD, 3]);
    launchFor(53); pad(T.ACID1, 2, 'кислотная линия A'); pad(T.BASS, 4);
    heard(54, [T.BASS, 4], [T.ACID1, 2]);
    st.crash(57);
    launchFor(59); pad(T.ACID1, 4, 'плотная линия на два такта');
    launchFor(61); pad(T.ACID1, 2, 'линия A');
    launchFor(63); pad(T.ACID1, 4, 'плотная');
    launchFor(65); pad(T.ACID1, 2, 'линия A'); pad(T.ARP, 2, 'длинное арпеджио');
    launchFor(67); pad(T.ACID2, 4);
    snapshot(68);
    heard(68, [T.ARP, 2], [T.ACID2, 4]);
    onDownbeat(72); mute(T.KICK, 'бочка пропускает такт');
    snapshot(72);
    launchFor(73); pad(T.LEAD, 4, 'мелодия B');
    onDownbeat(73); mute(T.KICK, 'и возвращается');
    at(73, 2);
    rig('На синтезаторе: больше обратной связи в задержке', [['delay.feedback', snd.delayHi, 4]]);
    heard(74, [T.LEAD, 4]);

    // Outro: back to the kick and the acid it began with.
    onDownbeat(81); mutePair(T.LEAD, T.ARP, 'мелодия и арпеджио уходят');
    silent(81, T.LEAD, T.ARP);
    at(81, 2);
    rig('На синтезаторах: фильтры закрываются 12 тактов, задержка обратно',
      [['acid1.cutoff', snd.acid1.out, 12], ['acid2.cutoff', snd.acid2.out, 4], ['delay.feedback', snd.delay, 2]]);
    onDownbeat(85); mute(T.ACID2, 'вторая кислота уходит');
    silent(85, T.ACID2);
    onDownbeat(89); mute(T.KIT, 'хэты уходят');
    silent(89, T.KIT);
    onDownbeat(93); mute(T.BASS, 'бас уходит: бочка и кислота, как в начале');
    silent(93, T.BASS);
    snapshot(94);
    st.stop(97);
  }

  // The forms. `start` is where the artist leaves the device before PLAY: the
  // row of each track (row 8 is empty in every song) and the muted tracks.
  const FORMS = [
    { id: 'classic', title: 'Классика', about: 'от одной бочки к пику, брейк с райзером, возврат, дорожки уходят по одной и парами',
      start: { rows: [1, 1, 1, 1, 8, 8, 8, 8], muted: [T.KIT, T.BASS, T.ACID1],
               text: 'сет начнётся с одной бочки' },
      play: playClassic },
    { id: 'sunrise', title: 'Рассвет', about: 'вступление без бочки: мелодия в реверберации, затем ритм; мелодия ведёт и уходит последней',
      start: { rows: [1, 1, 1, 1, 8, 5, 8, 8], muted: [T.KICK, T.KIT, T.BASS, T.ACID1],
               text: 'сет начнётся с одной медленной мелодии' },
      play: playSunrise },
    { id: 'twopeaks', title: 'Два пика', about: 'бочка и бас с первого такта, короткий брейк, первый пик, длинный брейк и второй пик',
      start: { rows: [1, 1, 1, 1, 8, 8, 8, 8], muted: [T.KIT, T.ACID1],
               text: 'сет начнётся с бочки и баса' },
      play: playTwoPeaks },
    { id: 'hypnotic', title: 'Гипноз', about: 'без долгого брейка: кислота с первого такта, бочка изредка пропускает такт, один провал на два такта без бочки и баса, линии меняются медленно',
      start: { rows: [1, 1, 1, 1, 8, 8, 8, 8], muted: [T.KIT, T.BASS],
               text: 'сет начнётся с бочки и глухой кислоты' },
      play: playHypnotic },
  ];
  // The form each song was written for.
  const SONG_FORM = { set: 'classic', garden: 'twopeaks', spiral: 'classic', mercury: 'twopeaks', transit: 'sunrise', beacon: 'sunrise',
                      desert: 'classic', penumbra: 'hypnotic', farside: 'twopeaks', tape: 'hypnotic', loop: 'hypnotic' };
  for (const song of SONGS) song.form = SONG_FORM[song.id];

  function perform(s) {
    const song = s.song, form = s.form;
    s.say('info', 'ЖИВОЙ СЕТ «' + song.title + '», ' + song.bpm + ' BPM, ' + song.key + '. Аранжировка «' + form.title + '»: ' +
      form.about + '. Запуски пэдов ждут границы такта (1 BAR).');
    const st = stage(s, form);
    s.say('device', 'PLAY');
    s.tap(PLAY, 80, 100);
    form.play(st);
    return st.out;
  }

  // The whole show as a sorted list of actions.
  function buildShow(seed, options) {
    const s = new Script(seed || 20261117, options);
    s.wait(600);
    prepare(s);
    const prepEnd = s.t;
    const prepModel = JSON.parse(JSON.stringify(s.model));   // before the live edits
    const live = perform(s);
    const actions = s.actions.map((a, i) => Object.assign(a, { i })).sort((a, b) => a.t - b.t || a.i - b.i);
    return {
      actions,
      prepEnd,
      playAt: live.playAt,
      t0: live.t0,
      end: s.t,
      barUs: live.barUs,
      bpm: s.song.bpm,
      song: s.song,
      form: s.form,
      model: s.model,
      prepModel,
      expectedPlaying: live.expected.slice().sort((a, b) => a.t - b.t),
      heard: live.heard,
      silent: live.silent,
      fills: live.fills,
      edited: live.edited,
      clips: { synth: s.song.synth, drum: s.song.drum, kit: s.song.kit },
    };
  }

  // Hands the whole show over: every finger movement is scheduled in the
  // device at its time, and what happens beside the device (the light under
  // the finger, captions, synth knobs) is queued in `hands` for `io`:
  // io.rig(id, value, rampSeconds), io.say(kind, text), io.touch(action).
  function schedule(show, device, hands, io) {
    for (const action of show.actions) {
      switch (action.op) {
        case 'pad': device.pad(action.track, action.row, action.down, action.t); break;
        case 'scene': device.scene(action.row, action.down, action.t); break;
        case 'mute': device.mute(action.track, action.down, action.t); break;
        case 'step': device.step(action.index, action.down, action.t); break;
        case 'play': device.play(action.down, action.t); break;
        case 'encsw': device.encoderSwitch(action.enc, action.down, action.t); break;
        case 'edge': device.encoderEdge(action.enc, action.dir, action.t); break;
        case 'key':
          device.midiIn(action.down ? [0x90, action.note, action.velocity] : [0x80, action.note, 0], action.t);
          break;
      }
      if (!io) continue;
      if (action.op === 'rig') { if (io.rig) hands.at(action.t, () => io.rig(action.id, action.value, action.ramp)); }
      else if (action.op === 'say') { if (io.say) hands.at(action.t, () => io.say(action.kind, action.text)); }
      else if (io.touch) hands.at(action.t, () => io.touch(action));
    }
  }

  const api = { buildShow, schedule, TRACK_NAME, LANE_NAME, SONGS, FORMS };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Artist = api;
})(typeof window !== 'undefined' ? window : globalThis);
