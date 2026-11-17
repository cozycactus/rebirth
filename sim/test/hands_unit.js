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

for (const { name, fn } of sections) {
  try {
    fn();
  } catch (e) {
    console.log('hands_unit: FAIL in "' + name + '": ' + (e && e.stack || e));
    process.exit(1);
  }
}
console.log('hands_unit: ok');
