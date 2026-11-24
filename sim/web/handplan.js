// handplan.js: the plan of the simulator page's hands. HandPlan.PANEL is the
// panel's geometry: where each control of the drawn panel is, how high it
// stands, and where the eye is. HandPlan.build(show.actions) is the plan:
// which hand and finger plays each press and turn, where and when.
//
// World units are page units: the panel square is 1400 units = 300 mm. Page
// point (u, v) is world (u, -v, 0); +z is up out of the plate. Heights in mm
// are named ...Mm. The rectangles are app.js buildPanel()'s box() calls; the
// heights come from hardware/panel/build_panel.py.
(function (root) {
  'use strict';
  const UNITS_PER_MM = 1400 / 300;
  const EYE = Object.freeze({ x: 700, y: -2100, z: 4600 });

  // app.js: the corner of pad (track, row) and of step key i.
  const PAD_X = t => 288 + 91 * t;
  const PAD_Y = r => 368 + 91 * r;
  const STEP_X = i => [180, 446, 712, 978][i >> 2] + 63 * (i & 3);
  const KNOB_U = [583, 770], KNOB_V = 233;   // TRACK, VALUE
  const KNOB_R_MM = 10;
  const TOP_MM = { pad: 5.6, scene: 4.6, mute: 3.5, step: 4.0, play: 5.0, knob: 14.5 };

  const whole = (x, n, what) => {
    if (!Number.isInteger(x) || x < 0 || x >= n) throw new Error('HandPlan: ' + what + ' ' + x + ' is not 0..' + (n - 1));
    return x;
  };
  const at = (kind, key, u, v) => ({ kind, key, u, v, topMm: TOP_MM[kind] });

  // The control an action of show.actions touches: its kind, the key that
  // pairs a down with its up (an encoder's press and its turns share one),
  // its centre on the page and its top above the plate. Null for actions
  // that touch no control (pad-midi: the score's PAD phrases, MIDI sent
  // straight to the synth, played by no hand).
  function control(a) {
    switch (a.op) {
      case 'pad': {
        const t = whole(a.track, 8, 'track'), r = whole(a.row, 8, 'row');
        return at('pad', 'pad:' + t + ':' + r, PAD_X(t) + 40, PAD_Y(r) + 40);
      }
      case 'scene': {
        const r = whole(a.row, 8, 'row');
        return at('scene', 'scene:' + r, 1040 + 32.5, PAD_Y(r) + 7 + 32.5);
      }
      case 'mute': {
        const t = whole(a.track, 8, 'track');
        return at('mute', 'mute:' + t, PAD_X(t) + 40, 1113 + 18.5);
      }
      case 'step': {
        const i = whole(a.index, 16, 'step');
        return at('step', 'step:' + i, STEP_X(i) + 26, 1212 + 29);
      }
      case 'play': return at('play', 'play', 955 + 33.5, 200 + 33.5);
      case 'encsw':
      case 'edge': {
        const e = whole(a.enc, 2, 'encoder');
        return at('knob', 'knob:' + e, KNOB_U[e], KNOB_V);
      }
      case 'key': case 'rig': case 'say': case 'pad-midi': return null;
    }
    throw new Error('HandPlan: no control for op ' + a.op);
  }

  // Page point (u, v) raised hMm above the plate along the eye's ray through
  // it, so that it is still drawn on (u, v).
  function onPanel(u, v, hMm) {
    const h = (hMm || 0) * UNITS_PER_MM, k = h / EYE.z;
    return { x: u + (EYE.x - u) * k, y: -v + (EYE.y + v) * k, z: h };
  }

  // The keys' volumes: the drawn rectangle from the plate up to the key's
  // top, as [u0, v0, u1, v1, top] in page units.
  const BOX = (() => {
    const out = [];
    const box = (u, v, w, h, kind) => out.push(u, v, u + w, v + h, TOP_MM[kind] * UNITS_PER_MM);
    for (let r = 0; r < 8; r++) {
      for (let t = 0; t < 8; t++) box(PAD_X(t), PAD_Y(r), 80, 80, 'pad');
      box(1040, PAD_Y(r) + 7, 65, 65, 'scene');
    }
    for (let t = 0; t < 8; t++) box(PAD_X(t), 1113, 80, 37, 'mute');
    for (let i = 0; i < 16; i++) box(STEP_X(i), 1212, 52, 58, 'step');
    box(955, 200, 67, 67, 'play');
    return Float64Array.from(out);
  })();
  const KNOB_R = KNOB_R_MM * UNITS_PER_MM, KNOB_TOP = TOP_MM.knob * UNITS_PER_MM;
  // The boxes by cell of a CELL-unit grid over the panel square: a point is
  // tested against the few boxes of its cell (the same tests as against all
  // of them; the fitting asks this some hundred thousand times a second).
  const CELL = 25, CELLS = Math.ceil(1400 / CELL);
  const GRID = Array.from({ length: CELLS * CELLS }, () => []);
  for (let i = 0; i < BOX.length; i += 5) {
    const c0 = Math.max(0, Math.floor(BOX[i] / CELL)), c1 = Math.min(CELLS - 1, Math.floor(BOX[i + 2] / CELL));
    const r0 = Math.max(0, Math.floor(BOX[i + 1] / CELL)), r1 = Math.min(CELLS - 1, Math.floor(BOX[i + 3] / CELL));
    for (let r = r0; r <= r1; r++) for (let c = c0; c <= c1; c++) GRID[r * CELLS + c].push(i);
  }

  // Whether world point (x, y, z) is inside a control or below the plate. An
  // encoder knob is a cylinder 20 mm across, so a finger beside it is free.
  function inside(x, y, z) {
    if (z < 0) return true;
    if (z > KNOB_TOP) return false;
    const u = x, v = -y;
    for (let e = 0; e < 2; e++) {
      const du = u - KNOB_U[e], dv = v - KNOB_V;
      if (du * du + dv * dv <= KNOB_R * KNOB_R) return true;
    }
    const c = Math.floor(u / CELL), r = Math.floor(v / CELL);
    if (c < 0 || c >= CELLS || r < 0 || r >= CELLS) return false;
    for (const i of GRID[r * CELLS + c]) {
      if (u >= BOX[i] && u <= BOX[i + 2] && v >= BOX[i + 1] && v <= BOX[i + 3] && z <= BOX[i + 4]) return true;
    }
    return false;
  }

  const PANEL = { UNITS_PER_MM, EYE, control, onPanel, inside };

  // ─── The plan: which hand and finger, where and when ───────────
  // build(show.actions) pairs each control's down with its up (touches),
  // gathers an encoder's edges into turns, finds the score's gestures (clip
  // copies, groups, MUTE pairs, holds, a press that continues a turn), gives
  // each a hand and fingers, and lays out per hand a timeline of keyframes:
  // what each finger aims at, how high above it, and when. Joint angles and
  // the hand's root are the driver's (handrig.js). Times are device µs.
  const TURN_GAP_US = 150000;   // an encoder's edges closer than this are one turn
  const TURN_TAIL_US = 20000;   // a turn ends this long after its last edge
  const PUSH_US = 200000;       // a press of the knob this soon after a turn continues it
  const GROUP_US = 100000;      // a group's downs are within this of its first
  const APPROACH_US = 80000;    // 15 mm above the control this long before a down
  const LIFT_US = 60000;        // 15 mm up this long after an up
  const MIN_US = 5000;          // keyframes closer than this are one
  const AWAY_US = 1500000;      // a hand idle longer than this leaves the frame
  const LEAVE_US = 400000;      // from the lift out of the frame
  const RETURN_US = 400000;     // from out of the frame to the approach
  const WAIT_SPAN_US = 200000;  // a shorter pause goes straight from the lift to the approach
  const TRAVEL_US = 250000;     // from the lift to the wait above the next control
  const SETTLE_US = 60000;      // the wait's last keyframe before the approach
  // A hand travels between controls at 1.5 m/s on average (a minimum-jerk
  // move peaks at 1.875 times that, under the check's 4 m/s).
  const SPEED_M_S = 1.5;
  const RUSH = SPEED_M_S / 2;   // before a key a hand may hurry to 2 m/s
  const travelUs = (a, b) => Math.hypot(a.u - b.u, a.v - b.v) / UNITS_PER_MM / SPEED_M_S * 1000;
  const LIFT_MM = 15, WAIT_MM = 30;
  const CENTRE_U = 700;
  // Where a hand out of the frame is: under its shoulder (handrig.js
  // SHOULDER), just past the panel's front edge.
  const AWAY_AT = { left: { u: 350, v: 1400 + 50 * UNITS_PER_MM }, right: { u: 1050, v: 1400 + 50 * UNITS_PER_MM } };
  // The arms cross when the lines from the shoulders (handrig.js
  // SHOULDER) to the hands' targets cross.
  const SHOULDER = { left: { u: 350, v: 2000 }, right: { u: 1050, v: 2000 } };
  const turnOf = (a, b, c) => Math.sign((b.u - a.u) * (c.v - a.v) - (b.v - a.v) * (c.u - a.u));
  const crossing = (left, right) => {
    const sl = SHOULDER.left, sr = SHOULDER.right;
    return turnOf(sl, left, sr) !== turnOf(sl, left, right) && turnOf(sr, right, sl) !== turnOf(sr, right, left);
  };
  const CROSS_COST = 1400;      // a hand crossing the other costs a panel's width
  const LATE_COST = 2800;       // a hand that cannot get there in time costs more
  const RIGHT_BIAS = 1.1;       // the right hand unless the left one is more than 10 % nearer
  const BUSY_COST = 1e7;        // a hand already playing something else
  const BEAM = 32;              // hand choices kept at each gesture (16 already gives the same plans)
  const GROUPED = ['pad', 'step', 'mute'];
  // A group's fingers from its outer side in; a pair's (from the ring: the
  // rig cannot clear a little finger pressing alone, which a pair would
  // leave at its first down or last up) takes the index instead of the
  // middle when its buttons are further apart than the middle of the two
  // fingers' natural spreads from the ring (33 and 76 mm, handrig.js
  // STRAIGHT).
  const OUTER = ['pinky', 'ring', 'middle', 'index'];
  const PAIR_WIDE_MM = 54.5;
  const other = hand => (hand === 'left' ? 'right' : 'left');
  const aimOf = c => ({ u: c.u, v: c.v, topMm: c.topMm });
  const field = (key, i) => +key.split(':')[i];    // 'pad:t:r', 'mute:t', 'knob:e'

  // Downs paired with ups by control key; edges in turns; MIDI keys.
  function intervals(list) {
    const touches = [], edges = [[], []], keys = [];
    const open = new Map(), notes = new Map();
    const close = (o, t) => touches.push({ id: -1, key: o.key, control: o.control, tDown: o.tDown, tUp: t });
    let last = 0;
    for (const a of list) {
      last = a.t;
      if (a.op === 'key') {
        if (a.down) { if (!notes.has(a.note)) notes.set(a.note, a.t); }
        else if (notes.has(a.note)) { keys.push({ tDown: notes.get(a.note), tUp: a.t }); notes.delete(a.note); }
        continue;
      }
      const c = control(a);
      if (!c) continue;
      if (a.op === 'edge') { edges[a.enc].push({ t: a.t, dir: a.dir }); continue; }
      if (a.down) {
        if (open.has(c.key)) close(open.get(c.key), a.t);   // a second down ends the first press
        open.set(c.key, { key: c.key, control: c, tDown: a.t });
      } else if (open.has(c.key)) {
        close(open.get(c.key), a.t);
        open.delete(c.key);
      }
    }
    for (const o of open.values()) close(o, Math.max(o.tDown, last));
    for (const [, t] of notes) keys.push({ tDown: t, tUp: Math.max(t, last) });
    touches.sort((x, y) => x.tDown - y.tDown || x.tUp - y.tUp);
    touches.forEach((x, id) => { x.id = id; });
    keys.sort((x, y) => x.tDown - y.tDown);

    const turns = [];
    edges.forEach((list, enc) => {
      let cur = null;
      for (const e of list) {
        if (cur && e.t - cur.edges[cur.edges.length - 1].t < TURN_GAP_US) cur.edges.push(e);
        else turns.push(cur = { enc, edges: [e] });
      }
    });
    turns.sort((x, y) => x.edges[0].t - y.edges[0].t || x.enc - y.enc);
    const out = turns.map((x, id) => ({ id, enc: x.enc, tStart: x.edges[0].t,
      tEnd: x.edges[x.edges.length - 1].t + TURN_TAIL_US, edges: x.edges, push: null }));
    // A press of the knob from its turn's last edge to 200 ms after the turn
    // ends continues the turn (the first such press).
    for (const x of touches) {
      if (x.control.kind !== 'knob') continue;
      const enc = field(x.key, 1);
      let turn = null;
      for (const y of out) if (y.enc === enc && y.edges[y.edges.length - 1].t <= x.tDown) turn = y;
      if (turn && turn.push === null && x.tDown <= turn.tEnd + PUSH_US) turn.push = x.id;
    }
    return { touches, turns: out, keys };
  }

  // The gestures: one hand each (a far MUTE pair is two), sorted by start.
  function gestures(touches, turns) {
    const out = [], used = new Uint8Array(touches.length);
    const make = (type, members, extra) => {
      for (const x of members) used[x.id] = 1;
      const g = Object.assign({ type, touches: members, turn: null, start: Infinity, end: -Infinity, u: 0, v: 0,
        freeYaw: false, inner: [], hand: null, lines: null }, extra);
      for (const x of members) {
        g.start = Math.min(g.start, x.tDown); g.end = Math.max(g.end, x.tUp);
        g.u += x.control.u / members.length; g.v += x.control.v / members.length;
      }
      out.push(g);
      return g;
    };
    for (const turn of turns) {
      const push = turn.push === null ? [] : [touches[turn.push]];
      const c = control({ op: 'encsw', enc: turn.enc });
      const g = make('turn', push, { turn });
      Object.assign(g, { start: turn.tStart, end: Math.max(turn.tEnd, g.end), u: c.u, v: c.v });
    }
    // A clip copy: a pad held, then a pad of its track 1 to 3 rows nearer
    // the player pressed during the hold.
    for (const p of touches) {
      if (used[p.id] || p.control.kind !== 'pad') continue;
      for (let j = p.id + 1; j < touches.length && touches[j].tDown < p.tUp; j++) {
        const q = touches[j], rows = field(q.key, 2) - field(p.key, 2);
        if (!used[q.id] && q.control.kind === 'pad' && q.tDown > p.tDown && field(q.key, 1) === field(p.key, 1) &&
            rows >= 1 && rows <= 3) {
          make('copy', [p, q], { freeYaw: true });
          break;
        }
      }
    }
    // Groups: up to four controls of one kind, each down within 100 ms of
    // the first while all the others are still down. Two MUTE keys are a
    // pair: one hand when at most two tracks apart, else both.
    for (const f of touches) {
      if (used[f.id]) continue;
      const members = [f];
      if (GROUPED.includes(f.control.kind)) {
        let until = f.tUp;
        for (let j = f.id + 1; j < touches.length && touches[j].tDown - f.tDown <= GROUP_US && members.length < 4; j++) {
          const x = touches[j];
          if (used[x.id] || x.control.kind !== f.control.kind || x.tDown >= until) continue;
          members.push(x);
          until = Math.min(until, x.tUp);
        }
      }
      if (members.length === 1) make('single', members);
      else if (f.control.kind === 'mute' && members.length === 2) {
        const [a, b] = members.slice().sort((x, y) => x.control.u - y.control.u);
        const apart = field(b.key, 1) - field(a.key, 1);
        if (apart <= 2) make('pair', members, { apart });
        else { make('single', [a], { hand: 'left' }); make('single', [b], { hand: 'right' }); }
      } else make('group', members);
    }
    out.sort((x, y) => x.start - y.start || x.end - y.end);
    // A hold: other gestures begin and end while it lasts; they go to the
    // other hand.
    out.forEach((g, i) => {
      if (g.type === 'turn') return;
      for (let j = i + 1; j < out.length && out[j].start < g.end; j++) {
        if (out[j].start > g.start && out[j].end < g.end) g.inner.push(out[j]);
      }
    });
    return out;
  }

  // Hands for the gestures. Fixed first: a far MUTE pair (made with its
  // hands), a hold or a group by its side, the inside of a hold on the
  // other hand. The others go to the free hand nearer by cost, the costs
  // summed over the show and kept least by a beam search over the last
  // gesture of each hand: the travel from where the hand is (its last
  // target, or out of the frame), the left hand's 10 % more, a crossing of
  // the other hand, coming late, and a hand still busy.
  function hands(list) {
    const side = g => {
      if (g.inner.length) {
        const u = g.inner.reduce((s, x) => s + x.u, 0) / g.inner.length;
        if (u !== g.u) return g.u < u ? 'left' : 'right';
      }
      return g.u < CENTRE_U ? 'left' : 'right';
    };
    for (const g of list) {
      if (!g.hand && (g.inner.length || g.type === 'group')) g.hand = side(g);
      if (g.hand) for (const x of g.inner) if (!x.hand) x.hand = other(g.hand);
    }
    // The cost of gesture i on `hand` after the state's last gestures.
    const cost = (s, i, hand) => {
      const g = list[i], mine = hand === 'left' ? s.left : s.right, theirs = hand === 'left' ? s.right : s.left;
      const p = mine < 0 ? null : list[mine], o = theirs < 0 ? null : list[theirs];
      const near = p && g.start - p.end <= AWAY_US, from = near ? p : AWAY_AT[hand];
      let c = Math.hypot(g.u - from.u, g.v - from.v) * (hand === 'left' ? RIGHT_BIAS : 1);
      if (p && p.end >= g.start) c += BUSY_COST;
      if (near && travelUs(p, g) + 2 * MIN_US > g.start - p.end) c += LATE_COST;
      // Crossing: the other hand where it last was, and what it plays while
      // this hand waits above g or comes back to it (every gesture between
      // this hand's last one and g is the other hand's).
      const cross = x => (hand === 'left' ? crossing(g, x) : crossing(x, g));
      if (o && g.start - o.end <= AWAY_US && cross(o)) c += CROSS_COST;
      else {
        const since = near ? p.end : g.start - APPROACH_US - RETURN_US;
        for (let k = mine + 1; k < i; k++) if (list[k].end > since && cross(list[k])) { c += CROSS_COST; break; }
      }
      return c;
    };
    let beam = [{ left: -1, right: -1, cost: 0, hand: null, back: null }];
    list.forEach((g, i) => {
      const next = new Map();
      for (const s of beam) {
        for (const hand of g.hand ? [g.hand] : ['right', 'left']) {
          const c = s.cost + cost(s, i, hand);
          const t = { left: hand === 'left' ? i : s.left, right: hand === 'right' ? i : s.right, cost: c, hand, back: s };
          const key = t.left + ':' + t.right, was = next.get(key);
          if (!was || c < was.cost) next.set(key, t);
        }
      }
      beam = [...next.values()].sort((a, b) => a.cost - b.cost).slice(0, BEAM);
    });
    const byHand = { left: [], right: [] };
    for (let s = beam[0], i = list.length - 1; s && s.hand; s = s.back, i--) {
      list[i].hand = s.hand;
      byHand[s.hand].push(list[i]);
    }
    return byHand;
  }

  // The fingers of a gesture on its hand: lines of {finger, aim, ref,
  // tDown, tUp, kind}, kind the keyframe kind while the line is in contact.
  function lines(g) {
    const touch = (x, finger) => ({ finger, aim: aimOf(x.control), ref: { type: 'touch', id: x.id },
      tDown: x.tDown, tUp: x.tUp, kind: 'contact' });
    switch (g.type) {
      case 'single': return [touch(g.touches[0], 'index')];
      case 'copy': return [touch(g.touches[0], 'index'), touch(g.touches[1], 'thumb')];
      case 'pair': {
        // The index on the thumb's side: the left key for the right hand.
        const [a, b] = g.touches.slice().sort((x, y) => x.control.u - y.control.u);
        const second = g.apart === 1 ? 'middle' : 'ring';
        return g.hand === 'right' ? [touch(a, 'index'), touch(b, second)] : [touch(a, second), touch(b, 'index')];
      }
      case 'group': {
        // In order across the buttons from the outer side: three or four
        // from the little finger, a pair from the ring.
        const sign = g.hand === 'left' ? 1 : -1;
        const order = g.touches.slice().sort((x, y) => sign * (x.control.u - y.control.u));
        if (order.length === 2) {
          const wide = Math.abs(order[1].control.u - order[0].control.u) / UNITS_PER_MM > PAIR_WIDE_MM;
          return [touch(order[0], 'ring'), touch(order[1], wide ? 'index' : 'middle')];
        }
        return order.map((x, i) => touch(x, OUTER[i]));
      }
      case 'turn': {
        // The pinch to the turn's end, or to just before the press that
        // continues it when that comes in the turn's tail.
        const aim = aimOf(control({ op: 'encsw', enc: g.turn.enc })), ref = { type: 'turn', id: g.turn.id };
        const push = g.touches[0], end = push && push.tDown <= g.turn.tEnd ? push.tDown - 1 : g.turn.tEnd;
        const out = ['thumb', 'index'].map(finger => ({ finger, aim, ref, tDown: g.turn.tStart, tUp: end, kind: 'pinch' }));
        if (g.touches.length) out.push(Object.assign(touch(g.touches[0], 'index'), { kind: 'push' }));
        return out;
      }
    }
    throw new Error('HandPlan: no fingers for a ' + g.type);
  }

  // One hand's keyframes from its gestures (sorted) and its still spans
  // (sorted, none overlapping a gesture).
  function timeline(side, list, stills) {
    // Gestures of one hand that overlap (none in the score) are played as one.
    const items = [];
    for (const g of list) {
      const last = items[items.length - 1];
      if (last && g.start <= last.end) {
        last.lines = last.lines.concat(g.lines);
        last.end = Math.max(last.end, g.end);
        last.freeYaw = last.freeYaw || g.freeYaw;
      } else items.push({ lines: g.lines, start: g.start, end: g.end, freeYaw: g.freeYaw });
    }
    for (const it of items) {
      it.byFinger = new Map();
      for (const l of it.lines.slice().sort((a, b) => a.tDown - b.tDown)) {
        if (!it.byFinger.has(l.finger)) it.byFinger.set(l.finger, []);
        it.byFinger.get(l.finger).push(l);
      }
      it.u = it.lines.reduce((s, l) => s + l.aim.u, 0) / it.lines.length;
      it.v = it.lines.reduce((s, l) => s + l.aim.v, 0) / it.lines.length;
    }
    const home = AWAY_AT[side];
    const away = t => ({ t, kind: 'away', fingers: {}, freeYaw: false });
    // The item at t, every aimed finger at one height (a finger cannot rise
    // 15 mm above a neighbour that presses): while any finger touches, only
    // the touching ones, the others free; else the fingers still to come
    // down, 15 mm up; else (lifting after the item) all of them, 15 mm up.
    const at = (it, t) => {
      const now = [];
      let touching = false, later = false;
      for (const [finger, ls] of it.byFinger) {
        let j = 0;
        while (j + 1 < ls.length && t > ls[j].tUp) j++;
        const l = ls[j];
        now.push([finger, l]);
        if (t >= l.tDown && t <= l.tUp) touching = true;
        if (ls.some(x => x.tDown > t)) later = true;
      }
      const fingers = {};
      let kind = null;
      for (const [finger, l] of now) {
        if (touching) {
          if (t < l.tDown || t > l.tUp) continue;
          if (!kind || l.kind === 'pinch' || (l.kind === 'push' && kind === 'contact')) kind = l.kind;
          fingers[finger] = { aim: l.aim, ref: l.ref, liftMm: 0 };
        } else if (later ? t < l.tDown : t <= (l.kind === 'pinch' ? l.tUp : it.end) + LIFT_US) {
          fingers[finger] = { aim: l.aim, ref: l.ref, liftMm: LIFT_MM };
        }
      }
      return { t, kind: kind || (later ? 'approach' : 'lift'), fingers, freeYaw: it.freeYaw };
    };
    const wait = (it, t) => {
      const fingers = {};
      for (const [finger, ls] of it.byFinger) fingers[finger] = { aim: ls[0].aim, ref: ls[0].ref, liftMm: WAIT_MM };
      return { t, kind: 'wait', fingers, freeYaw: it.freeYaw };
    };

    const out = [];
    const emit = k => {
      k.t = Math.round(k.t);
      if (!out.length || k.t > out[out.length - 1].t) out.push(k);
    };
    const lastT = () => out[out.length - 1].t;
    // The hand after its last keyframe: where it is (home, or an item's
    // place), and the item it is still in contact with (to lift from).
    let place = home, from = null, si = 0;
    // Toward an idle target (home, or the wait above item `it`) before
    // `until`: the lift from the last contact, then the target if the hand
    // gets there in time (and the pause is at least minSpan); before a key
    // it may hurry (rush < 1 of the travel time), straight from the contact
    // when the lift would make it late. Out of the frame, the hand stays out
    // until it is time to come back.
    const idleTo = (target, it, until, minSpan, rush) => {
      const travel = travelUs(place, target) * (rush || 1);
      if (from) {
        const room = until - MIN_US - lastT();
        if (!(rush && place !== target && room < LIFT_US + travel && room >= travel)) {
          if (room < LIFT_US) return;   // no room: the still lifts it
          emit(at(from, lastT() + LIFT_US));
        }
        from = null;
      }
      const t0 = lastT(), span = until - t0;
      if (place === target || span < minSpan) return;
      let arrive;
      if (place === home) {
        arrive = until - MIN_US;
        if (arrive - Math.max(travel, RETURN_US) > t0 + MIN_US) emit(away(arrive - Math.max(travel, RETURN_US)));
      } else {
        arrive = t0 + Math.max(travel, Math.min(target === home ? LEAVE_US : TRAVEL_US, span / 2));
        if (rush) arrive = Math.min(arrive, Math.max(t0 + travel, until - MIN_US));
      }
      if (arrive > until - MIN_US || arrive - lastT() < travel - 1) return;
      emit(target === home ? away(arrive) : wait(it, arrive));
      place = target;
    };
    // A key sounds: the hand stays where its last keyframe left it, lifted.
    const still = s => {
      const keep = out[out.length - 1], fingers = {};
      for (const f in keep.fingers) {
        const x = keep.fingers[f];
        fingers[f] = { aim: x.aim, ref: x.ref, liftMm: Math.max(x.liftMm, LIFT_MM) };
      }
      for (const t of [s.tDown, s.tUp]) emit({ t, kind: 'still', fingers, freeYaw: keep.freeYaw });
      from = null;
    };

    if (!items.length) emit(away(0));
    items.forEach((it, i) => {
      const prev = items[i - 1];
      const target = !prev || it.start - prev.end > AWAY_US ? home : it;
      const back = it.start - APPROACH_US - Math.max(RETURN_US, travelUs(home, it));
      if (!prev) emit(away(Math.min(0, back)));
      // A key sounds before `it`: the hand gets where it idles first, or,
      // when the key falls in the time to come back, above `it`.
      while (si < stills.length && stills[si].tDown < it.start) {
        const s = stills[si++];
        idleTo(target === home && s.tDown > back ? it : target, it, s.tDown, 0, RUSH);
        still(s);
      }
      // Right after a contact with less room than a whole lift and approach
      // need besides the travel: both shortened in their ratio, or, when
      // there is no room even for the travel, a direct move.
      let near = false;
      if (from) {
        const t0 = lastT(), gap = it.start - t0, travel = travelUs(place, it), room = gap - travel;
        if (room < LIFT_US + APPROACH_US + MIN_US) {
          const was = from;
          from = null;
          near = true;
          if (room >= 2 * MIN_US) {
            const k = room / (LIFT_US + APPROACH_US + MIN_US);
            if (travel >= MIN_US) emit(at(was, t0 + LIFT_US * k));
            emit(at(it, it.start - APPROACH_US * k));
          }
        }
      }
      if (!near) {
        // Idle: out of the frame and back, or wait 30 mm above the control
        // (settling before the approach after a long wait); then the
        // approach, shortened when the hand comes late.
        if (target === home) {
          idleTo(home, it, back, 0);
          if (place === home && back > lastT() + MIN_US) emit(away(back));
        } else {
          idleTo(it, it, it.start - APPROACH_US, WAIT_SPAN_US);
          const settle = it.start - APPROACH_US - SETTLE_US;
          if (place === it && out[out.length - 1].kind === 'wait' && settle - lastT() >= WAIT_SPAN_US / 2) emit(wait(it, settle));
        }
        if (from) { emit(at(from, lastT() + LIFT_US)); from = null; }
        const approach = Math.min(APPROACH_US, it.start - lastT() - Math.max(travelUs(place, it), MIN_US));
        if (approach >= MIN_US) emit(at(it, it.start - approach));
      }
      // The gesture: every down and up, and a finger's second press (the
      // index from the pinch onto the knob's top) approaching on its own.
      const times = new Set();
      for (const ls of it.byFinger.values()) {
        ls.forEach((l, j) => {
          times.add(l.tDown); times.add(l.tUp);
          const own = j ? Math.min(APPROACH_US, (l.tDown - ls[j - 1].tUp) / 2) : 0;
          if (own >= MIN_US) times.add(l.tDown - own);
        });
      }
      for (const t of [...times].sort((a, b) => a - b)) emit(at(it, t));
      from = it;
      place = it;
    });
    // After the last gesture: keys still sound, then the lift and away.
    while (si < stills.length) {
      const s = stills[si++];
      idleTo(home, null, s.tDown, 0, RUSH);
      still(s);
    }
    if (from) { emit(at(from, lastT() + LIFT_US)); from = null; }
    if (place !== home) emit(away(lastT() + Math.max(LEAVE_US, travelUs(place, home))));
    return out;
  }

  // The plan of a show's actions (see the interface in the design's
  // planner section): touches, turns, keys, per-hand keyframes, which hand
  // and finger plays each touch and turn, and the times of the downs and ups.
  function build(actions) {
    const list = actions.map((a, i) => [a, i]).sort((x, y) => x[0].t - y[0].t || x[1] - y[1]).map(x => x[0]);
    const { touches, turns, keys } = intervals(list);
    const all = gestures(touches, turns);
    const byHand = hands(all);
    const assign = {};
    for (const g of all) {
      g.lines = lines(g);
      for (const l of g.lines) {
        if (l.ref.type === 'touch') assign['touch:' + l.ref.id] = { hand: g.hand, finger: l.finger };
      }
      if (g.turn) assign['turn:' + g.turn.id] = { hand: g.hand };
    }
    // A key sounds while one hand holds a control: the other hand, unless
    // it is busy then, stays still.
    const stills = { left: [], right: [] };
    for (const k of keys) {
      const holders = new Set();
      for (const x of touches) {
        if (x.tDown > k.tDown) break;
        if (x.tUp >= k.tDown) holders.add(assign['touch:' + x.id].hand);
      }
      if (holders.size !== 1) continue;
      const hand = other([...holders][0]);
      if (byHand[hand].some(g => g.start <= k.tUp && g.end >= k.tDown)) continue;
      const s = stills[hand], last = s[s.length - 1];
      if (last && k.tDown <= last.tUp) last.tUp = Math.max(last.tUp, k.tUp);
      else s.push({ tDown: k.tDown, tUp: k.tUp });
    }
    const plan = { touches, turns, keys, hands: {}, assign, events: [] };
    for (const hand of ['left', 'right']) {
      const list = byHand[hand].sort((x, y) => x.start - y.start || x.end - y.end);
      plan.hands[hand] = timeline(hand, list, stills[hand]);
    }
    // The device's events the hands must not run ahead of: every down and
    // up, and every edge (app.js turns the drawn knob at each).
    plan.events = [...new Set(touches.flatMap(x => [x.tDown, x.tUp]).concat(turns.flatMap(x => x.edges.map(e => e.t))))]
      .sort((a, b) => a - b);
    return plan;
  }

  const api = { PANEL, TURN_GAP_US, build };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.HandPlan = api;
})(typeof window !== 'undefined' ? window : globalThis);
