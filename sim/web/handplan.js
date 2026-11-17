// handplan.js: the plan of the simulator page's hands. So far the panel's
// geometry, as HandPlan.PANEL: where each control of the drawn panel is, how
// high it stands, and where the eye is.
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
  // that touch no control.
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
      case 'key': case 'rig': case 'say': return null;
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
    for (let i = 0; i < BOX.length; i += 5) {
      if (u >= BOX[i] && u <= BOX[i + 2] && v >= BOX[i + 1] && v <= BOX[i + 3] && z <= BOX[i + 4]) return true;
    }
    return false;
  }

  const PANEL = { UNITS_PER_MM, EYE, control, onPanel, inside };

  const api = { PANEL };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.HandPlan = api;
})(typeof window !== 'undefined' ? window : globalThis);
