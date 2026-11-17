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
//
// Fitting (contact, place, fit, pinch, clear, violations) works in page
// units, with the panel of HandPlan.PANEL. The hand's root is then a pivot
// (the middle finger's knuckle at rest), a yaw and a pitch: the fingers
// point along dir(yaw, pitch) and the back of the hand faces up (+z); only
// a pinch may roll it, thumb side up.
(function (root) {
  'use strict';
  const D2R = Math.PI / 180;
  const HAND_SCALE = 0.92;
  const FINGERS = ['thumb', 'index', 'middle', 'ring', 'pinky'];

  // Curl per joint, degrees, as setCurls() takes them: metacarpal, proximal,
  // intermediate, distal (thumb: metacarpal, proximal, distal). From the
  // approved prototype; pinch: the seed of the pinch fit (see GRIP).
  const POSES = {
    point: { thumb: [12, 14, 12], index: [0, 6, 14, 8], middle: [3, 62, 78, 38], ring: [4, 68, 80, 38], pinky: [5, 72, 80, 38] },
    fist: { thumb: [18, 26, 26], index: [5, 82, 95, 55], middle: [5, 85, 95, 55], ring: [6, 88, 95, 55], pinky: [8, 90, 95, 55] },
    lift: { thumb: [-25, -20, 0], index: [0, 4, 60, 38], middle: [1, 4, 62, 38], ring: [2, 6, 62, 38], pinky: [3, 8, 60, 38] },
    relax: { thumb: [5, 8, 10], index: [0, 18, 26, 14], middle: [1, 22, 30, 16], ring: [2, 25, 32, 16], pinky: [3, 28, 32, 16] },
  };
  POSES.pinch = { thumb: [8.75, 4.5, 4.5], index: [0, 3, 26, 17],
    middle: POSES.fist.middle.slice(), ring: POSES.fist.ring.slice(), pinky: POSES.fist.pinky.slice() };
  for (const p in POSES) {
    for (const f in POSES[p]) Object.freeze(POSES[p][f]);
    Object.freeze(POSES[p]);
  }
  Object.freeze(POSES);

  // The shoulders, world units: page x 350 and 1050, 130 mm in front of the
  // panel's front edge, 350 mm above the plate.
  const SHOULDER = { left: [350, -2000, 1633], right: [1050, -2000, 1633] };
  const ARM_PITCH = 20 * D2R;
  // The fit: damping, the pull toward the seed pose (mm per radian), joint
  // limits (radians), the yaw window about the seed and the pitch window
  // (the brief sets none for the pitch; ±30° keeps the hand from standing
  // on its fingertips or pointing up).
  const LAMBDA = 0.1, PULL = 0.05, ITERATIONS = 40;
  const SPREAD = 20 * D2R, FLEX = [0, 95 * D2R], THUMB_BASE = [-30 * D2R, 60 * D2R];
  const YAW = 30 * D2R, FREE_YAW = 100 * D2R, PITCH = 30 * D2R;
  // The pinch: the knob's radius and the grip's height (mm); at twist 0 the
  // line from thumb to index is the arm's heading turned GRIP toward the
  // little finger's side (right hand: clockwise seen from above): the thumb
  // grips the knob's near side on the thumb's side (the right thumb
  // near-left), the index its far side, and the wrist stays nearer the
  // player than the knob. The hand may roll up to PINCH_ROLL, thumb side up.
  // GRIP is the bearing of the grip the hand takes by itself, so that at
  // twist 0 the fitted hand lies along the arm and the twist window (±20°)
  // turns the forearm no further than the knob: at 34° the fit turns the
  // right hand 0.8° and the left 1.1° off armDir (31° to 38° keep both
  // within 5°). From POSES.pinch (the thumb nearly straight, the index bent
  // at its middle joint) both hands settle on one grip: the thumb spread
  // 14° to 15° toward the index, the index curled about 44° at its middle
  // joint, the wrist 81 mm (right) and 68 mm (left) up, the thumb's
  // underside 0.6 and 0.3 mm above the plate. The fit's basin is narrow:
  // half a degree on either thumb angle still fits clean, three quarters of
  // a degree more on the thumb's joints sinks it under the plate, and a
  // thumb hanging about 100 mm under the knuckles (the first pinch pose)
  // leaves the wrist 91 mm up and the thumb 3 mm under the plate. That is
  // enough: there are only four pinch fits (two hands, two knobs; a twist
  // turns a fit rigidly), fixed and tested.
  const KNOB_R_MM = 10, PINCH_MM = 12, GRIP = 34 * D2R, PINCH_ROLL = 20 * D2R;
  // A fit's seed for the touching fingers: nearly straight (the distal at
  // two thirds of the intermediate, as the fit keeps it).
  const STRAIGHT = { thumb: [0, 5, 5], index: [0, 5, 6, 4], middle: [0, 5, 6, 4], ring: [0, 5, 6, 4], pinky: [0, 5, 6, 4] };
  // A fingertip hooking under gets its last segment's descent capped (the
  // fit's residual row CAP_WEIGHT mm per radian over it).
  const CAP = 45 * D2R, CAP_WEIGHT = 100;
  // A free finger curls until it is this far clear of the controls.
  const CURL_MARGIN_MM = 0.5;
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
    // grid over the palm from the metacarpals' bases toward the knuckles, the
    // five pads, and each last segment's underside (below).
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
    // The underside of each last segment: the vertices that become its
    // lowest as it tilts, level, half down and straight down (the deepest,
    // in the distal bone's frame, toward the palm, at 45° toward the tip
    // and toward the tip), among those weighted at least 0.5 to the distal
    // or tip.
    for (const f of FINGERS) {
      const jt = jointOf[tipOf(f)], jd = jointOf[bone(f, CHAIN[f].length - 1)], inv = skeleton.boneInverses[jd];
      for (const tilt of [0, 45, 90]) {
        const c = Math.cos(tilt * D2R), s = Math.sin(tilt * D2R);
        let best = -1, most = -Infinity;
        for (let v = 0; v < n; v++) {
          let w = 0;
          for (let k = 0; k < 4; k++) if (SI[4 * v + k] === jt || SI[4 * v + k] === jd) w += SW[4 * v + k];
          if (w < 0.5) continue;
          v3.fromArray(P, 3 * v).applyMatrix4(inv);
          const d = palmarX * v3.x * c + v3.y * s;
          if (d > most) { most = d; best = v; }
        }
        add(best);
      }
    }
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

    // The hand's own axes at rest, in the model: along the Wrist bone (+Y,
    // as the prototype's press points the hand; the line to the middle
    // knuckle is 9.6° more dorsal), out of the back of the hand, across; and
    // the pivot the root turns about.
    const alongM = axis('Wrist', 1);
    const backM = palmar.clone().negate().addScaledVector(alongM, palmar.dot(alongM)).normalize();
    const fromModel = new T.Matrix4().makeBasis(backM, alongM, new T.Vector3().crossVectors(backM, alongM)).transpose();
    const PIVOT = at('Middle_Proximal');

    // ── Posing ──
    const qa = new T.Quaternion();
    // The current pose, degrees; and the targets of the last place, fit or
    // pinch (null once setCurls or setRoot moves the hand by hand).
    const now = { curls: {}, spread: {} };
    for (const f of FINGERS) { now.curls[f] = CHAIN[f].map(() => 0); now.spread[f] = 0; }
    let last = null;

    function writeFinger(f) {
      const c = now.curls[f];
      for (let k = 0; k < CHAIN[f].length; k++) {
        const name = bone(f, k), b = byName[name];
        b.quaternion.copy(restQ[name]);
        if (c[k]) b.quaternion.multiply(qa.setFromAxisAngle(Z, curlSign * c[k] * D2R));
        if (k === SPREAD_AT[f] && now.spread[f]) b.quaternion.multiply(qa.setFromAxisAngle(X, spreadSign * now.spread[f] * D2R));
      }
    }
    function writePose(curls, spread) {
      for (const f of FINGERS) {
        const c = (curls && curls[f]) || [];
        for (let k = 0; k < CHAIN[f].length; k++) now.curls[f][k] = c[k] || 0;
        now.spread[f] = (spread && spread[f]) || 0;
        writeFinger(f);
      }
    }

    // curls: { thumb: [metacarpal, proximal, distal], others: [metacarpal,
    // proximal, intermediate, distal] } and spread: { finger: deg }, degrees.
    // A finger left out is at rest.
    function setCurls(curls, spread) {
      curls = curls || {};
      spread = spread || {};
      for (const f in curls) if (!CHAIN[f]) throw new Error('HandRig: no finger ' + f);
      for (const f in spread) if (!CHAIN[f]) throw new Error('HandRig: no finger ' + f);
      writePose(curls, spread);
      last = null;
      group.updateMatrixWorld(true);
    }

    const scale = opts.unitScale ? 1 : HAND_SCALE * 1000 * PANEL.UNITS_PER_MM;
    const S = new T.Vector3(scale, scale, scale);
    function setRoot(position, quaternion) {
      group.matrix.compose(position, quaternion, S);
      group.matrixWorldNeedsUpdate = true;
      last = null;
      group.updateMatrixWorld(true);
    }

    // The root as the fit sees it: the middle knuckle's rest point (PIVOT)
    // at pivot (world units), the yaw (about +z, counter-clockwise seen from
    // above, 0 = fingers toward +y), the pitch (fingers down) and the roll
    // about the fingers' direction (thumb side up; 0 = back of the hand
    // straight up), radians.
    const UPV = new T.Vector3(0, 0, 1);
    const pose = { pivot: new T.Vector3(), yaw: 0, pitch: 0, roll: 0 };
    // Which way across the hand (back × along) the thumb lies.
    const thumbSide = Math.sign(radial.dot(new T.Vector3().crossVectors(backM, alongM)));
    const dirOf = (yaw, pitch) => new T.Vector3(-Math.sin(yaw) * Math.cos(pitch), Math.cos(yaw) * Math.cos(pitch),
      -Math.sin(pitch));
    const yawOf = d => Math.atan2(-d.x, d.y);
    const pitchOf = d => Math.asin(Math.max(-1, Math.min(1, -d.z / d.length())));
    const rx = new T.Vector3(), ry = new T.Vector3(), rz = new T.Vector3(), rm = new T.Matrix4();
    const rq = new T.Quaternion(), rp = new T.Vector3();
    function writeRoot() {
      ry.copy(dirOf(pose.yaw, pose.pitch));
      rx.copy(UPV).addScaledVector(ry, -ry.z).normalize();       // the back of the hand
      rz.crossVectors(rx, ry);
      if (pose.roll) {
        // Turn about the fingers' direction so the thumb's side rises.
        const a = thumbSide * pose.roll, c = Math.cos(a), s = Math.sin(a), x0 = rx.clone();
        rx.multiplyScalar(c).addScaledVector(rz, -s);
        rz.multiplyScalar(c).addScaledVector(x0, s);
      }
      rq.setFromRotationMatrix(rm.makeBasis(rx, ry, rz).multiply(fromModel));
      rp.copy(PIVOT).multiplyScalar(scale).applyQuaternion(rq);
      group.matrix.compose(rp.negate().add(pose.pivot), rq, S);
      group.matrixWorldNeedsUpdate = true;
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

    // ── Fitting, in page units: residuals and translations in mm, angles in
    // radians, so that the damping and the pull compare like with like ──
    const MM = PANEL.UNITS_PER_MM;

    // From the shoulder of this side toward the target, flattened, then
    // pitched 20° down.
    function armDir(target) {
      const s = SHOULDER[side];
      const h = new T.Vector3(target.x - s[0], target.y - s[1], 0).normalize();
      return h.multiplyScalar(Math.cos(ARM_PITCH)).setZ(-Math.sin(ARM_PITCH));
    }

    // A fitted finger's three angles (radians): the four fingers' spread
    // and flex at the proximal and flex at the intermediate, the distal at
    // two thirds of it; the thumb's spread and flex at its metacarpal and
    // one flex shared by its proximal and distal.
    const anglesOf = f => {
      const c = now.curls[f];
      return f === 'thumb' ? [now.spread.thumb * D2R, c[0] * D2R, (c[1] + c[2]) / 2 * D2R]
        : [now.spread[f] * D2R, c[1] * D2R, c[2] * D2R];
    };
    const limitsOf = f => [[-SPREAD, SPREAD], f === 'thumb' ? THUMB_BASE : FLEX, FLEX];
    function writeAngles(f, a, b, c) {
      const cu = now.curls[f];
      now.spread[f] = a / D2R;
      if (f === 'thumb') { cu[0] = b / D2R; cu[1] = cu[2] = c / D2R; }
      else { cu[1] = b / D2R; cu[2] = c / D2R; cu[3] = cu[2] * 2 / 3; }
      writeFinger(f);
    }
    const clamp = (x, lo, hi) => Math.min(hi, Math.max(lo, x));

    // Small dense solve, A (n × n, row-major) x = b, partial pivoting.
    function solveLinear(A, b, n) {
      for (let c = 0; c < n; c++) {
        let p = c;
        for (let r = c + 1; r < n; r++) if (Math.abs(A[r * n + c]) > Math.abs(A[p * n + c])) p = r;
        if (p !== c) {
          for (let k = 0; k < n; k++) { const t = A[c * n + k]; A[c * n + k] = A[p * n + k]; A[p * n + k] = t; }
          const t = b[c]; b[c] = b[p]; b[p] = t;
        }
        const d = A[c * n + c];
        for (let r = c + 1; r < n; r++) {
          const k = A[r * n + c] / d;
          if (!k) continue;
          for (let j = c; j < n; j++) A[r * n + j] -= k * A[c * n + j];
          b[r] -= k * b[c];
        }
      }
      for (let c = n - 1; c >= 0; c--) {
        let s = b[c];
        for (let j = c + 1; j < n; j++) s -= A[c * n + j] * b[j];
        b[c] = s / A[c * n + c];
      }
      return b;
    }

    // ── Contact: where a fingertip touches ──
    // The vertices of a finger's last two segments: at least half their
    // weight on its distal or tip bone.
    const TIP_VERTS = {}, TIP_SET = {};
    for (const f of FINGERS) {
      const jt = jointOf[tipOf(f)], jd = jointOf[bone(f, CHAIN[f].length - 1)], list = [];
      for (let v = 0; v < n; v++) {
        let w = 0;
        for (let k = 0; k < 4; k++) if (SI[4 * v + k] === jt || SI[4 * v + k] === jd) w += SW[4 * v + k];
        if (w >= 0.5) list.push(v);
      }
      TIP_VERTS[f] = list;
      TIP_SET[f] = new Set(list);
    }
    const DOWN = Object.freeze({ x: 0, y: 0, z: -1 });
    // The deepest vertex of finger f's last two segments along the contact
    // direction `toward`: a unit vector (default −z, onto a top) or
    // {axis: point}, toward the vertical axis through point (a knob's side).
    function contactVertex(f, toward) {
      const verts = TIP_VERTS[f], p = skinInto(verts, new Float64Array(3 * verts.length));
      const axis = toward && toward.axis, d = axis ? null : toward || DOWN;
      let best = -1, most = -Infinity;
      for (let k = 0; k < verts.length; k++) {
        const x = p[3 * k], y = p[3 * k + 1], z = p[3 * k + 2];
        const deep = axis ? -Math.hypot(x - axis.x, y - axis.y) : x * d.x + y * d.y + z * d.z;
        if (deep > most) { most = deep; best = verts[k]; }
      }
      return best;
    }
    // The contact point of `finger` in the current pose, world units.
    function contact(finger, toward) {
      if (!TIP_VERTS[finger]) throw new Error('HandRig: no finger ' + finger);
      skinInto([contactVertex(finger, toward)], one);
      return new T.Vector3(one[0], one[1], one[2]);
    }
    // A unit vector into the target's control from a target: down, or
    // toward the knob's axis.
    const inward = (target, toward) => {
      if (toward && toward.axis) return new T.Vector3(toward.axis.x - target.x, toward.axis.y - target.y, 0).normalize();
      const d = toward || DOWN;
      return new T.Vector3(d.x, d.y, d.z);
    };

    // How far the last segment of finger f descends toward the plate,
    // radians: 0 level, 90° straight down, past 90° when it hooks back. Its
    // horizontal run is signed along the finger's own heading (from its
    // metacarpal to its proximal joint, flattened), so a hook does not read
    // as a shallower slope. Read after skinInto has updated the bones.
    const descentOf = f => {
      const a = byName[bone(f, CHAIN[f].length - 1)].matrixWorld.elements, b = byName[tipOf(f)].matrixWorld.elements;
      const m = byName[bone(f, 0)].matrixWorld.elements, q = byName[bone(f, 1)].matrixWorld.elements;
      const hx = q[12] - m[12], hy = q[13] - m[13], h = Math.hypot(hx, hy) || 1;
      return Math.atan2(a[14] - b[14], ((b[12] - a[12]) * hx + (b[13] - a[13]) * hy) / h);
    };

    // One damped-least-squares solve: the contact vertices job.verts of
    // job.fingers onto job.targets, over the root (pivot in mm, no lower
    // than job.floorMm when given; yaw and pitch) and three angles per
    // finger, from the current pose, within the limits, pulled toward
    // job.seed (radians per finger); a finger in job.capped keeps its last
    // segment's descent within CAP (a stiff one-sided row). Numeric Jacobian.
    function solve(job) {
      const list = job.fingers, nf = list.length, nr = job.rollMax ? 6 : 5, n = nr + 3 * nf;
      const capped = list.filter(f => job.capped && job.capped.includes(f)), m = 6 * nf + capped.length;
      const x = new Float64Array(n), lo = new Float64Array(n), hi = new Float64Array(n), seed = new Float64Array(3 * nf);
      x[0] = pose.pivot.x / MM; x[1] = pose.pivot.y / MM; x[2] = pose.pivot.z / MM;
      x[3] = pose.yaw; x[4] = pose.pitch;
      lo.fill(-Infinity, 0, 3); hi.fill(Infinity, 0, 3);
      if (job.floorMm !== undefined) lo[2] = job.floorMm;
      lo[3] = job.yaw0 - job.yawWindow; hi[3] = job.yaw0 + job.yawWindow;
      lo[4] = job.pitch0 - PITCH; hi[4] = job.pitch0 + PITCH;
      if (job.rollMax) { x[5] = pose.roll; lo[5] = 0; hi[5] = job.rollMax; }
      list.forEach((f, i) => {
        const a = anglesOf(f), l = limitsOf(f);
        for (let j = 0; j < 3; j++) {
          const k = nr + 3 * i + j;
          lo[k] = l[j][0]; hi[k] = l[j][1];
          x[k] = clamp(a[j], lo[k], hi[k]);
          seed[3 * i + j] = job.seed[f][j];
        }
      });
      const verts = list.map(f => job.verts[f]), at3 = new Float64Array(3 * nf), aim = new Float64Array(3 * nf);
      list.forEach((f, i) => { aim[3 * i] = job.targets[f].x; aim[3 * i + 1] = job.targets[f].y; aim[3 * i + 2] = job.targets[f].z; });
      const apply = v => {
        pose.pivot.set(v[0] * MM, v[1] * MM, v[2] * MM);
        pose.yaw = v[3]; pose.pitch = v[4];
        if (job.rollMax) pose.roll = v[5];
        writeRoot();
        for (let i = 0; i < nf; i++) writeAngles(list[i], v[nr + 3 * i], v[nr + 3 * i + 1], v[nr + 3 * i + 2]);
      };
      // Residuals: the contacts' offsets (mm), the pull rows, the caps.
      const residual = (v, r) => {
        apply(v);
        skinInto(verts, at3);
        let cost = 0;
        for (let k = 0; k < 3 * nf; k++) { r[k] = (at3[k] - aim[k]) / MM; cost += r[k] * r[k]; }
        for (let k = 0; k < 3 * nf; k++) { r[3 * nf + k] = PULL * (v[nr + k] - seed[k]); cost += r[3 * nf + k] * r[3 * nf + k]; }
        capped.forEach((f, i) => {
          const over = Math.max(0, descentOf(f) - CAP);
          r[6 * nf + i] = CAP_WEIGHT * over;
          cost += r[6 * nf + i] * r[6 * nf + i];
        });
        return cost;
      };
      const r = new Float64Array(m), rt = new Float64Array(m), J = new Float64Array(m * n);
      const xt = new Float64Array(n), g = new Float64Array(n), step = new Float64Array(n);
      const H = 1e-6, worst = rr => {
        let w = 0;
        for (let i = 0; i < nf; i++) w = Math.max(w, Math.hypot(rr[3 * i], rr[3 * i + 1], rr[3 * i + 2]));
        for (let i = 0; i < capped.length; i++) w = Math.max(w, rr[6 * nf + i]);
        return w;
      };
      // Until the contacts are on their targets and the steps have settled,
      // so that the pull can still act once they are (it drifts slowly: the
      // damping outweighs it).
      let cost = residual(x, r);
      for (let it = 0, moved = Infinity; it < (job.iterations || ITERATIONS) && (worst(r) > 0.005 || moved > 1e-6); it++) {
        for (let j = 0; j < n; j++) {
          xt.set(x);
          xt[j] += H;
          residual(xt, rt);
          for (let k = 0; k < m; k++) J[k * n + j] = (rt[k] - r[k]) / H;
        }
        // The variables free to move: not held at a limit by the gradient.
        const free = [];
        for (let j = 0; j < n; j++) {
          let s = 0;
          for (let k = 0; k < m; k++) s += J[k * n + j] * r[k];
          g[j] = s;
          if (!((x[j] <= lo[j] && s > 0) || (x[j] >= hi[j] && s < 0))) free.push(j);
        }
        const nfree = free.length, A = new Float64Array(nfree * nfree), b = new Float64Array(nfree);
        for (let a = 0; a < nfree; a++) {
          for (let c = a; c < nfree; c++) {
            let s = 0;
            for (let k = 0; k < m; k++) s += J[k * n + free[a]] * J[k * n + free[c]];
            A[a * nfree + c] = A[c * nfree + a] = s;
          }
          A[a * nfree + a] += LAMBDA * LAMBDA;
          b[a] = -g[free[a]];
        }
        solveLinear(A, b, nfree);
        // At most 15° or 20 mm a step; then halve until the cost drops.
        step.fill(0);
        let big = 1;
        free.forEach((j, a) => {
          step[j] = b[a];
          big = Math.max(big, j < 3 ? Math.abs(b[a]) / 20 : Math.abs(b[a]) / (15 * D2R));
        });
        let taken = false;
        for (let h = 1 / big, tries = 0; tries < 6 && !taken; h /= 2, tries++) {
          moved = 0;
          for (let j = 0; j < n; j++) {
            xt[j] = clamp(x[j] + h * step[j], lo[j], hi[j]);
            moved = Math.max(moved, Math.abs(xt[j] - x[j]));
          }
          const c = residual(xt, rt);
          if (c < cost) { x.set(xt); r.set(rt); cost = c; taken = true; }
        }
        if (!taken) break;
      }
      apply(x);
      group.updateMatrixWorld(true);
    }

    // The contacts' distances to their targets (mm), re-picked.
    const residualsOf = (targets, toward) => {
      const out = {};
      for (const f in targets) out[f] = contact(f, toward[f]).distanceTo(targets[f]) / MM;
      return out;
    };
    // Solves with the contact vertices picked in the current pose, re-picks
    // them and solves again while they change (at most 3 rounds).
    function solveRounds(job) {
      for (let round = 0; round < 3; round++) {
        const verts = {};
        let same = round > 0;
        for (const f of job.fingers) {
          verts[f] = contactVertex(f, job.toward[f]);
          if (!job.verts || verts[f] !== job.verts[f]) same = false;
        }
        if (same) break;
        job.verts = verts;
        solve(job);
      }
      return residualsOf(job.targets, job.toward);
    }
    // The fingers of `list` hooking under: a clearance vertex below the plate.
    const hooked = list => list.filter(f => {
      const vs = FINGER_CLEARANCE[f], p = skinInto(vs, new Float64Array(3 * vs.length));
      for (let k = 0; k < vs.length; k++) if (p[3 * k + 2] < 0) return true;
      return false;
    });

    const result = (residualMm, limitMm, yaw0) => ({
      ok: Object.values(residualMm).every(d => d <= limitMm), residualMm,
      yawDeg: (pose.yaw - yaw0) / D2R, pitchDeg: pose.pitch / D2R, rollDeg: pose.roll / D2R, dir: dirOf(pose.yaw, pose.pitch),
    });
    const copyTargets = targets => {
      const out = {};
      for (const f in targets) {
        if (!CHAIN[f]) throw new Error('HandRig: no finger ' + f);
        out[f] = new T.Vector3(targets[f].x, targets[f].y, targets[f].z);
      }
      return out;
    };
    const seedOf = fingers => {
      const out = {};
      for (const f of fingers) out[f] = anglesOf(f).map((a, j) => clamp(a, limitsOf(f)[j][0], limitsOf(f)[j][1]));
      return out;
    };
    const centreOf = points => {
      const c = new T.Vector3();
      for (const p of points) c.add(p);
      return c.multiplyScalar(1 / points.length);
    };
    // Turns the hand by `angle` (counter-clockwise seen from above) about
    // the vertical axis through `point`.
    function turnAbout(point, angle) {
      const dx = pose.pivot.x - point.x, dy = pose.pivot.y - point.y, c = Math.cos(angle), s = Math.sin(angle);
      pose.pivot.x = point.x + c * dx - s * dy;
      pose.pivot.y = point.y + s * dx + c * dy;
      pose.yaw += angle;
      writeRoot();
      group.updateMatrixWorld(true);
    }

    // A press: the pose `point`, the root turned along dir (back of the
    // hand up), then moved so that the contact point of `finger` (straight
    // down) is on `target`. Moving the hand does not change which vertex is
    // deepest, so one round settles it.
    function place(finger, target, dir) {
      const targets = copyTargets({ [finger]: target }), toward = { [finger]: DOWN };
      writePose(POSES.point, {});
      pose.yaw = yawOf(dir); pose.pitch = pitchOf(dir); pose.roll = 0;
      pose.pivot.set(0, 0, 0);
      writeRoot();
      for (let round = 0; round < 3; round++) {
        const off = targets[finger].clone().sub(contact(finger, DOWN));
        if (off.length() < 1e-9) break;
        pose.pivot.add(off);
        writeRoot();
      }
      group.updateMatrixWorld(true);
      last = { targets, toward, seed: seedOf([finger]), yaw0: pose.yaw, pitch0: pose.pitch, yawWindow: YAW };
      return result(residualsOf(targets, toward), 1, pose.yaw);
    }

    // The fit, from the seed pose `curls` and the contact directions
    // `toward` ({finger: direction}).
    function fitFrom(targets, opts, curls, toward) {
      const list = Object.keys(targets);
      if (!list.length) throw new Error('HandRig: fit without targets');
      writePose(curls, {});
      const middle = centreOf(list.map(f => targets[f]));
      const dir = opts.dir || armDir(middle), yawWindow = opts.freeYaw ? FREE_YAW : YAW;
      const yaw0 = yawOf(dir), pitch0 = pitchOf(dir);
      pose.yaw = yaw0; pose.pitch = pitch0; pose.roll = 0;
      pose.pivot.set(0, 0, 0);
      writeRoot();
      const seed = seedOf(list);
      for (const f of list) writeAngles(f, ...seed[f]);
      // The contacts' centre onto the targets' centre (twice: a contact
      // toward a knob's axis depends on where the hand is).
      const centre = () => {
        pose.pivot.add(middle).sub(centreOf(list.map(f => contact(f, toward[f]))));
        writeRoot();
        group.updateMatrixWorld(true);
      };
      centre();
      centre();
      // The yaw of the best rigid alignment of the contacts with the
      // targets, within the yaw window.
      if (list.length > 1) {
        let dot = 0, cross = 0;
        const c = centreOf(list.map(f => contact(f, toward[f])));
        for (const f of list) {
          const p = contact(f, toward[f]).sub(c), t = targets[f].clone().sub(middle);
          dot += p.x * t.x + p.y * t.y;
          cross += p.x * t.y - p.y * t.x;
        }
        turnAbout(middle, clamp(yaw0 + Math.atan2(cross, dot), yaw0 - yawWindow, yaw0 + yawWindow) - pose.yaw);
        centre();
      }
      last = { targets, toward, seed, yaw0, pitch0, yawWindow, rollMax: opts.rollMax || 0, fingers: list };
      const job = Object.assign({ iterations: opts.iterations }, last);
      let residualMm = solveRounds(job);
      // A fingertip still hooking under: cap its last segment's descent.
      const hooks = hooked(list);
      if (hooks.length) {
        last.capped = job.capped = hooks;
        job.verts = null;
        residualMm = solveRounds(job);
      }
      const out = result(residualMm, 1, yaw0);
      out.capped = hooks;
      return out;
    }

    // Several contacts on their targets ({finger: Vector3}) in one solve,
    // from nearly straight fingers (the little finger bent as in relax: from
    // straight, the shortest finger's reach stalls the solve, or drops the
    // hand until the curled index and middle go under the plate; when that
    // misses, again with the ring finger bent too; the others as in point),
    // the root
    // turned along opts.dir (default armDir of the targets' centre), then
    // to the best rigid alignment of the contacts with the targets, and
    // moved onto them. opts.freeYaw widens the yaw window to ±100°;
    // opts.toward gives a finger another contact direction (as contact()
    // takes it; default down). ok iff every contact is within 1 mm. The
    // result also gives the fitted yaw (from opts.dir, counter-clockwise
    // seen from above), the pitch, the roll, dir, and `capped`: the fingers
    // whose last segment hooked under the plate and was capped.
    function fit(targets, opts) {
      opts = opts || {};
      targets = copyTargets(targets);
      const curls = {}, toward = {};
      for (const f of FINGERS) curls[f] = f in targets ? (f === 'pinky' ? POSES.relax.pinky : STRAIGHT[f]) : POSES.point[f];
      for (const f in targets) toward[f] = (opts.toward && opts.toward[f]) || DOWN;
      const first = fitFrom(targets, opts, curls, toward);
      if (first.ok || !('ring' in targets)) return first;
      // Missed: once more with the ring finger bent too; the better stays.
      const again = Object.assign({}, curls, { ring: POSES.relax.ring });
      const second = fitFrom(targets, opts, again, toward);
      const worst = r => Math.max(...Object.values(r.residualMm));
      return second.ok || worst(second) < worst(first) ? second : fitFrom(targets, opts, curls, toward);
    }

    // Thumb and index on the side of encoder enc's knob, opposite each
    // other on its rim, 12 mm above the plate: the grip fitted at twist 0
    // (from POSES.pinch and the arm's direction, the hand free to roll up
    // to 20° thumb side up), then turned about the
    // knob's axis by twistDeg, clockwise seen from above (as the knob turns
    // up). ok iff both contacts are within 2 mm. The result also carries
    // the targets.
    function pinch(enc, twistDeg) {
      const c = PANEL.control({ op: 'encsw', enc });
      const middle = new T.Vector3(c.u, -c.v, PINCH_MM * MM), dir = armDir(middle);
      const grip = Math.atan2(dir.y, dir.x) + (side === 'right' ? -GRIP : GRIP);
      const across = new T.Vector3(Math.cos(grip), Math.sin(grip), 0).multiplyScalar(KNOB_R_MM * MM);
      const targets = { thumb: middle.clone().sub(across), index: middle.clone().add(across) };
      const axis = { axis: middle.clone() }, toward = { thumb: axis, index: axis };
      fitFrom(targets, { dir, rollMax: PINCH_ROLL }, POSES.pinch, toward);
      const turn = -(twistDeg || 0) * D2R;
      if (turn) {
        turnAbout(middle, turn);
        for (const f in targets) targets[f].sub(middle).applyAxisAngle(UPV, turn).add(middle);
        last.yaw0 += turn;
      }
      const out = result(residualsOf(targets, toward), 2, last.yaw0 - turn);
      out.targets = targets;
      return out;
    }

    // How far (mm) world point p must move to leave every control and the
    // space below the plate: up, and the least of up, sideways and (by a
    // knob) outward. Measured with PANEL.inside, 0.25 mm steps refined by
    // bisection.
    const KNOBS = [0, 1].map(enc => PANEL.control({ op: 'encsw', enc }));
    function escape(x, y, z, dx, dy, dz, maxMm) {
      let a = 0;
      for (let d = 0.25; d <= maxMm + 1e-9; d += 0.25) {
        if (!PANEL.inside(x + dx * d * MM, y + dy * d * MM, z + dz * d * MM)) {
          let b = d;
          for (let i = 0; i < 10; i++) {
            const h = (a + b) / 2;
            if (PANEL.inside(x + dx * h * MM, y + dy * h * MM, z + dz * h * MM)) a = h; else b = h;
          }
          return b;
        }
        a = d;
      }
      return Infinity;
    }
    function depth(x, y, z) {
      const up = escape(x, y, z, 0, 0, 1, Math.max(0, -z / MM) + 20);
      let least = up;
      if (z >= 0) {
        for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) least = Math.min(least, escape(x, y, z, dx, dy, 0, 12));
        for (const k of KNOBS) {
          const du = x - k.u, dv = y + k.v, r = Math.hypot(du, dv);
          if (r > 1e-9 && r <= (KNOB_R_MM + 1) * MM) least = Math.min(least, escape(x, y, z, du / r, dv / r, 0, 12));
        }
      }
      return { mm: least, upMm: up };
    }
    // Whether world point (x, y, z), inside a control, is inside the one
    // under finger f's target: the controls are convex and apart, so the
    // segment to a point just inside the target stays inside only then.
    function inOwnTarget(f, x, y, z) {
      const t = last.targets[f], q = inward(t, last.toward[f]).multiplyScalar(0.3 * MM).add(t);
      for (let i = 1; i <= 40; i++) {
        const s = i / 40;
        if (!PANEL.inside(x + (q.x - x) * s, y + (q.y - y) * s, z + (q.z - z) * s)) return false;
      }
      return true;
    }

    // The clearance vertices inside a control or below the plate, deepest
    // first. A vertex of a touching finger's last two segments may sink up
    // to 1 mm into its own target (a fingertip pressed into silicone);
    // nothing is exempt below the plate.
    function violationsUp() {
      const p = skinInto(CLEARANCE, new Float64Array(3 * CLEARANCE.length)), out = [];
      CLEARANCE.forEach((v, k) => {
        const x = p[3 * k], y = p[3 * k + 1], z = p[3 * k + 2];
        if (!PANEL.inside(x, y, z)) return;
        const d = depth(x, y, z);
        if (last && z >= 0 && d.mm <= 1) {
          for (const f in last.targets) if (TIP_SET[f].has(v) && inOwnTarget(f, x, y, z)) return;
        }
        out.push({ vertex: v, mm: d.mm, upMm: d.upMm });
      });
      return out.sort((a, b) => b.mm - a.mm);
    }
    const violations = () => violationsUp().map(({ vertex, mm }) => ({ vertex, mm }));

    // Curls free finger f toward the fist (the thumb toward lift) just
    // enough, found by bisection, that each of its clearance vertices is
    // CURL_MARGIN_MM above any control or the plate under it; all the way
    // when even that is not enough.
    function curlAway(f) {
      const verts = FINGER_CLEARANCE[f], buf = new Float64Array(3 * verts.length), margin = CURL_MARGIN_MM * MM;
      const hit = () => {
        skinInto(verts, buf);
        for (let k = 0; k < verts.length; k++) if (PANEL.inside(buf[3 * k], buf[3 * k + 1], buf[3 * k + 2] - margin)) return true;
        return false;
      };
      if (!hit()) return;
      const from = now.curls[f].slice(), to = f === 'thumb' ? POSES.lift.thumb : POSES.fist[f];
      const at = k => {
        for (let j = 0; j < from.length; j++) now.curls[f][j] = from[j] + (to[j] - from[j]) * k;
        writeFinger(f);
      };
      let lo = 0, hi = 1;
      at(1);
      if (hit()) return;
      for (let i = 0; i < 12; i++) {
        const k = (lo + hi) / 2;
        at(k);
        if (hit()) lo = k; else hi = k;
      }
      at(hi);
      group.updateMatrixWorld(true);
    }

    // The free fingers (not in `active`) curl out of the controls; then,
    // while a palm vertex is still in one, the root rises by its depth and
    // the active fingers are refitted to their targets from there, the
    // pivot kept no lower (≤ 3 rounds).
    function clear(active) {
      active = [].concat(active || []);
      for (let round = 0; ; round++) {
        for (const f of FINGERS) if (!active.includes(f)) curlAway(f);
        const left = violationsUp().filter(v => PALM.includes(v.vertex));
        if (!left.length || round === 3 || !last) return;
        const fingers = active.filter(f => f in last.targets);
        pose.pivot.z += (Math.max(...left.map(v => v.upMm)) + 0.25) * MM;
        writeRoot();
        if (fingers.length) solveRounds(Object.assign({}, last, { fingers, verts: null, floorMm: pose.pivot.z / MM }));
        group.updateMatrixWorld(true);
      }
    }

    setRoot(new T.Vector3(), new T.Quaternion());   // at the origin, in page units
    return { side, root: group, mesh, bone: boneOf, setCurls, setRoot, pad, skinned, CLEARANCE, PALM,
      FINGER_CLEARANCE, armDir, contact, place, fit, pinch, clear, violations };
  }

  for (const f in STRAIGHT) Object.freeze(STRAIGHT[f]);
  Object.freeze(STRAIGHT);
  const api = { create, POSES, STRAIGHT };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.HandRig = api;
})(typeof window !== 'undefined' ? window : globalThis);
