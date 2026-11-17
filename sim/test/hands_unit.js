// Unit checks of the hands on the simulator page, without a browser: the
// vendored three.js and hand model first, then (as they are added) the rig,
// the planner and the driver.   node sim/test/hands_unit.js
// Sections run in order; the first failure ends the run with exit status 1.
'use strict';
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const web = path.join(__dirname, '..', 'web');

const sections = [];
const section = (name, fn) => sections.push({ name, fn });

// A base64 string of the model as a typed array (copied, so it is aligned).
const decode = (b64, Type) => {
  const bytes = Buffer.from(b64, 'base64');
  assert.strictEqual(bytes.length % Type.BYTES_PER_ELEMENT, 0, 'base64 length fits ' + Type.name);
  return new Type(new Uint8Array(bytes).buffer);
};

// ─── assets: three.js and the hand model ─────────────────────────
section('assets', () => {
  require(path.join(web, 'three.js'));
  const THREE = globalThis.THREE;
  assert.ok(THREE, 'three.js sets THREE on the global object');
  for (const name of ['Scene', 'Group', 'Object3D', 'Bone', 'Skeleton', 'SkinnedMesh', 'Mesh', 'BufferGeometry',
    'BufferAttribute', 'Float32BufferAttribute', 'Uint16BufferAttribute', 'Matrix4', 'Quaternion', 'Vector3',
    'Vector2', 'Euler', 'Color', 'Box3', 'MathUtils', 'WebGLRenderer', 'PerspectiveCamera', 'HemisphereLight',
    'DirectionalLight', 'MeshPhysicalMaterial', 'MeshStandardMaterial', 'ShadowMaterial', 'MeshBasicMaterial',
    'PlaneGeometry', 'SphereGeometry', 'Texture', 'PMREMGenerator', 'PCFShadowMap', 'SRGBColorSpace', 'FrontSide',
    'DoubleSide', 'RoomEnvironment']) {
    assert.notStrictEqual(THREE[name], undefined, 'THREE.' + name);
  }
  assert.strictEqual(typeof THREE.Skeleton, 'function', 'THREE.Skeleton');
  assert.strictEqual(typeof THREE.SkinnedMesh, 'function', 'THREE.SkinnedMesh');
  assert.strictEqual(THREE.GLTFLoader, undefined, 'no GLTFLoader in the bundle');

  const HAND_MODEL = require(path.join(web, 'handmodel.js'));
  const bases = ['Wrist', 'Palm', 'Thumb_Metacarpal', 'Thumb_Proximal', 'Thumb_Distal', 'Thumb_Tip'];
  for (const finger of ['Index', 'Middle', 'Ring', 'Little']) {
    for (const bone of ['Metacarpal', 'Proximal', 'Intermediate', 'Distal', 'Tip']) bases.push(finger + '_' + bone);
  }
  for (const [side, suffix] of [['right', '_R'], ['left', '_L']]) {
    const model = HAND_MODEL[side];
    const { bones, joints, mesh } = model;
    assert.strictEqual(bones.length, 26, side + ': 26 bones');
    for (const b of bones) assert.ok(b.name.endsWith(suffix), side + ': ' + b.name + ' ends with ' + suffix);
    assert.deepStrictEqual(bones.map(b => b.name.slice(0, -suffix.length)).sort(), bases.slice().sort(),
      side + ': bone names');
    bones.forEach((b, i) => {
      assert.ok(b.parent === -1 || (Number.isInteger(b.parent) && b.parent >= 0 && b.parent < i),
        side + ': parent of ' + b.name + ' is ' + b.parent);
      assert.strictEqual(b.t.length, 3, side + ': ' + b.name + '.t');
      assert.strictEqual(b.r.length, 4, side + ': ' + b.name + '.r');
      assert.strictEqual(b.s.length, 3, side + ': ' + b.name + '.s');
    });
    assert.strictEqual(bones.filter(b => b.parent === -1).length, 1, side + ': one root bone');
    assert.deepStrictEqual(joints.slice().sort((a, b) => a - b), bones.map((b, i) => i),
      side + ': every bone is a joint of the skin, once');
    assert.strictEqual(decode(model.inverseBind, Float32Array).length, 16 * joints.length,
      side + ': an inverse bind matrix per joint');

    const n = mesh.count;
    assert.ok(Number.isInteger(n) && n > 0, side + ': vertex count');
    assert.strictEqual(decode(mesh.position, Float32Array).length, 3 * n, side + ': position');
    assert.strictEqual(decode(mesh.normal, Float32Array).length, 3 * n, side + ': normal');
    assert.strictEqual(decode(mesh.uv, Float32Array).length, 2 * n, side + ': uv');
    const skinIndex = decode(mesh.skinIndex, Uint16Array);
    assert.strictEqual(skinIndex.length, 4 * n, side + ': skinIndex');
    assert.ok(skinIndex.every(j => j < joints.length), side + ': skinIndex within the joints');
    const skinWeight = decode(mesh.skinWeight, Float32Array);
    assert.strictEqual(skinWeight.length, 4 * n, side + ': skinWeight');
    for (let v = 0; v < n; v++) {
      const sum = skinWeight[4 * v] + skinWeight[4 * v + 1] + skinWeight[4 * v + 2] + skinWeight[4 * v + 3];
      assert.ok(Math.abs(sum - 1) <= 1e-3, side + ': skin weights of vertex ' + v + ' sum to ' + sum);
    }
    const index = decode(mesh.index, Uint16Array);
    assert.ok(index.length > 0 && index.length % 3 === 0, side + ': index makes triangles');
    assert.ok(index.every(i => i < n), side + ': index within the vertices');
  }
  for (const key of ['albedo', 'normal', 'orm']) {
    assert.ok(HAND_MODEL.textures[key].startsWith('data:image/jpeg;base64,'), 'texture ' + key + ' is a JPEG');
  }
  const size = fs.statSync(path.join(web, 'handmodel.js')).size;
  assert.ok(size <= 1200000, 'handmodel.js is ' + size + ' bytes');
});

// ─── geometry: the drawn panel in world units ────────────────────
section('geometry', () => {
  const { PANEL } = require(path.join(web, 'handplan.js'));
  const MM = PANEL.UNITS_PER_MM;
  assert.strictEqual(MM, 1400 / 300, 'UNITS_PER_MM');
  assert.deepStrictEqual(PANEL.EYE, { x: 700, y: -2100, z: 4600 }, 'EYE');

  const pad = PANEL.control({ op: 'pad', track: 3, row: 4 });
  assert.deepStrictEqual(pad, { kind: 'pad', key: 'pad:3:4', u: 601, v: 772, topMm: 5.6 }, 'pad (3, 4)');
  for (const [index, u, v] of [[0, 206, 1241], [4, 472, 1241], [15, 1193, 1241]]) {
    assert.deepStrictEqual(PANEL.control({ op: 'step', index }),
      { kind: 'step', key: 'step:' + index, u, v, topMm: 4 }, 'step ' + index);
  }
  assert.deepStrictEqual(PANEL.control({ op: 'scene', row: 2 }),
    { kind: 'scene', key: 'scene:2', u: 1072.5, v: 368 + 182 + 39.5, topMm: 4.6 }, 'scene 2');
  assert.deepStrictEqual(PANEL.control({ op: 'mute', track: 7 }),
    { kind: 'mute', key: 'mute:7', u: 288 + 637 + 40, v: 1131.5, topMm: 3.5 }, 'mute 7');
  assert.deepStrictEqual(PANEL.control({ op: 'play' }),
    { kind: 'play', key: 'play', u: 988.5, v: 233.5, topMm: 5 }, 'play');
  assert.deepStrictEqual(PANEL.control({ op: 'encsw', enc: 0 }),
    { kind: 'knob', key: 'knob:0', u: 583, v: 233, topMm: 14.5 }, 'TRACK');
  assert.deepStrictEqual(PANEL.control({ op: 'edge', enc: 1, dir: -1 }),
    { kind: 'knob', key: 'knob:1', u: 770, v: 233, topMm: 14.5 }, 'VALUE');

  // Two pads with track and row swapped, a scene, a mute and a step of the
  // same number, both encoders and PLAY: all keys differ. A press and the
  // edges of one encoder share its key.
  const keys = [{ op: 'pad', track: 1, row: 2 }, { op: 'pad', track: 2, row: 1 }, { op: 'scene', row: 1 },
    { op: 'mute', track: 1 }, { op: 'step', index: 1 }, { op: 'encsw', enc: 0 }, { op: 'encsw', enc: 1 },
    { op: 'play' }].map(a => PANEL.control(a).key);
  assert.deepStrictEqual(keys, ['pad:1:2', 'pad:2:1', 'scene:1', 'mute:1', 'step:1', 'knob:0', 'knob:1', 'play'],
    'keys');
  assert.strictEqual(new Set(keys).size, keys.length, 'keys differ');
  assert.strictEqual(PANEL.control({ op: 'encsw', enc: 1, down: true }).key,
    PANEL.control({ op: 'edge', enc: 1, dir: 1 }).key, 'encsw and edge of encoder 1 share a key');
  for (const op of ['key', 'rig', 'say']) assert.strictEqual(PANEL.control({ op }), null, op + ' has no control');
  assert.throws(() => PANEL.control({ op: 'pad', track: 8, row: 0 }), /track/, 'pad track 8');
  assert.throws(() => PANEL.control({ op: 'wave' }), /wave/, 'unknown op');

  assert.deepStrictEqual(PANEL.onPanel(601, 772, 0), { x: 601, y: -772, z: 0 }, 'onPanel at the plate');
  for (const h of [3.5, 5.6, 14.5]) {
    const p = PANEL.onPanel(700, 700, h), e = PANEL.EYE;
    assert.ok(Math.abs(p.z - h * MM) <= 1e-9, 'onPanel z at ' + h + ' mm: ' + p.z);
    // The eye, the raised point and the drawn point lie on one line.
    const a = [p.x - 700, p.y + 700, p.z], b = [e.x - 700, e.y + 700, e.z];
    const la = Math.hypot(...a), lb = Math.hypot(...b);
    const cross = Math.hypot(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);
    assert.ok(cross / la / lb < 1e-6, 'onPanel on the eye ray at ' + h + ' mm: ' + cross / la / lb);
  }

  assert.strictEqual(PANEL.inside(601, -772, 2 * MM), true, 'inside pad (3, 4) at 2 mm');
  assert.strictEqual(PANEL.inside(601, -772, 6 * MM), false, 'above pad (3, 4) at 6 mm');
  assert.strictEqual(PANEL.inside(583 + 11 * MM, -233, 8 * MM), false, 'beside TRACK at 11 mm');
  assert.strictEqual(PANEL.inside(583 + 9 * MM, -233, 8 * MM), true, 'in TRACK at 9 mm');
  assert.strictEqual(PANEL.inside(583, -233, 14 * MM), true, 'in TRACK at 14 mm up');
  assert.strictEqual(PANEL.inside(583, -233, 15 * MM), false, 'above TRACK at 15 mm up');
  assert.strictEqual(PANEL.inside(206, -1241, 3.9 * MM), true, 'inside step 0');
  assert.strictEqual(PANEL.inside(206 + 27, -1241, 1 * MM), false, 'between steps 0 and 1');
  assert.strictEqual(PANEL.inside(988.5, -233.5, 4.9 * MM), true, 'inside PLAY');
  assert.strictEqual(PANEL.inside(100, -100, 1 * MM), false, 'off the controls');
  assert.strictEqual(PANEL.inside(100, -100, -0.1), true, 'below the plate');
});

// ─── rig: build, pose and skin one hand ─────────────────────────
section('rig', () => {
  // A fresh Node, with no globals set by anyone, builds both hands.
  const { execFileSync } = require('child_process');
  const fresh = execFileSync(process.execPath, ['-e',
    "const R = require(process.argv[1]); for (const s of ['right', 'left']) R.create(s);" +
    "console.log(typeof HandRig, typeof HandPlan, typeof HAND_MODEL)", path.join(web, 'handrig.js')],
  { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  assert.strictEqual(fresh.trim(), 'undefined undefined undefined', 'handrig.js under Node sets no page globals');

  const HandRig = require(path.join(web, 'handrig.js'));
  const HAND_MODEL = require(path.join(web, 'handmodel.js'));
  const { PANEL } = require(path.join(web, 'handplan.js'));
  const T = globalThis.THREE;
  const FINGERS = ['thumb', 'index', 'middle', 'ring', 'pinky'];
  const pads = {};
  for (const side of ['right', 'left']) {
    const rig = HandRig.create(side, { unitScale: true });
    assert.strictEqual(rig.side, side, side + ': side');
    assert.ok(rig.root.isGroup && rig.root.matrixAutoUpdate === false, side + ': root');
    assert.ok(rig.mesh.isSkinnedMesh && rig.mesh.frustumCulled === false, side + ': mesh');
    assert.strictEqual(rig.bone('Index_Tip').name, 'Index_Tip' + (side === 'right' ? '_R' : '_L'), side + ': bone()');
    assert.strictEqual(rig.mesh.skeleton.bones.length, 26, side + ': skeleton');

    // The hand's palmar direction, from joint positions and handedness alone:
    // a right hand, fingers away and thumb to the left, shows its back upward.
    const at = name => rig.bone(name).getWorldPosition(new T.Vector3());
    const fingers = at('Middle_Proximal').sub(at('Wrist')).normalize();
    const radial = at('Index_Proximal').sub(at('Little_Proximal')).normalize();
    const palmar = (side === 'right' ? radial.clone().cross(fingers) : fingers.clone().cross(radial)).normalize();

    rig.setCurls({}, {});
    const pad = rig.pad('index'), tip = at('Index_Tip');
    const tipX = new T.Vector3().setFromMatrixColumn(rig.bone('Index_Tip').matrixWorld, 0).normalize();
    const tipPalmar = tipX.dot(palmar) > 0 ? tipX : tipX.negate();
    assert.ok(pad.clone().sub(tip).dot(tipPalmar) > 0, side + ': index pad on the palmar side of its tip bone');
    assert.ok(pad.distanceTo(tip) < 0.012, side + ': index pad ' + pad.distanceTo(tip) * 1000 + ' mm from its tip bone');
    pads[side] = FINGERS.map(f => rig.pad(f));

    rig.setCurls({ index: [0, 45, 0, 0] });
    const curled = rig.pad('index').sub(pad).dot(palmar);
    assert.ok(curled > 0.010, side + ': a 45° index curl moves its pad ' + curled * 1000 + ' mm toward the palm');
    rig.setCurls({}, { index: 10 });
    const spread = rig.pad('index').sub(pad).dot(radial);
    assert.ok(spread > 0.005, side + ': a 10° index spread moves its pad ' + spread * 1000 + ' mm toward the thumb');
    rig.setCurls({}, {});
    assert.ok(rig.pad('index').distanceTo(pad) < 1e-9, side + ': zero curls give the rest pose back');
    assert.throws(() => rig.setCurls({ little: [0, 10, 0, 0] }), /little/, side + ': the fifth finger is pinky');

    const { CLEARANCE, PALM } = rig;
    const n = HAND_MODEL[side].mesh.count;
    // About 60 over the palm, knuckles and pads, plus three under each
    // fingertip's last segment (Task 3).
    assert.ok(CLEARANCE.length >= 40 && CLEARANCE.length <= 90, side + ': CLEARANCE has ' + CLEARANCE.length);
    assert.strictEqual(new Set(CLEARANCE).size, CLEARANCE.length, side + ': CLEARANCE vertices differ');
    assert.ok(CLEARANCE.every(v => Number.isInteger(v) && v >= 0 && v < n), side + ': CLEARANCE indices');
    assert.ok(PALM.length >= 20 && PALM.length < CLEARANCE.length && PALM.every(v => CLEARANCE.includes(v)),
      side + ': PALM is a part of CLEARANCE');

    // The groups partition CLEARANCE by what moves a vertex: a finger's
    // bones (all of the thumb's; the others' from the proximal on) when they
    // carry at least half its weight, else the palm.
    const G = rig.FINGER_CLEARANCE, model = HAND_MODEL[side];
    assert.deepStrictEqual(Object.keys(G).sort(), FINGERS.concat('palm').sort(), side + ': FINGER_CLEARANCE groups');
    assert.deepStrictEqual(G.palm, PALM, side + ': the palm group is PALM');
    const grouped = Object.values(G).flat();
    assert.deepStrictEqual(grouped.slice().sort((a, b) => a - b), CLEARANCE.slice().sort((a, b) => a - b),
      side + ': the groups cover CLEARANCE, each vertex once');
    const owner = model.joints.map(i => {
      const name = model.bones[i].name.slice(0, -2);
      const f = FINGERS.find(g => name.startsWith((g === 'pinky' ? 'Little' : g[0].toUpperCase() + g.slice(1)) + '_'));
      return f && (f === 'thumb' || !name.endsWith('_Metacarpal')) ? f : 'palm';
    });
    const SI = decode(model.mesh.skinIndex, Uint16Array), SW = decode(model.mesh.skinWeight, Float32Array);
    const weightOn = (v, f) => [0, 1, 2, 3].reduce((w, k) => w + (owner[SI[4 * v + k]] === f ? SW[4 * v + k] : 0), 0);
    for (const f of FINGERS) {
      assert.ok(G[f].length > 0 && G[f].every(v => weightOn(v, f) >= 0.5), side + ': the ' + f + ' group is on the ' + f);
    }
    assert.ok(PALM.every(v => FINGERS.every(f => weightOn(v, f) < 0.5)), side + ': the palm group is on no finger');

    // In a fist each curled finger's lowest point is sampled: the deepest of
    // all its vertices is at most 3 mm deeper than the deepest of its group,
    // with the palm level and pitched 20° toward the fingertips.
    const FIST = [0, 80, 90, 60];
    rig.setCurls({ index: FIST, middle: FIST, ring: FIST, pinky: FIST });
    const every = Array.from({ length: n }, (_, v) => v), fist = rig.skinned(every);
    for (const pitch of [0, 20]) {
      const down = palmar.clone().multiplyScalar(Math.cos(pitch * Math.PI / 180))
        .addScaledVector(fingers, Math.sin(pitch * Math.PI / 180));
      const depth = v => fist[3 * v] * down.x + fist[3 * v + 1] * down.y + fist[3 * v + 2] * down.z;
      for (const f of FINGERS.slice(1)) {
        const deepest = Math.max(...every.filter(v => weightOn(v, f) >= 0.5).map(depth));
        const gap = deepest - Math.max(...G[f].map(depth));
        assert.ok(gap <= 0.003, side + ': fist, pitch ' + pitch + '°: the ' + f + ' reaches ' + gap * 1000 +
          ' mm below its clearance vertices');
      }
    }
    rig.setCurls({}, {});

    const bind = decode(model.mesh.position, Float32Array);
    const rest = rig.skinned(CLEARANCE);
    assert.ok(rest instanceof Float32Array && rest.length === 3 * CLEARANCE.length, side + ': skinned() shape');
    CLEARANCE.forEach((v, k) => {
      for (let c = 0; c < 3; c++) {
        assert.ok(Math.abs(rest[3 * k + c] - bind[3 * v + c]) <= 1e-4, side + ': vertex ' + v + ' at rest');
      }
    });

    // Posed and placed, the CPU skin matches three's own bone transform.
    rig.setCurls({ thumb: [10, 20, 30], index: [5, 40, 30, 20], pinky: [0, 80, 90, 60] }, { index: -8, thumb: 12 });
    rig.setRoot(new T.Vector3(0.1, -0.2, 0.3), new T.Quaternion().setFromEuler(new T.Euler(0.3, -0.5, 1.1)));
    const posed = rig.skinned(CLEARANCE), v3 = new T.Vector3();
    CLEARANCE.forEach((v, k) => {
      rig.mesh.applyBoneTransform(v, v3.fromArray(bind, 3 * v)).applyMatrix4(rig.mesh.matrixWorld);
      assert.ok(v3.distanceTo(new T.Vector3().fromArray(posed, 3 * k)) <= 1e-6, side + ': vertex ' + v + ' posed');
    });
  }
  // The left hand is the right one mirrored in x (the model's left mesh sits
  // 0.6 mm off the mirror image), so each pad is the mirror of the other's.
  pads.left.forEach((p, i) => {
    const d = new T.Vector3(-p.x, p.y, p.z).distanceTo(pads.right[i]);
    assert.ok(d < 0.001, FINGERS[i] + ' pads: left and right ' + d * 1000 + ' mm off mirror images');
  });

  // The default scale: 0.92 of the model, in page units.
  const rig = HandRig.create('right');
  const p = new T.Vector3(700, -700, 50), q = new T.Quaternion().setFromEuler(new T.Euler(0.2, 0.4, -0.6));
  rig.setRoot(p, q);
  const unit = HandRig.create('right', { unitScale: true });
  const want = unit.pad('middle').applyQuaternion(q).multiplyScalar(0.92 * 1000 * PANEL.UNITS_PER_MM).add(p);
  assert.ok(rig.pad('middle').distanceTo(want) < 1e-6, 'setRoot scales the model 0.92 into page units');
});

// ─── fit: fingertips on controls, the pinch, clearance ───────────
// The score's hardest moments: a press, four held steps across seven keys,
// a clip copy, a MUTE pair, the pinches. Each fit is within its limit, and
// after clear() no clearance vertex is in a control or below the plate and
// the contact points are still on their targets.
section('fit', () => {
  const HandRig = require(path.join(web, 'handrig.js'));
  const { PANEL } = require(path.join(web, 'handplan.js'));
  require(path.join(web, 'three.js'));
  const T = globalThis.THREE;
  const MM = PANEL.UNITS_PER_MM, D2R = Math.PI / 180;
  const UP = new T.Vector3(0, 0, 1);
  const fmt = o => JSON.stringify(o, (k, x) => (typeof x === 'number' ? +x.toFixed(2) : x));
  // A press lands on the control's centre raised to its top along the eye ray.
  const top = a => {
    const c = PANEL.control(a), p = PANEL.onPanel(c.u, c.v, c.topMm);
    return new T.Vector3(p.x, p.y, p.z);
  };
  const aims = (op, key, list) => {
    const out = {};
    for (const [finger, n] of list) out[finger] = top({ op, [key]: n });
    return out;
  };
  const centre = targets => {
    const c = new T.Vector3(), list = Object.values(targets);
    for (const p of list) c.add(p);
    return c.multiplyScalar(1 / list.length);
  };
  const at = (rig, name) => rig.bone(name).getWorldPosition(new T.Vector3());
  const yawOf = d => Math.atan2(-d.x, d.y) / D2R;
  const HAND_MODEL = require(path.join(web, 'handmodel.js'));
  const everyVertex = {};
  for (const side of ['right', 'left']) everyVertex[side] = Array.from({ length: HAND_MODEL[side].mesh.count }, (_, v) => v);
  // A pinch's contacts face the knob's axis.
  const knob = enc => {
    const c = PANEL.control({ op: 'encsw', enc });
    return { axis: new T.Vector3(c.u, -c.v, 0) };
  };

  // Fitted within limitMm; after clear() no violations and the contact
  // points (toward: the contact direction, default down) still within
  // limitMm of their targets.
  const settle = (what, rig, targets, res, limitMm, toward) => {
    assert.ok(res.ok, what + ': not fitted: ' + fmt(res.residualMm));
    assert.deepStrictEqual(Object.keys(res.residualMm).sort(), Object.keys(targets).sort(), what + ': residuals');
    for (const f in targets) {
      assert.ok(res.residualMm[f] <= limitMm, what + ': the ' + f + ' is ' + fmt(res.residualMm[f]) + ' mm off');
    }
    rig.clear(Object.keys(targets));
    const left = rig.violations();
    assert.deepStrictEqual(left, [], what + ': after clear: ' + fmt(left.slice(0, 5)));
    // Nothing of the hand, not only its clearance vertices, under the plate.
    const mesh = rig.skinned(everyVertex[rig.side]);
    let lowest = Infinity, at0 = -1;
    for (let v = 0; v < mesh.length / 3; v++) if (mesh[3 * v + 2] < lowest) { lowest = mesh[3 * v + 2]; at0 = v; }
    assert.ok(lowest >= -0.5 * MM, what + ': vertex ' + at0 + ' is ' + fmt(-lowest / MM) + ' mm under the plate');
    for (const f in targets) {
      const d = rig.contact(f, toward).distanceTo(targets[f]) / MM;
      assert.ok(d <= limitMm, what + ': after clear the ' + f + ' contact is ' + fmt(d) + ' mm off');
    }
  };
  // The two hands apart: their palms' boxes disjoint, the left wrist left.
  const apart = (what, left, right) => {
    const box = rig => new T.Box3().setFromArray(rig.skinned(rig.PALM));
    assert.ok(!box(left).intersectsBox(box(right)), what + ': the palms overlap');
    assert.ok(at(left, 'Wrist').x < at(right, 'Wrist').x, what + ': the left wrist is not left of the right one');
  };

  // place(): a level right hand, fingers away from the player, its index
  // contact 2 mm below the plate. The palm faces down, the wrist points
  // along dir, and violations() reports the sunk vertices, deepest first.
  const flat = HandRig.create('right');
  const sunk = new T.Vector3(700, -700, -2 * MM);
  flat.place('index', sunk, new T.Vector3(0, 1, 0));
  assert.ok(flat.contact('index').distanceTo(sunk) < 1e-6, 'place() puts the contact on its target');
  const along = new T.Vector3().setFromMatrixColumn(flat.bone('Wrist').matrixWorld, 1).normalize();
  assert.ok(along.y > 0.999, 'place(): the wrist points along dir: ' + fmt(along));
  const radial = at(flat, 'Index_Proximal').sub(at(flat, 'Little_Proximal')).normalize();
  const palm = radial.clone().cross(along).normalize();
  assert.ok(palm.z < -0.95, 'place(): the right palm faces down: ' + fmt(palm));
  const below = flat.violations();
  assert.ok(below.length > 0, 'a hand below the plate violates');
  below.forEach((x, i) => {
    assert.ok(flat.CLEARANCE.includes(x.vertex) && x.mm > 0, 'violation ' + fmt(x));
    assert.ok(i === 0 || below[i - 1].mm >= x.mm, 'violations deepest first: ' + fmt(below.slice(0, i + 1)));
  });
  assert.ok(below[0].mm >= 1.9, 'the deepest violation is ' + fmt(below[0]) + ' mm');
  // The contact point is the fingertip's deepest point along its direction.
  for (const toward of [undefined, new T.Vector3(0, 1, 0), knob(0)]) {
    const c = flat.contact('index', toward), tip = at(flat, 'Index_Tip');
    assert.ok(c.distanceTo(tip) < 15 * MM, 'the contact is on the fingertip: ' + fmt(c.distanceTo(tip) / MM) + ' mm');
  }
  assert.ok(flat.contact('index', new T.Vector3(0, 1, 0)).y > flat.contact('index').y - 1e-9, 'a contact ahead');

  // The pose library: the prototype's, and the pinch (the pinch fit's seed).
  const P = HandRig.POSES;
  assert.deepStrictEqual(P.point,
    { thumb: [12, 14, 12], index: [0, 6, 14, 8], middle: [3, 62, 78, 38], ring: [4, 68, 80, 38], pinky: [5, 72, 80, 38] });
  assert.deepStrictEqual(P.fist,
    { thumb: [18, 26, 26], index: [5, 82, 95, 55], middle: [5, 85, 95, 55], ring: [6, 88, 95, 55], pinky: [8, 90, 95, 55] });
  assert.deepStrictEqual(P.lift,
    { thumb: [-25, -20, 0], index: [0, 4, 60, 38], middle: [1, 4, 62, 38], ring: [2, 6, 62, 38], pinky: [3, 8, 60, 38] });
  assert.deepStrictEqual(P.relax,
    { thumb: [5, 8, 10], index: [0, 18, 26, 14], middle: [1, 22, 30, 16], ring: [2, 25, 32, 16], pinky: [3, 28, 32, 16] });
  assert.deepStrictEqual(P.pinch,
    { thumb: [8.75, 4.5, 4.5], index: [0, 3, 26, 17], middle: [5, 85, 95, 55], ring: [6, 88, 95, 55], pinky: [8, 90, 95, 55] });

  // armDir(): from the side's shoulder toward the target, pitched 20° down.
  const right = HandRig.create('right'), left = HandRig.create('left');
  for (const [rig, sx] of [[right, 1050], [left, 350]]) {
    const target = new T.Vector3(600, -800, 30), d = rig.armDir(target);
    assert.ok(Math.abs(d.length() - 1) < 1e-9 && Math.abs(d.z + Math.sin(20 * D2R)) < 1e-9, rig.side + ': armDir pitch');
    const tx = target.x - sx, ty = target.y + 2000;
    assert.ok(Math.abs(tx * d.y - ty * d.x) < 1e-6 && tx * d.x + ty * d.y > 0, rig.side + ': armDir heading');
  }

  // One fingertip: the right index on pad (6, 4). The press keeps the look
  // of the approved prototype, which put the load-time pad on the target
  // with the wrist along dir: the root is within 8 mm and 5° of that (the
  // pad and the contact are about 6 mm apart on the same fingertip).
  const pad64 = top({ op: 'pad', track: 6, row: 4 }), dir64 = right.armDir(pad64);
  const pressed = right.place('index', pad64, dir64);
  const wrist = new T.Vector3().setFromMatrixColumn(right.bone('Wrist').matrixWorld, 1).normalize();
  assert.ok(wrist.angleTo(dir64) <= 5 * D2R, 'press: the wrist turned ' + fmt(wrist.angleTo(dir64) / D2R) + '° off dir');
  assert.ok(right.pad('index').distanceTo(pad64) <= 8 * MM,
    'press: the root moved ' + fmt(right.pad('index').distanceTo(pad64) / MM) + ' mm from the prototype frame');
  settle('right index on pad (6, 4)', right, { index: pad64 }, pressed, 1);

  // The left hand holds steps 2, 4, 6, 8 (four keys across seven) while the
  // right one turns TRACK at the edge of its twist window.
  const held = aims('step', 'index', [['pinky', 1], ['ring', 3], ['middle', 5], ['index', 7]]);
  settle('left hand on steps 1 3 5 7', left, held, left.fit(held, { dir: left.armDir(centre(held)) }), 1);
  const track = PANEL.control({ op: 'encsw', enc: 0 });
  const pinched = right.pinch(0, 20);
  settle('right pinch on TRACK at 20°', right, pinched.targets, pinched, 2, knob(0));
  for (const f of ['thumb', 'index']) {
    const p = pinched.targets[f];
    assert.ok(Math.abs(Math.hypot(p.x - track.u, p.y + track.v) / MM - 10) < 1e-6 && Math.abs(p.z / MM - 12) < 1e-6,
      'pinch: the ' + f + ' target is on the rim, 12 mm up: ' + fmt(p));
  }
  apart('left steps, right pinch', left, right);

  // The twist turns the grip and the hand with it, clockwise seen from
  // above as the knob turns up: -20° and +20°, the ends of the twist
  // window, are 40° apart.
  const grip = t => Math.atan2(t.index.y - t.thumb.y, t.index.x - t.thumb.x) / D2R;
  const rig0 = HandRig.create('right'), rig20 = HandRig.create('right');
  const straight = rig0.pinch(0, 0), back20 = rig20.pinch(0, -20);
  // At twist 0 the thumb is nearer the player than the index, and for a
  // right hand to its left (the grip turned toward the little finger's side).
  assert.ok(straight.targets.thumb.y < straight.targets.index.y && straight.targets.thumb.x < straight.targets.index.x,
    'right pinch: thumb near and left');
  assert.ok(Math.abs(grip(straight.targets) - grip(pinched.targets) - 20) < 1e-6, 'pinch: twist 20° is clockwise');
  settle('right pinch on TRACK at -20°', rig20, back20.targets, back20, 2, knob(0));
  const twist = yawOf(back20.dir) - yawOf(pinched.dir);
  assert.ok(Math.abs(twist - 40) <= 5, 'pinch: the roots at -20° and 20° are ' + fmt(twist) + '° apart');

  // The left hand still on its steps, the right index presses pad (6, 5).
  const pad65 = top({ op: 'pad', track: 6, row: 5 });
  settle('right index on pad (6, 5), left on steps', right, { index: pad65 },
    right.place('index', pad65, right.armDir(pad65)), 1);
  apart('left steps, right pad (6, 5)', left, right);

  // The right hand holds steps 9, 11, 12, 15: two adjacent keys, then a gap
  // of two.
  const wide = aims('step', 'index', [['index', 8], ['middle', 10], ['ring', 11], ['pinky', 14]]);
  settle('right hand on steps 8 10 11 14', right, wide, right.fit(wide, { dir: right.armDir(centre(wide)) }), 1);

  // A clip copy: the index holds pad (2, 0), the thumb presses pad (2, 3);
  // the hand turns so that the thumb comes below the index.
  const copy = { index: top({ op: 'pad', track: 2, row: 0 }), thumb: top({ op: 'pad', track: 2, row: 3 }) };
  settle('right clip copy in column 2', right, copy,
    right.fit(copy, { dir: right.armDir(centre(copy)), freeYaw: true }), 1);

  // Two MUTE keys two tracks apart with one hand.
  const mutes = aims('mute', 'track', [['index', 0], ['ring', 2]]);
  settle('right MUTE 0 and 2', right, mutes, right.fit(mutes, { dir: right.armDir(centre(mutes)) }), 1);

  // The left hand on VALUE: the mirror image of the right grip.
  const value = left.pinch(1, 0);
  settle('left pinch on VALUE at 0°', left, value.targets, value, 2, knob(1));
  assert.ok(value.targets.thumb.y < value.targets.index.y && value.targets.thumb.x > value.targets.index.x,
    'left pinch: thumb near and right (the mirror image)');

  // A natural pinch, both hands on both knobs at the edges (±20°) and the
  // middle of the twist window: the hand comes from the player's side, so
  // the wrist is at least 40 mm nearer the player than the knob's centre
  // and at most 85 mm above the plate (after clear(), as drawn; a pinch
  // from the side of a knob 14.5 mm tall holds the hand higher than a
  // press: the fitted grips put it 68 to 81 mm up); the fit's yaw (from
  // armDir, the twist taken out) is within 5° of the arm's direction, so
  // the forearm swings no further than the twist, and with the pitch it
  // stays 2° inside its window (±30° about armDir; 20° ± 30° down); the
  // roll is 0° to 20° thumb side up; at twist 0, seen from above, the right
  // thumb's contact is left of the index's (the left hand's right of it).
  for (const side of ['right', 'left']) {
    for (const enc of [0, 1]) {
      const k = PANEL.control({ op: 'encsw', enc }), axis = knob(enc);
      for (const twistDeg of [-20, 0, 20]) {
        const what = side + ' pinch(' + enc + ', ' + twistDeg + ')';
        const rig = HandRig.create(side), res = rig.pinch(enc, twistDeg);
        settle(what, rig, res.targets, res, 2, axis);
        assert.ok(res.rollDeg >= 0 && res.rollDeg <= 20, what + ': roll ' + fmt(res.rollDeg) + '°');
        const yawDeg = res.yawDeg + twistDeg;
        assert.ok(Math.abs(yawDeg) <= 30 - 2, what + ': yaw ' + fmt(yawDeg) + '° at the ±30° window');
        assert.ok(Math.abs(yawDeg) <= 5, what + ': the hand turned ' + fmt(yawDeg) + '° off the arm');
        const pitch0 = Math.asin(-rig.armDir(new T.Vector3(k.u, -k.v, 0)).z) / D2R;
        assert.ok(Math.abs(res.pitchDeg - pitch0) <= 30 - 2, what + ': pitch ' + fmt(res.pitchDeg) + '° at the window ' +
          fmt(pitch0) + '° ± 30°');
        const w = at(rig, 'Wrist');
        assert.ok(w.y <= -k.v - 40 * MM, what + ': the wrist is ' + fmt((-k.v - w.y) / MM) + ' mm nearer the player than the knob');
        assert.ok(w.z <= 85 * MM, what + ': the wrist is ' + fmt(w.z / MM) + ' mm up');
        if (twistDeg === 0) {
          const thumb = rig.contact('thumb', axis), index = rig.contact('index', axis);
          assert.ok(side === 'right' ? thumb.x < index.x : thumb.x > index.x,
            what + ': the thumb is ' + (side === 'right' ? 'right' : 'left') + ' of the index: ' + fmt({ thumb, index }));
        }
      }
    }
  }

  // The solve turns the root: the index and middle contacts of the fit's
  // own seed (STRAIGHT on the touching fingers, the others as in point),
  // 20 mm above the plate, spread to 40 mm apart and turned 30° counter-
  // clockwise about their midpoint, heights kept. Translating the hand
  // leaves a zero mean residual.
  const turn = HandRig.create('right');
  const d0 = turn.armDir(new T.Vector3(700, -900, 0));
  turn.place('index', new T.Vector3(700, -900, 20 * MM), d0);
  turn.setCurls(Object.assign({}, P.point, { index: HandRig.STRAIGHT.index, middle: HandRig.STRAIGHT.middle }));
  const seedPair = { index: turn.contact('index'), middle: turn.contact('middle') };
  const mid = seedPair.index.clone().add(seedPair.middle).multiplyScalar(0.5), turned = {};
  for (const f in seedPair) {
    const off = seedPair[f].clone().sub(mid);
    turned[f] = off.multiplyScalar(20 * MM / off.length()).applyAxisAngle(UP, 30 * D2R).add(mid);
  }
  const res = turn.fit(turned, { dir: d0 });
  settle('two targets turned 30°', turn, turned, res, 1);
  assert.ok(res.yawDeg >= 20 && res.yawDeg <= 40, 'the root turned ' + fmt(res.yawDeg) + '° for a 30° turn');

  // The cap on a hooking fingertip: the index alone on TRACK's side 10 mm
  // up, its contact facing the knob's axis, first hooks its last segment
  // under the plate (73° down); the fit caps the descent and the hook is
  // gone, the contact still on its target.
  const hook = HandRig.create('right');
  const east = new T.Vector3(track.u + 10 * MM, -track.v, 10 * MM);
  const capped = hook.fit({ index: east }, { dir: hook.armDir(east), toward: { index: knob(0) } });
  assert.ok(capped.ok && capped.capped.includes('index'), 'the index on the knob side is capped: ' + fmt(capped));
  const tipVerts = hook.FINGER_CLEARANCE.index, tipAt = hook.skinned(tipVerts);
  tipVerts.forEach((v, k) => assert.ok(tipAt[3 * k + 2] >= 0,
    'capped: index vertex ' + v + ' is ' + fmt(-tipAt[3 * k + 2] / MM) + ' mm under the plate'));

  // The palm rises: the index on bare plate left of the pads with the hand
  // pointing 10° up puts the palm under the plate; clear() lifts the root
  // and refits the index, which stays on its target.
  const low = HandRig.create('right'), bare = new T.Vector3(150, -700, 0);
  const flatDir = low.armDir(bare).setZ(0).normalize();
  const lowRes = low.place('index', bare, flatDir.multiplyScalar(Math.cos(10 * D2R)).setZ(Math.sin(10 * D2R)));
  assert.ok(low.violations().some(x => low.PALM.includes(x.vertex)), 'the palm starts under the plate');
  const z0 = low.root.matrix.elements[14];
  settle('index on bare plate, the hand pointing up', low, { index: bare }, lowRes, 1);
  assert.ok(low.root.matrix.elements[14] > z0 + 10 * MM, 'clear() lifted the root ' +
    fmt((low.root.matrix.elements[14] - z0) / MM) + ' mm');
});

// ─── plan: touches, groups, turns, hands, fingers, timing ────────
// Synthetic action lists in the score's shapes (times in µs), then a whole
// show. Every plan also passes the plan's invariants (valid() below).
section('plan', () => {
  const HandPlan = require(path.join(web, 'handplan.js'));
  const Artist = require(path.join(web, 'artist.js'));
  const { PANEL } = HandPlan;
  assert.strictEqual(HandPlan.TURN_GAP_US, 150000, 'TURN_GAP_US');
  assert.strictEqual(typeof HandPlan.build, 'function', 'HandPlan.build');

  const KINDS = ['away', 'approach', 'contact', 'lift', 'wait', 'pinch', 'push', 'still'];
  const FINGERS = ['thumb', 'index', 'middle', 'ring', 'pinky'];
  const PANEL_OPS = ['pad', 'scene', 'mute', 'step', 'play', 'encsw'];
  const show = list => JSON.stringify(list);
  const at = (frames, t) => frames.find(k => k.t === t);
  const aimOf = a => { const c = PANEL.control(a); return { u: c.u, v: c.v, topMm: c.topMm }; };

  // The plan's invariants: keyframes sorted, of the known kinds and fingers,
  // lifts 0, 15 or 30, one lift per keyframe (the rig cannot hold a finger
  // 15 mm above a neighbour that presses); each touch is in contact (its
  // finger, its ref, lift 0)
  // at its down and at its up on its hand; each turn pinches at its start
  // and end with thumb and index; the touches are the panel's downs.
  const valid = (plan, actions) => {
    for (const side of ['left', 'right']) {
      const frames = plan.hands[side];
      assert.ok(Array.isArray(frames) && frames.length > 0, side + ': keyframes');
      frames.forEach((k, i) => {
        assert.ok(i === 0 || frames[i - 1].t < k.t, side + ': keyframes sorted at ' + i + ': ' + show(frames.slice(i - 1, i + 1)));
        assert.ok(KINDS.includes(k.kind), side + ': kind ' + k.kind);
        assert.strictEqual(typeof k.freeYaw, 'boolean', side + ': freeYaw at ' + k.t);
        for (const f in k.fingers) {
          const s = k.fingers[f];
          assert.ok(FINGERS.includes(f), side + ': finger ' + f);
          assert.ok([0, 15, 30].includes(s.liftMm), side + ': liftMm ' + s.liftMm + ' at ' + k.t);
          assert.ok(s.liftMm !== 0 || (s.aim && s.ref), side + ': a contact without aim or ref at ' + k.t);
        }
        assert.ok(new Set(Object.values(k.fingers).map(s => s.liftMm)).size <= 1, side + ': one lift at ' + show(k));
      });
    }
    for (const x of plan.touches) {
      const who = plan.assign['touch:' + x.id];
      assert.ok(who && FINGERS.includes(who.finger) && plan.hands[who.hand], 'assign of touch ' + show(x));
      for (const t of [x.tDown, x.tUp]) {
        const k = at(plan.hands[who.hand], t), s = k && k.fingers[who.finger];
        assert.ok(s && s.liftMm === 0 && s.ref.type === 'touch' && s.ref.id === x.id &&
          s.aim.u === x.control.u && s.aim.v === x.control.v && s.aim.topMm === x.control.topMm,
        'touch ' + x.key + ' in contact at ' + t + ': ' + show(k));
      }
    }
    for (const turn of plan.turns) {
      const who = plan.assign['turn:' + turn.id];
      assert.ok(who && plan.hands[who.hand], 'assign of turn ' + turn.id);
      for (const t of [turn.tStart, turn.tEnd]) {
        const k = at(plan.hands[who.hand], t);
        assert.ok(k && k.kind === 'pinch' && ['thumb', 'index'].every(f => k.fingers[f] && k.fingers[f].liftMm === 0 &&
          k.fingers[f].ref.type === 'turn' && k.fingers[f].ref.id === turn.id), 'turn ' + turn.id + ' pinched at ' + t + ': ' + show(k));
      }
    }
    const downs = actions.filter(a => PANEL_OPS.includes(a.op) && a.down);
    assert.strictEqual(plan.touches.length, downs.length, 'a touch per panel down');
    for (const a of downs) {
      const key = PANEL.control(a).key;
      assert.ok(plan.touches.some(x => x.key === key && x.tDown === a.t), 'a touch for ' + key + ' down at ' + a.t);
    }
    const times = [...new Set(plan.touches.flatMap(x => [x.tDown, x.tUp]))].sort((a, b) => a - b);
    assert.deepStrictEqual(plan.events, times, 'events');
    return plan;
  };
  const plan = actions => valid(HandPlan.build(actions), actions);

  // Action builders, as artist.js pushes them.
  const press = (list, op, o, tDown, tUp) => {
    list.push(Object.assign({ op, down: true, t: tDown }, o), Object.assign({ op, down: false, t: tUp }, o));
  };
  const sorted = list => list.slice().sort((a, b) => a.t - b.t);
  // A click is four edges 3 000 apart; a burst's clicks are 16 000 apart
  // (last edge to next first edge). Both return the last edge's time.
  const click = (list, enc, dir, t) => {
    for (let e = 0; e < 4; e++) list.push({ op: 'edge', enc, dir, t: t + 3000 * e });
    return t + 9000;
  };
  const burst = (list, enc, dir, t, n) => {
    let last = t;
    for (let i = 0; i < n; i++) { last = click(list, enc, dir, t); t = last + 16000; }
    return last;
  };
  const touchOf = (p, key) => p.touches.find(x => x.key === key);
  const who = (p, key) => p.assign['touch:' + touchOf(p, key).id];

  // One tap on pad (6, 4): the right index; it approaches 15 mm up about
  // 80 ms before, is in contact from down to up, lifts, and leaves when
  // nothing follows.
  {
    const list = [];
    press(list, 'pad', { track: 6, row: 4 }, 1000000, 1070000);
    const p = plan(list), x = p.touches[0];
    assert.strictEqual(p.touches.length, 1, 'tap: one touch');
    assert.deepStrictEqual(x, { id: x.id, key: 'pad:6:4', control: PANEL.control({ op: 'pad', track: 6, row: 4 }),
      tDown: 1000000, tUp: 1070000 }, 'tap: the touch');
    assert.deepStrictEqual(p.assign['touch:' + x.id], { hand: 'right', finger: 'index' }, 'tap: right index');
    assert.deepStrictEqual([p.turns, p.keys], [[], []], 'tap: no turns, no keys');
    const right = p.hands.right;
    const approach = right.find(k => k.kind === 'approach');
    assert.ok(approach && approach.t >= x.tDown - 90000 && approach.t <= x.tDown - 70000, 'tap: approach ' + show(approach));
    assert.deepStrictEqual(approach.fingers.index, { aim: aimOf({ op: 'pad', track: 6, row: 4 }),
      ref: { type: 'touch', id: x.id }, liftMm: 15 }, 'tap: approaching 15 mm up');
    assert.strictEqual(at(right, x.tDown).kind, 'contact', 'tap: contact at the down');
    assert.strictEqual(at(right, x.tUp).kind, 'contact', 'tap: contact at the up');
    const after = right.filter(k => k.t > x.tUp);
    assert.ok(after.length >= 2 && after[0].kind === 'lift' && after[0].fingers.index.liftMm === 15, 'tap: lift after ' + show(after));
    assert.ok(after.some(k => k.kind === 'away' && k.t < x.tUp + 2000000), 'tap: away when nothing follows');
    assert.strictEqual(right[right.length - 1].kind, 'away', 'tap: ends away');
    assert.ok(p.hands.left.every(k => k.kind === 'away'), 'tap: the left hand stays away');
  }

  // holdEdit (artist.js:647-657): steps down 30 000 apart, after 80 000 and
  // turnNumber's pause of 110 000 two bursts of TRACK clicks, an encoder
  // press 96 000 after the last edge, the steps up 25 000 apart.
  const holdEdit = (steps, burstGap) => {
    const list = [];
    let t = 1000000;
    steps.forEach((index, i) => list.push({ op: 'step', index, down: true, t: t + 30000 * i }));
    t += 30000 * steps.length + 80000 + 110000;
    let last = burst(list, 0, 1, t, 2);
    last = burst(list, 0, 1, last + burstGap, 4);
    const push = last + 96000;
    press(list, 'encsw', { enc: 0 }, push, push + 90000);
    t = push + 90000 + 90000;
    steps.forEach((index, i) => list.push({ op: 'step', index, down: false, t: t + 25000 * i }));
    return { list: sorted(list), last, push };
  };
  {
    const { list, last, push } = holdEdit([0, 1, 2], 126000);
    const p = plan(list);
    assert.strictEqual(p.touches.length, 4, 'holdEdit: three steps and a press');
    assert.strictEqual(p.turns.length, 1, 'holdEdit: bursts 126 000 apart are one turn: ' + show(p.turns.map(x => x.tStart)));
    const turn = p.turns[0], sw = touchOf(p, 'knob:0');
    assert.strictEqual(turn.enc, 0, 'holdEdit: TRACK');
    assert.strictEqual(turn.edges.length, 24, 'holdEdit: six clicks of four edges');
    assert.deepStrictEqual(turn.edges[0], { t: list.find(a => a.op === 'edge').t, dir: 1 }, 'holdEdit: an edge');
    assert.strictEqual(turn.tStart, turn.edges[0].t, 'holdEdit: tStart');
    assert.strictEqual(turn.tEnd, last + 20000, 'holdEdit: tEnd 20 000 after the last edge');
    assert.strictEqual(sw.tDown, push, 'holdEdit: the press');
    assert.strictEqual(turn.push, sw.id, 'holdEdit: the press continues the turn');
    assert.deepStrictEqual([0, 1, 2].map(i => who(p, 'step:' + i)),
      [{ hand: 'left', finger: 'pinky' }, { hand: 'left', finger: 'ring' }, { hand: 'left', finger: 'middle' }],
      'holdEdit: the left hand holds steps 0 1 2 with pinky, ring, middle');
    assert.deepStrictEqual(p.assign['turn:' + turn.id], { hand: 'right' }, 'holdEdit: the right hand turns');
    assert.deepStrictEqual(who(p, 'knob:0'), { hand: 'right', finger: 'index' }, 'holdEdit: the right index pushes');
    const held = at(p.hands.left, touchOf(p, 'step:2').tDown);
    assert.deepStrictEqual(['pinky', 'ring', 'middle'].map(f => held.fingers[f].liftMm), [0, 0, 0], 'holdEdit: three held');
    assert.strictEqual(at(p.hands.right, sw.tDown).kind, 'push', 'holdEdit: a push');
    const between = p.hands.right.filter(k => k.t > turn.tEnd && k.t < sw.tDown);
    assert.ok(between.every(k => k.kind === 'approach' && k.fingers.index && k.fingers.index.liftMm === 15),
      'holdEdit: from the pinch to the push the hand stays: ' + show(between));
  }
  {
    const p = plan(holdEdit([0, 1, 2], 160000).list);
    assert.strictEqual(p.turns.length, 2, 'holdEdit: bursts 160 000 apart are two turns');
    assert.strictEqual(p.turns[0].push, null, 'holdEdit: the first turn has no press');
    assert.strictEqual(p.turns[1].push, touchOf(p, 'knob:0').id, 'holdEdit: the press continues the second turn');
    assert.ok(p.turns.every(x => p.assign['turn:' + x.id].hand === 'right'), 'holdEdit: both turns right');
  }
  // The group rule: fingers in order across the buttons from the outer
  // side, three or four from the little finger however far apart, a pair
  // from the ring (the index past 54.5 mm: up to three keys apart the
  // middle, six apart the index). Left groups lie left of TRACK, right ones
  // right of it.
  for (const [steps, hand, fingers] of [
    [[1, 2], 'left', ['ring', 'middle']], [[0, 3], 'left', ['ring', 'middle']], [[1, 7], 'left', ['ring', 'index']],
    [[12, 15], 'right', ['middle', 'ring']], [[9, 15], 'right', ['index', 'ring']],
    [[1, 3, 5], 'left', ['pinky', 'ring', 'middle']], [[9, 11, 13], 'right', ['middle', 'ring', 'pinky']],
    [[0, 2, 3, 5], 'left', ['pinky', 'ring', 'middle', 'index']], [[6, 9, 10, 11], 'right', ['index', 'middle', 'ring', 'pinky']]]) {
    const p = plan(holdEdit(steps, 126000).list);
    assert.deepStrictEqual(steps.map(i => who(p, 'step:' + i)), fingers.map(finger => ({ hand, finger })),
      'holdEdit: steps ' + steps + ' with ' + fingers);
  }
  {
    const p = plan(holdEdit([12, 13, 14, 15], 126000).list);
    assert.deepStrictEqual([12, 13, 14, 15].map(i => who(p, 'step:' + i)),
      ['index', 'middle', 'ring', 'pinky'].map(finger => ({ hand: 'right', finger })),
      'holdEdit: the right hand holds steps 12-15 with index, middle, ring, pinky');
    assert.deepStrictEqual(p.assign['turn:' + p.turns[0].id], { hand: 'left' }, 'holdEdit: the left hand turns');
    assert.deepStrictEqual(who(p, 'knob:0'), { hand: 'left', finger: 'index' }, 'holdEdit: the left index pushes');
  }

  // A clip copy: one hand, the index on the source, the thumb on the target,
  // its yaw free.
  {
    const list = [];
    press(list, 'pad', { track: 2, row: 0 }, 1000000, 1720000);
    press(list, 'pad', { track: 2, row: 3 }, 1620000, 1690000);
    const p = plan(sorted(list)), src = who(p, 'pad:2:0'), dst = who(p, 'pad:2:3');
    assert.deepStrictEqual([src.finger, dst.finger, dst.hand], ['index', 'thumb', src.hand], 'copy: index and thumb of one hand');
    const k = at(p.hands[src.hand], 1620000);
    assert.deepStrictEqual([k.fingers.index.liftMm, k.fingers.thumb.liftMm, k.freeYaw], [0, 0, true], 'copy: both down, yaw free');
    assert.ok(p.hands[src.hand].filter(x => x.t >= 1000000 && x.t <= 1720000).every(x => x.freeYaw), 'copy: yaw free throughout');
    assert.deepStrictEqual(Object.keys(at(p.hands[src.hand], 1000000).fingers), ['index'],
      'copy: while the index holds alone the thumb is free');
  }

  // mutePair (artist.js:896-902): downs 12 000 apart, ups 10 000 apart.
  const mutePair = (a, b) => {
    const list = [];
    press(list, 'mute', { track: a }, 1000000, 1092000);
    press(list, 'mute', { track: b }, 1012000, 1102000);
    return plan(sorted(list));
  };
  {
    const p = mutePair(3, 2), a = who(p, 'mute:3'), b = who(p, 'mute:2');
    assert.strictEqual(a.hand, b.hand, 'mutePair(3, 2): one hand');
    assert.deepStrictEqual([a.finger, b.finger].sort(), ['index', 'middle'], 'mutePair(3, 2): index and middle');
    // The index on the thumb's side: the left of a right hand, the right of a left one.
    assert.strictEqual(a.hand === 'right' ? b.finger : a.finger, 'index', 'mutePair(3, 2): the index on the thumb side');
    const q = mutePair(4, 6);
    assert.deepStrictEqual([who(q, 'mute:4'), who(q, 'mute:6')].map(x => x.finger).sort(), ['index', 'ring'],
      'mutePair(4, 6): index and ring');
    const r = mutePair(1, 6);
    assert.deepStrictEqual([who(r, 'mute:1'), who(r, 'mute:6')],
      [{ hand: 'left', finger: 'index' }, { hand: 'right', finger: 'index' }], 'mutePair(1, 6): two hands');
  }

  // Two taps on pads far apart: a hand that has 1 s to wait waits 30 mm
  // above its next pad; with 2 s it leaves the frame in between.
  for (const [gap, idle] of [[1000000, 'wait'], [2000000, 'away']]) {
    const list = [];
    press(list, 'pad', { track: 5, row: 0 }, 1000000, 1070000);
    press(list, 'pad', { track: 7, row: 7 }, 1000000 + gap, 1070000 + gap);
    const p = plan(list), a = who(p, 'pad:5:0'), b = who(p, 'pad:7:7');
    assert.strictEqual(a.hand, b.hand, 'two taps ' + gap + ' apart: one hand');
    const between = p.hands[a.hand].filter(k => k.t > 1070000 && k.t < 1000000 + gap);
    const kinds = between.map(k => k.kind);
    assert.ok(kinds.includes(idle) && !kinds.includes(idle === 'wait' ? 'away' : 'wait'),
      'two taps ' + gap + ' apart: ' + idle + ' between: ' + kinds);
    if (idle === 'wait') {
      const w = between.find(k => k.kind === 'wait');
      assert.deepStrictEqual(w.fingers.index, { aim: aimOf({ op: 'pad', track: 7, row: 7 }),
        ref: { type: 'touch', id: touchOf(p, 'pad:7:7').id }, liftMm: 30 }, 'wait: 30 mm above the next pad');
    }
  }

  // Taps 140 ms apart on neighbouring steps (artist.js programDrum): one
  // hand; the lift and the approach between them are shortened, both
  // within the 80 ms between the up and the next down.
  {
    const list = [];
    press(list, 'step', { index: 0 }, 1000000, 1060000);
    press(list, 'step', { index: 1 }, 1140000, 1200000);
    const p = plan(list), a = who(p, 'step:0'), b = who(p, 'step:1');
    assert.strictEqual(a.hand, b.hand, 'neighbouring taps: one hand');
    const between = p.hands[a.hand].filter(k => k.t > 1060000 && k.t < 1140000);
    const approach = between.find(k => k.kind === 'approach');
    assert.ok(approach && approach.t > 1060000 && 1140000 - approach.t < 80000 && approach.fingers.index.liftMm === 15,
      'neighbouring taps: a shortened approach: ' + show(between));
  }

  // Coming late: step 8, then pad (4, 0) 60 ms later. The hand on step 8 is
  // the nearer (178 mm) but would need 120 ms: the other hand takes it.
  {
    const list = [];
    press(list, 'step', { index: 8 }, 1000000, 1060000);
    press(list, 'pad', { track: 4, row: 0 }, 1120000, 1190000);
    const p = plan(list);
    assert.notStrictEqual(who(p, 'step:8').hand, who(p, 'pad:4:0').hand, 'late: the other hand');
  }
  // Crossing: the right hand turns VALUE, the left presses step 1, then
  // step 12. The left is the nearer, but its arm would cross the right one
  // (still at VALUE): the right takes step 12.
  {
    const list = [];
    click(list, 1, 1, 1000000);
    press(list, 'step', { index: 1 }, 1100000, 1160000);
    press(list, 'step', { index: 12 }, 1700000, 1760000);
    const p = plan(sorted(list));
    assert.deepStrictEqual([p.assign['turn:0'].hand, who(p, 'step:1').hand, who(p, 'step:12').hand],
      ['right', 'left', 'right'], 'crossing: the right hand takes step 12');
  }

  // A key of the MIDI keyboard while a step is held: not a touch; the hand
  // that would edit stays still from the key's down to its up.
  {
    const list = [];
    press(list, 'step', { index: 3 }, 1000000, 1500000);
    list.push({ op: 'key', note: 60, velocity: 100, down: true, t: 1200000 },
      { op: 'key', note: 60, velocity: 0, down: false, t: 1300000 });
    const p = plan(sorted(list));
    assert.strictEqual(p.touches.length, 1, 'key: no touch for the key');
    assert.deepStrictEqual(p.keys, [{ tDown: 1200000, tUp: 1300000 }], 'key: its interval');
    const holder = who(p, 'step:3').hand, editor = holder === 'left' ? 'right' : 'left';
    const frames = p.hands[editor];
    assert.deepStrictEqual([at(frames, 1200000), at(frames, 1300000)].map(k => k && k.kind), ['still', 'still'],
      'key: the editing hand is still at the key down and up');
    assert.deepStrictEqual(frames.filter(k => k.t > 1200000 && k.t < 1300000), [], 'key: nothing else during the key');
  }

  // Keyframe shapes of the 176 shows that the rig could not fit before its
  // seeding changed (the little finger bent, the ring too on a miss): each
  // fits within 1 mm, and after clear() nothing is in a control or under
  // the plate and the contacts are still within 1 mm.
  {
    require(path.join(web, 'three.js'));
    const T = globalThis.THREE, HandRig = require(path.join(web, 'handrig.js')), MM = PANEL.UNITS_PER_MM;
    const top = (i, liftMm) => {
      const c = PANEL.control({ op: 'step', index: i }), q = PANEL.onPanel(c.u, c.v, c.topMm + liftMm);
      return new T.Vector3(q.x, q.y, q.z);
    };
    for (const [side, steps, liftMm] of [
      ['left', { pinky: 0 }, 0], ['right', { pinky: 15 }, 0],
      ['right', { pinky: 11, ring: 10, middle: 9, index: 6 }, 0], ['left', { pinky: 0, ring: 2, middle: 3, index: 5 }, 0],
      ['left', { pinky: 1, ring: 3, middle: 4, index: 6 }, 15], ['right', { pinky: 14, ring: 12, middle: 9, index: 8 }, 30],
      ['left', { pinky: 1, ring: 2, middle: 3, index: 5 }, 15], ['left', { pinky: 6, ring: 7, middle: 9 }, 0],
      ['right', { pinky: 7, ring: 5 }, 0], ['right', { pinky: 10, ring: 8 }, 0], ['left', { pinky: 0, ring: 1 }, 0]]) {
      const what = side + ' ' + JSON.stringify(steps) + ' +' + liftMm + ' mm';
      const rig = HandRig.create(side), targets = {}, centre = new T.Vector3();
      for (const f in steps) centre.add(targets[f] = top(steps[f], liftMm));
      const res = rig.fit(targets, { dir: rig.armDir(centre.multiplyScalar(1 / Object.keys(steps).length)) });
      assert.ok(res.ok, what + ': fitted ' + show(res.residualMm));
      rig.clear(Object.keys(targets));
      assert.deepStrictEqual(rig.violations(), [], what + ': nothing in a control after clear()');
      for (const f in targets) {
        const d = rig.contact(f).distanceTo(targets[f]) / MM;
        assert.ok(d <= 1, what + ': the ' + f + ' is ' + d.toFixed(2) + ' mm off after clear()');
      }
    }
  }

  // A whole show plans, and every panel down is assigned.
  {
    const s = Artist.buildShow(undefined, { song: 0, fw2: true, pads: true });
    const p = plan(s.actions);
    assert.ok(p.touches.length > 400 && p.turns.length > 10, 'song 0: ' + p.touches.length + ' touches, ' +
      p.turns.length + ' turns');
    assert.strictEqual(p.turns.reduce((n, x) => n + x.edges.length, 0), s.actions.filter(a => a.op === 'edge').length,
      'song 0: every edge in a turn');
  }
});

for (const { name, fn } of sections) {
  try {
    fn();
  } catch (e) {
    console.log('hands_unit: FAIL in "' + name + '": ' + (e && e.stack || e));
    process.exit(1);
  }
}
console.log('hands_unit: ok');
