// handrig.js: one hand of the simulator page's hands, as HandRig.create(side).
// The model's skeleton and skinned mesh (HAND_MODEL), posed by curls, placed
// by its root, and skinned on the CPU wherever vertex positions are needed.
// At the end, the driver (HandRig.Driver, HandRig.peakSpeed): both hands
// posed from the plan of handplan.js at a device time.
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

    // The vertices of each finger's last two segments: at least half their
    // weight on its distal or tip bone. A contact point is one of them
    // (contact() below), and all of them are clearance vertices.
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

    // The clearance vertices: per joint the nearest palmar and the nearest
    // dorsal vertex (a curled finger reaches down with the back of its
    // joints; Palm is a helper point outside the skin, not a joint), a 5 × 4
    // grid over the palm from the metacarpals' bases toward the knuckles, the
    // five pads, each last segment's underside (below), and every vertex of
    // the last two segments (a fingertip's side or a thumb's joint can reach
    // into a key the underside misses).
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
    for (const f of FINGERS) for (const v of TIP_VERTS[f]) add(v);
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
    // pinch (null once setCurls, setRoot or setPose moves the hand by hand).
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
      readRoot();
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
    // The inverse: the fit's root read back from the group's matrix, so that
    // a hand placed by setRoot or setPose is where a refit or clear() starts.
    const toModel = fromModel.clone().transpose();
    const dp = new T.Vector3(), dq = new T.Quaternion(), ds = new T.Vector3();
    const bx = new T.Vector3(), by = new T.Vector3(), b0 = new T.Vector3(), bz = new T.Vector3(), bm = new T.Matrix4();
    function readRoot() {
      group.matrix.decompose(dp, dq, ds);
      pose.pivot.copy(PIVOT).multiplyScalar(scale).applyQuaternion(dq).add(dp);
      bm.makeRotationFromQuaternion(dq).multiply(toModel);
      bx.setFromMatrixColumn(bm, 0);
      by.setFromMatrixColumn(bm, 1);
      pose.yaw = yawOf(by);
      pose.pitch = pitchOf(by);
      // The back of the hand at roll 0, as writeRoot builds it; the roll
      // turns it about the fingers' direction (by the thumb's side).
      b0.copy(UPV).addScaledVector(by, -by.z);
      if (b0.lengthSq() < 1e-12) { pose.roll = 0; return; }
      b0.normalize();
      bz.crossVectors(b0, by);
      pose.roll = thumbSide * Math.atan2(-bx.dot(bz), bx.dot(b0));
    }

    // The pose in the fit's terms, copied: the pivot (world units), the
    // root's rotation, and the curls and spreads as setCurls takes them.
    function getPose() {
      group.matrix.decompose(dp, dq, ds);
      const curls = {}, spread = {};
      for (const f of FINGERS) { curls[f] = now.curls[f].slice(); spread[f] = now.spread[f]; }
      return { pivot: PIVOT.clone().multiplyScalar(scale).applyQuaternion(dq).add(dp), quaternion: dq.clone(), curls, spread };
    }
    // Puts back a pose as getPose() gives it: the pivot and the quaternion,
    // or the pivot and a direction `dir` (the back of the hand up, as place()
    // turns the hand); the curls and spreads when given (a finger left out
    // at rest). Like setRoot, the hand keeps no targets.
    function setPose(p) {
      if (p.curls || p.spread) {
        for (const f in p.curls || {}) if (!CHAIN[f]) throw new Error('HandRig: no finger ' + f);
        for (const f in p.spread || {}) if (!CHAIN[f]) throw new Error('HandRig: no finger ' + f);
        writePose(p.curls, p.spread);
      }
      if (p.dir) {
        pose.pivot.set(p.pivot.x, p.pivot.y, p.pivot.z);
        pose.yaw = yawOf(p.dir); pose.pitch = pitchOf(p.dir); pose.roll = 0;
        writeRoot();
      } else {
        const q = rq.copy(p.quaternion).normalize();
        rp.copy(PIVOT).multiplyScalar(scale).applyQuaternion(q).negate().add(p.pivot);
        group.matrix.compose(rp, q, S);
        readRoot();
      }
      group.matrixWorldNeedsUpdate = true;
      last = null;
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
    // fn(v, k, x, y, z) for each vertex v = indices[k] whose skinned z is at
    // most zMax (the others' x and y are not computed), as skinInto skins
    // them, until fn returns true; then true.
    function eachLow(indices, zMax, fn) {
      group.updateMatrixWorld(true);
      pre.multiplyMatrices(mesh.matrixWorld, mesh.bindMatrixInverse);
      for (let j = 0; j < skin.length; j++) {
        skin[j].multiplyMatrices(skeleton.bones[j].matrixWorld, skeleton.boneInverses[j])
          .premultiply(pre).multiply(mesh.bindMatrix);
      }
      for (let k = 0; k < indices.length; k++) {
        const v = indices[k], px = P[3 * v], py = P[3 * v + 1], pz = P[3 * v + 2];
        let z = 0;
        for (let i = 0; i < 4; i++) {
          const w = SW[4 * v + i];
          if (!w) continue;
          const e = skin[SI[4 * v + i]].elements;
          z += w * (e[2] * px + e[6] * py + e[10] * pz + e[14]);
        }
        if (z > zMax) continue;
        let x = 0, y = 0;
        for (let i = 0; i < 4; i++) {
          const w = SW[4 * v + i];
          if (!w) continue;
          const e = skin[SI[4 * v + i]].elements;
          x += w * (e[0] * px + e[4] * py + e[8] * pz + e[12]);
          y += w * (e[1] * px + e[5] * py + e[9] * pz + e[13]);
        }
        if (fn(v, k, x, y, z)) return true;
      }
      return false;
    }
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
    // On the vertices of a finger's last two segments (TIP_VERTS above).
    const ALL_TIPS = [].concat(...FINGERS.map(f => TIP_VERTS[f])), tipsAt = new Float64Array(3 * ALL_TIPS.length);
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
    // With job.fixRoot only the fingers' angles move; with job.doneMm the
    // solve stops once every contact is that close (else it also waits for
    // the steps to settle).
    function solve(job) {
      const list = job.fingers, nf = list.length, nr = job.rollMax ? 6 : 5, n = nr + 3 * nf;
      const first = job.fixRoot ? nr : 0;
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
      let cost = residual(x, r), moved = Infinity;
      const settled = () => (job.doneMm !== undefined ? worst(r) <= job.doneMm : worst(r) <= 0.005 && moved <= 1e-6);
      for (let it = 0; it < (job.iterations || ITERATIONS) && !settled(); it++) {
        for (let j = first; j < n; j++) {
          xt.set(x);
          xt[j] += H;
          residual(xt, rt);
          for (let k = 0; k < m; k++) J[k * n + j] = (rt[k] - r[k]) / H;
        }
        // The variables free to move: not held at a limit by the gradient.
        const free = [];
        for (let j = first; j < n; j++) {
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
    // The touching fingers' targets and contact directions: those given as
    // [{finger, target, toward}] (toward as contact() takes it, default
    // down), or, given nothing, those of the last place, fit or pinch (none
    // once the hand was moved by hand). `given` marks the first kind.
    function touchingOf(touching) {
      if (touching === undefined) return last && { targets: last.targets, toward: last.toward, given: false };
      const targets = {}, toward = {};
      for (const c of touching || []) {
        if (!TIP_VERTS[c.finger]) throw new Error('HandRig: no finger ' + c.finger);
        targets[c.finger] = new T.Vector3(c.target.x, c.target.y, c.target.z);
        toward[c.finger] = c.toward || DOWN;
      }
      return { targets, toward, given: true };
    }
    // A contact is on its target within 1 mm; on a knob's side (a pinch)
    // within 2 mm, as pinch() says.
    const limitOf = toward => (toward && toward.axis ? 2 : 1);
    const settledOf = (touch, residualMm) => Object.keys(touch.targets).every(f => residualMm[f] <= limitOf(touch.toward[f]));

    // Whether world point (x, y, z), inside a control, is inside the one
    // under finger f's target: the controls are convex and apart, so the
    // segment to a point just inside the target stays inside only then.
    function inOwnTarget(touch, f, x, y, z) {
      const t = touch.targets[f], q = inward(t, touch.toward[f]).multiplyScalar(0.3 * MM).add(t);
      for (let i = 1; i <= 40; i++) {
        const s = i / 40;
        if (!PANEL.inside(x + (q.x - x) * s, y + (q.y - y) * s, z + (q.z - z) * s)) return false;
      }
      return true;
    }

    // The clearance vertices (or `verts`) inside a control or below the
    // plate (with `above`, those below it left out), deepest first. A vertex
    // of a touching finger's last two segments (`touch`, as touchingOf gives
    // them) may sink up to 1 mm into its own target (a fingertip pressed
    // into silicone); nothing is exempt below the plate.
    const EVERY = Array.from({ length: n }, (_, v) => v);
    // (Above the tallest control nothing is inside one: those vertices are
    // passed over before their x and y are skinned; a hair above it still
    // gets the exact test.)
    const TALLEST = PANEL.control({ op: 'encsw', enc: 0 }).topMm * MM + 1e-6;
    function violationsUp(touch, verts, above, at) {
      verts = verts || CLEARANCE;
      const out = [];
      const test = (v, k, x, y, z) => {
        if ((above && z < 0) || !PANEL.inside(x, y, z)) return;
        // Exempt: a touching fingertip's vertex in its own target with a way
        // out within 1 mm (up, the cheap one first, else any).
        if (touch && z >= 0) {
          for (const f in touch.targets) {
            if (TIP_SET[f].has(v) && inOwnTarget(touch, f, x, y, z) && (escape(x, y, z, 0, 0, 1, 1) <= 1 || depth(x, y, z).mm <= 1)) return;
          }
        }
        const d = depth(x, y, z);
        out.push({ vertex: v, mm: d.mm, upMm: d.upMm });
      };
      if (at) verts.forEach((v, k) => test(v, k, at[3 * k], at[3 * k + 1], at[3 * k + 2]));
      else eachLow(verts, TALLEST, test);
      return out.sort((a, b) => b.mm - a.mm);
    }
    // violations(touching, opts): touching as touchingOf takes it; with
    // opts.mesh every vertex of the mesh at or above the plate (below it a
    // bound of its own applies), else the clearance vertices; opts.skinned,
    // with opts.mesh: every vertex's position in the current pose as
    // skinned() gives it, not skinned again.
    const violations = (touching, opts) => {
      const mesh = !!(opts && opts.mesh);
      if (mesh && opts.skinned && opts.skinned.length !== 3 * n) throw new Error('HandRig: violations: skinned holds ' + opts.skinned.length / 3 + ' vertices, not ' + n);
      return violationsUp(touchingOf(touching), mesh ? EVERY : null, mesh, mesh && opts.skinned).map(({ vertex, mm }) => ({ vertex, mm }));
    };

    // Curls free finger f toward the fist (the thumb toward lift) just
    // enough, found by bisection, that each of its clearance vertices is
    // CURL_MARGIN_MM above any control or the plate under it; all the way
    // when even that is not enough.
    function curlAway(f) {
      const verts = FINGER_CLEARANCE[f], margin = CURL_MARGIN_MM * MM;
      const hit = () => eachLow(verts, TALLEST + margin, (v, k, x, y, z) => PANEL.inside(x, y, z - margin));
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
    // the active touching fingers are refitted to their targets from there,
    // the pivot kept no lower (≤ 3 rounds). touching as touchingOf takes it:
    // given, the refit starts from the hand's own root (wherever setRoot,
    // setPose or a fit left it); given nothing, from the last fit's, and a
    // hand moved by hand does not rise. With opts.free (a hand moving
    // between its keyframes, nothing held) any vertex left in a control,
    // and any vertex of a fingertip (its last two segments, whose sides the
    // clearance vertices do not all cover) under the plate, raises the
    // root, and nothing is refitted. Returns { ok, residualMm,
    // clean }: the touching fingers' contacts (mm), ok iff each is on its
    // target (within 1 mm; 2 mm on a knob's side), and clean iff
    // violations() is left empty (an active finger is never curled, so one
    // may stay in a control).
    function clear(active, touching, opts) {
      active = [].concat(active || []);
      const touch = touchingOf(touching), free = !!(opts && opts.free);
      let all;
      for (let round = 0; ; round++) {
        for (const f of FINGERS) if (!active.includes(f)) curlAway(f);
        all = violationsUp(touch);
        const left = free ? all : all.filter(v => PALM.includes(v.vertex));
        let upMm = left.length ? Math.max(...left.map(v => v.upMm)) : 0;
        if (free) {
          skinInto(ALL_TIPS, tipsAt);
          for (let k = 2; k < tipsAt.length; k += 3) upMm = Math.max(upMm, -tipsAt[k] / MM);
        }
        if (upMm <= 0 || round === 3 || !touch) break;
        const fingers = free ? [] : active.filter(f => f in touch.targets);
        pose.pivot.z += (upMm + 0.25) * MM;
        writeRoot();
        if (fingers.length) {
          const from = touch.given ? { targets: touch.targets, toward: touch.toward, seed: seedOf(fingers), yaw0: pose.yaw,
            pitch0: pose.pitch, yawWindow: YAW, rollMax: fingers.some(f => touch.toward[f].axis) ? PINCH_ROLL : 0 } : last;
          solveRounds(Object.assign({}, from, { fingers, verts: null, floorMm: pose.pivot.z / MM }));
        }
        group.updateMatrixWorld(true);
      }
      const residualMm = touch ? residualsOf(touch.targets, touch.toward) : {};
      return { ok: !touch || settledOf(touch, residualMm), residualMm, clean: !all.length };
    }

    // The touching fingers' contacts (touching as touchingOf takes it)
    // re-solved from the hand's current pose, its root held: only when one
    // is more than opts.toleranceMm (default 1) off its target, then by at
    // most opts.iterations (default 8) steps of the fit's solve over the
    // touching fingers' angles, from the angles they have (with opts.root
    // over the root's position, yaw and pitch too, within the fit's windows
    // about where it is). Returns { ok, residualMm, solved }, ok as clear()
    // says it.
    function refit(touching, opts) {
      opts = opts || {};
      const touch = touchingOf(touching), fingers = touch ? Object.keys(touch.targets) : [];
      if (!fingers.length) return { ok: true, residualMm: {}, solved: false };
      let residualMm = residualsOf(touch.targets, touch.toward);
      const tolerance = opts.toleranceMm === undefined ? 1 : opts.toleranceMm;
      if (fingers.every(f => residualMm[f] <= tolerance)) return { ok: settledOf(touch, residualMm), residualMm, solved: false };
      const verts = {};
      for (const f of fingers) verts[f] = contactVertex(f, touch.toward[f]);
      solve({ fingers, targets: touch.targets, toward: touch.toward, verts, seed: seedOf(fingers), yaw0: pose.yaw,
        pitch0: pose.pitch, yawWindow: YAW, fixRoot: !opts.root, iterations: opts.iterations || 8, doneMm: 0.05 });
      residualMm = residualsOf(touch.targets, touch.toward);
      return { ok: settledOf(touch, residualMm), residualMm, solved: true };
    }

    setRoot(new T.Vector3(), new T.Quaternion());   // at the origin, in page units
    return { side, scale, root: group, mesh, bone: boneOf, setCurls, setRoot, getPose, setPose, pad, skinned, CLEARANCE, PALM,
      FINGER_CLEARANCE, armDir, contact, place, fit, pinch, clear, refit, violations };
  }

  // ─── The driver: the plan's keyframes as poses ─────────────────
  // new Driver({left, right}, plan) poses the two rigs (at page scale) at a
  // device time from the keyframes of HandPlan.build:
  // - a keyframe's pose is the rig's fit of its aims, each raised by its
  //   liftMm: no fingers, the hand away (the driver's own pose, out of the
  //   panel toward the player); a finger on a turn, the pinch of that knob
  //   at twist 0 raised by the lift; the index alone, place(); else fit().
  //   Then clear(), the aimed fingers held to their (raised) targets. A
  //   keyframe pressing some of the controls its neighbour presses takes
  //   that one's pose instead, its other fingers let go (derivedShapes).
  //   Each is fitted on first use and cached by its geometry (side, the rig's scale, how
  //   it is fitted, the aims, freeYaw; a derived one by its fingers and its
  //   root's key) in one LRU for every driver, so equal situations in later
  //   shows reuse it;
  // - between two keyframes the root and the curls blend with minimum-jerk
  //   easing: the rotation by slerp, the position by lerp of the pivot, or,
  //   while fingers stay in contact across the two keyframes, of the
  //   contacts' centre (in the world and in the hand), so they stay put; a
  //   finger reaching a control's top ends the blend HOVER_MM above it and
  //   one leaving starts there (the keyframe at the down or the up itself
  //   takes the last HOVER_MM), so it is never on a control the device
  //   does not see pressed;
  // - a finger in contact across the two keyframes is re-solved by the rig
  //   (refit) when its contact is more than 1 mm off: its angles from the
  //   blended root, at most 8 steps; failing that, the root too;
  // - between two keyframes that pinch the same turn the grip follows the
  //   knob (twistTable below), turned about the knob's axis and lifted to
  //   regrip; a pinch keyframe elsewhere takes the twist at its time; a
  //   grip is left and taken straight up and down (GRIP_LIFT_MM);
  // - then clear(): the free fingers (a finger not aimed at by the later
  //   keyframe nor touching is let go) curled out of the controls; a hand
  //   that holds nothing rises out of any control it would pass through.
  // pose(t) depends on t alone. opts.windowUs > 0 (fast preparation): the
  // root is the average of 5 samples over [t − windowUs/2, t + windowUs/2]
  // and the contacts held at t are re-solved from it by the fingers alone,
  // kept only within reach. Times are device µs.
  const SIDES = ['left', 'right'];
  const CACHE_SIZE = 4000;
  const RESOLVE_MM = 1, RESOLVE_ITERATIONS = 8;
  const WINDOW_SAMPLES = 5;
  // Away: the hand in front of the panel's front edge (page v 1400), its
  // fingers relaxed; every clearance vertex this far in front (beyond the
  // other hand's palm, which hangs some 110 mm past the edge when it
  // presses the MUTE keys on this hand's side) and its lowest this high
  // above the plate.
  const FRONT_V = 1400, AWAY_CLEAR_MM = 120, AWAY_LOW_MM = 10;
  // On a knob: an edge turns it 3.75° (app.js; 15° a detent of four edges),
  // the hand follows within ±20°.
  const EDGE_DEG = 15 / 4, TWIST_DEG = 20;
  // An edge's turn spread over 16 ms (a burst's click is 25 ms); at the
  // window's edge the twist eases back to the middle over 60 ms; tabled
  // every 1 ms. A pause of 100 ms or more between two edges is a regrip:
  // the hand grips until 20 ms after the last edge and from 20 ms before
  // the next, and lifts 3 mm in between.
  const TWIST_RAMP_US = 16000, TWIST_SLIDE_US = 60000, TWIST_STEP_US = 1000;
  const REGRIP_US = 100000, REGRIP_MARGIN_US = 20000, REGRIP_MM = 3;
  // Off a knob's side to anything but the plan's own lift (the press that
  // continues a turn, a direct move), and onto it from anything but its
  // approach: the grip straight up or down 15 mm (the plan's lift, enough
  // for a thumb that reaches nearly to the plate to clear the knob's top)
  // over the first or last third of the time, at most 20 ms, the move
  // overlapping half of it (so it is hurried by at most 10 ms).
  const GRIP_LIFT_MM = 15, GRIP_SPLIT = 1 / 3, GRIP_MAX_US = 20000;
  // A finger let go while others of its gesture press (see derivedShapes),
  // lifted 20° at its knuckle, which may take it past the fit's flex floor
  // of 0 into a slight hyperextension (as a lifted finger does): held at 0,
  // a finger pressing with its knuckle nearly straight barely lifts, the
  // derived pose is left in a control, and the hand falls back to fits of
  // its own that swing it between presses.
  const LET_GO_DEG = 20;
  // A finger reaching a control's top ends its eased move this high above
  // it, and one leaving it starts there: the keyframe at the down (or the
  // up) itself takes the last HOVER_MM, so before the device sees a press,
  // and after it sees the release, the finger is never on the control
  // (drawTime may stop a microsecond short of the down, and a tick can end
  // anywhere before it). Its own angles re-solved, the root kept, to
  // HOVER_TOL_MM.
  const HOVER_MM = 0.3, HOVER_TOL_MM = 0.02, HOVER_ITERATIONS = 20, HOVER_ROUNDS = 4;
  const minJerk = u => u * u * u * (10 + u * (6 * u - 15));
  const NO_CONTACTS = Object.freeze([]);

  // The keyframes' poses, by geometry, least recently used dropped first.
  const cache = {
    map: new Map(),
    get(key) {
      const e = this.map.get(key);
      if (e !== undefined) { this.map.delete(key); this.map.set(key, e); }
      return e;
    },
    set(key, e) {
      this.map.set(key, e);
      if (this.map.size > CACHE_SIZE) this.map.delete(this.map.keys().next().value);
    },
    clear() { this.map.clear(); },
    get size() { return this.map.size; },
  };

  // How keyframe k of a hand is fitted, and its cache key: prefixed by
  // `rig` (the side and the rig's scale, so that rigs of another scale
  // never share a fit).
  function shapeOf(plan, rig, k) {
    const list = Object.keys(k.fingers).sort();
    if (!list.length) return { how: 'away', key: rig + ' away' };
    const onTurn = list.find(f => k.fingers[f].ref && k.fingers[f].ref.type === 'turn');
    if (onTurn) {
      const x = k.fingers[onTurn], turn = plan.turns[x.ref.id];
      if (!turn || turn.id !== x.ref.id) throw new Error('HandRig.Driver: no turn ' + x.ref.id);
      return { how: 'pinch', turn, enc: turn.enc, liftMm: x.liftMm, key: rig + ' pinch ' + turn.enc + ' +' + x.liftMm };
    }
    const how = list.length === 1 && list[0] === 'index' ? 'place' : 'fit';
    const aims = list.map(f => {
      const x = k.fingers[f];
      return f + '@' + x.aim.u + ',' + x.aim.v + ',' + (x.aim.topMm + x.liftMm);
    });
    return { how, fingers: list, liftMm: k.fingers[list[0]].liftMm,
      key: rig + ' ' + how + ' ' + aims.join(' ') + (how === 'fit' && k.freeYaw ? ' freeYaw' : '') };
  }

  // The hand away: relaxed, pointing from its shoulder straight at the
  // panel, 20° down, every clearance vertex AWAY_CLEAR_MM in front of the
  // panel's front edge, the lowest AWAY_LOW_MM above the plate.
  function awayPose(rig, T, MM) {
    const s = SHOULDER[rig.side], dir = rig.armDir(new T.Vector3(s[0], -FRONT_V, 0));
    const pivot = new T.Vector3(s[0], -FRONT_V - 60 * MM, 60 * MM);
    rig.setPose({ pivot, dir, curls: POSES.relax, spread: {} });
    const p = rig.skinned(rig.CLEARANCE);
    let front = -Infinity, low = Infinity;
    for (let k = 0; k < p.length; k += 3) { front = Math.max(front, p[k + 1]); low = Math.min(low, p[k + 2]); }
    pivot.y += -FRONT_V - AWAY_CLEAR_MM * MM - front;
    pivot.z += AWAY_LOW_MM * MM - low;
    rig.setPose({ pivot, dir, curls: POSES.relax, spread: {} });
    return rig.getPose();
  }

  // A keyframe's pose on its rig: { how, state (rig.getPose() after clear),
  // aimed (its fingers), touching (its contacts, [{finger, target,
  // toward}], when its lift is 0), ok (fitted and cleared) }.
  function fitShape(rig, k, shape, T, PANEL) {
    const MM = PANEL.UNITS_PER_MM;
    if (shape.how === 'away') {
      return { how: 'away', state: awayPose(rig, T, MM), aimed: NO_CONTACTS, touching: NO_CONTACTS, ok: true, clean: true };
    }
    const raised = f => {
      const x = k.fingers[f], p = PANEL.onPanel(x.aim.u, x.aim.v, x.aim.topMm + x.liftMm);
      return new T.Vector3(p.x, p.y, p.z);
    };
    let res, targets;
    if (shape.how === 'pinch') {
      const c = PANEL.control({ op: 'encsw', enc: shape.enc }), axis = { axis: new T.Vector3(c.u, -c.v, 0) };
      res = rig.pinch(shape.enc, 0);
      if (shape.liftMm) {
        const p = rig.getPose();
        p.pivot.z += shape.liftMm * MM;
        rig.setPose(p);
      }
      targets = ['thumb', 'index'].map(finger => ({ finger, toward: axis,
        target: res.targets[finger].clone().setZ(res.targets[finger].z + shape.liftMm * MM) }));
    } else if (shape.how === 'place') {
      const target = raised('index');
      res = rig.place('index', target, rig.armDir(target));
      targets = [{ finger: 'index', target }];
    } else {
      const aims = {}, centre = new T.Vector3();
      for (const f of shape.fingers) centre.add(aims[f] = raised(f));
      res = rig.fit(aims, { dir: rig.armDir(centre.multiplyScalar(1 / shape.fingers.length)), freeYaw: k.freeYaw });
      targets = shape.fingers.map(finger => ({ finger, target: aims[finger] }));
    }
    const aimed = targets.map(c => c.finger), cleared = rig.clear(aimed, targets);
    return { how: shape.how, state: rig.getPose(), aimed, touching: shape.liftMm ? NO_CONTACTS : targets,
      ok: res.ok && cleared.ok && cleared.clean, clean: cleared.clean };
  }

  // The fingers of keyframe k pressing controls (every finger it aims at,
  // none on a knob's side), sorted; null if it is not such a keyframe.
  function pressingOf(k) {
    const list = Object.keys(k.fingers).sort();
    return list.length && list.every(f => k.fingers[f].liftMm === 0 && k.fingers[f].ref.type !== 'turn') ? list : null;
  }
  // A keyframe whose pressing fingers are some of a neighbour's (on the
  // same controls) takes the pose of the largest such keyframe around it
  // (its `root`), the other fingers let go: fitted alone, a lone index or
  // a pair would put the root elsewhere (two MUTE keys a track apart turn
  // the pair's hand 30° from the index's alone), and the hand would swing
  // between presses milliseconds apart.
  function derivedShapes(rig, frames, shapes) {
    const sets = frames.map(pressingOf);
    const within = (i, j) => j >= 0 && j < frames.length && sets[i] && sets[j] && sets[j].length > sets[i].length &&
      sets[i].every(f => {
        const x = frames[i].fingers[f].ref, y = frames[j].fingers[f];
        return y && y.liftMm === 0 && y.ref.type === x.type && y.ref.id === x.id;
      });
    const from = frames.map((k, i) => {
      const p = within(i, i - 1), n = within(i, i + 1);
      return p && n ? (sets[i + 1].length >= sets[i - 1].length ? i + 1 : i - 1) : n ? i + 1 : p ? i - 1 : -1;
    });
    return shapes.map((shape, i) => {
      if (from[i] < 0) return shape;
      let root = i;
      while (from[root] >= 0) root = from[root];
      return { how: 'derive', root, fingers: sets[i], own: shape, key: rig + ' ' + sets[i].join(',') + ' of ' + shapes[root].key };
    });
  }
  // A derived keyframe's pose: its root's, the fingers it does not press
  // let go: lifted LET_GO_DEG at the knuckle (the thumb at its base), as a
  // finger leaves a key (curled toward the fist from a press, a fingertip
  // sweeps under the plate). ok also when nothing is left in a control.
  function deriveShape(rig, root, shape) {
    const curls = {};
    for (const f of FINGERS) {
      curls[f] = root.state.curls[f].slice();
      if (root.aimed.includes(f) && !shape.fingers.includes(f)) curls[f][f === 'thumb' ? 0 : 1] -= LET_GO_DEG;
    }
    rig.setPose({ pivot: root.state.pivot, quaternion: root.state.quaternion, curls, spread: root.state.spread });
    const touching = root.touching.filter(c => shape.fingers.includes(c.finger)), cleared = rig.clear(shape.fingers, touching);
    return { how: 'derive', state: rig.getPose(), aimed: shape.fingers, touching, ok: root.ok && cleared.ok && cleared.clean,
      clean: cleared.clean };
  }

  // The hand's twist on a knob through a turn (degrees, clockwise seen
  // from above, as the knob turns up) and its lift to regrip (mm), every
  // TWIST_STEP_US from the turn's start to its end: the twist follows the
  // knob's turn up to ±TWIST_DEG; there the fingers slide on the knob and
  // the twist eases back to the middle (TWIST_SLIDE_US, the knob's turn
  // meanwhile let slip), then follows again; in a regrip the hand lifts
  // and turns back to the middle.
  function twistTable(turn) {
    const t0 = turn.tStart, n = Math.max(1, Math.ceil((turn.tEnd - t0) / TWIST_STEP_US)) + 1, edges = turn.edges;
    const deg = new Float64Array(n), lift = new Float64Array(n), regrips = [];
    for (let i = 1; i < edges.length; i++) {
      if (edges[i].t - edges[i - 1].t >= REGRIP_US) regrips.push([edges[i - 1].t + REGRIP_MARGIN_US, edges[i].t - REGRIP_MARGIN_US]);
    }
    let w = 0, from = 0, r = 0, held = null, slide = null;
    for (let k = 1; k < n; k++) {
      const a = t0 + (k - 1) * TWIST_STEP_US, b = a + TWIST_STEP_US;
      while (r < regrips.length && regrips[r][1] <= a) { r++; held = null; slide = null; w = 0; }
      if (r < regrips.length && b > regrips[r][0]) {
        const [g0, g1] = regrips[r], u = Math.min(1, Math.max(0, (b - g0) / (g1 - g0)));
        if (held === null) held = w;
        deg[k] = w = held * (1 - minJerk(u));
        lift[k] = REGRIP_MM * Math.sin(Math.PI * u) ** 2;
        continue;
      }
      let turned = 0;
      while (from < edges.length && edges[from].t + TWIST_RAMP_US <= a) from++;
      for (let i = from; i < edges.length && edges[i].t < b; i++) {
        const lo = Math.max(a, edges[i].t), hi = Math.min(b, edges[i].t + TWIST_RAMP_US);
        if (hi > lo) turned += Math.sign(edges[i].dir) * EDGE_DEG * (hi - lo) / TWIST_RAMP_US;
      }
      if (slide) {
        const u = (b - slide.t) / TWIST_SLIDE_US;
        w = u >= 1 ? 0 : slide.w * (1 - minJerk(u));
        if (u >= 1) slide = null;
      } else {
        w = Math.max(-TWIST_DEG, Math.min(TWIST_DEG, w + turned));
        if (Math.abs(w) >= TWIST_DEG) slide = { t: b, w };
      }
      deg[k] = w;
    }
    return { t0, deg, lift };
  }

  function Driver(rigs, plan) {
    const { T, PANEL } = deps();
    const MM = PANEL.UNITS_PER_MM, Z = new T.Vector3(0, 0, 1);
    for (const side of SIDES) if (!rigs[side] || rigs[side].side !== side) throw new Error('HandRig.Driver: no ' + side + ' rig');
    const knobs = [0, 1].map(enc => PANEL.control({ op: 'encsw', enc }));
    const hands = {};
    for (const side of SIDES) {
      const frames = plan.hands[side];
      if (!frames || !frames.length) throw new Error('HandRig.Driver: no keyframes for the ' + side + ' hand');
      const key = side + ' ×' + rigs[side].scale;
      hands[side] = { frames, times: frames.map(k => k.t), shapes: derivedShapes(key, frames, frames.map(k => shapeOf(plan, key, k))),
        entries: [] };
    }
    const twists = new Map();
    const self = { rigs, plan, failures: 0, pose, drawTime };

    // Keyframe i's pose, from the cache or fitted (a derived one that does
    // not settle is the keyframe's own fit, marked `fallback`).
    function entry(side, i) {
      const h = hands[side];
      let e = h.entries[i];
      if (e) return e;
      const shape = h.shapes[i];
      e = cache.get(shape.key);
      if (!e) {
        if (shape.how !== 'derive') e = fitShape(rigs[side], h.frames[i], shape, T, PANEL);
        else {
          e = deriveShape(rigs[side], entry(side, shape.root), shape);
          if (!e.ok) {
            e = cache.get(shape.own.key);
            if (!e) cache.set(shape.own.key, e = fitShape(rigs[side], h.frames[i], shape.own, T, PANEL));
            e = Object.assign({ fallback: true }, e);
          }
        }
        cache.set(shape.key, e);
      }
      return (h.entries[i] = e);
    }
    // The twist (degrees) and the regrip lift (mm) of a turn at t, held at
    // its ends outside it.
    function twistAt(turn, t) {
      let tab = twists.get(turn.id);
      if (!tab) twists.set(turn.id, tab = twistTable(turn));
      const n = tab.deg.length, x = Math.max(0, Math.min(n - 1, (t - tab.t0) / TWIST_STEP_US));
      const k = Math.min(n - 2, Math.floor(x)), f = x - k;
      return { deg: tab.deg[k] + (tab.deg[k + 1] - tab.deg[k]) * f, liftMm: tab.lift[k] + (tab.lift[k + 1] - tab.lift[k]) * f };
    }
    // A pose turned clockwise (seen from above) by deg about the vertical
    // through point c, and raised by upMm; a contact turned likewise.
    function turned(state, c, deg, upMm) {
      const a = -deg * D2R, cs = Math.cos(a), sn = Math.sin(a), dx = state.pivot.x - c.x, dy = state.pivot.y - c.y;
      return { pivot: new T.Vector3(c.x + cs * dx - sn * dy, c.y + sn * dx + cs * dy, state.pivot.z + upMm * MM),
        quaternion: new T.Quaternion().setFromAxisAngle(Z, a).multiply(state.quaternion), curls: state.curls, spread: state.spread };
    }
    function turnedContact(contact, c, deg, upMm) {
      const a = -deg * D2R, cs = Math.cos(a), sn = Math.sin(a), p = contact.target, dx = p.x - c.x, dy = p.y - c.y;
      return { finger: contact.finger, toward: contact.toward,
        target: new T.Vector3(c.x + cs * dx - sn * dy, c.y + sn * dx + cs * dy, p.z + upMm * MM) };
    }
    const knobAxis = enc => ({ x: knobs[enc].u, y: -knobs[enc].v });
    // A keyframe's pose: a pinch at the twist of its time.
    const stateOf = (shape, e, t) => (shape.how === 'pinch' ? turned(e.state, knobAxis(shape.enc), twistAt(shape.turn, t).deg, 0) : e.state);
    // The contacts of keyframe A held across to B: the same finger on the
    // same control (ref), touching in both.
    function heldAcross(A, B, ea) {
      if (!ea.touching.length) return NO_CONTACTS;
      const out = [];
      for (const c of ea.touching) {
        const x = A.fingers[c.finger], y = B.fingers[c.finger];
        if (x && y && x.liftMm === 0 && y.liftMm === 0 && x.ref.type === y.ref.type && x.ref.id === y.ref.id) out.push(c);
      }
      return out;
    }
    // Poses a and b blended at s (see the section's header).
    function blend(a, b, s, held) {
      const quaternion = new T.Quaternion().slerpQuaternions(a.quaternion, b.quaternion, s), pivot = new T.Vector3();
      if (held.length) {
        const anchor = new T.Vector3();
        for (const c of held) anchor.add(c.target);
        anchor.multiplyScalar(1 / held.length);
        const inA = anchor.clone().sub(a.pivot).applyQuaternion(a.quaternion.clone().invert());
        const inB = anchor.clone().sub(b.pivot).applyQuaternion(b.quaternion.clone().invert());
        pivot.copy(anchor).sub(inA.lerp(inB, s).applyQuaternion(quaternion));
      } else {
        pivot.lerpVectors(a.pivot, b.pivot, s);
      }
      const curls = {}, spread = {};
      for (const f of FINGERS) {
        const ca = a.curls[f], cb = b.curls[f];
        curls[f] = ca.map((x, j) => x + (cb[j] - x) * s);
        spread[f] = a.spread[f] + (b.spread[f] - a.spread[f]) * s;
      }
      return { pivot, quaternion, curls, spread };
    }
    // A grip on a knob's side (a pinch keyframe at lift 0), turned by deg
    // and raised by upMm, with its contacts turned and raised alike (the
    // fingertips slide along the knob's side).
    function gripAt(shape, e, deg, upMm) {
      const c = knobAxis(shape.enc);
      return { state: turned(e.state, c, deg, upMm), touching: e.touching.map(x => turnedContact(x, c, deg, upMm)) };
    }
    const grips = shape => shape.how === 'pinch' && !shape.liftMm;
    // Keyframe entry e's finger c.finger lifted HOVER_MM straight up off its
    // contact c (its angles re-solved from e's pose, the root kept), as
    // {curls, spread} of that finger; cached on the entry.
    function hovered(side, e, c) {
      const memo = e.hover || (e.hover = {});
      let h = memo[c.finger];
      if (!h) {
        const rig = rigs[side], z = c.target.z + HOVER_MM * MM;
        rig.setPose(e.state);
        // The contact is re-picked after each solve (lifted, another vertex
        // of the fingertip may be the lowest): a few rounds.
        for (let round = 0; round < HOVER_ROUNDS; round++) {
          const p = rig.contact(c.finger, c.toward);
          if (p.z >= z - HOVER_TOL_MM * MM) break;
          rig.refit([{ finger: c.finger, target: new T.Vector3(p.x, p.y, z), toward: c.toward }],
            { toleranceMm: HOVER_TOL_MM, iterations: HOVER_ITERATIONS });
        }
        const q = rig.getPose();
        h = memo[c.finger] = { curls: q.curls[c.finger], spread: q.spread[c.finger] };
      }
      return h;
    }
    // A keyframe's state with the fingers of contacts `list` hovered.
    function withHover(side, e, state, list) {
      if (!list.length) return state;
      const curls = Object.assign({}, state.curls), spread = Object.assign({}, state.spread);
      for (const c of list) {
        const h = hovered(side, e, c);
        curls[c.finger] = h.curls;
        spread[c.finger] = h.spread;
      }
      return { pivot: state.pivot, quaternion: state.quaternion, curls, spread };
    }
    // Of a keyframe's contacts, those not held across to the other
    // keyframe (a finger reaching or leaving its control), on a top.
    const notHeld = (list, held) => list.filter(c => !(c.toward && c.toward.axis) && !held.some(x => x.finger === c.finger));
    // Of keyframe A's contacts (entry e), those on a top whose touch ends at t.
    const releasedAt = (A, e, t) => e.touching.filter(c => {
      const x = A.fingers[c.finger], touch = x && x.ref && x.ref.type === 'touch' ? plan.touches[x.ref.id] : null;
      return touch && touch.id === x.ref.id && touch.tUp === t && !(c.toward && c.toward.axis);
    });
    // The pose of a hand at t from its keyframes A and B around t, before
    // the rig re-solves or clears it: { state, held (the contacts held
    // across A and B, on their targets), touching (those and the fingers
    // pressing at A or B alone, leaving or reaching their controls: exempt
    // from violations), aimed (the fingers not to curl: B's and those
    // touching; A's others are let go), away, ok (A and B fitted), exact (a
    // cached pose as it is, with its clean), rigid (a fitted pose moved as
    // a whole: nothing to re-solve) }.
    function frame(side, t) {
      const h = hands[side], n = h.times.length;
      let lo = 0, hi = n;
      while (lo < hi) {
        const m = (lo + hi) >> 1;
        if (h.times[m] <= t) lo = m + 1; else hi = m;
      }
      const ia = Math.max(0, lo - 1), ib = Math.min(lo, n - 1);
      const A = h.frames[ia], B = h.frames[ib], sa = h.shapes[ia], sb = h.shapes[ib], ea = entry(side, ia);
      // At a keyframe's time (or outside them all), the keyframe as it is;
      // but at a touch's up the device has seen the release, so that
      // finger already hovers HOVER_MM above its control (a contact holds
      // from its down to just before its up).
      if (ia === ib || t <= A.t) {
        if (grips(sa)) {
          const g = gripAt(sa, ea, twistAt(sa.turn, A.t).deg, 0);
          return { state: g.state, held: g.touching, touching: g.touching, aimed: ea.aimed, away: false, ok: ea.ok, rigid: true };
        }
        const up = t === A.t ? releasedAt(A, ea, t) : NO_CONTACTS;
        if (up.length) {
          return { state: withHover(side, ea, stateOf(sa, ea, A.t), up), held: ea.touching.filter(c => !up.includes(c)),
            touching: ea.touching, aimed: ea.aimed, away: false, ok: ea.ok };
        }
        return { state: stateOf(sa, ea, A.t), held: ea.touching, touching: ea.touching, aimed: ea.aimed, away: ea.how === 'away',
          ok: ea.ok, clean: ea.clean, exact: sa.how !== 'pinch', rigid: sa.how === 'pinch' };
      }
      const eb = entry(side, ib), away = ea.how === 'away' && eb.how === 'away', ok = ea.ok && eb.ok;
      // Both grip the same turn's knob: the grip follows the knob.
      if (grips(sa) && grips(sb) && sa.turn === sb.turn) {
        const tw = twistAt(sa.turn, t), g = gripAt(sa, ea, tw.deg, tw.liftMm);
        return { state: g.state, held: g.touching, touching: g.touching, aimed: ea.aimed, away, ok, rigid: true };
      }
      const held = heldAcross(A, B, ea);
      if (ea === eb && sa.how !== 'pinch') {
        // The same pose at both ends: as it is, but a finger that lets go of
        // its control at A and presses it again at B (another touch) hovers
        // in between.
        const lift = notHeld(ea.touching, held);
        if (!lift.length) return { state: ea.state, held, touching: ea.touching, aimed: ea.aimed, away, ok, clean: ea.clean, exact: true };
        return { state: withHover(side, ea, ea.state, lift), held, touching: ea.touching, aimed: ea.aimed, away, ok };
      }
      // Off or onto a knob's side, unless the plan lifts there: the grip
      // straight up or down GRIP_LIFT_MM over a fraction `lead` of the time
      // at the start or the end; the move takes the rest and half of that
      // (the grip half up by then, the fingertips over the knob's top).
      const u = (t - A.t) / (B.t - A.t), lead = Math.min(GRIP_SPLIT, GRIP_MAX_US / (B.t - A.t));
      const leaves = grips(sa) && !(sb.how === 'pinch' && sb.turn === sa.turn);
      const enters = grips(sb) && !(sa.how === 'pinch' && sa.turn === sb.turn);
      const upAt = w => GRIP_LIFT_MM * minJerk(Math.min(1, w / lead));
      const u0 = leaves ? lead / 2 : 0, u1 = enters ? 1 - lead / 2 : 1;
      if ((leaves && u <= u0) || (enters && u >= u1)) {
        const off = leaves && u <= u0, shape = off ? sa : sb, e = off ? ea : eb;
        const g = gripAt(shape, e, twistAt(shape.turn, off ? A.t : B.t).deg, upAt(off ? u : 1 - u));
        return { state: g.state, held: g.touching, touching: g.touching, aimed: e.aimed, away, ok, rigid: true };
      }
      const touching = held.slice();
      if (!leaves) for (const c of ea.touching) if (!held.includes(c)) touching.push(c);
      if (!enters) for (const c of eb.touching) if (!touching.some(x => x.finger === c.finger)) touching.push(c);
      const aimed = eb.aimed.concat(touching.map(c => c.finger).filter(f => !eb.aimed.includes(f)));
      // A finger leaving its control at A starts HOVER_MM above it, one
      // reaching its control at B ends there.
      const pa = leaves ? gripAt(sa, ea, twistAt(sa.turn, A.t).deg, upAt(u)).state
        : withHover(side, ea, stateOf(sa, ea, A.t), notHeld(ea.touching, held));
      const pb = enters ? gripAt(sb, eb, twistAt(sb.turn, B.t).deg, upAt(1 - u)).state
        : withHover(side, eb, stateOf(sb, eb, B.t), notHeld(eb.touching, held));
      const s = minJerk((u - u0) / (u1 - u0));
      return { state: blend(pa, pb, s, held), held, touching, aimed, away, ok };
    }
    // Whether the contacts `held` are on their targets in clear()'s result.
    const onTarget = (held, cleared) => held.every(c => cleared.residualMm[c.finger] <= limitOf(c));
    const limitOf = c => (c.toward && c.toward.axis ? 2 : 1);

    function poseAt(side, t) {
      const rig = rigs[side], f = frame(side, t);
      rig.setPose(f.state);
      let ok = f.ok, clean = f.exact ? f.clean : true;
      if (!f.exact) {
        if (f.held.length && !f.rigid) {
          const opts = { toleranceMm: RESOLVE_MM, iterations: RESOLVE_ITERATIONS };
          if (!rig.refit(f.held, opts).ok) rig.refit(f.held, Object.assign({ root: true }, opts));
        }
        const cleared = rig.clear(f.aimed, f.touching, { free: !f.held.length });
        ok = onTarget(f.held, cleared) && ok;
        clean = cleared.clean;
      }
      if (!ok) self.failures++;
      return { away: f.away, ok, clean, touching: f.touching };
    }
    function poseWindow(side, t, windowUs) {
      const rig = rigs[side], mid = (WINDOW_SAMPLES - 1) / 2, c = frame(side, t);
      const pivot = new T.Vector3(), sum = new T.Quaternion(0, 0, 0, 0);
      let away = true, ok = true, first = null;
      for (let i = 0; i < WINDOW_SAMPLES; i++) {
        const f = i === mid ? c : frame(side, t + windowUs * (i / (WINDOW_SAMPLES - 1) - 0.5)), q = f.state.quaternion;
        if (!first) first = q;
        const sign = first.dot(q) < 0 ? -1 : 1;
        pivot.add(f.state.pivot);
        sum.set(sum.x + sign * q.x, sum.y + sign * q.y, sum.z + sign * q.z, sum.w + sign * q.w);
        away = away && f.away;
        ok = ok && f.ok;
      }
      rig.setPose({ pivot: pivot.multiplyScalar(1 / WINDOW_SAMPLES), quaternion: sum.normalize(), curls: c.state.curls, spread: c.state.spread });
      let held = c.held, touching = c.touching;
      if (held.length) {
        const res = rig.refit(held, { toleranceMm: RESOLVE_MM, iterations: RESOLVE_ITERATIONS });
        if (!res.ok) {
          // Out of reach from the averaged root: those fingers as blended.
          const reached = held.filter(x => res.residualMm[x.finger] <= limitOf(x)), p = rig.getPose();
          for (const x of held) {
            if (reached.includes(x)) continue;
            p.curls[x.finger] = c.state.curls[x.finger];
            p.spread[x.finger] = c.state.spread[x.finger];
          }
          rig.setPose(p);
          touching = touching.filter(x => !held.includes(x) || reached.includes(x));
          held = reached;
        }
      }
      const cleared = rig.clear(c.aimed, touching, { free: !held.length });
      return { away, ok: onTarget(held, cleared) && ok, clean: cleared.clean, touching };
    }

    // Poses both rigs at t: { left: {away, ok, clean, touching}, right: … }:
    // away when the hand is out of the panel; ok when its keyframes were
    // fitted (on target, nothing in a control) and every contact held is on
    // its target (else counted in driver.failures; not with a window, where
    // a touch may be missed); clean when nothing of it is in a control;
    // touching, the fingertips on (or leaving or reaching) their controls,
    // as violations() and clear() take them.
    function pose(t, opts) {
      const windowUs = (opts && opts.windowUs) || 0, out = {};
      for (const side of SIDES) out[side] = windowUs > 0 ? poseWindow(side, t, windowUs) : poseAt(side, t);
      return out;
    }
    // The time to draw at: never at or past the next down, up or edge the
    // device has not processed (the first of plan.events after now), and,
    // while now does not go back, never before the last one (the
    // extrapolation may fall back when the audio clock lags): that is held
    // until now passes it.
    let lastNow = -Infinity, lastDraw = -Infinity;
    function drawTime(now, extrapolated) {
      const ev = plan.events;
      let lo = 0, hi = ev.length;
      while (lo < hi) {
        const m = (lo + hi) >> 1;
        if (ev[m] > now) hi = m; else lo = m + 1;
      }
      let t = lo === ev.length ? extrapolated : Math.max(now, Math.min(extrapolated, ev[lo] - 1));
      if (now >= lastNow) t = Math.max(t, lastDraw);
      lastNow = now;
      lastDraw = t;
      return t;
    }
    return self;
  }
  Driver.cache = cache;

  // The hands' peak speeds (m/s) between t0 and t1: the wrists posed by the
  // driver every stepUs.
  function peakSpeed(driver, t0, t1, stepUs) {
    const { T, PANEL } = deps(), MM = PANEL.UNITS_PER_MM, out = { left: 0, right: 0 }, prev = {};
    for (let t = t0; t <= t1; t += stepUs) {
      driver.pose(t);
      for (const side of SIDES) {
        const w = driver.rigs[side].bone('Wrist').getWorldPosition(new T.Vector3());
        if (prev[side]) out[side] = Math.max(out[side], w.distanceTo(prev[side]) / MM / 1000 / (stepUs / 1e6));
        prev[side] = w;
      }
    }
    return out;
  }

  for (const f in STRAIGHT) Object.freeze(STRAIGHT[f]);
  Object.freeze(STRAIGHT);
  const api = { create, POSES, STRAIGHT, Driver, peakSpeed };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.HandRig = api;
})(typeof window !== 'undefined' ? window : globalThis);
