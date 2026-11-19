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
  for (const op of ['key', 'rig', 'say', 'pad-midi']) assert.strictEqual(PANEL.control({ op }), null, op + ' has no control');
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

  // The panel as app.js's buildPanel() draws it: its PAD_X, PAD_Y, STEP_X,
  // the knobs' centres and the box() of every pad, SCENE key, MUTE key,
  // step key and PLAY, read from its source, are where HandPlan.PANEL puts
  // them (a layout change there must not leave the hands off the controls).
  const src = fs.readFileSync(path.join(web, 'app.js'), 'utf8');
  const grab = (re, what) => {
    const m = src.match(re);
    assert.ok(m, 'app.js: no ' + what + ' (buildPanel() changed: update HandPlan.PANEL and this test)');
    return m;
  };
  const fn = name => {
    const m = grab(new RegExp('const ' + name + ' = (\\w+) => ([^;\\n]+);'), name);
    return new Function(m[1], 'return ' + m[2] + ';');
  };
  const [padX, padY, stepX] = ['PAD_X', 'PAD_Y', 'STEP_X'].map(fn);
  const nums = (re, what) => grab(re, what).slice(1).map(Number);
  const [padW, padH] = nums(/box\('pad', PAD_X\(track\), PAD_Y\(row\), (\d+), (\d+)\)/, 'pad box');
  const [sceneU, sceneDv, sceneW, sceneH] = nums(/box\('scene', (\d+), PAD_Y\(row\) \+ (\d+), (\d+), (\d+)\)/, 'scene box');
  const [muteV, muteW, muteH] = nums(/box\('mutekey', PAD_X\(track\), (\d+), (\d+), (\d+)\)/, 'mute box');
  const [stepV, stepW, stepH] = nums(/box\('stepkey', STEP_X\(i\), (\d+), (\d+), (\d+)\)/, 'step box');
  const [playU, playV, playW, playH] = nums(/els\.play = box\('', (\d+), (\d+), (\d+), (\d+)\)/, 'PLAY box');
  const knobU = nums(/\[\[(\d+), 'TRACK'\], \[(\d+), 'VALUE'\]\]/, 'knob centres');
  const [knobDu, knobV, knobW, knobH] = nums(/box\('knob', cx - (\d+), (\d+), (\d+), (\d+)\)/, 'knob box');
  // Each rectangle [u, v, w, h]: its centre is the control's, and it is the
  // control's volume seen from above (just inside its edges inside, just
  // outside them not, 0.5 mm up).
  const drawn = [];
  for (let t = 0; t < 8; t++) {
    for (let r = 0; r < 8; r++) drawn.push([{ op: 'pad', track: t, row: r }, [padX(t), padY(r), padW, padH]]);
    drawn.push([{ op: 'scene', row: t }, [sceneU, padY(t) + sceneDv, sceneW, sceneH]]);
    drawn.push([{ op: 'mute', track: t }, [padX(t), muteV, muteW, muteH]]);
  }
  for (let i = 0; i < 16; i++) drawn.push([{ op: 'step', index: i }, [stepX(i), stepV, stepW, stepH]]);
  drawn.push([{ op: 'play' }, [playU, playV, playW, playH]]);
  for (const [a, [u, v, w, h]] of drawn) {
    const c = PANEL.control(a), what = c.key + ' drawn at ' + [u, v, w, h];
    assert.ok(Math.abs(c.u - (u + w / 2)) < 1e-9 && Math.abs(c.v - (v + h / 2)) < 1e-9, what + ': PANEL puts its centre at ' + c.u + ', ' + c.v);
    const z = 0.5 * MM, e = 0.5;
    for (const [pu, pv] of [[u + e, v + e], [u + w - e, v + h - e], [u + w / 2, v + h / 2]]) {
      assert.strictEqual(PANEL.inside(pu, -pv, z), true, what + ': ' + pu + ', ' + pv + ' is not inside it');
    }
    for (const [pu, pv] of [[u - e, v + h / 2], [u + w + e, v + h / 2], [u + w / 2, v - e], [u + w / 2, v + h + e]]) {
      assert.strictEqual(PANEL.inside(pu, -pv, z), false, what + ': ' + pu + ', ' + pv + ' is inside a control');
    }
  }
  knobU.forEach((cx, enc) => {
    const c = PANEL.control({ op: 'encsw', enc });
    assert.deepStrictEqual([c.u, c.v], [cx - knobDu + knobW / 2, knobV + knobH / 2], 'knob ' + enc + ': its drawn centre');
  });
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
    // Every vertex of each finger's last two segments (at least half its
    // weight on the distal or tip bone: where a contact point is picked);
    // besides them about 60 over the palm, knuckles and pads.
    const boneName = f => (f === 'pinky' ? 'Little' : f[0].toUpperCase() + f.slice(1));
    const tipsOf = f => {
      const model = HAND_MODEL[side], SI = decode(model.mesh.skinIndex, Uint16Array), SW = decode(model.mesh.skinWeight, Float32Array);
      const want = new Set([boneName(f) + '_Distal', boneName(f) + '_Tip'].map(b => b + (side === 'right' ? '_R' : '_L')));
      const out = [];
      for (let v = 0; v < n; v++) {
        let w = 0;
        for (let k = 0; k < 4; k++) if (want.has(model.bones[model.joints[SI[4 * v + k]]].name)) w += SW[4 * v + k];
        if (w >= 0.5) out.push(v);
      }
      return out;
    };
    const tips = new Set();
    for (const f of FINGERS) {
      const list = tipsOf(f);
      assert.ok(list.length > 100, side + ': the ' + f + '\'s last two segments have ' + list.length + ' vertices');
      for (const v of list) tips.add(v);
      assert.ok(list.every(v => rig.FINGER_CLEARANCE[f].includes(v)), side + ': the ' + f + '\'s fingertip is in its clearance group');
    }
    const others = CLEARANCE.filter(v => !tips.has(v)).length;
    assert.ok(others >= 40 && others <= 90, side + ': CLEARANCE has ' + others + ' vertices besides the fingertips');
    assert.strictEqual(CLEARANCE.length, others + tips.size, side + ': CLEARANCE holds every fingertip vertex');
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

  // Fitted within limitMm; after clear() (which says so too) no violations
  // and the contact points (toward: the contact direction, default down)
  // still within limitMm of their targets.
  const settle = (what, rig, targets, res, limitMm, toward) => {
    assert.ok(res.ok, what + ': not fitted: ' + fmt(res.residualMm));
    assert.deepStrictEqual(Object.keys(res.residualMm).sort(), Object.keys(targets).sort(), what + ': residuals');
    for (const f in targets) {
      assert.ok(res.residualMm[f] <= limitMm, what + ': the ' + f + ' is ' + fmt(res.residualMm[f]) + ' mm off');
    }
    const cleared = rig.clear(Object.keys(targets));
    assert.ok(cleared.ok && Object.keys(cleared.residualMm).sort().join() === Object.keys(targets).sort().join(),
      what + ': clear() says ' + fmt(cleared));
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

  // A pose set by hand. getPose() is the pose in the fit's terms (the
  // pivot, the root's rotation, the curls and spreads); setPose() puts it
  // back on any rig of that side, the same to the vertex.
  const pad = top({ op: 'pad', track: 6, row: 4 }), pressed08 = pad.clone().setZ(pad.z - 0.8 * MM);
  const own = HandRig.create('right');
  own.place('index', pressed08, own.armDir(pad));
  own.clear(['index']);
  assert.deepStrictEqual(own.violations(), [], 'after place() and clear() the index pressed 0.8 mm in is exempt');
  const saved = own.getPose(), meshAt = own.skinned(everyVertex.right);
  assert.ok(saved.pivot.isVector3 && saved.quaternion.isQuaternion && saved.curls.index.length === 4 &&
    saved.curls.thumb.length === 3 && typeof saved.spread.index === 'number', 'getPose(): ' + fmt(saved));
  const copyRig = HandRig.create('right');
  copyRig.setPose(saved);
  const copyAt = copyRig.skinned(everyVertex.right);
  let apartMm = 0;
  for (let k = 0; k < copyAt.length; k++) apartMm = Math.max(apartMm, Math.abs(copyAt[k] - meshAt[k]) / MM);
  assert.ok(apartMm < 1e-3, 'setPose(getPose()) moves a vertex ' + fmt(apartMm) + ' mm');
  // It keeps no targets: violations() and clear() take the touching fingers
  // as [{finger, target, toward}] (toward as contact() takes it).
  own.setPose(saved);
  assert.ok(own.violations().length > 0, 'a pose set by hand: the pressed fingertip is not exempt unless listed');
  const touching = [{ finger: 'index', target: pressed08 }];
  assert.deepStrictEqual(own.violations(touching), [], 'listed, the pressed fingertip is exempt');
  const kept = own.clear(['index'], touching);
  assert.ok(kept.ok && kept.residualMm.index <= 1, 'clear(active, touching): ' + fmt(kept));
  // clear() reports a contact it leaves off its target (no silent pose).
  const far = own.clear(['index'], [{ finger: 'index', target: pressed08.clone().setX(pressed08.x + 20 * MM) }]);
  assert.ok(!far.ok && far.residualMm.index > 19, 'clear() with the index 20 mm off: ' + fmt(far));
  // The palm rises from a pose set by hand too, from where the hand is.
  low.place('index', bare, flatDir);
  low.setPose(low.getPose());
  const zHand = low.root.matrix.elements[14];
  const risen = low.clear(['index'], [{ finger: 'index', target: bare }]);
  assert.ok(low.root.matrix.elements[14] > zHand + 10 * MM, 'clear() after setPose() lifted the root ' +
    fmt((low.root.matrix.elements[14] - zHand) / MM) + ' mm');
  assert.ok(risen.ok && risen.residualMm.index <= 1, 'clear() after setPose() refits the index: ' + fmt(risen));
  assert.deepStrictEqual(low.violations([{ finger: 'index', target: bare }]), [], 'after the rise nothing is in a control');
  // setRoot() keeps the fit's root too: the same rise from a root set by it.
  const viaRoot = HandRig.create('right'), at0 = new T.Vector3(), q0 = new T.Quaternion(), s0 = new T.Vector3();
  low.place('index', bare, flatDir);
  const placed = low.getPose();
  low.root.matrix.decompose(at0, q0, s0);
  viaRoot.setCurls(placed.curls, placed.spread);
  viaRoot.setRoot(at0, q0);
  const zRoot = viaRoot.root.matrix.elements[14];
  assert.ok(viaRoot.clear(['index'], [{ finger: 'index', target: bare }]).ok &&
    viaRoot.root.matrix.elements[14] > zRoot + 10 * MM, 'clear() after setRoot() lifts and refits');

  // refit(): the contacts re-solved from the hand's root, which stays where
  // it is: the left hand on steps 0 1 2, its root moved 3 mm sideways, puts
  // its fingertips back within 1 mm by its fingers alone; within 1 mm it
  // does nothing.
  const holder = HandRig.create('left');
  const three = aims('step', 'index', [['pinky', 0], ['ring', 1], ['middle', 2]]);
  holder.fit(three, { dir: holder.armDir(centre(three)) });
  const list3 = Object.keys(three).map(finger => ({ finger, target: three[finger] }));
  assert.ok(holder.clear(Object.keys(three), list3).ok, 'steps 0 1 2 fitted');
  const fitted = holder.getPose();
  assert.deepStrictEqual(holder.refit(list3).solved, false, 'refit() within 1 mm: nothing to solve');
  const moved = Object.assign({}, fitted, { pivot: fitted.pivot.clone().add(new T.Vector3(3 * MM, 0, 0)) });
  holder.setPose(moved);
  const back = holder.refit(list3);
  assert.ok(back.solved && back.ok, 'refit() after a 3 mm move: ' + fmt(back));
  for (const f in three) {
    assert.ok(holder.contact(f).distanceTo(three[f]) / MM <= 1, 'refit(): the ' + f + ' is ' +
      fmt(holder.contact(f).distanceTo(three[f]) / MM) + ' mm off');
  }
  const still = holder.getPose();
  assert.ok(still.pivot.distanceTo(moved.pivot) < 1e-6 * MM && still.quaternion.angleTo(moved.quaternion) < 1e-9,
    'refit() leaves the root where it was');
});

// ─── Action builders, as artist.js pushes them (times in µs) ───────
// Shared by the sections that plan and drive synthetic shows.
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
      // The pinch lasts to the turn's end, or to just before the press that
      // continues it when that comes first.
      const push = turn.push === null ? null : plan.touches[turn.push];
      const end = push && push.tDown <= turn.tEnd ? push.tDown - 1 : turn.tEnd;
      for (const t of [turn.tStart, end]) {
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
    // The device's events the hands wait for: every down and up, and every
    // encoder edge (the knob as app.js draws it turns at its edges).
    const times = [...new Set(plan.touches.flatMap(x => [x.tDown, x.tUp])
      .concat(actions.filter(a => a.op === 'edge').map(a => a.t)))].sort((a, b) => a - b);
    assert.deepStrictEqual(plan.events, times, 'events');
    return plan;
  };
  const plan = actions => valid(HandPlan.build(actions), actions);

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
  // A press of the knob 10 000 after the turn's last edge, inside the turn's
  // 20 000 tail: the pinch lets go just before it, and the index is on the
  // knob's top from its down.
  {
    const list = [];
    const last = burst(list, 0, 1, 1000000, 3);
    press(list, 'encsw', { enc: 0 }, last + 10000, last + 100000);
    const p = plan(sorted(list)), turn = p.turns[0], sw = touchOf(p, 'knob:0');
    assert.strictEqual(turn.push, sw.id, 'push in the tail: the press continues the turn');
    const frames = p.hands[p.assign['turn:' + turn.id].hand];
    const k = at(frames, sw.tDown), before = at(frames, sw.tDown - 1);
    assert.ok(k && k.kind === 'push' && k.fingers.index.ref.id === sw.id && !k.fingers.thumb, 'push in the tail: ' + show(k));
    assert.ok(before && before.kind === 'pinch' && before.fingers.thumb && before.fingers.index.ref.type === 'turn',
      'push in the tail: pinched until 1 before: ' + show(before));
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

  // A PAD phrase note (pad-midi: MIDI the score sends straight to the
  // synth) while a step is held: no touch, no key, the plan as without it.
  {
    const list = [];
    press(list, 'step', { index: 3 }, 1000000, 1500000);
    const without = plan(sorted(list.slice()));
    list.push({ op: 'pad-midi', t: 1200000, status: 0x98, d1: 49, d2: 92 },
      { op: 'pad-midi', t: 1300000, status: 0x88, d1: 49, d2: 0 });
    const p = plan(sorted(list));
    assert.strictEqual(p.touches.length, 1, 'pad-midi: no touch');
    assert.deepStrictEqual(p.keys, [], 'pad-midi: no key');
    assert.deepStrictEqual(p.hands, without.hands, 'pad-midi: the hands as without it');
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

// ─── drive: the plan's keyframes as poses ────────────────────────
// The driver poses both rigs at a device time: each keyframe fitted once
// (cached by its geometry), blended between them, contacts held, the pinch
// following the knob, clear() after; drawTime and peakSpeed. HANDS_TIMING=1
// prints the timing of song 0.
section('drive', () => {
  require(path.join(web, 'three.js'));
  const T = globalThis.THREE;
  const HandRig = require(path.join(web, 'handrig.js'));
  const HandPlan = require(path.join(web, 'handplan.js'));
  const Artist = require(path.join(web, 'artist.js'));
  const { PANEL } = HandPlan, MM = PANEL.UNITS_PER_MM, D2R = Math.PI / 180;
  const fmt = o => JSON.stringify(o, (k, x) => (typeof x === 'number' ? +x.toFixed(2) : x));
  assert.strictEqual(typeof HandRig.Driver, 'function', 'HandRig.Driver');
  assert.strictEqual(typeof HandRig.peakSpeed, 'function', 'HandRig.peakSpeed');
  // A press lands on the control's centre raised to its top along the eye ray.
  const top = a => {
    const c = PANEL.control(a), p = PANEL.onPanel(c.u, c.v, c.topMm);
    return new T.Vector3(p.x, p.y, p.z);
  };
  const rigs = { left: HandRig.create('left'), right: HandRig.create('right') };
  const drive = list => {
    const plan = HandPlan.build(list);
    return { plan, driver: new HandRig.Driver(rigs, plan) };
  };
  const offMm = (side, finger, target, toward) => rigs[side].contact(finger, toward).distanceTo(target) / MM;
  const wrist = side => rigs[side].bone('Wrist').getWorldPosition(new T.Vector3());
  const fingersOf = touching => touching.map(c => c.finger).sort().join(' ');
  // Nothing of either hand in a control, the touching fingertips (as pose()
  // lists them) exempt.
  const clean = (what, res) => {
    for (const side of ['left', 'right']) {
      const left = rigs[side].violations(res[side].touching);
      assert.deepStrictEqual(left, [], what + ': the ' + side + ' hand is in a control: ' + fmt(left.slice(0, 3)));
    }
  };
  // Never on a control the device does not see pressed: 1 and 3 000 before
  // every down, and at and 1 after every up (the device has processed the
  // up by then), the assigned finger's contact point at least 0.2 mm above
  // the control's top (the eased move ends 0.3 mm above it); at the down
  // and 1 before the up on its target (within 1.5 mm): a contact holds
  // from its down to just before its up.
  const hovering = (what, plan, driver) => {
    for (const x of plan.touches) {
      const who = plan.assign['touch:' + x.id], c = x.control, p = PANEL.onPanel(c.u, c.v, c.topMm);
      const target = new T.Vector3(p.x, p.y, p.z);
      for (const t of [x.tDown - 1, x.tDown - 3000, x.tUp, x.tUp + 1]) {
        driver.pose(t);
        const above = rigs[who.hand].contact(who.finger).z / MM - c.topMm;
        assert.ok(above >= 0.2, what + ': ' + x.key + ' at ' + t + ': the ' + who.hand + ' ' + who.finger + ' is ' + fmt(above) +
          ' mm above the top');
      }
      for (const t of [x.tDown, x.tUp - 1]) {
        driver.pose(t);
        const d = offMm(who.hand, who.finger, target);
        assert.ok(d <= 1.5, what + ': ' + x.key + ' at ' + t + ': the ' + who.hand + ' ' + who.finger + ' is ' + fmt(d) + ' mm off');
      }
    }
    assert.strictEqual(driver.failures, 0, what + ': failures');
  };

  // The Task 5 tap on pad (6, 4), the right index: on its target at the
  // down, the middle and 1 000 before the up; at least 5 mm above it
  // 40 000 before the down; away after, every clearance vertex out of the
  // panel toward the player. The hand travels in from away.
  const tapList = [];
  press(tapList, 'pad', { track: 6, row: 4 }, 1000000, 1070000);
  {
    const { plan, driver } = drive(tapList), target = top({ op: 'pad', track: 6, row: 4 });
    for (const t of [1000000, 1035000, 1069000]) {
      const res = driver.pose(t);
      assert.ok(!res.right.away && res.right.ok && res.left.away, 'tap at ' + t + ': ' + fmt(res));
      assert.strictEqual(fingersOf(res.right.touching), 'index', 'tap at ' + t + ': the index touches');
      const d = offMm('right', 'index', target);
      assert.ok(d <= 1.5, 'tap at ' + t + ': the index is ' + fmt(d) + ' mm off');
      clean('tap at ' + t, res);
    }
    driver.pose(960000);
    const above = (rigs.right.contact('index').z - target.z) / MM;
    assert.ok(above >= 5, 'tap: 40 000 before the down the index is ' + fmt(above) + ' mm above');
    const gone = plan.hands.right.find(k => k.kind === 'away' && k.t > 1070000);
    const res = driver.pose(gone.t);
    assert.ok(res.right.away && res.left.away, 'tap: both away at ' + gone.t + ': ' + fmt(res));
    for (const side of ['left', 'right']) {
      const p = rigs[side].skinned(rigs[side].CLEARANCE);
      let most = -Infinity;
      for (let k = 1; k < p.length; k += 3) most = Math.max(most, p[k]);
      assert.ok(most < -1400, 'tap: away, a ' + side + ' clearance vertex is at y ' + fmt(most));
    }
    const v = HandRig.peakSpeed(driver, 0, gone.t, 10000);
    assert.ok(v.right > 0.5 && v.left < 1e-9, 'tap: peak speeds ' + fmt(v) + ' m/s');
    assert.strictEqual(driver.failures, 0, 'tap: failures');
    hovering('tap', plan, driver);
  }

  // drawTime never reaches an unprocessed event: no finger on a control
  // before the device shows it down, nor off it before it shows it up.
  {
    const { driver } = drive(tapList);
    assert.strictEqual(driver.drawTime(1000000 - 5000, 1000000 + 20000), 1000000 - 1, 'drawTime: before the down');
    assert.ok(driver.drawTime(1000000, 1000000 + 20000) >= 1000000, 'drawTime: the down processed');
    assert.strictEqual(driver.drawTime(1000000, 1100000), 1070000 - 1, 'drawTime: before the up');
    assert.strictEqual(driver.drawTime(1070000, 1090000), 1090000, 'drawTime: no later event');
    assert.strictEqual(driver.drawTime(1000000 - 5000, 1000000 - 9000), 1000000 - 5000, 'drawTime: never before now');
  }
  // Within a show the draw time never goes back, though the extrapolation
  // may (the audio clock lagging): it is held until the device passes it.
  {
    const { driver } = drive(tapList);
    assert.strictEqual(driver.drawTime(1010000, 1019000), 1019000, 'drawTime: extrapolated');
    assert.strictEqual(driver.drawTime(1012000, 1015000), 1019000, 'drawTime: held while the extrapolation falls back');
    assert.strictEqual(driver.drawTime(1016000, 1016000), 1019000, 'drawTime: held until now passes it');
    assert.strictEqual(driver.drawTime(1020000, 1021000), 1021000, 'drawTime: on once now passed it');
    assert.strictEqual(driver.drawTime(1060000, 1090000), 1069999, 'drawTime: the held time still stops before the up');
    assert.strictEqual(driver.drawTime(1062000, 1062000), 1069999, 'drawTime: held there');
  }
  // The edges of an encoder are events too: the pinch never turns before
  // the device has turned the knob (app.js draws the knob at each edge).
  {
    const list = [];
    burst(list, 0, 1, 1000000, 2);
    const { plan, driver } = drive(sorted(list)), edges = list.filter(a => a.op === 'edge').map(a => a.t);
    for (let i = 1; i < edges.length; i++) {
      assert.strictEqual(driver.drawTime(edges[i - 1], edges[i] + 20000), edges[i] - 1, 'drawTime: stops before the edge at ' + edges[i]);
    }
    assert.ok(edges.every(t => plan.events.includes(t)), 'drawTime: every edge is an event');
  }

  // holdEdit: the left hand holds steps 0 1 2 (pinky, ring, middle) while
  // the right one turns TRACK in two bursts 126 000 apart and presses it.
  // The held contacts stay on their steps; at every edge both pinch
  // contacts are on the knob's rim (10 mm from its axis, 12 mm up), the
  // hand turned clockwise with the knob (it turns up) and never past the
  // ±20° window; in the gap between the bursts the hand lifts 3 mm and
  // regrips at the middle of the window; the press puts the index on the
  // knob's top. A pose depends on its time alone.
  {
    const { list, push } = holdEdit([0, 1, 2], 126000);
    const { plan, driver } = drive(list);
    const steps = [0, 1, 2].map(i => touchOf(plan, 'step:' + i)), fingers = ['pinky', 'ring', 'middle'];
    assert.deepStrictEqual(steps.map(x => plan.assign['touch:' + x.id]), fingers.map(finger => ({ hand: 'left', finger })),
      'holdEdit: the left pinky, ring and middle hold');
    const from = Math.max(...steps.map(x => x.tDown)), to = Math.min(...steps.map(x => x.tUp));
    for (let t = from; t <= to + 9999; t += 10000) {
      const at = Math.min(t, to), res = driver.pose(at);
      assert.ok(res.left.ok && fingersOf(res.left.touching) === 'middle pinky ring', 'holdEdit at ' + at + ': ' + fmt(res.left));
      steps.forEach((x, i) => {
        const d = offMm('left', fingers[i], top({ op: 'step', index: i }));
        assert.ok(d <= 1.5, 'holdEdit at ' + at + ': the ' + fingers[i] + ' is ' + fmt(d) + ' mm off step ' + i);
      });
      clean('holdEdit at ' + at, res);
    }
    const knob = PANEL.control({ op: 'encsw', enc: 0 }), axis = { axis: new T.Vector3(knob.u, -knob.v, 0) };
    const rimMm = f => {
      const c = rigs.right.contact(f, axis);
      return Math.hypot(Math.hypot(c.x - knob.u, c.y + knob.v) / MM - 10, c.z / MM - 12);
    };
    const grip = () => {
      const a = rigs.right.contact('thumb', axis), b = rigs.right.contact('index', axis);
      return Math.atan2(b.y - a.y, b.x - a.x) / D2R;
    };
    const turn = plan.turns[0];
    assert.deepStrictEqual(plan.assign['turn:' + turn.id], { hand: 'right' }, 'holdEdit: the right hand turns');
    driver.pose(turn.tStart);
    const grip0 = grip(), twist = () => ((grip0 - grip()) % 360 + 540) % 360 - 180;
    let most = 0;
    for (const e of turn.edges) {
      const res = driver.pose(e.t);
      assert.ok(res.right.ok && fingersOf(res.right.touching) === 'index thumb', 'holdEdit: pinched at the edge at ' + e.t +
        ': ' + fmt(res.right));
      for (const f of ['thumb', 'index']) {
        assert.ok(rimMm(f) <= 2, 'holdEdit: at the edge at ' + e.t + ' the ' + f + ' is ' + fmt(rimMm(f)) + ' mm off the rim');
      }
      assert.ok(Math.abs(twist()) <= 20.5, 'holdEdit: twisted ' + fmt(twist()) + '° at ' + e.t);
      most = Math.max(most, twist());
      clean('holdEdit at the edge at ' + e.t, res);
    }
    assert.ok(most >= 10, 'holdEdit: the hand turns with the knob, at most ' + fmt(most) + '°');
    // The second burst turns the knob 60°: the hand turns with it to the
    // window's edge, then eases back toward the middle while the fingers
    // slide on the knob, inside the burst.
    const second = turn.edges.slice(turn.edges.findIndex((e, i) => i > 0 && e.t - turn.edges[i - 1].t >= 100000));
    const twists = second.map(e => (driver.pose(e.t), twist())), peak = twists.indexOf(Math.max(...twists));
    assert.ok(twists[peak] >= 15 && Math.min(...twists.slice(peak)) <= 10, 'holdEdit: in the second burst the twist goes ' +
      fmt(twists) + '°');
    const k = turn.edges.findIndex((e, i) => i > 0 && e.t - turn.edges[i - 1].t >= 100000);
    const before = turn.edges[k - 1].t, after = turn.edges[k].t;
    driver.pose(before);
    const zGrip = rigs.right.contact('index', axis).z;
    const mid = driver.pose((before + after) / 2), rise = (rigs.right.contact('index', axis).z - zGrip) / MM;
    assert.ok(rise > 2.5 && rise < 3.5, 'holdEdit: between the bursts the hand lifts ' + fmt(rise) + ' mm');
    clean('holdEdit between the bursts', mid);
    driver.pose(after);
    assert.ok(Math.abs(twist()) < 0.5, 'holdEdit: regripped at ' + fmt(twist()) + '°');
    // From the grip to the press the plan does not lift: the hand leaves
    // the knob's side straight up before it moves over the top.
    const zEnd = (driver.pose(turn.tEnd), rigs.right.contact('index', axis).z);
    driver.pose(turn.tEnd + 5000);
    for (const f of ['thumb', 'index']) {
      const c = rigs.right.contact(f, axis), r = Math.hypot(c.x - knob.u, c.y + knob.v) / MM;
      assert.ok(Math.abs(r - 10) <= 2, 'holdEdit: leaving the grip, the ' + f + ' is ' + fmt(r) + ' mm from the axis');
    }
    const up = (rigs.right.contact('index', axis).z - zEnd) / MM;
    assert.ok(up >= 3, 'holdEdit: 5 000 after the grip the hand is ' + fmt(up) + ' mm up');
    driver.pose(push);
    const d = offMm('right', 'index', top({ op: 'encsw', enc: 0 }));
    assert.ok(d <= 1.5, 'holdEdit: at the press the index is ' + fmt(d) + ' mm off the knob top');
    const t1 = turn.edges[5].t + 1234;
    driver.pose(t1);
    const once = [wrist('left'), wrist('right')];
    driver.pose(push);
    driver.pose(from);
    driver.pose(t1);
    assert.ok(wrist('left').distanceTo(once[0]) < 1e-9 && wrist('right').distanceTo(once[1]) < 1e-9, 'holdEdit: pose(t) depends on t alone');
    assert.strictEqual(driver.failures, 0, 'holdEdit: failures');
  }

  // Every touch on its target (its finger's contact within 1.5 mm) at its
  // down, its middle and 1 000 before its up; nothing of either hand in a
  // control then, nor more than 0.5 mm under the plate.
  const vertices = {};
  for (const side of ['left', 'right']) {
    vertices[side] = Array.from({ length: rigs[side].mesh.geometry.attributes.position.count }, (_, v) => v);
  }
  const played = (what, plan, driver) => {
    for (const x of plan.touches) {
      const who = plan.assign['touch:' + x.id], c = x.control, p = PANEL.onPanel(c.u, c.v, c.topMm);
      const target = new T.Vector3(p.x, p.y, p.z);
      for (const t of [x.tDown, Math.round((x.tDown + x.tUp) / 2), x.tUp - 1000]) {
        const res = driver.pose(t), d = offMm(who.hand, who.finger, target), at = what + ': ' + x.key + ' at ' + t;
        assert.ok(d <= 1.5, at + ': the ' + who.hand + ' ' + who.finger + ' is ' + fmt(d) + ' mm off');
        clean(at, res);
        for (const side of ['left', 'right']) {
          const mesh = rigs[side].skinned(vertices[side]);
          let lowest = Infinity;
          for (let k = 2; k < mesh.length; k += 3) lowest = Math.min(lowest, mesh[k]);
          assert.ok(lowest >= -0.5 * MM, at + ': the ' + side + ' hand is ' + fmt(-lowest / MM) + ' mm under the plate');
        }
      }
    }
    assert.strictEqual(driver.failures, 0, what + ': failures');
    hovering(what, plan, driver);
  };
  // A group, a MUTE pair and a clip copy: the hand stays where the whole
  // gesture puts it while its fingers come and go (a lone index fitted
  // alone would turn the hand 30° from the pair's 12 000 earlier); a finger
  // let go lifts off its control. mutePair (artist.js:896-902): downs
  // 12 000 apart, ups 10 000 apart.
  for (const steps of [[0, 1, 2], [0, 2, 3, 5]]) {
    const { plan, driver } = drive(holdEdit(steps, 126000).list);
    played('holdEdit steps ' + steps.join(' '), plan, driver);
  }
  for (const [a, b] of [[3, 2], [4, 6]]) {
    const list = [];
    press(list, 'mute', { track: a }, 1000000, 1092000);
    press(list, 'mute', { track: b }, 1012000, 1102000);
    const { plan, driver } = drive(sorted(list));
    assert.strictEqual(who(plan, 'mute:' + a).hand, who(plan, 'mute:' + b).hand, 'mutePair(' + a + ', ' + b + '): one hand');
    played('mutePair(' + a + ', ' + b + ')', plan, driver);
    const v = HandRig.peakSpeed(driver, 900000, 1300000, 5000);
    assert.ok(v.left < 4 && v.right < 4, 'mutePair(' + a + ', ' + b + '): peak speeds ' + fmt(v) + ' m/s');
  }
  {
    const list = [];
    press(list, 'pad', { track: 2, row: 0 }, 1000000, 1720000);
    press(list, 'pad', { track: 2, row: 3 }, 1620000, 1690000);
    const { plan, driver } = drive(sorted(list));
    played('copy', plan, driver);
    const v = HandRig.peakSpeed(driver, 1000000, 1720000, 5000);
    assert.ok(v.left < 4 && v.right < 4, 'copy: while the index holds, peak speeds ' + fmt(v) + ' m/s');
  }

  // Between its keyframes a hand that holds nothing passes over the panel:
  // every 5 000, nothing of either hand in a control (the fingertips
  // touching exempt) nor more than 0.5 mm under the plate. The left hand
  // goes from a pair of steps to TRACK, its grip seen from the steps' lift
  // dipping its fingertips under the plate on the way; holdEdit.
  const everywhere = (what, list) => {
    const { driver } = drive(list), end = list[list.length - 1].t + 1000000;
    for (let t = 0; t <= end; t += 5000) {
      const res = driver.pose(t);
      clean(what + ' at ' + t, res);
      for (const side of ['left', 'right']) {
        const mesh = rigs[side].skinned(vertices[side]);
        let lowest = Infinity;
        for (let k = 2; k < mesh.length; k += 3) lowest = Math.min(lowest, mesh[k]);
        assert.ok(lowest >= -0.5 * MM, what + ' at ' + t + ': the ' + side + ' hand is ' + fmt(-lowest / MM) + ' mm under the plate');
      }
    }
    assert.strictEqual(driver.failures, 0, what + ': failures');
  };
  {
    const list = [];
    press(list, 'step', { index: 1 }, 1000000, 1200000);
    press(list, 'step', { index: 2 }, 1010000, 1200000);
    burst(list, 0, 1, 2000000, 3);
    everywhere('steps 1 2, then TRACK', sorted(list));
    everywhere('holdEdit', holdEdit([0, 1, 2], 126000).list);
  }

  // A free fingertip's side in an unpressed key: in song 0 (classic, base)
  // 222 093 529 into the show the left index presses pad (2, 2) and its
  // thumb's last segment hung 3.4 mm into pad (4, 6) (vertex 1004), which
  // no clearance vertex covered. Every vertex of the last two segments is
  // a clearance vertex now: the free thumb curls out, and nothing of either
  // mesh is in a control.
  {
    const show = Artist.buildShow(undefined, { song: 0, form: 'classic' });
    const driver = new HandRig.Driver(rigs, HandPlan.build(show.actions)), res = driver.pose(222093529);
    assert.ok(!res.left.away && fingersOf(res.left.touching) === 'index', 'song 0 at 222 093 529: ' + fmt(res.left));
    for (const side of ['left', 'right']) {
      const left = rigs[side].violations(res[side].touching, { mesh: true });
      assert.deepStrictEqual(left, [], 'song 0 at 222 093 529: the ' + side + ' mesh is in a control: ' + fmt(left.slice(0, 3)));
    }
    // The mesh skinned by the caller gives the same answer; here, pressed
    // 2 mm deeper (the index's own pad then holds it more than 1 mm deep).
    const all = vertices.left, deeper = rigs.left.skinned(all);
    for (let k = 2; k < deeper.length; k += 3) deeper[k] -= 2 * MM;
    const p0 = rigs.left.getPose();
    rigs.left.setPose(Object.assign({}, p0, { pivot: p0.pivot.clone().setZ(p0.pivot.z - 2 * MM) }));
    const own = rigs.left.violations(res.left.touching, { mesh: true });
    assert.ok(own.length > 0, 'pressed 2 mm deeper the left mesh is in a control');
    rigs.left.setPose(p0);
    const given = rigs.left.violations(res.left.touching, { mesh: true, skinned: deeper });
    assert.ok(given.length === own.length && given.every((x, i) => x.vertex === own[i].vertex && Math.abs(x.mm - own[i].mm) < 0.01),
      'violations({ mesh, skinned }) takes the positions given: ' + fmt([given.slice(0, 3), own.slice(0, 3)]));
    assert.throws(() => rigs.left.violations(res.left.touching, { mesh: true, skinned: new Float32Array(3) }), /skinned/,
      'violations: skinned of the wrong size');
    const p = rigs.left.skinned([1004]);
    assert.ok(!PANEL.inside(p[0], p[1], p[2]), 'song 0 at 222 093 529: the left thumb\'s vertex 1004 is in a control: ' + fmt(p));
    assert.strictEqual(driver.failures, 0, 'song 0 at 222 093 529: failures');
  }

  // The away hand is out of the other hand's way: the right hand on MUTE 0
  // and 2 (its palm past the panel's front edge), the left one away, their
  // palms apart.
  {
    const aim = a => { const c = PANEL.control(a); return { u: c.u, v: c.v, topMm: c.topMm }; };
    const plan = { touches: [], turns: [], keys: [], assign: {}, events: [], hands: {
      left: [{ t: 0, kind: 'away', fingers: {}, freeYaw: false }],
      right: [{ t: 0, kind: 'contact', freeYaw: false, fingers: {
        index: { aim: aim({ op: 'mute', track: 0 }), ref: { type: 'touch', id: 0 }, liftMm: 0 },
        ring: { aim: aim({ op: 'mute', track: 2 }), ref: { type: 'touch', id: 1 }, liftMm: 0 } } }] } };
    const res = new HandRig.Driver(rigs, plan).pose(0);
    const box = side => new T.Box3().setFromArray(rigs[side].skinned(rigs[side].PALM));
    assert.ok(res.left.away && !res.right.away && res.right.ok, 'away: ' + fmt(res));
    assert.ok(!box('left').intersectsBox(box('right')), 'away: the left palm is in the way of the right hand on MUTE 0 and 2');
  }

  // A contact held across two keyframes fitted apart (index and middle on
  // pads, then index and thumb with the hand turned, as a copy turns it)
  // is re-solved from the blended root: the index stays on its pad.
  {
    const aim = a => { const c = PANEL.control(a); return { u: c.u, v: c.v, topMm: c.topMm }; };
    const on = (a, id) => ({ aim: aim(a), ref: { type: 'touch', id }, liftMm: 0 });
    const index = on({ op: 'pad', track: 2, row: 0 }, 0);
    const plan = { touches: [], turns: [], keys: [], assign: {}, events: [], hands: {
      left: [{ t: 0, kind: 'away', fingers: {}, freeYaw: false }],
      right: [{ t: 1000000, kind: 'contact', fingers: { index, middle: on({ op: 'pad', track: 3, row: 0 }, 1) }, freeYaw: true },
        { t: 1100000, kind: 'contact', fingers: { index, thumb: on({ op: 'pad', track: 2, row: 3 }, 2) }, freeYaw: true }] } };
    const driver = new HandRig.Driver(rigs, plan), target = top({ op: 'pad', track: 2, row: 0 });
    for (let t = 1000000; t <= 1100000; t += 5000) {
      const res = driver.pose(t), d = offMm('right', 'index', target);
      assert.ok(res.right.ok && d <= 1, 'held across two fits at ' + t + ': the index is ' + fmt(d) + ' mm off');
    }
    assert.strictEqual(driver.failures, 0, 'held across two fits: failures');
  }

  // The window: the left hand holds step 0 while the right one taps steps 3
  // and 15 in turn, 160 000 apart. With windowUs = 160000 (prep at about
  // x5) the root is an average over the window: the right wrist moves less
  // than 25 mm between samples 5 000 apart, and less than half as far as
  // without the window.
  {
    const list = [];
    press(list, 'step', { index: 0 }, 800000, 2500000);
    for (let i = 0; i < 8; i++) press(list, 'step', { index: i % 2 ? 15 : 3 }, 1000000 + 160000 * i, 1030000 + 160000 * i);
    const { plan, driver } = drive(sorted(list));
    assert.ok(plan.touches.every(x => plan.assign['touch:' + x.id].hand === (x.key === 'step:0' ? 'left' : 'right')),
      'window: the left hand holds, the right one taps');
    const worst = windowUs => {
      let prev = null, most = 0;
      for (let t = 700000; t <= 2600000; t += 5000) {
        driver.pose(t, { windowUs });
        const w = wrist('right');
        if (prev) most = Math.max(most, w.distanceTo(prev) / MM);
        prev = w;
      }
      return most;
    };
    const exact = worst(0), smooth = worst(160000);
    assert.ok(smooth < 25 && smooth < exact / 2, 'window: the wrist moves up to ' + fmt(smooth) + ' mm in 5 000 (' +
      fmt(exact) + ' mm without the window)');
  }

  // peakSpeed catches an impossible move: the left hand holds step 0, the
  // right one taps pad (0, 0) and then pad (7, 7), 193 mm away, 20 000
  // after the first one's up.
  {
    const list = [];
    press(list, 'step', { index: 0 }, 1000000, 1600000);
    press(list, 'pad', { track: 0, row: 0 }, 1200000, 1210000);
    press(list, 'pad', { track: 7, row: 7 }, 1230000, 1240000);
    const { plan, driver } = drive(sorted(list));
    assert.deepStrictEqual(['step:0', 'pad:0:0', 'pad:7:7'].map(key => who(plan, key).hand), ['left', 'right', 'right'],
      'speed: the left hand holds, the right one plays both pads');
    const v = HandRig.peakSpeed(driver, 1100000, 1300000, 5000);
    assert.ok(v.right > 4, 'speed: peak ' + fmt(v) + ' m/s');
  }

  // The fits are cached by the rigs' scale too: rigs in the model's own
  // units (unitScale) driving a show first leave the page-scale rigs' poses
  // of that show as they are.
  {
    HandRig.Driver.cache.clear();
    const unit = { left: HandRig.create('left', { unitScale: true }), right: HandRig.create('right', { unitScale: true }) };
    assert.deepStrictEqual([unit.left.scale, rigs.right.scale], [1, 0.92 * 1000 * MM], 'the rigs\' scales');
    const plan = HandPlan.build(tapList);
    new HandRig.Driver(unit, plan).pose(1000000);
    const res = new HandRig.Driver(rigs, plan).pose(1000000), d = offMm('right', 'index', top({ op: 'pad', track: 6, row: 4 }));
    assert.ok(res.right.ok && d <= 1.5, 'after unit-scale rigs: the page-scale index is ' + fmt(d) + ' mm off');
  }

  // 1 000 poses over song 0 (fw2, pads), 50 000 apart: under 2 s from an
  // empty cache, under 0.5 s again with a new driver on the same show (the
  // fits from the cache), and no failures.
  {
    const show = Artist.buildShow(undefined, { song: 0, fw2: true, pads: true });
    const pass = () => {
      const driver = new HandRig.Driver(rigs, HandPlan.build(show.actions)), t0 = process.hrtime.bigint();
      for (let i = 0; i < 1000; i++) driver.pose(i * 50000);
      return { ms: Number(process.hrtime.bigint() - t0) / 1e6, failures: driver.failures };
    };
    HandRig.Driver.cache.clear();
    const cold = pass(), warm = pass();
    if (process.env.HANDS_TIMING) console.log('drive: song 0, 1 000 poses: ' + fmt(cold.ms) + ' ms cold, ' + fmt(warm.ms) + ' ms warm; ' + HandRig.Driver.cache.size + ' fits cached');
    assert.ok(cold.ms < 2000 && warm.ms < 500, 'song 0: 1 000 poses in ' + fmt(cold.ms) + ' ms, again in ' + fmt(warm.ms) + ' ms');
    assert.ok(HandRig.Driver.cache.size > 0 && HandRig.Driver.cache.size <= 4000, 'song 0: ' + HandRig.Driver.cache.size + ' fits cached');
    assert.deepStrictEqual([cold.failures, warm.failures], [0, 0], 'song 0: failures');
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
