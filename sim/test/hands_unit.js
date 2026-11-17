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
    assert.ok(CLEARANCE.length >= 40 && CLEARANCE.length <= 80, side + ': CLEARANCE has ' + CLEARANCE.length);
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

for (const { name, fn } of sections) {
  try {
    fn();
  } catch (e) {
    console.log('hands_unit: FAIL in "' + name + '": ' + (e && e.stack || e));
    process.exit(1);
  }
}
console.log('hands_unit: ok');
