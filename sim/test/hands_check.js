// The hands' whole-show check, without a browser: every song in every
// arrangement in all four scores (base, fw2, keys, pads: 240 shows), and the
// eight Twisted songs performed with MUTE over their clips (posed in full,
// the whole show live, as it plays at speed 1), planned
// by handplan.js and posed by handrig.js's driver, against a table of
// touches built here from show.actions alone (never from the plan's own
// touches or turns):
//   1. every interval of a panel control has an assign entry, and its finger
//      is on it in the hand's keyframes from down to up; every encoder run
//      has a turn, pinched on its encoder;
//   2. the hand rules: a hold and the touches (and runs) inside it on
//      different hands; a clip copy one hand, index and thumb; a MUTE pair
//      one hand iff at most two tracks apart; a group one hand, its fingers
//      in order across the buttons from the hand's outer side;
//   3. at the down, the middle and 1 000 before the up of every interval,
//      its finger's contact point within 1.5 mm of the target; at the first,
//      middle and last edge of every run, both pinch contacts within 2 mm
//      of the knob's rim, and the grip turned with the knob (see twisted);
//   4. at those moments nothing of either hand in a control (its clearance
//      vertices and its whole mesh), no vertex of either hand more than
//      0.5 mm under the plate, the driver's poses ok; and in the live set
//      every 20 000 between them, nothing of either hand in a control (its
//      clearance vertices) nor more than 0.5 mm under the plate;
//   5. at those moments in the live set the palms apart and the left wrist
//      left of the right one while neither hand is away; in the preparation
//      crossings and palm overlaps are counted, not failed;
//   6. peak speed (10 000 steps) under 4 m/s per hand in the live set, under
//      9 m/s in the preparation (only ever drawn at x8 or x16).
// (1) and (2) run on every show; (3)-(6) on song 0 and song 1 + formIndex in
// every arrangement and score (32 shows), or on every show with HANDS_FULL=1;
// the live set every 20 000 (in 4) on 4 of those, song 1 + formIndex in the
// formIndex-th score (four songs, forms and scores; an eighth of the samples,
// to keep the default run short), or on every show with HANDS_FULL=1.
// QMUTE=1 builds every score with quantMutes, as the page does when MUTE
// waits for the launch point (sim/check.sh passes it from the core).
//   node sim/test/hands_check.js
//   HANDS_FULL=1 node sim/test/hands_check.js
//   QMUTE=1 node sim/test/hands_check.js
'use strict';
const path = require('path');
const web = path.join(__dirname, '..', 'web');
require(path.join(web, 'three.js'));
const T = globalThis.THREE;
const HandRig = require(path.join(web, 'handrig.js'));
const HandPlan = require(path.join(web, 'handplan.js'));
const Artist = require(path.join(web, 'artist.js'));
const MidiSongs = require(path.join(web, 'midi_songs.js'));
const MidiClips = require(path.join(web, 'midi_clips.js'));
const { PANEL } = HandPlan, MM = PANEL.UNITS_PER_MM;

const SCORES = [['base', {}], ['fw2', { fw2: true }], ['keys', { fw2: true, keys: true }], ['pads', { fw2: true, pads: true }]];
const FULL = process.env.HANDS_FULL === '1';
const QMUTE = process.env.QMUTE === '1';
const RUN_GAP_US = 150000;      // an encoder's edges this far apart begin a new run
const GROUP_US = 100000;        // a group's downs are within this of its first
const GROUPED = ['pad', 'step', 'mute'];
const NEAR_TRACKS = 2;          // a MUTE pair this close is one hand
const CONTACT_MM = 1.5, RIM_MM = 2, KNOB_R_MM = 10, GRIP_MM = 12, UNDER_MM = 0.5;
const SPEED_STEP_US = 10000, LIVE_M_S = 4, PREP_M_S = 9;
const SIDES = ['left', 'right'], FINGERS = ['thumb', 'index', 'middle', 'ring', 'pinky'];
const OUTER = ['pinky', 'ring', 'middle', 'index'];   // a group's fingers from the hand's outer side in
const mm = x => x.toFixed(2);

const failures = [];
let where = null;   // the show being checked: {song, form, score}
const fail = (t, key, what, distMm) => failures.push(Object.assign({ t, key, what, distMm }, where));

// ─── The table: a show's touches, from its actions alone ─────────
// Intervals: a panel control's down paired with its up, by control key.
// Runs: an encoder's edges, split at gaps of 150 000 or more. Ids as the
// plan's assign keys them: intervals by down, then up; runs by first edge,
// then encoder. Gestures: clip copies (a pad held, a pad of its track 1 to
// 3 rows greater down while it is held), then groups (one kind, downs
// within 100 000 of the first, each overlapping those before it), the rest
// single; a group of two MUTE keys is a MUTE pair, and a pair more than two
// tracks apart is two singles. A hold is a gesture during which an interval
// or run outside it begins and ends.
function table(actions) {
  const intervals = [], open = new Map(), edges = [[], []];
  for (const a of actions.slice().sort((x, y) => x.t - y.t)) {
    const c = PANEL.control(a);
    if (!c) continue;
    if (a.op === 'edge') { edges[a.enc].push([a.t, a.dir]); continue; }
    if (a.down) {
      if (open.has(c.key)) fail(a.t, c.key, '(1) down again while down');
      open.set(c.key, { key: c.key, control: c, op: a.op, track: a.track, row: a.row, tDown: a.t });
    } else if (open.has(c.key)) {
      const x = open.get(c.key);
      x.tUp = a.t;
      intervals.push(x);
      open.delete(c.key);
    } else fail(a.t, c.key, '(1) up without a down');
  }
  for (const x of open.values()) fail(x.tDown, x.key, '(1) never up');
  intervals.sort((x, y) => x.tDown - y.tDown || x.tUp - y.tUp);
  intervals.forEach((x, id) => { x.id = id; });

  const runs = [];
  edges.forEach((list, enc) => {
    let run = null;
    for (const [t, dir] of list) {
      if (!run || t - run.edges[run.edges.length - 1] >= RUN_GAP_US) runs.push(run = { enc, key: 'knob:' + enc, edges: [], dirs: [] });
      run.edges.push(t);
      run.dirs.push(dir);
    }
  });
  runs.sort((x, y) => x.edges[0] - y.edges[0] || x.enc - y.enc);
  runs.forEach((r, id) => Object.assign(r, { id, tDown: r.edges[0], tUp: r.edges[r.edges.length - 1] }));

  const gestureOf = new Map(), gestures = [], copies = [], groups = [], pairs = [], extra = { secondCopy: 0, bigGroup: 0 };
  const gesture = (type, members) => {
    const g = { type, members, start: Math.min(...members.map(x => x.tDown)), end: Math.max(...members.map(x => x.tUp)) };
    for (const x of members) gestureOf.set(x, g);
    gestures.push(g);
    return g;
  };
  const copyOf = (p, q) => q.op === 'pad' && q.track === p.track && q.tDown > p.tDown && q.tDown < p.tUp &&
    q.row - p.row >= 1 && q.row - p.row <= 3;
  for (const p of intervals) {
    if (p.op !== 'pad' || gestureOf.has(p)) continue;
    for (let j = p.id + 1; j < intervals.length && intervals[j].tDown < p.tUp; j++) {
      const q = intervals[j];
      if (!gestureOf.has(q) && copyOf(p, q)) { copies.push(gesture('copy', [p, q])); break; }
    }
  }
  for (const f of intervals) {
    if (gestureOf.has(f)) continue;
    const members = [f];
    if (GROUPED.includes(f.control.kind)) {
      let until = f.tUp;
      for (let j = f.id + 1; j < intervals.length && intervals[j].tDown - f.tDown <= GROUP_US; j++) {
        const x = intervals[j];
        if (gestureOf.has(x) || x.control.kind !== f.control.kind || x.tDown >= until) continue;
        members.push(x);
        until = Math.min(until, x.tUp);
      }
    }
    if (members.length === 1) { gesture('single', members); continue; }
    if (members.length > 4) extra.bigGroup++;
    if (f.control.kind === 'mute' && members.length === 2) {
      const pair = { members, apart: Math.abs(members[0].track - members[1].track) };
      pairs.push(pair);
      if (pair.apart > NEAR_TRACKS) { gesture('single', [members[0]]); gesture('single', [members[1]]); continue; }
      gesture('pair', members);
    } else groups.push(gesture('group', members));
  }
  // A pad that would copy again from a pad already copied from.
  for (const g of copies) {
    const p = g.members[0];
    for (let j = p.id + 1; j < intervals.length && intervals[j].tDown < p.tUp; j++) {
      if (intervals[j] !== g.members[1] && copyOf(p, intervals[j])) extra.secondCopy++;
    }
  }

  // Holds and what begins and ends inside them.
  const holds = [];
  for (const g of gestures) {
    const inner = intervals.filter(x => gestureOf.get(x) !== g && x.tDown > g.start && x.tUp < g.end)
      .concat(runs.filter(r => r.tDown > g.start && r.tUp < g.end));
    if (inner.length) holds.push(Object.assign(g, { inner }));
  }
  let nested = 0, farInHold = 0;
  for (const h of holds) {
    nested += holds.filter(o => o !== h && o.start > h.start && o.end < h.end && o.members.every(x => h.inner.includes(x))).length;
    farInHold += pairs.filter(p => p.apart > NEAR_TRACKS && p.members.every(x => h.inner.includes(x))).length;
  }
  return { intervals, runs, copies, groups, pairs, holds, nested, farInHold, secondCopy: extra.secondCopy, bigGroup: extra.bigGroup };
}

// ─── (1) and (2): the plan's assignment against the table ────────
// The keyframes of a hand from t0 to t1 (binary search on t).
function framesIn(frames, t0, t1) {
  let lo = 0, hi = frames.length;
  while (lo < hi) {
    const m = (lo + hi) >> 1;
    if (frames[m].t < t0) lo = m + 1; else hi = m;
  }
  const out = [];
  for (let i = lo; i < frames.length && frames[i].t <= t1; i++) out.push(frames[i]);
  return out;
}
const sameAim = (aim, c) => aim && aim.u === c.u && aim.v === c.v && aim.topMm === c.topMm;

function assigned(plan, tab) {
  const keys = Object.keys(plan.assign);
  const touches = keys.filter(k => k.startsWith('touch:')).length, turns = keys.filter(k => k.startsWith('turn:')).length;
  if (touches !== tab.intervals.length) fail(0, '-', '(1) the plan assigns ' + touches + ' touches, the table has ' + tab.intervals.length);
  if (turns !== tab.runs.length) fail(0, '-', '(1) the plan assigns ' + turns + ' turns, the table has ' + tab.runs.length + ' runs');
  // Every interval: its finger on it, at lift 0, in every keyframe of its
  // hand from its down to its up, those two included.
  const hand = {};
  for (const x of tab.intervals) {
    const a = plan.assign['touch:' + x.id];
    if (!a || !SIDES.includes(a.hand) || !FINGERS.includes(a.finger)) {
      fail(x.tDown, x.key, '(1) no hand and finger: ' + JSON.stringify(a));
      continue;
    }
    hand[x.id] = a.hand;
    const frames = framesIn(plan.hands[a.hand], x.tDown, x.tUp);
    if (!frames.length || frames[0].t !== x.tDown || frames[frames.length - 1].t !== x.tUp) {
      fail(x.tDown, x.key, '(1) the ' + a.hand + ' hand has no keyframe at the down or the up');
    }
    for (const k of frames) {
      const s = k.fingers[a.finger];
      if (!s || s.liftMm !== 0 || !s.ref || s.ref.type !== 'touch' || s.ref.id !== x.id || !sameAim(s.aim, x.control)) {
        fail(k.t, x.key, '(1) the ' + a.hand + ' ' + a.finger + ' is not on it: ' + JSON.stringify(s));
        break;
      }
    }
  }
  // Every run: its hand pinching its knob with thumb and index from its
  // first edge to its last.
  const knobs = [0, 1].map(enc => PANEL.control({ op: 'encsw', enc }));
  for (const r of tab.runs) {
    const a = plan.assign['turn:' + r.id];
    if (!a || !SIDES.includes(a.hand)) {
      fail(r.tDown, r.key, '(1) no hand turns it: ' + JSON.stringify(a));
      continue;
    }
    r.hand = a.hand;
    const frames = framesIn(plan.hands[a.hand], r.tDown, r.tUp);
    if (!frames.length || frames[0].t !== r.tDown) fail(r.tDown, r.key, '(1) the ' + a.hand + ' hand has no keyframe at the first edge');
    for (const k of frames) {
      const bad = ['thumb', 'index'].find(f => {
        const s = k.fingers[f];
        return !s || s.liftMm !== 0 || !s.ref || s.ref.type !== 'turn' || s.ref.id !== r.id || !sameAim(s.aim, knobs[r.enc]);
      });
      if (bad) { fail(k.t, r.key, '(1) the ' + a.hand + ' ' + bad + ' does not pinch it: ' + JSON.stringify(k.fingers[bad])); break; }
    }
  }

  // (2) The hand rules.
  const handOf = x => (x.edges ? x.hand : hand[x.id]);
  for (const h of tab.holds) {
    const mine = new Set(h.members.map(handOf));
    for (const x of h.inner) {
      const theirs = handOf(x);
      if (theirs && mine.has(theirs)) {
        fail(x.tDown, x.key, '(2) inside the hold of ' + h.members.map(m => m.key).join(' ') + ' (' + h.start + '-' + h.end +
          '), on the same hand: ' + theirs);
      }
    }
  }
  for (const g of tab.copies) {
    const [p, q] = g.members, a = plan.assign['touch:' + p.id], b = plan.assign['touch:' + q.id];
    if (!a || !b || a.hand !== b.hand || a.finger !== 'index' || b.finger !== 'thumb') {
      fail(q.tDown, p.key + ' ' + q.key, '(2) a copy not one hand, index then thumb: ' + JSON.stringify([a, b]));
    }
  }
  for (const p of tab.pairs) {
    const [x, y] = p.members, one = hand[x.id] === hand[y.id];
    if (one !== (p.apart <= NEAR_TRACKS)) {
      fail(y.tDown, x.key + ' ' + y.key, '(2) a MUTE pair ' + p.apart + ' tracks apart on ' + (one ? 'one hand' : 'two hands'));
    }
  }
  // A group: one hand, its fingers in order across the buttons from the
  // hand's outer side (the left hand's little finger leftmost; mirrored for
  // the right): little, ring, middle, index, any of them left out.
  for (const g of tab.groups) {
    const as = g.members.map(x => plan.assign['touch:' + x.id]), keys = g.members.map(x => x.key).join(' ');
    if (as.some(a => !a) || new Set(as.map(a => a.hand)).size !== 1) {
      fail(g.start, keys, '(2) a group not on one hand: ' + JSON.stringify(as));
      continue;
    }
    const sign = as[0].hand === 'left' ? 1 : -1;
    const order = g.members.map((x, i) => ({ u: x.control.u, rank: OUTER.indexOf(as[i].finger) })).sort((a, b) => sign * (a.u - b.u));
    if (order.some((o, i) => o.rank < 0 || (i && o.rank <= order[i - 1].rank))) {
      fail(g.start, keys, '(2) a group\'s fingers not in order from the ' + as[0].hand + ' hand\'s outer side: ' +
        as.map(a => a.finger).join(' '));
    }
  }
}

// ─── (3)-(6): the driver's poses ─────────────────────────────────
const rigs = { left: HandRig.create('left'), right: HandRig.create('right') };
const everyVertex = {};
for (const side of SIDES) everyVertex[side] = Array.from({ length: rigs[side].mesh.geometry.attributes.position.count }, (_, v) => v);
const wrist = side => rigs[side].bone('Wrist').getWorldPosition(new T.Vector3());
const palmBox = side => new T.Box3().setFromArray(rigs[side].skinned(rigs[side].PALM));
// The lowest vertex (mm above the plate) of a hand's whole mesh, skinned
// now or as given.
const lowestMm = (side, p) => {
  p = p || rigs[side].skinned(everyVertex[side]);
  let low = Infinity;
  for (let k = 2; k < p.length; k += 3) low = Math.min(low, p[k]);
  return low / MM;
};

// (3) The grip turns with the knob: the line from the thumb's contact to
// the index's, seen from above, at the run's first edge, then at its middle
// and last edge turned (clockwise positive, as the knob turns up) at most
// TWIST_DEG + 0.5° from there, never against the run's direction, and by at
// least 0.5° in it unless the hand may have let go and come back to the
// middle by then: an earlier pause of REGRIP_US or more (a regrip), or the
// knob turned more than TWIST_DEG (the fingers slide on it).
const TWIST_DEG = 20, REGRIP_US = 100000, EDGE_DEG = 15 / 4;
function twisted(t, it, rig) {
  const a = rig.contact('thumb', it.axis), b = rig.contact('index', it.axis), r = it.x;
  const angle = Math.atan2(b.y - a.y, b.x - a.x) * 180 / Math.PI;
  if (it.edge === 0) { r.grip0 = angle; return; }
  const turned = ((r.grip0 - angle) % 360 + 540) % 360 - 180, net = Math.sign(r.dirs.reduce((s, d) => s + d, 0));
  const knob = EDGE_DEG * r.dirs.slice(0, it.edge + 1).reduce((s, d) => s + d, 0), along = turned * net;
  let regrip = false;
  for (let j = 1; j <= it.edge; j++) if (r.edges[j] - r.edges[j - 1] >= REGRIP_US) regrip = true;
  const at = ' at edge ' + it.edge + ' of ' + r.edges.length + ' (the knob ' + mm(knob) + '°)';
  if (Math.abs(turned) > TWIST_DEG + 0.5) fail(t, r.key, '(3) the ' + it.side + ' grip turned ' + mm(turned) + '° from the first edge' + at);
  else if (along < -0.5) fail(t, r.key, '(3) the ' + it.side + ' grip turned ' + mm(turned) + '° against the knob' + at);
  else if (along < 0.5 && !regrip && Math.abs(knob) <= TWIST_DEG) {
    fail(t, r.key, '(3) the ' + it.side + ' grip has not turned with the knob: ' + mm(turned) + '°' + at);
  }
}

// (4) Between the moments, the live set every TRAVEL_US (with the peak
// speed's poses, every other one): nothing of either hand in a control
// (the clearance vertices; the touching fingertips exempt as above) nor
// more than 0.5 mm under the plate (every vertex).
const TRAVEL_US = 20000;

function posed(show, plan, tab, stats, travelled) {
  const driver = new HandRig.Driver(rigs, plan);
  // The moments: each interval's down, middle and 1 000 before its up; each
  // run's first, middle and last edge.
  const moments = new Map();
  const add = (t, item) => {
    if (!moments.has(t)) moments.set(t, []);
    moments.get(t).push(item);
  };
  for (const x of tab.intervals) {
    const a = plan.assign['touch:' + x.id];
    if (!a) continue;
    const c = x.control, p = PANEL.onPanel(c.u, c.v, c.topMm), target = new T.Vector3(p.x, p.y, p.z);
    for (const t of [x.tDown, Math.round((x.tDown + x.tUp) / 2), Math.max(x.tDown, x.tUp - 1000)]) {
      add(t, { x, side: a.hand, finger: a.finger, target });
    }
  }
  for (const r of tab.runs) {
    if (!r.hand) continue;
    const c = PANEL.control({ op: 'encsw', enc: r.enc }), axis = { axis: new T.Vector3(c.u, -c.v, 0) };
    for (const i of new Set([0, r.edges.length >> 1, r.edges.length - 1])) add(r.edges[i], { x: r, side: r.hand, axis, c, edge: i });
  }
  let crossings = 0, overlaps = 0;
  for (const t of [...moments.keys()].sort((a, b) => a - b)) {
    const res = driver.pose(t), items = moments.get(t), key = items[0].x.key;
    for (const it of items) {
      const rig = rigs[it.side];
      if (it.target) {
        const d = rig.contact(it.finger).distanceTo(it.target) / MM;
        if (d > CONTACT_MM) fail(t, it.x.key, '(3) the ' + it.side + ' ' + it.finger + ' is off its target', d);
      } else {
        for (const f of ['thumb', 'index']) {
          const p = rig.contact(f, it.axis), d = Math.hypot(Math.hypot(p.x - it.c.u, p.y + it.c.v) / MM - KNOB_R_MM, p.z / MM - GRIP_MM);
          if (d > RIM_MM) fail(t, it.x.key, '(3) the ' + it.side + ' ' + f + ' is off the rim', d);
        }
        twisted(t, it, rig);
      }
    }
    for (const side of SIDES) {
      if (!res[side].ok) fail(t, key, '(4) the driver says the ' + side + ' pose is not ok');
      const left = rigs[side].violations(res[side].touching);
      if (left.length) fail(t, key, '(4) the ' + side + ' hand is in a control (' + left.length + ' vertices)', left[0].mm);
      // Every vertex of the mesh too, not only the clearance vertices (a
      // touching fingertip may still sink 1 mm into its own control).
      const mesh = rigs[side].skinned(everyVertex[side]), all = rigs[side].violations(res[side].touching, { mesh: true, skinned: mesh });
      if (all.length) fail(t, key, '(4) the ' + side + ' hand\'s mesh is in a control (vertex ' + all[0].vertex + ', ' + all.length + ' vertices)', all[0].mm);
      const low = lowestMm(side, mesh);
      if (low < -UNDER_MM) fail(t, key, '(4) the ' + side + ' hand is under the plate', -low);
    }
    const overlap = palmBox('left').intersectsBox(palmBox('right'));
    const crossed = !res.left.away && !res.right.away && wrist('left').x >= wrist('right').x;
    if (t >= show.prepEnd) {
      if (overlap) fail(t, key, '(5) the palms overlap');
      if (crossed) fail(t, key, '(5) the left wrist is not left of the right one');
    } else {
      overlaps += overlap;
      crossings += crossed;
    }
  }
  // (6) Peak speeds, the preparation from 0, the live set to its end (as
  // HandRig.peakSpeed measures them), with (4) between the moments there.
  const last = Math.max(show.end, ...SIDES.map(s => plan.hands[s][plan.hands[s].length - 1].t));
  const prep = HandRig.peakSpeed(driver, 0, show.prepEnd, SPEED_STEP_US);
  const live = { left: 0, right: 0 }, prev = {};
  let travel = 0;
  for (let t = show.prepEnd, k = 0; t <= last; t += SPEED_STEP_US, k++) {
    const res = driver.pose(t);
    for (const side of SIDES) {
      const w = wrist(side);
      if (prev[side]) live[side] = Math.max(live[side], w.distanceTo(prev[side]) / MM / 1000 / (SPEED_STEP_US / 1e6));
      prev[side] = w;
    }
    if (!travelled || k % (TRAVEL_US / SPEED_STEP_US)) continue;
    travel++;
    for (const side of SIDES) {
      const left = rigs[side].violations(res[side].touching);
      if (left.length) fail(t, side, '(4) between the moments the ' + side + ' hand is in a control (vertex ' + left[0].vertex + ')', left[0].mm);
      const low = lowestMm(side);
      if (low < -UNDER_MM) fail(t, side, '(4) between the moments the ' + side + ' hand is under the plate', -low);
    }
  }
  stats.travel += travel;
  for (const side of SIDES) {
    const peaks = ' hand peaks at ';
    if (prep[side] >= PREP_M_S) fail(show.prepEnd, side, '(6) the ' + side + peaks + mm(prep[side]) + ' m/s in the preparation');
    if (live[side] >= LIVE_M_S) fail(last, side, '(6) the ' + side + peaks + mm(live[side]) + ' m/s in the live set');
  }
  if (driver.failures) fail(0, '-', '(4) the driver counted ' + driver.failures + ' poses off their targets');
  stats.moments += moments.size;
  stats.crossings += crossings;
  stats.overlaps += overlaps;
  stats.live = Math.max(stats.live, live.left, live.right);
  stats.prep = Math.max(stats.prep, prep.left, prep.right);
  return 'live ' + mm(live.left) + '/' + mm(live.right) + ' m/s, prep ' + mm(prep.left) + '/' + mm(prep.right) +
    ' m/s, prep crossings ' + crossings + ', palm overlaps ' + overlaps;
}

// ─── Every show ──────────────────────────────────────────────────
const started = Date.now();
console.log('mode: ' + (FULL ? 'full (HANDS_FULL=1): every show posed' :
  'sample: song 0 and song 1 + formIndex posed in every arrangement and score (HANDS_FULL=1 for all)') +
  (QMUTE ? '; MUTE waits for the launch point (QMUTE=1)' : ''));
const stats = { shows: 0, posed: 0, moments: 0, travel: 0, crossings: 0, overlaps: 0, live: 0, prep: 0 };
const sums = { intervals: 0, runs: 0, copies: 0, groups: 0, nearPairs: 0, farPairs: 0, holds: 0, inner: 0, innerRuns: 0,
  nested: 0, farInHold: 0, secondCopy: 0, bigGroup: 0 };
Artist.SONGS.forEach((song, si) => {
  Artist.FORMS.forEach((form, fi) => {
    for (const [score, options] of SCORES) {
      where = { song: song.id, form: form.id, score };
      const before = failures.length;
      const show = Artist.buildShow(undefined, Object.assign({ song: si, form: form.id, quantMutes: QMUTE }, options));
      const plan = HandPlan.build(show.actions), tab = table(show.actions);
      assigned(plan, tab);
      sums.intervals += tab.intervals.length;
      sums.runs += tab.runs.length;
      sums.copies += tab.copies.length;
      sums.groups += tab.groups.length;
      sums.nearPairs += tab.pairs.filter(p => p.apart <= NEAR_TRACKS).length;
      sums.farPairs += tab.pairs.filter(p => p.apart > NEAR_TRACKS).length;
      sums.holds += tab.holds.length;
      sums.inner += tab.holds.reduce((s, h) => s + h.inner.filter(x => !x.edges).length, 0);
      sums.innerRuns += tab.holds.reduce((s, h) => s + h.inner.filter(x => x.edges).length, 0);
      for (const k of ['nested', 'farInHold', 'secondCopy', 'bigGroup']) sums[k] += tab[k];
      let line = song.id + ' ' + form.id + ' ' + score + ': ' + tab.intervals.length + ' touches, ' + tab.runs.length + ' turns';
      if (FULL || si === 0 || si === 1 + fi) {
        const travelled = FULL || (si === 1 + fi && SCORES[fi][0] === score);
        line += ', posed' + (travelled ? ' and travelled' : '') + ': ' + posed(show, plan, tab, stats, travelled);
        stats.posed++;
      }
      stats.shows++;
      console.log(line + ', ' + (failures.length > before ? 'FAILED (' + (failures.length - before) + ')' : 'ok'));
    }
  });
});
// The eight Twisted songs laid out as clips and performed with MUTE
// ("Клипы на падах · артист играет MUTE"): every check on each, the whole
// show live (it plays at speed 1, its loading too), the live set sampled
// every 20 000.
const LIBRARY = path.join(web, 'songs', 'twisted', 'Midi Files');
for (const file of require('fs').readdirSync(LIBRARY).filter(f => f.endsWith('.mid')).sort()) {
  const song = MidiSongs.read(new Uint8Array(require('fs').readFileSync(path.join(LIBRARY, file))), file);
  // The chain the core holds (sim/check.sh passes it, as it reads it from the core).
  const clips = MidiClips.plan(song, { rows: +process.env.ROWS || 384 }), live = MidiClips.mutes(clips);
  const run = MidiClips.show(clips, 600000, { moves: live.moves, bars: live.setEnd });
  const show = { actions: run.actions, prepEnd: run.actions[0].t - 1, end: run.end };
  where = { song: song.title, form: 'clips', score: 'MUTE' };
  const before = failures.length;
  if (run.moves.length !== live.moves.length) {
    fail(0, '-', '(1) ' + (live.moves.length - run.moves.length) + ' of the script\'s moves do not fit the show');
  }
  const plan = HandPlan.build(show.actions), tab = table(show.actions);
  assigned(plan, tab);
  sums.intervals += tab.intervals.length;
  sums.runs += tab.runs.length;
  sums.groups += tab.groups.length;
  sums.nearPairs += tab.pairs.filter(p => p.apart <= NEAR_TRACKS).length;
  sums.farPairs += tab.pairs.filter(p => p.apart > NEAR_TRACKS).length;
  sums.copies += tab.copies.length;
  sums.holds += tab.holds.length;
  sums.inner += tab.holds.reduce((n, h) => n + h.inner.filter(x => !x.edges).length, 0);
  sums.innerRuns += tab.holds.reduce((n, h) => n + h.inner.filter(x => x.edges).length, 0);
  for (const k of ['nested', 'farInHold', 'secondCopy', 'bigGroup']) sums[k] += tab[k];
  const line = song.title + ' clips MUTE: ' + tab.intervals.length + ' touches, ' + tab.runs.length + ' turns, posed and travelled: ' +
    posed(show, plan, tab, stats, true);
  stats.posed++;
  stats.shows++;
  console.log(line + ', ' + (failures.length > before ? 'FAILED (' + (failures.length - before) + ')' : 'ok'));
}

const secs = ((Date.now() - started) / 1000).toFixed(1);
console.log('table: ' + sums.intervals + ' intervals, ' + sums.runs + ' runs, ' + sums.copies + ' copies, ' + sums.groups + ' groups, ' +
  sums.nearPairs + ' one-hand and ' + sums.farPairs + ' two-hand MUTE pairs, ' + sums.holds + ' holds with ' + sums.inner +
  ' touches and ' + sums.innerRuns + ' runs inside; ' + sums.nested + ' holds inside holds, ' + sums.farInHold +
  ' far MUTE pairs inside holds, ' + sums.secondCopy + ' second copies, ' + sums.bigGroup + ' groups over four');
console.log('posed: ' + stats.moments + ' moments and ' + stats.travel + ' samples of the live set in ' + stats.posed + ' shows; preparation: ' + stats.crossings + ' crossings, ' +
  stats.overlaps + ' palm overlaps; peak ' + mm(stats.live) + ' m/s live, ' + mm(stats.prep) + ' m/s in the preparation');
if (failures.length) {
  for (const f of failures.slice(0, 5)) {
    console.log('FAIL ' + f.song + ' ' + f.form + ' ' + f.score + ' t=' + f.t + ' ' + f.key + ': ' + f.what +
      (f.distMm === undefined ? '' : ' (' + mm(f.distMm) + ' mm)'));
  }
  const byCheck = {};
  for (const f of failures) byCheck[f.what.slice(0, 3)] = (byCheck[f.what.slice(0, 3)] || 0) + 1;
  console.log('FAILED: ' + failures.length + ' checks (' + Object.keys(byCheck).sort().map(k => k + ' ' + byCheck[k]).join(', ') +
    '); hands ' + (FULL ? 'full' : 'sample') + ', ' + stats.shows + ' shows, ' + stats.posed + ' posed, ' + secs + ' s');
  process.exit(1);
}
console.log('hands ' + (FULL ? 'full' : 'sample') + ': ' + stats.shows + ' shows, ' + stats.posed + ' posed, ' + secs + ' s: ok');
