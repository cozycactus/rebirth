// The scripted Goa trance artist. It is a score of finger movements: every
// entry presses or releases a control, turns an encoder by one quadrature
// edge, or turns a knob on the synths (which are not part of the device). It
// never reads or writes the device's memory. It is open loop, like a rehearsed
// set: what it expects to have programmed is recorded in `expected`, and
// sim/test/artist_check.js compares that with the device afterwards.
(function (root) {
  'use strict';

  const BPM = 145;
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
    s.say('device', 'VALUE: темп 120 → ' + BPM + ' BPM (быстрый поворот ускоряет счёт: ×1, ×4, ×8)');
    s.turnNumber(VALUE_ENC, Math.round((BPM - 120) * 10));
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
      if (delta && s.keys && !drum) s.key(target);
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
    s.say('info', 'ПОДГОТОВКА. Прибор только что включён: в памяти заводская бочка и хэты. ' +
      (s.keys ? 'К входу MIDI подключена клавиатура: высоту нот артист берёт с неё, держа шаг.'
        : s.fw2 ? 'Клипы копируются удержанием пэда, новый шаг берёт последнюю ноту дорожки.'
        : 'Копирования клипов на приборе нет, поэтому каждый клип набирается руками.'));
    s.say('rig', 'На синтезаторах: фильтры кислот прикрыты, чтобы было что открывать');
    s.rig('acid1.cutoff', 0.22);
    s.rig('acid1.resonance', 0.72);
    s.rig('acid2.cutoff', 0.3);
    s.rig('lead.cutoff', 0.45);
    s.rig('delay.feedback', 0.45);
    s.rig('reverb.level', 0.3);
    s.wait(400);
    s.model.types = { 0: 1, 1: 2, 2: 0, 3: 0, 4: 1, 5: 1, 6: 1, 7: 1 };
    for (const st of [0, 4, 8, 12]) Object.assign(s.model.step(0, T.KICK, st), { active: true, note: 36 });   // the factory kick
    setTempo(s);
    configureTracks(s);
    for (const clip of SYNTH_CLIPS.filter(c => c.track === T.BASS)) programSynth(s, clip);
    for (const clip of DRUM_CLIPS.filter(c => c.track === T.KICK)) programDrum(s, clip);
    for (const clip of KIT_CLIPS) programKit(s, clip);
    for (const clip of SYNTH_CLIPS.filter(c => c.track !== T.BASS)) programSynth(s, clip);
    for (const clip of DRUM_CLIPS.filter(c => c.track === T.FX)) programDrum(s, clip);
    saveToFlash(s);

    s.say('device', 'Исходная расстановка: ряд 1 на бочке, хэтах, басу и кислоте; остальные дорожки на пустом ряду 8');
    for (const track of [T.KICK, T.KIT, T.BASS, T.ACID1]) s.tap(PAD(track, 1), 70, 110);
    for (const track of [T.ACID2, T.LEAD, T.ARP, T.FX]) s.tap(PAD(track, 8), 70, 110);
    s.say('device', 'MUTE на хэтах, басу и кислоте: сет начнётся с одной бочки');
    for (const track of [T.KIT, T.BASS, T.ACID1]) s.tap(MUTE(track), 70, 110);
    s.wait(600);
  }

  // ─── The live set ──────────────────────────────────────────────

  const PULSE_US = Math.floor(60000000 / (BPM * 96));   // the firmware's integer timer period
  const STEP_US = PULSE_US * 24;
  const BAR_US = STEP_US * 16;
  const BEAT_MS = STEP_US * 4 / 1000;
  const START_LATENCY_US = 23000;                       // PLAY debounce (20 ms) plus the next pass

  function perform(s) {
    s.say('info', 'ЖИВОЙ СЕТ, ' + BPM + ' BPM, ля фригийский доминантовый. Запуски пэдов ждут границы такта (1 BAR).');
    const playAt = s.t;
    const t0 = playAt + START_LATENCY_US;
    const expected = [];
    // Moves the cursor to a moment in the set; bars and beats count from 1.
    const at = (bar, beat, offsetMs) => {
      const t = t0 + Math.round(((bar - 1) * 16 + (beat - 1) * 4) * STEP_US + (offsetMs || 0) * 1000);
      if (t < s.t) throw new Error('artist: bar ' + bar + ' beat ' + beat + ' is in the past by ' + (s.t - t) / 1000 + ' ms');
      s.t = t;
    };
    // A launch pressed in the bar before `bar`, a little after its third beat.
    const launchFor = bar => at(bar - 1, 3, 40 + 120 * s.rnd());
    // A mute hit just ahead of the downbeat of `bar`.
    const onDownbeat = bar => at(bar, 1, -70 - 20 * s.rnd());
    const sweepBars = bars => bars * BAR_US / 1e6;
    const playing = (bar, map) => expected.push({ t: t0 + (bar - 1) * BAR_US + BAR_US / 2, bar, map });
    const state = { 0: 1, 1: 1, 2: 1, 3: 1, 4: 8, 5: 8, 6: 8, 7: 8 };   // row per track
    let muted = (1 << T.KIT) | (1 << T.BASS) | (1 << T.ACID1);
    const pad = (track, row, why) => {
      s.say('device', 'Пэд ' + TRACK_NAME[track] + ', ряд ' + row + ': ' + why);
      s.tap(PAD(track, row), 80, 120);
      state[track] = row;
    };
    const mute = (track, why) => {
      s.say('device', 'MUTE ' + TRACK_NAME[track] + ': ' + why);
      s.tap(MUTE(track), 80, 100);
      muted ^= 1 << track;
    };
    // Two MUTE keys with two fingers, so both land ahead of the downbeat.
    const mutePair = (a, b, why) => {
      s.say('device', 'MUTE ' + TRACK_NAME[a] + ' и ' + TRACK_NAME[b] + ' двумя пальцами: ' + why);
      s.down(MUTE(a)); s.wait(12); s.down(MUTE(b));
      s.wait(s.human(80));
      s.up(MUTE(a)); s.wait(10); s.up(MUTE(b));
      s.wait(s.human(100));
      muted ^= (1 << a) | (1 << b);
    };
    const snapshot = bar => playing(bar, Object.assign({ muted }, state));

    s.say('device', 'PLAY');
    s.tap(PLAY, 80, 100);
    snapshot(2);

    onDownbeat(5); mute(T.KIT, 'входят хэты и малый');
    onDownbeat(9); mute(T.BASS, 'входит бас');
    onDownbeat(13); mute(T.ACID1, 'входит кислота, пока глухо');
    at(13, 2);
    s.say('rig', 'На синтезаторе: срез фильтра ACID 1 медленно открывается 8 тактов. Прибор этого не умеет: CC он не шлёт.');
    s.rig('acid1.cutoff', 0.5, sweepBars(8));

    launchFor(17); pad(T.KIT, 2, 'открытый хэт на слабую долю и клэп');
    snapshot(18);
    launchFor(21); pad(T.ACID1, 2, 'полная кислотная линия'); pad(T.BASS, 2, 'бас с октавой');
    snapshot(22);
    launchFor(25); pad(T.ACID2, 2, 'вторая кислота');
    at(25, 2);
    s.say('rig', 'На синтезаторе: резонанс ACID 1 выше');
    s.rig('acid1.resonance', 0.88, sweepBars(4));
    launchFor(29); pad(T.ARP, 2, 'арпеджио');
    snapshot(30);

    launchFor(33); pad(T.ACID1, 3, '12 шагов против 16: рисунок смещается каждый такт');
    pad(T.KIT, 3, 'дробь малого в конце такта');
    snapshot(34);
    launchFor(37); pad(T.LEAD, 3, 'мелодия');
    at(37, 2);
    s.say('rig', 'На синтезаторе: фильтр LEAD открывается');
    s.rig('lead.cutoff', 0.75, sweepBars(4));
    launchFor(39); pad(T.BASS, 3, 'бас на полутон выше, напряжение');
    snapshot(40);
    launchFor(41); pad(T.BASS, 2, 'бас возвращается');

    // Live editing of the clip that is playing.
    at(41, 2);
    s.say('device', 'Правка на ходу: TRACK повернуть (фокус на ACID 1), TRACK нажать (экран паттерна)');
    s.turnChoice(TRACK_ENC, 1);               // focus: BASS -> ACID 1, on the clip it plays
    s.tap(TRACK_SW, 80, 200);
    at(42, 1, 60);
    s.say('device', 'Держит шаг 1 и крутит TRACK: нота ля на октаву выше, слышно со следующего круга');
    holdEdit(s, loc(3), T.ACID1, [0], N.A3 + 12, true, false);
    at(42, 3);
    s.say('device', 'Шаг 7, затем шаг 8: обе ноты вниз');
    holdEdit(s, loc(3), T.ACID1, [6], N.E3, false, false);
    at(43, 1, 60);
    holdEdit(s, loc(3), T.ACID1, [7], N.G3, false, false);
    at(43, 3);
    s.long(TRACK_SW);                         // back to the Session view
    launchFor(45); pad(T.ACID2, 3, 'вторая кислота, спуск');
    snapshot(46);

    // The drop.
    onDownbeat(47); mute(T.KICK, 'бочка уходит на два такта');
    at(47, 2);
    s.say('rig', 'На синтезаторе: срез ACID 1 вверх до упора за два такта');
    s.rig('acid1.cutoff', 0.92, sweepBars(1.8));
    launchFor(49);
    s.say('device', 'SCENE 4: весь ряд 4 на всех дорожках со следующего такта');
    s.tap(SCENE(4), 80, 120);
    for (let track = 0; track < 8; track++) state[track] = 4;
    onDownbeat(49); mute(T.KICK, 'бочка возвращается на сильную долю: пик');
    at(49, 2);
    s.rig('acid1.cutoff', 0.62, sweepBars(2));
    snapshot(50);

    // One crash: launch the FX clip for a single bar, then park the track.
    launchFor(53); pad(T.FX, 3, 'удар с тарелкой на один такт');
    snapshot(53);
    launchFor(54); pad(T.FX, 8, 'пустой клип: дорожка снова молчит');
    snapshot(54);

    // A fill written into the playing kick clip, then taken out.
    at(55, 2);
    s.say('device', 'Сбивка: пэд бочки (фокус), кнопки шагов 15 и 16 добавляют два удара в играющий клип');
    s.tap(PAD(T.KICK, 4), 80, 120);
    s.tap(STEP(15), 60, 80);
    s.tap(STEP(16), 60, 80);
    at(57, 2);
    s.say('device', 'Те же кнопки убирают сбивку');
    s.tap(STEP(15), 60, 80);
    s.tap(STEP(16), 60, 80);

    // Faster launches for a moment.
    at(57, 3);
    s.say('device', 'VALUE нажать: квантование запуска 1 BAR → 1 BEAT');
    s.tap(VALUE_SW, 80, 150);
    at(58, 1, 80); pad(T.ARP, 2, 'смена арпеджио по долям');
    at(58, 3, 80); pad(T.ARP, 4, 'и обратно');
    at(59, 1, 80); pad(T.ARP, 2, 'ещё раз');
    at(59, 3, 80); pad(T.ARP, 4, 'и обратно');
    at(60, 1, 100);
    s.say('device', 'VALUE нажать три раза: NEXT STEP, TRACK LOOP и снова 1 BAR');
    for (let i = 0; i < 3; i++) s.tap(VALUE_SW, 80, 260);
    snapshot(62);

    at(61, 2);
    s.say('rig', 'На синтезаторах: больше обратной связи в задержке, фильтр кислоты прикрывается к брейку');
    s.rig('delay.feedback', 0.68, sweepBars(3));
    s.rig('acid1.cutoff', 0.4, sweepBars(3.5));

    // The break.
    launchFor(65);
    s.say('device', 'SCENE 5: брейк. На ряду 5 у бочки, хэтов и баса пусто, они замолкают');
    s.tap(SCENE(5), 80, 120);
    for (let track = 0; track < 8; track++) state[track] = 5;
    at(65, 2);
    s.say('rig', 'На синтезаторах: больше реверберации; срез ACID 1 ползёт вверх 8 тактов вместе с райзером');
    s.rig('reverb.level', 0.6, sweepBars(1));
    s.rig('acid1.cutoff', 0.9, sweepBars(7.5));
    s.rig('lead.cutoff', 0.85, sweepBars(7.5));
    at(65, 3); pad(T.FX, 8, 'райзер запущен один раз, дорожка FX снова на пустом клипе');
    snapshot(67);

    launchFor(73);
    s.say('device', 'SCENE 4: возврат');
    s.tap(SCENE(4), 80, 120);
    for (let track = 0; track < 8; track++) state[track] = 4;
    at(73, 1, 150);
    s.say('rig', 'На синтезаторах: реверберация и задержка обратно');
    s.rig('reverb.level', 0.3, sweepBars(0.5));
    s.rig('delay.feedback', 0.45, sweepBars(1));
    s.rig('acid1.cutoff', 0.66, sweepBars(1));
    snapshot(74);
    launchFor(77); pad(T.FX, 3, 'удар с тарелкой');
    launchFor(78); pad(T.FX, 8, 'пустой клип');
    launchFor(81); pad(T.ACID1, 2, 'кислотная линия A вместо плотной');
    pad(T.ARP, 2, 'длинное арпеджио');
    snapshot(82);
    at(81, 2);
    s.say('rig', 'На синтезаторе: фильтр ACID 2 открывается');
    s.rig('acid2.cutoff', 0.6, sweepBars(4));

    // Outro: tracks leave one by one.
    onDownbeat(89); mutePair(T.LEAD, T.ARP, 'мелодия и арпеджио уходят');
    at(89, 2);
    s.rig('acid1.cutoff', 0.3, sweepBars(8));
    s.rig('acid2.cutoff', 0.25, sweepBars(8));
    onDownbeat(93); mute(T.ACID2, 'вторая кислота уходит');
    onDownbeat(97); mutePair(T.ACID1, T.BASS, 'кислота и бас уходят');
    onDownbeat(101); mute(T.KIT, 'хэты уходят, остаётся бочка');
    onDownbeat(105);
    s.say('device', 'PLAY: стоп');
    s.tap(PLAY, 80, 100);
    s.wait(1500);
    s.say('info', 'Конец сета.');
    return { playAt, t0, expected };
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
      barUs: BAR_US,
      bpm: BPM,
      model: s.model,
      prepModel,
      expectedPlaying: live.expected,
      clips: { synth: SYNTH_CLIPS, drum: DRUM_CLIPS, kit: KIT_CLIPS },
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

  const api = { buildShow, schedule, TRACK_NAME, LANE_NAME, BPM };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Artist = api;
})(typeof window !== 'undefined' ? window : globalThis);
