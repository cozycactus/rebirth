// SPIKE (throwaway): one right hand pressing one pad over the drawn panel, to
// get the look approved. Not the feature: no planner, no artist hookup.
(function (root) {
  'use strict';
  const T = root.THREE;
  // The page's panel square is 1400 units; the real pad pitch is 19.5 mm and
  // 91 units here, so the square is 300 mm.
  const UNITS_PER_M = 1400 / 0.300;
  const MMU = UNITS_PER_M / 1000;       // units per millimetre
  const D2R = Math.PI / 180;
  const HAND_SCALE = 0.92;              // the model is a 20 cm hand; this makes it 18.7 cm

  const smooth = k => k < 0 ? 0 : k > 1 ? 1 : k * k * (3 - 2 * k);

  function bytes(b64) {
    const bin = atob(b64), a = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i);
    return a.buffer;
  }
  function image(src) {
    const img = new Image();
    img.src = src;
    return img.decode().then(() => img);
  }

  const FINGERS = ['thumb', 'index', 'middle', 'ring', 'pinky'];
  const BONES = {
    thumb: ['Thumb_Metacarpal', 'Thumb_Proximal', 'Thumb_Distal', 'Thumb_Tip'],
    index: ['Index_Metacarpal', 'Index_Proximal', 'Index_Intermediate', 'Index_Distal', 'Index_Tip'],
    middle: ['Middle_Metacarpal', 'Middle_Proximal', 'Middle_Intermediate', 'Middle_Distal', 'Middle_Tip'],
    ring: ['Ring_Metacarpal', 'Ring_Proximal', 'Ring_Intermediate', 'Ring_Distal', 'Ring_Tip'],
    pinky: ['Little_Metacarpal', 'Little_Proximal', 'Little_Intermediate', 'Little_Distal', 'Little_Tip'],
  };

  // Curl per joint, degrees: metacarpal base, MCP, PIP, DIP (thumb: CMC, MCP, IP).
  const POSES = {
    point: { thumb: [12, 14, 12], index: [0, 6, 14, 8], middle: [3, 62, 78, 38], ring: [4, 68, 80, 38], pinky: [5, 72, 80, 38] },
    fist: { thumb: [18, 26, 26], index: [5, 82, 95, 55], middle: [5, 85, 95, 55], ring: [6, 88, 95, 55], pinky: [8, 90, 95, 55] },
    lift: { thumb: [-25, -20, 0], index: [0, 4, 60, 38], middle: [1, 4, 62, 38], ring: [2, 6, 62, 38], pinky: [3, 8, 60, 38] },
    relax: { thumb: [5, 8, 10], index: [0, 18, 26, 14], middle: [1, 22, 30, 16], ring: [2, 25, 32, 16], pinky: [3, 28, 32, 16] },
  };

  function skinMaterial(maps, side) {
    const m = new T.MeshPhysicalMaterial({
      map: maps.albedo || null, normalMap: maps.normal || null, normalScale: new T.Vector2(0.3, 0.3),
      aoMap: maps.orm || null, aoMapIntensity: 0.7, roughness: 0.62, metalness: 0, color: new T.Color(0.95, 0.95, 0.95),
      sheen: 0.2, sheenRoughness: 0.6, sheenColor: new T.Color(0xffb8a0),
      specularIntensity: 0.6, side: side || T.FrontSide,
    });
    // A cheap stand-in for light scattered under the skin: diffuse light wraps
    // past the terminator and turns red there.
    m.onBeforeCompile = shader => {
      // The texture's skin is a saturated salmon; take a third of the colour out.
      shader.fragmentShader = shader.fragmentShader.replace('#include <map_fragment>', `#include <map_fragment>
        diffuseColor.rgb = mix(vec3(dot(diffuseColor.rgb, vec3(0.3, 0.59, 0.11))), diffuseColor.rgb, 0.68) * vec3(1.06, 1.0, 0.97);`);
      shader.fragmentShader = shader.fragmentShader.replace(
        '#include <lights_fragment_end>',
        `#include <lights_fragment_end>
        {
          float wrapK = 0.0;
          #if NUM_DIR_LIGHTS > 0
            for (int i = 0; i < NUM_DIR_LIGHTS; i++) {
              float nl = dot(geometryNormal, directionalLights[i].direction);
              wrapK += max(0.0, (nl + 0.4) / 1.4) - max(0.0, nl);
            }
          #endif
          reflectedLight.directDiffuse += wrapK * vec3(0.30, 0.08, 0.04) * diffuseColor.rgb;
        }`);
    };
    return m;
  }

  // An elliptic tube along -Y from y0 to y1 (metres), semi-axes ax(y) across
  // X and az(y) across Z, with outward normals.
  function tube(y0, y1, ax, az, rings, segs) {
    const pos = [], nor = [], idx = [];
    for (let i = 0; i <= rings; i++) {
      const y = y0 + (y1 - y0) * i / rings, a = ax(y), b = az(y);
      for (let j = 0; j <= segs; j++) {
        const t = j / segs * Math.PI * 2, c = Math.cos(t), s = Math.sin(t);
        pos.push(a * c, y, b * s);
        const nx = c / a, nz = s / b, l = Math.hypot(nx, nz);
        nor.push(nx / l, 0, nz / l);
      }
    }
    for (let i = 0; i < rings; i++) {
      for (let j = 0; j < segs; j++) {
        const p = i * (segs + 1) + j, q = p + segs + 1;
        idx.push(p, p + 1, q, p + 1, q + 1, q);
      }
    }
    const g = new T.BufferGeometry();
    g.setAttribute('position', new T.Float32BufferAttribute(pos, 3));
    g.setAttribute('normal', new T.Float32BufferAttribute(nor, 3));
    g.setIndex(idx);
    return g;
  }

  class Hand {
    constructor(gltf, side, maps) {
      this.side = side;
      const sfx = side === 'right' ? '_R' : '_L';
      this.root = new T.Group();
      this.root.matrixAutoUpdate = false;
      this.root.add(gltf.scene);
      let mesh = null;
      gltf.scene.traverse(o => { if (o.isSkinnedMesh) mesh = o; });
      this.mesh = mesh;
      this.skin = skinMaterial(maps);
      mesh.material = this.skin;
      mesh.castShadow = true;
      mesh.receiveShadow = true;
      mesh.frustumCulled = false;
      this.bone = {};
      for (const b of mesh.skeleton.bones) this.bone[b.name.replace(sfx, '')] = b;
      this.restQ = {};
      for (const name in this.bone) this.restQ[name] = this.bone[name].quaternion.clone();
      this.bindInv = {};
      mesh.skeleton.bones.forEach((b, i) => { this.bindInv[b.name.replace(sfx, '')] = mesh.skeleton.boneInverses[i]; });
      this.tips = {};
      for (const f of FINGERS) this.tips[f] = this.measureTip(f);
      this.addForearm(maps);
      this.curls = null;
    }

    // The pad of a fingertip, measured on the mesh: the most palmar point near
    // the tip joint, in the tip bone's frame (+Y along the bone, +X out of the
    // back of the hand, Z across).
    measureTip(finger) {
      const chain = BONES[finger], tip = chain[chain.length - 1], distal = chain[chain.length - 2];
      const geo = this.mesh.geometry, P = geo.attributes.position, J = geo.attributes.skinIndex, W = geo.attributes.skinWeight;
      const bones = this.mesh.skeleton.bones, sfx = this.side === 'right' ? '_R' : '_L';
      const inv = this.bindInv[tip], v = new T.Vector3();
      let best = null;
      for (let i = 0; i < P.count; i++) {
        let wt = 0;
        for (let k = 0; k < 4; k++) {
          const name = bones[J.getComponent(i, k)].name.replace(sfx, '');
          if (name === tip || name === distal) wt += W.getComponent(i, k);
        }
        if (wt < 0.5) continue;
        v.fromBufferAttribute(P, i).applyMatrix4(inv);
        if (Math.abs(v.z) > 0.003 || v.y < -0.006 || v.y > 0.006) continue;
        if (!best || v.x < best.x) best = v.clone();
      }
      return best || new T.Vector3(-0.006, 0, 0);
    }

    // Sets the bones from curls (degrees per joint) and MCP spreads (degrees).
    pose(curls, spread) {
      this.curls = curls; this.spread = spread || {};
      for (const f of FINGERS) this.setFinger(f, curls[f]);
    }
    setFinger(f, c) {
      const qz = new T.Quaternion(), qx = new T.Quaternion(), Z = new T.Vector3(0, 0, 1), X = new T.Vector3(1, 0, 0);
      const sign = this.side === 'right' ? 1 : -1;
      BONES[f].forEach((name, k) => {
        const b = this.bone[name];
        b.quaternion.copy(this.restQ[name]);
        const curl = (c && c[k]) || 0;
        if (curl) b.quaternion.multiply(qz.setFromAxisAngle(Z, sign * curl * D2R));
        if (k === 1 && this.spread[f]) b.quaternion.multiply(qx.setFromAxisAngle(X, sign * this.spread[f] * D2R));
        b.updateMatrix();
      });
    }
    // An object's matrix in the hand's own space (above this.root).
    inHand(obj) {
      const m = obj.matrix.clone();
      for (let o = obj.parent; o && o !== this.root; o = o.parent) m.premultiply(o.matrix);
      return m;
    }
    contactInHand(f) {
      const chain = BONES[f];
      return this.tips[f].clone().applyMatrix4(this.inHand(this.bone[chain[chain.length - 1]]));
    }
    contactZ(f) { return this.contactInHand(f).applyMatrix4(this.root.matrix).z; }

    // After placing: lifts each finger but `active` (curling it in at the
    // middle joints, raising it at the knuckle) until its pad is `minZ` above
    // the drawing plane, so no finger sinks into the panel.
    clear(active, minZ) {
      for (const f of FINGERS) {
        if (f === active || this.contactZ(f) >= minZ) continue;
        const a = this.curls[f], b = f === 'thumb' ? POSES.lift.thumb : POSES.fist[f];
        let lo = 0, hi = 1;
        for (let i = 0; i < 10; i++) {
          const k = (lo + hi) / 2;
          this.setFinger(f, a.map((v, j) => v + (b[j] - v) * k));
          if (this.contactZ(f) >= minZ) hi = k; else lo = k;
        }
        this.setFinger(f, a.map((v, j) => v + (b[j] - v) * hi));
      }
    }

    // Places the hand: fingers along `dir` (world), back of the hand toward
    // `up`, and the pad of `finger` at `target` (world units).
    place(dir, up, finger, target) {
      const y = dir.clone().normalize();                          // wrist +Y = fingers
      const x = up.clone().sub(y.clone().multiplyScalar(up.dot(y))).normalize();   // +X = back of the hand
      const z = new T.Vector3().crossVectors(x, y);
      const want = new T.Matrix4().makeBasis(x, y, z);
      const wrist = new T.Matrix4().extractRotation(this.inHand(this.bone.Wrist));
      const s = UNITS_PER_M * HAND_SCALE;
      const m = want.multiply(wrist.invert()).multiply(new T.Matrix4().makeScale(s, s, s));
      const p = this.contactInHand(finger).applyMatrix4(m);
      m.setPosition(target.x - p.x, target.y - p.y, target.z - p.z);
      this.root.matrix.copy(m);
      this.root.matrixWorldNeedsUpdate = true;
    }

    // The forearm: an elliptic tube from inside the wrist backwards, the size
    // of the mesh's wrist, coloured like the skin there, then a dark sleeve.
    addForearm(maps) {
      const geo = this.mesh.geometry, P = geo.attributes.position, UV = geo.attributes.uv;
      const inv = this.bindInv.Wrist, v = new T.Vector3();
      let x0 = 1, x1 = -1, z0 = 1, z1 = -1;
      const uvs = [];
      for (let i = 0; i < P.count; i++) {
        v.fromBufferAttribute(P, i).applyMatrix4(inv);
        if (v.y < 0 || v.y > 0.012) continue;
        x0 = Math.min(x0, v.x); x1 = Math.max(x1, v.x); z0 = Math.min(z0, v.z); z1 = Math.max(z1, v.z);
        if (v.y < 0.006) uvs.push([UV.getX(i), UV.getY(i)]);
      }
      const ax = (x1 - x0) / 2, az = (z1 - z0) / 2, cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
      // The skin colour at the wrist, from the albedo under those vertices.
      const img = maps.albedo.image, c = document.createElement('canvas');
      c.width = img.width; c.height = img.height;
      const g2 = c.getContext('2d');
      g2.drawImage(img, 0, 0);
      const px = g2.getImageData(0, 0, c.width, c.height).data;
      let r = 0, g = 0, b = 0;
      for (const [u, w] of uvs) {
        const o = (Math.min(c.height - 1, Math.floor(w * c.height)) * c.width + Math.min(c.width - 1, Math.floor(u * c.width))) * 4;
        r += px[o]; g += px[o + 1]; b += px[o + 2];
      }
      const n = Math.max(1, uvs.length);
      const tone = new T.Color().setRGB(r / n / 255, g / n / 255, b / n / 255, T.SRGBColorSpace);
      const armSkin = skinMaterial({}, T.DoubleSide);
      armSkin.color = tone;
      const grow = y => 1 + 0.32 * smooth((-y - 0.01) / 0.15);
      const arm = new T.Mesh(tube(0.008, -0.30, y => 0.96 * ax * grow(y), y => 0.96 * az * grow(y), 40, 40), armSkin);
      arm.position.set(cx, 0, cz);
      arm.castShadow = true;
      arm.receiveShadow = true;
      this.bone.Wrist.add(arm);
      const cloth = new T.MeshStandardMaterial({ color: 0x24262c, roughness: 0.92, side: T.DoubleSide });
      const cuff = -0.002;
      const sleeve = new T.Mesh(tube(cuff, -0.32, y => 1.12 * ax * grow(y) + 0.004, y => 1.12 * az * grow(y) + 0.004, 12, 40), cloth);
      sleeve.position.set(cx, 0, cz);
      sleeve.castShadow = true;
      this.bone.Wrist.add(sleeve);
    }
  }

  class HandLayer {
    constructor(container, opts) {
      this.opts = opts || {};
      this.container = container;
      const canvas = this.canvas = document.createElement('canvas');
      canvas.className = 'handsgl';
      container.appendChild(canvas);
      const r = this.renderer = new T.WebGLRenderer({ canvas, alpha: true, antialias: true });
      r.setClearColor(0x000000, 0);
      r.shadowMap.enabled = true;
      r.shadowMap.type = T.PCFShadowMap;
      r.outputColorSpace = T.SRGBColorSpace;
      r.toneMapping = 0;     // none: the skin keeps its texture's colour
      this.scene = new T.Scene();
      const pmrem = new T.PMREMGenerator(r);
      this.scene.environment = pmrem.fromScene(new T.RoomEnvironment(), 0.04).texture;
      this.scene.environmentIntensity = 0.35;

      // Eye in front of the panel and high above it; the image plane stays
      // parallel to the panel (a shifted lens), so the drawing is not keystoned.
      this.eye = new T.Vector3(700, -1400 - 700, 4600);
      this.camera = new T.PerspectiveCamera();
      this.camera.position.copy(this.eye);
      this.camera.updateMatrixWorld();

      this.scene.add(new T.HemisphereLight(0xf4f0ff, 0x24252a, 0.6));
      const key = this.key = new T.DirectionalLight(0xfffaf4, 2.2);
      key.position.set(700 - 1900, -700 + 1500, 2600);
      key.target.position.set(700, -700, 0);
      key.castShadow = true;
      key.shadow.mapSize.set(2048, 2048);
      const sc = key.shadow.camera;
      sc.left = -1300; sc.right = 1300; sc.top = 1300; sc.bottom = -1300; sc.near = 500; sc.far = 7000;
      key.shadow.radius = 5;
      key.shadow.bias = -0.0004;
      key.shadow.normalBias = 2;
      this.scene.add(key, key.target);
      const fill = new T.DirectionalLight(0xdfe8ff, 0.7);
      fill.position.set(700 + 1800, -700 - 600, 1800);
      fill.target.position.set(700, -700, 0);
      this.scene.add(fill, fill.target);

      const catcher = new T.Mesh(new T.PlaneGeometry(4000, 4000), new T.ShadowMaterial({ opacity: 0.42 }));
      catcher.position.set(700, -700, 0);
      catcher.receiveShadow = true;
      this.scene.add(catcher);

      const A = root.HAND_ASSETS;
      this.hands = {};
      this.ready = Promise.all([image(A.albedo), image(A.normal), image(A.orm)]).then(([al, no, orm]) => {
        const tex = (img, srgb) => {
          const t = new T.CanvasTexture(img);
          t.flipY = false;
          if (srgb) t.colorSpace = T.SRGBColorSpace;
          t.anisotropy = 4;
          return t;
        };
        const maps = { albedo: tex(al, true), normal: tex(no), orm: tex(orm) };
        return new Promise(done => {
          new T.GLTFLoader().parse(bytes(A.glbR), '', gltf => {
            const h = this.hands.right = new Hand(gltf, 'right', maps);
            this.scene.add(h.root);
            done();
          }, e => console.error(e));
        });
      });
      this.resize();
      new ResizeObserver(() => this.resize()).observe(container);
    }

    resize() {
      const w = this.container.clientWidth, h = this.container.clientHeight;
      if (!w || !h) return;
      this.renderer.setPixelRatio(Math.min(2, root.devicePixelRatio || 1));
      this.renderer.setSize(w, h, false);
      const e = this.eye, n = 200, f = 20000, k = n / e.z;
      this.camera.projectionMatrix.makePerspective((0 - e.x) * k, (1400 - e.x) * k, (0 - e.y) * k, (-1400 - e.y) * k, n, f);
      this.camera.projectionMatrixInverse.copy(this.camera.projectionMatrix).invert();
    }

    // A point of the drawing (panel units, y down) raised `h` units, on the
    // eye's ray through it, so it still lands on the drawn spot.
    onPanel(u, v, h) {
      const p = new T.Vector3(u, -v, 0);
      return p.add(this.eye.clone().sub(p).multiplyScalar((h || 0) / this.eye.z));
    }

    debugDots(points) {
      const g = new T.SphereGeometry(6, 12, 8), m = new T.MeshBasicMaterial({ color: 0x00ffff });
      for (const [u, v] of points) { const d = new T.Mesh(g, m); d.position.copy(this.onPanel(u, v, 0)); this.scene.add(d); }
    }

    render() { this.renderer.render(this.scene, this.camera); }

    marker(p, color) {
      const m = new T.Mesh(new T.SphereGeometry(5, 12, 8), new T.MeshBasicMaterial({ color, depthTest: false }));
      m.renderOrder = 10;
      m.position.copy(p);
      this.scene.add(m);
      return m;
    }

    // Heights of the finger pads above the drawing plane, in mm (debug).
    tipHeights(hand) {
      const out = {};
      for (const f of FINGERS) out[f] = +(hand.contactZ(f) / MMU).toFixed(1);
      return out;
    }
  }

  // The demo: hover, press pad (track 4, row 5), lift, back to rest; loops.
  function demo(layer, padEl, padUV) {
    const hand = layer.hands.right;
    const PAD_H = 4 * MMU;       // silicone pad top above the drawing plane
    const press = layer.onPanel(padUV[0], padUV[1], PAD_H);
    const restAt = layer.onPanel(1250, 1900, 30 * MMU);
    const pointDir = new T.Vector3(-0.3, 1, -0.42);
    const restDir = new T.Vector3(-0.22, 1, -0.06);
    const up = new T.Vector3(0, 0, 1);
    const lerp = (a, b, k) => a + (b - a) * k;
    const mix = (pa, pb, k) => {
      const out = {};
      for (const f of FINGERS) out[f] = pa[f].map((v, i) => lerp(v, pb[f][i], k));
      return out;
    };
    let frozen = null;
    root.handsgl = { layer, freeze(p) { frozen = p; }, run() { frozen = null; } };
    const CYCLE = 3.2;
    function step(now) {
      const t = frozen !== null ? frozen : (now / 1000) % CYCLE;
      // 0-0.7 rest; 0.7-1.3 travel to hover; 1.3-1.5 down; 1.5-1.75 held; 1.75-1.95 up; 1.95-2.7 back
      let reach, down, held = false;
      if (t < 0.7) { reach = 0; down = 0; }
      else if (t < 1.3) { reach = smooth((t - 0.7) / 0.6); down = 0; }
      else if (t < 1.5) { reach = 1; down = smooth((t - 1.3) / 0.2); }
      else if (t < 1.75) { reach = 1; down = 1; held = true; }
      else if (t < 1.95) { reach = 1; down = 1 - smooth((t - 1.75) / 0.2); }
      else if (t < 2.7) { reach = 1 - smooth((t - 1.95) / 0.75); down = 0; }
      else { reach = 0; down = 0; }
      hand.pose(mix(POSES.relax, POSES.point, reach), { index: -3 * reach, middle: 1, ring: 3, pinky: 6 });
      const hover = press.clone().add(new T.Vector3(0, -6 * MMU, 14 * MMU));
      const tip = hover.clone().lerp(press, down);
      const target = restAt.clone().lerp(tip, reach);
      const dir = restDir.clone().lerp(pointDir, reach);
      hand.place(dir, up, 'index', target);
      hand.clear('index', PAD_H + 3 * MMU);
      if (/marks/.test(location.hash)) {
        layer.mTarget = layer.mTarget || layer.marker(target, 0xff0000);
        layer.mTarget.position.copy(target);
        layer.mTip = layer.mTip || layer.marker(target, 0x00ff00);
        layer.mTip.position.copy(hand.contactInHand('index').applyMatrix4(hand.root.matrix));
      }
      if (padEl) padEl.classList.toggle('down', held);
      layer.render();
      requestAnimationFrame(step);
    }
    requestAnimationFrame(step);
  }

  root.HandLayer = HandLayer;
  root.HandLayer.demo = demo;
})(typeof window !== 'undefined' ? window : globalThis);
