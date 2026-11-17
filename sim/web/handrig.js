// handrig.js: one hand of the simulator page's hands, as HandRig.create(side).
// The model's skeleton and skinned mesh (HAND_MODEL), posed by curls, placed
// by its root, and skinned on the CPU wherever vertex positions are needed.
//
// The model: a bone's local +Y runs along the bone toward the fingertip, X
// and Z are across it. Which way X points (to the palm or to the back of the
// hand) and which way a positive rotation moves a finger are measured per
// side at load, never assumed, since the left hand is a mirror image:
// - a positive curl (about local Z) moves the fingertip toward the palm;
// - a positive spread (about local X) moves the fingertip toward the thumb's
//   side of the hand, so equal numbers give mirror-image poses.
// The pose is rest · Rz(curl) · Rx(spread): the spread swings a bone across
// the finger whatever its curl. Fingers are thumb, index, middle, ring and
// pinky (the model's Little_* bones).
//
// Lengths: the model is in metres. setRoot() scales it to page units (× 0.92,
// an 18.7 cm hand) unless opts.unitScale, which the tests use.
(function (root) {
  'use strict';
  const D2R = Math.PI / 180;
  const HAND_SCALE = 0.92;
  const FINGERS = ['thumb', 'index', 'middle', 'ring', 'pinky'];
  const NAME = { thumb: 'Thumb', index: 'Index', middle: 'Middle', ring: 'Ring', pinky: 'Little' };
  // The bones a pose moves, in the order of curls[finger].
  const CHAIN = {
    thumb: ['Metacarpal', 'Proximal', 'Distal'],
    index: ['Metacarpal', 'Proximal', 'Intermediate', 'Distal'],
  };
  CHAIN.middle = CHAIN.ring = CHAIN.pinky = CHAIN.index;
  const SPREAD_AT = { thumb: 0, index: 1, middle: 1, ring: 1, pinky: 1 };   // in CHAIN
  const bone = (f, k) => NAME[f] + '_' + CHAIN[f][k];
  const tipOf = f => NAME[f] + '_Tip';

  // Dependencies, at call time: under Node required here; in the browser the
  // page's globals, which app.js loads before calling.
  function deps() {
    if (typeof module !== 'undefined' && module.exports) {
      require('./three.js');
      return { T: globalThis.THREE, MODEL: require('./handmodel.js'), PANEL: require('./handplan.js').PANEL };
    }
    return { T: root.THREE, MODEL: root.HAND_MODEL, PANEL: root.HandPlan.PANEL };
  }

  function bytes(b64) {
    const bin = atob(b64), a = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i);
    return a.buffer;
  }

  function create(side, opts) {
    opts = opts || {};
    if (side !== 'right' && side !== 'left') throw new Error('HandRig: no hand ' + side);
    const { T, MODEL, PANEL } = deps();
    const model = MODEL[side];
    const suffix = side === 'right' ? '_R' : '_L';

    // The skeleton: bones parented by index, in the rest pose of the model.
    const group = new T.Group();
    group.matrixAutoUpdate = false;
    const bones = model.bones.map(b => {
      const o = new T.Bone();
      o.name = b.name;
      o.position.fromArray(b.t);
      o.quaternion.fromArray(b.r);
      o.scale.fromArray(b.s);
      return o;
    });
    model.bones.forEach((b, i) => (b.parent < 0 ? group : bones[b.parent]).add(bones[i]));
    const byName = {};
    for (const b of bones) byName[b.name.slice(0, -suffix.length)] = b;
    const restQ = {};
    for (const name in byName) restQ[name] = byName[name].quaternion.clone();
    const boneOf = name => {
      const b = byName[name];
      if (!b) throw new Error('HandRig: no bone ' + name);
      return b;
    };

    // The skinned mesh, bound with an identity bind matrix.
    const ib = new Float32Array(bytes(model.inverseBind));
    const skeleton = new T.Skeleton(model.joints.map(i => bones[i]),
      model.joints.map((i, j) => new T.Matrix4().fromArray(ib, 16 * j)));
    const m = model.mesh;
    const P = new Float32Array(bytes(m.position));
    const SI = new Uint16Array(bytes(m.skinIndex));
    const SW = new Float32Array(bytes(m.skinWeight));
    const geometry = new T.BufferGeometry();
    geometry.setAttribute('position', new T.BufferAttribute(P, 3));
    geometry.setAttribute('normal', new T.BufferAttribute(new Float32Array(bytes(m.normal)), 3));
    geometry.setAttribute('uv', new T.BufferAttribute(new Float32Array(bytes(m.uv)), 2));
    geometry.setAttribute('skinIndex', new T.BufferAttribute(SI, 4));
    geometry.setAttribute('skinWeight', new T.BufferAttribute(SW, 4));
    geometry.setIndex(new T.BufferAttribute(new Uint16Array(bytes(m.index)), 1));
    const mesh = new T.SkinnedMesh(geometry);
    mesh.frustumCulled = false;
    group.add(mesh);
    mesh.bind(skeleton, new T.Matrix4());
    group.updateMatrixWorld(true);

    // ── Measured at load, in the rest pose (which is the bind pose), metres ──
    const at = name => boneOf(name).getWorldPosition(new T.Vector3());
    const axis = (name, column) => new T.Vector3().setFromMatrixColumn(boneOf(name).matrixWorld, column).normalize();
    const fingers = at('Middle_Proximal').sub(at('Wrist')).normalize();
    const radial = at('Index_Proximal').sub(at('Little_Proximal')).normalize();
    // A right hand with its fingers away and its thumb to the left shows its
    // back upward (back = fingers × radial); a left hand is the mirror image.
    const palmar = (side === 'right' ? radial.clone().cross(fingers) : fingers.clone().cross(radial)).normalize();

    // How far the tip of `f` moves along `dir` when `name` turns 10° about
    // its local `localAxis`.
    const probe = (f, name, localAxis, dir) => {
      const b = boneOf(name), q = b.quaternion.clone(), before = at(tipOf(f));
      b.quaternion.multiply(new T.Quaternion().setFromAxisAngle(localAxis, 10 * D2R));
      const moved = at(tipOf(f)).sub(before).dot(dir);
      b.quaternion.copy(q);
      return moved;
    };
    // One sign per side; a finger that disagrees means the model broke the
    // conventions above.
    const oneSign = (what, values) => {
      const s = Math.sign(values[0]);
      if (!values.every(x => Math.sign(x) === s && Math.abs(x) > 1e-4)) {
        throw new Error('HandRig: ' + side + ' hand: ' + what + ' differs between fingers: ' + values.join(' '));
      }
      return s;
    };
    const X = new T.Vector3(1, 0, 0), Z = new T.Vector3(0, 0, 1);
    const palmarX = oneSign('local X', FINGERS.map(f => axis(tipOf(f), 0).dot(palmar)));
    const curlSign = oneSign('curl', FINGERS.map(f => probe(f, bone(f, 1), Z, palmar)));
    const spreadSign = oneSign('spread', FINGERS.map(f => probe(f, bone(f, SPREAD_AT[f]), X, radial)));
    group.updateMatrixWorld(true);

    const jointOf = {};
    model.joints.forEach((i, j) => { jointOf[bones[i].name.slice(0, -suffix.length)] = j; });
    const n = m.count;
    const v3 = new T.Vector3();

    // The pad of a fingertip: the most palmar vertex within ±6 mm along and
    // ±3 mm across the tip bone, weighted at least 0.5 to the distal or tip.
    const padOf = {};
    for (const f of FINGERS) {
      const jt = jointOf[tipOf(f)], jd = jointOf[bone(f, CHAIN[f].length - 1)];
      const inv = skeleton.boneInverses[jt];
      let best = -1, most = -Infinity;
      for (let v = 0; v < n; v++) {
        let w = 0;
        for (let k = 0; k < 4; k++) if (SI[4 * v + k] === jt || SI[4 * v + k] === jd) w += SW[4 * v + k];
        if (w < 0.5) continue;
        v3.fromArray(P, 3 * v).applyMatrix4(inv);
        if (Math.abs(v3.y) > 0.006 || Math.abs(v3.z) > 0.003) continue;
        if (palmarX * v3.x > most) { most = palmarX * v3.x; best = v; }
      }
      if (best < 0) throw new Error('HandRig: no pad on the ' + side + ' ' + f);
      padOf[f] = best;
    }

    // The vertex under point `o` toward `dir`: of the vertices ahead of `o`,
    // the one nearest the ray from `o` along `dir`.
    const under = (o, dir) => {
      let best = -1, nearest = Infinity;
      for (let v = 0; v < n; v++) {
        v3.fromArray(P, 3 * v).sub(o);
        const a = v3.dot(dir), r2 = v3.lengthSq() - a * a;
        if (a > 0 && r2 < nearest) { nearest = r2; best = v; }
      }
      return best;
    };

    // The clearance vertices: per joint the nearest palmar and the nearest
    // dorsal vertex (a curled finger reaches down with the back of its
    // joints; Palm is a helper point outside the skin, not a joint), a 5 × 4
    // grid over the palm from the metacarpals' bases toward the knuckles, and
    // the five pads.
    const clearance = new Set();
    const add = v => { if (v >= 0) clearance.add(v); };
    for (const name in byName) {
      if (name === 'Palm') continue;
      add(under(at(name), axis(name, 0).multiplyScalar(palmarX)));
      add(under(at(name), axis(name, 0).multiplyScalar(-palmarX)));
    }
    const base = [at('Index_Metacarpal'), at('Little_Metacarpal')];
    const knuckle = [at('Index_Proximal'), at('Little_Proximal')];
    for (let a = 0; a < 5; a++) {
      for (let b = 0; b < 4; b++) {
        const along = (b + 1) / 5;
        const p = new T.Vector3().lerpVectors(base[0], knuckle[0], along)
          .lerp(new T.Vector3().lerpVectors(base[1], knuckle[1], along), a / 4);
        add(under(p, palmar));
      }
    }
    for (const f of FINGERS) add(padOf[f]);
    const CLEARANCE = Array.from(clearance);

    // The clearance vertices by what moves them: a finger's when at least
    // half their weight is on its bones (all of the thumb's; the others' from
    // the proximal on, as their metacarpals hardly move), else the palm's,
    // which only moving the root can lift.
    const ownerOfJoint = model.joints.map(i => {
      const name = bones[i].name.slice(0, -suffix.length);
      const f = FINGERS.find(g => name.startsWith(NAME[g] + '_'));
      return f && (f === 'thumb' || !name.endsWith('_Metacarpal')) ? f : 'palm';
    });
    const FINGER_CLEARANCE = { palm: [] };
    for (const f of FINGERS) FINGER_CLEARANCE[f] = [];
    for (const v of CLEARANCE) {
      const w = {};
      for (let k = 0; k < 4; k++) {
        const o = ownerOfJoint[SI[4 * v + k]];
        w[o] = (w[o] || 0) + SW[4 * v + k];
      }
      FINGER_CLEARANCE[FINGERS.find(f => w[f] >= 0.5) || 'palm'].push(v);
    }
    const PALM = FINGER_CLEARANCE.palm;

    // ── Posing ──
    const qa = new T.Quaternion();

    // curls: { thumb: [metacarpal, proximal, distal], others: [metacarpal,
    // proximal, intermediate, distal] } and spread: { finger: deg }, degrees.
    // A finger left out is at rest.
    function setCurls(curls, spread) {
      curls = curls || {};
      spread = spread || {};
      for (const f in curls) if (!CHAIN[f]) throw new Error('HandRig: no finger ' + f);
      for (const f in spread) if (!CHAIN[f]) throw new Error('HandRig: no finger ' + f);
      for (const f of FINGERS) {
        const c = curls[f] || [];
        for (let k = 0; k < CHAIN[f].length; k++) {
          const name = bone(f, k), b = byName[name];
          b.quaternion.copy(restQ[name]);
          if (c[k]) b.quaternion.multiply(qa.setFromAxisAngle(Z, curlSign * c[k] * D2R));
          if (k === SPREAD_AT[f] && spread[f]) b.quaternion.multiply(qa.setFromAxisAngle(X, spreadSign * spread[f] * D2R));
        }
      }
      group.updateMatrixWorld(true);
    }

    const scale = opts.unitScale ? 1 : HAND_SCALE * 1000 * PANEL.UNITS_PER_MM;
    const S = new T.Vector3(scale, scale, scale);
    function setRoot(position, quaternion) {
      group.matrix.compose(position, quaternion, S);
      group.matrixWorldNeedsUpdate = true;
      group.updateMatrixWorld(true);
    }

    // ── Skinning on the CPU, as three's vertex shader does it ──
    const skin = model.joints.map(() => new T.Matrix4());
    const pre = new T.Matrix4();
    function skinInto(indices, out) {
      group.updateMatrixWorld(true);
      pre.multiplyMatrices(mesh.matrixWorld, mesh.bindMatrixInverse);
      for (let j = 0; j < skin.length; j++) {
        skin[j].multiplyMatrices(skeleton.bones[j].matrixWorld, skeleton.boneInverses[j])
          .premultiply(pre).multiply(mesh.bindMatrix);
      }
      for (let k = 0; k < indices.length; k++) {
        const v = indices[k], px = P[3 * v], py = P[3 * v + 1], pz = P[3 * v + 2];
        let x = 0, y = 0, z = 0;
        for (let i = 0; i < 4; i++) {
          const w = SW[4 * v + i];
          if (!w) continue;
          const e = skin[SI[4 * v + i]].elements;
          x += w * (e[0] * px + e[4] * py + e[8] * pz + e[12]);
          y += w * (e[1] * px + e[5] * py + e[9] * pz + e[13]);
          z += w * (e[2] * px + e[6] * py + e[10] * pz + e[14]);
        }
        out[3 * k] = x; out[3 * k + 1] = y; out[3 * k + 2] = z;
      }
      return out;
    }
    const skinned = indices => skinInto(indices, new Float32Array(3 * indices.length));
    const one = new Float64Array(3);
    function pad(finger) {
      if (!(finger in padOf)) throw new Error('HandRig: no finger ' + finger);
      skinInto([padOf[finger]], one);
      return new T.Vector3(one[0], one[1], one[2]);
    }

    setRoot(new T.Vector3(), new T.Quaternion());   // at the origin, in page units
    return { side, root: group, mesh, bone: boneOf, setCurls, setRoot, pad, skinned, CLEARANCE, PALM,
      FINGER_CLEARANCE };
  }

  const api = { create };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.HandRig = api;
})(typeof window !== 'undefined' ? window : globalThis);
