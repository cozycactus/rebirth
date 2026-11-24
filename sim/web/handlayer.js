// handlayer.js: the drawing of the simulator page's hands, as
// HandLayer.create(container, opts): a transparent WebGL canvas over the drawn
// panel (#device), the camera above it, the lights, the skin, the forearms
// and sleeves, and the two hands of handrig.js in the scene. Browser only.
//
// The look is the approved prototype's (docs/superpowers/plans/
// 2026-11-17-sim-hands-prototype.js): the same camera, lights, skin shader and
// forearm, now on the rig's skeleton. The hands (layer.left, layer.right)
// start hidden. The page gives the layer a show (setShow: the plan of
// handplan.js and the driver of handrig.js) and calls frame(clock) at every
// animation frame while the hands are shown: the layer poses them at the
// draw time and draws only when a pose changed. drop() takes the hands out
// of the frame. render() draws as things are; resize() draws again with the
// new size. The layer lives as long as the page.
//
// World units are page units (HandPlan.PANEL): the panel square is 1400 units
// = 300 mm, page point (u, v) is world (u, -v, 0), +z is up out of the plate.
(function (root) {
  'use strict';
  const SIDE = 1400;                    // the panel square, page units
  const NEAR = 200, FAR = 20000;
  const smooth = k => k < 0 ? 0 : k > 1 ? 1 : k * k * (3 - 2 * k);
  // After drop(): the hands leave over this much wall time, rising this high
  // in the first third of it (with clear(), nothing passes through a control).
  const LEAVE_MS = 300, LEAVE_LIFT_MM = 25;
  const minJerk = u => u * u * u * (10 + u * (6 * u - 15));

  // Dependencies, at call time: in the browser the page's globals, which
  // app.js loads before calling create().
  function deps() {
    if (typeof module !== 'undefined' && module.exports) {
      require('./three.js');
      const HandPlan = require('./handplan.js');
      return { T: globalThis.THREE, MODEL: require('./handmodel.js'), HandRig: require('./handrig.js'),
        HandPlan, PANEL: HandPlan.PANEL };
    }
    return { T: root.THREE, MODEL: root.HAND_MODEL, HandRig: root.HandRig, HandPlan: root.HandPlan,
      PANEL: root.HandPlan.PANEL };
  }

  // Pose a blended toward pose b by s (as getPose() gives them): the pivot
  // by lerp, the rotation by slerp, the curls and spreads by lerp.
  function blendPose(T, a, b, s) {
    const curls = {}, spread = {};
    for (const f in a.curls) {
      curls[f] = a.curls[f].map((x, j) => x + (b.curls[f][j] - x) * s);
      spread[f] = a.spread[f] + (b.spread[f] - a.spread[f]) * s;
    }
    return { pivot: new T.Vector3().lerpVectors(a.pivot, b.pivot, s),
      quaternion: new T.Quaternion().slerpQuaternions(a.quaternion, b.quaternion, s), curls, spread };
  }
  // A pose as numbers, to tell whether a hand moved since the last drawing.
  function poseNumbers(rig) {
    if (!rig.root.visible) return [];
    const p = rig.getPose(), out = [p.pivot.x, p.pivot.y, p.pivot.z, p.quaternion.x, p.quaternion.y, p.quaternion.z, p.quaternion.w];
    for (const f in p.curls) out.push(...p.curls[f], p.spread[f]);
    return out;
  }
  const sameNumbers = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

  function image(src) {
    const img = new Image();
    img.src = src;
    return img.decode().then(() => img);
  }

  // A shader chunk followed by more code; a chunk that is not there is an
  // error, not a silently plainer skin.
  function after(source, chunk, code) {
    if (!source.includes(chunk)) throw new Error('HandLayer: the skin shader has no ' + chunk);
    return source.replace(chunk, chunk + '\n' + code);
  }

  // The skin: the albedo with a third of its colour taken out, the normal
  // map at 0.3, the ORM's occlusion, a little sheen, and diffuse light that
  // wraps past the terminator and turns red there (a cheap stand-in for
  // light scattered under the skin).
  function skinMaterial(T, maps, side) {
    const m = new T.MeshPhysicalMaterial({
      map: maps.albedo || null, normalMap: maps.normal || null, normalScale: new T.Vector2(0.3, 0.3),
      aoMap: maps.orm || null, aoMapIntensity: 0.7, roughness: 0.62, metalness: 0, color: new T.Color(0.95, 0.95, 0.95),
      sheen: 0.2, sheenRoughness: 0.6, sheenColor: new T.Color(0xffb8a0),
      specularIntensity: 0.6, side: side || T.FrontSide,
    });
    m.onBeforeCompile = shader => {
      // The texture's skin is a saturated salmon; take a third of the colour out.
      shader.fragmentShader = after(shader.fragmentShader, '#include <map_fragment>',
        `diffuseColor.rgb = mix(vec3(dot(diffuseColor.rgb, vec3(0.3, 0.59, 0.11))), diffuseColor.rgb, 0.68) * vec3(1.06, 1.0, 0.97);`);
      shader.fragmentShader = after(shader.fragmentShader, '#include <lights_fragment_end>',
        `{
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
  function tube(T, y0, y1, ax, az, rings, segs) {
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

  // The forearm of a rig: an elliptic tube from inside the wrist backwards,
  // the size of the mesh's wrist section, coloured like the skin there, then
  // a dark sleeve from 2 mm behind the wrist. Both hang on the Wrist bone, in
  // its frame (metres, -Y toward the elbow).
  function addForearm(T, rig, albedo) {
    const mesh = rig.mesh, geo = mesh.geometry, P = geo.attributes.position, UV = geo.attributes.uv;
    const wrist = rig.bone('Wrist'), j = mesh.skeleton.bones.indexOf(wrist);
    if (j < 0) throw new Error('HandLayer: the wrist is not a joint of the ' + rig.side + ' hand');
    const inv = mesh.skeleton.boneInverses[j], v = new T.Vector3();
    let x0 = 1, x1 = -1, z0 = 1, z1 = -1;
    const uvs = [];
    for (let i = 0; i < P.count; i++) {
      v.fromBufferAttribute(P, i).applyMatrix4(inv);
      if (v.y < 0 || v.y > 0.012) continue;
      x0 = Math.min(x0, v.x); x1 = Math.max(x1, v.x); z0 = Math.min(z0, v.z); z1 = Math.max(z1, v.z);
      if (v.y < 0.006) uvs.push([UV.getX(i), UV.getY(i)]);
    }
    const ax = (x1 - x0) / 2, az = (z1 - z0) / 2, cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
    // The skin colour at the wrist, from the albedo under those vertices
    // (v runs down the image: the UVs are as in the glTF).
    const img = albedo.image, c = document.createElement('canvas');
    c.width = img.naturalWidth || img.width; c.height = img.naturalHeight || img.height;
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
    const armSkin = skinMaterial(T, {}, T.DoubleSide);
    armSkin.color = tone;
    const grow = y => 1 + 0.32 * smooth((-y - 0.01) / 0.15);
    const arm = new T.Mesh(tube(T, 0.008, -0.30, y => 0.96 * ax * grow(y), y => 0.96 * az * grow(y), 40, 40), armSkin);
    arm.position.set(cx, 0, cz);
    arm.castShadow = true;
    arm.receiveShadow = true;
    wrist.add(arm);
    const cloth = new T.MeshStandardMaterial({ color: 0x24262c, roughness: 0.92, side: T.DoubleSide });
    const cuff = -0.002;
    const sleeve = new T.Mesh(tube(T, cuff, -0.32, y => 1.12 * ax * grow(y) + 0.004, y => 1.12 * az * grow(y) + 0.004, 12, 40), cloth);
    sleeve.position.set(cx, 0, cz);
    sleeve.castShadow = true;
    wrist.add(sleeve);
  }

  // container: the element the canvas covers (#device, whose square is the
  // panel's 1400 units). opts.quality: 'full' (default) or 'light' (a 1024
  // shadow map and a pixel ratio of at most 1.5 instead of 2048 and 2).
  // opts.until: a function whose promise the building waits for once the
  // textures are decoded (building is one long task; the page lets it run
  // only where it delays nothing that sounds). Resolves to the layer;
  // rejects, leaving nothing behind, when WebGL2 or a texture is not
  // available, or WebGL would be drawn in software (a major performance
  // caveat: the page shows the ring instead).
  function create(container, opts) {
    opts = opts || {};
    let canvas = null, renderer = null;
    return Promise.resolve().then(() => {
      const { T, MODEL, HandRig, HandPlan, PANEL } = deps();
      const MM = PANEL.UNITS_PER_MM;
      const light = opts.quality === 'light';
      const maxRatio = light ? 1.5 : 2, shadowSize = light ? 1024 : 2048;
      const A = MODEL.textures;
      return Promise.all([image(A.albedo), image(A.normal), image(A.orm)])
        .then(maps => Promise.resolve(opts.until && opts.until()).then(() => maps)).then(([al, no, orm]) => {
        canvas = document.createElement('canvas');
        canvas.className = 'handsgl';
        container.appendChild(canvas);
        const r = renderer = new T.WebGLRenderer({ canvas, alpha: true, antialias: true, failIfMajorPerformanceCaveat: true });
        r.setClearColor(0x000000, 0);
        r.shadowMap.enabled = true;
        r.shadowMap.type = T.PCFShadowMap;
        r.outputColorSpace = T.SRGBColorSpace;
        r.toneMapping = 0;              // none: the skin keeps its texture's colour

        const scene = new T.Scene();
        // The dim room the skin reflects, rendered into a texture (again
        // after a lost context: a rendered texture does not come back).
        const environment = () => {
          const pmrem = new T.PMREMGenerator(r), room = new T.RoomEnvironment();
          if (scene.environment) scene.environment.dispose();
          scene.environment = pmrem.fromScene(room, 0.04).texture;
          pmrem.dispose();
          if (room.dispose) room.dispose();
        };
        environment();
        scene.environmentIntensity = 0.35;

        // The eye is in front of the panel and high above it, looking straight
        // down; the image plane stays parallel to the panel (a shifted lens),
        // so the drawing is not keystoned. resize() sets the frustum.
        const EYE = PANEL.EYE;
        const camera = new T.PerspectiveCamera();
        camera.position.set(EYE.x, EYE.y, EYE.z);
        camera.updateMatrixWorld();

        scene.add(new T.HemisphereLight(0xf4f0ff, 0x24252a, 0.6));
        const mid = [SIDE / 2, -SIDE / 2, 0];
        const key = new T.DirectionalLight(0xfffaf4, 2.2);
        key.position.set(mid[0] - 1900, mid[1] + 1500, 2600);
        key.target.position.set(...mid);
        key.castShadow = true;
        key.shadow.mapSize.set(shadowSize, shadowSize);
        const sc = key.shadow.camera;
        sc.left = -1300; sc.right = 1300; sc.top = 1300; sc.bottom = -1300; sc.near = 500; sc.far = 7000;
        key.shadow.radius = 5;
        key.shadow.bias = -0.0004;
        key.shadow.normalBias = 2;
        scene.add(key, key.target);
        const fill = new T.DirectionalLight(0xdfe8ff, 0.7);
        fill.position.set(mid[0] + 1800, mid[1] - 600, 1800);
        fill.target.position.set(...mid);
        scene.add(fill, fill.target);

        // A transparent plane at the plate that only shows the shadows.
        const catcher = new T.Mesh(new T.PlaneGeometry(4000, 4000), new T.ShadowMaterial({ opacity: 0.42 }));
        catcher.position.set(...mid);
        catcher.receiveShadow = true;
        scene.add(catcher);

        const tex = (img, srgb) => {
          const t = new T.Texture(img);
          t.flipY = false;
          if (srgb) t.colorSpace = T.SRGBColorSpace;
          t.anisotropy = 4;
          t.needsUpdate = true;
          return t;
        };
        const maps = { albedo: tex(al, true), normal: tex(no), orm: tex(orm) };
        const hand = side => {
          const rig = HandRig.create(side);
          rig.mesh.material = skinMaterial(T, maps);
          rig.mesh.castShadow = true;
          rig.mesh.receiveShadow = true;
          addForearm(T, rig, maps.albedo);
          scene.add(rig.root);
          return rig;
        };

        // Debug marks (location.hash with hands-dots): cyan dots at world
        // points, drawn over everything; each call replaces the last set.
        const dots = new T.Group();
        const dotGeo = new T.SphereGeometry(6, 12, 8);
        const dotMat = new T.MeshBasicMaterial({ color: 0x00ffff, depthTest: false, depthWrite: false, transparent: true });
        scene.add(dots);

        const render = () => r.render(scene, camera);
        const dotsWanted = () => /hands-dots/.test((root.location && root.location.hash) || '');

        // The show: its driver (setShow; driver.plan is its plan), the
        // generation the page gave it; the hands leaving after drop(); what
        // was drawn last; the WebGL context lost (until the browser restores it).
        let driver = null, generation = null, leave = null, lost = false;
        let away = null;                // each hand out of the frame: the pose of its plan's first keyframe
        let shown = true, dirty = true, lastT = NaN, drawn = { left: [], right: [] };
        let last = null;                // the last frame: { t, left, right } as driver.pose() gave them
        const rigsOf = () => ({ left: layer.left, right: layer.right });
        const hide = () => { layer.left.root.visible = layer.right.root.visible = false; };

        // Where each hand goes when it leaves: the plan's first keyframe,
        // which is always out of the frame (handplan.js timeline); the same
        // pose for every plan, taken once.
        function awayPoses(d) {
          const res = d.pose(-Infinity), out = {};
          for (const side of ['left', 'right']) {
            const rig = layer[side];
            out[side] = rig.getPose();
            if (!res[side].away) {
              // Not out of the frame (never so): 400 mm toward the player.
              out[side].pivot.y -= 400 * MM;
              out[side].pivot.z += LEAVE_LIFT_MM * MM;
            }
          }
          return out;
        }
        // The hands leaving at wall time nowMs: true while they still move.
        function leaving(nowMs) {
          const u = Math.min(1, (nowMs - leave.start) / LEAVE_MS), s = minJerk(u);
          const up = LEAVE_LIFT_MM * MM * minJerk(Math.min(1, 3 * u));
          for (const side of ['left', 'right']) {
            const rig = layer[side], from = leave.from[side];
            if (!from) continue;
            const p = blendPose(T, from, away[side], s);
            p.pivot.z += up;
            rig.setPose(p);
            rig.clear([], [], { free: true });
          }
          if (u < 1) return true;
          hide();
          leave = null;
          return false;
        }
        // Debug dots at the contact points of the fingers on (or reaching
        // or leaving) their controls, with hands-dots in the address.
        function contactDots(res) {
          const points = [];
          for (const side of ['left', 'right']) {
            if (!res[side] || res[side].away) continue;
            for (const c of res[side].touching) points.push(layer[side].contact(c.finger, c.toward));
          }
          layer.debugDots(points);
        }

        const layer = {
          canvas, scene, camera, renderer: r,
          left: hand('left'), right: hand('right'),
          render,
          // The show the page runs, as the n-th show (generation): builds its
          // plan and driver; the same generation again changes nothing. Ends
          // a leave under way: the new plan's hands start out of the frame.
          setShow(show, n) {
            if (driver && n === generation) return;
            driver = null;
            leave = null;
            const d = new HandRig.Driver(rigsOf(), HandPlan.build(show.actions));
            if (!away) away = awayPoses(d);
            hide();
            driver = d; generation = n;
            dirty = true; lastT = NaN;
          },
          // The show is over: the plan is dropped and the hands leave the
          // frame over LEAVE_MS of wall time (at once while the canvas is
          // hidden).
          drop() {
            if (!driver) return;
            driver = null;
            dirty = true; lastT = NaN; last = null;
            layer.debugDots([]);
            const from = {};
            for (const side of ['left', 'right']) from[side] = layer[side].root.visible ? layer[side].getPose() : null;
            if (!shown || !away || (!from.left && !from.right)) { hide(); return; }
            leave = { start: performance.now(), from };
          },
          // One animation frame, clock = { now, extrapolated, speed,
          // windowUs } (device µs; speed, the device's speed against the
          // wall clock): the hands at the driver's draw time, drawn only when
          // a pose changed, a leave runs, or the layer was just shown.
          frame(clock) {
            if (leave) {
              leaving(performance.now());
              last = null;
              render();
              return;
            }
            const forced = dirty;
            dirty = false;
            if (!driver) {
              if (forced) { hide(); layer.debugDots([]); render(); }
              return;
            }
            const t = driver.drawTime(clock.now, clock.extrapolated);
            if (t === lastT && !forced) return;
            lastT = t;
            const res = driver.pose(t, { windowUs: clock.windowUs || 0 });
            layer.left.root.visible = !res.left.away;
            layer.right.root.visible = !res.right.away;
            last = { t, left: res.left, right: res.right };
            const now = { left: poseNumbers(layer.left), right: poseNumbers(layer.right) };
            if (!forced && sameNumbers(now.left, drawn.left) && sameNumbers(now.right, drawn.right)) return;
            drawn = now;
            if (dotsWanted()) contactDots(res);
            else layer.debugDots([]);
            render();
          },
          // Whether the layer can draw: not while its WebGL context is lost
          // (the page then shows the ring); again once it is restored.
          get available() { return !lost; },
          // For the page's checks: the last frame's draw time and hands, the
          // driver, the show's generation.
          get last() { return last; },
          get driver() { return driver; },
          get generation() { return generation; },
          // Follows the container: the canvas's size and pixel ratio, and the
          // frustum through the panel square; then draws.
          resize() {
            const w = container.clientWidth, h = container.clientHeight;
            if (!w || !h) return;
            r.setPixelRatio(Math.min(maxRatio, root.devicePixelRatio || 1));
            r.setSize(w, h, false);
            const k = NEAR / EYE.z;
            camera.projectionMatrix.makePerspective((0 - EYE.x) * k, (SIDE - EYE.x) * k, (0 - EYE.y) * k, (-SIDE - EYE.y) * k,
              NEAR, FAR);
            camera.projectionMatrixInverse.copy(camera.projectionMatrix).invert();
            render();
          },
          setVisible(on) {
            on = !!on;
            if (on && !shown) dirty = true;
            shown = on;
            canvas.style.display = on ? '' : 'none';
          },
          // Compiles the shaders (the shadow pass's too) and uploads the
          // textures once, so that the first visible frame is not a long
          // task: both hands drawn once in the middle of the panel with the
          // canvas hidden, then hidden again.
          warm() {
            const display = canvas.style.display;
            canvas.style.display = 'none';
            const pose = { left: layer.left.getPose(), right: layer.right.getPose() };
            const up = new T.Vector3(0, 1, 0);
            layer.left.setPose({ pivot: new T.Vector3(SIDE * 0.35, -SIDE / 2, 60 * MM), dir: up });
            layer.right.setPose({ pivot: new T.Vector3(SIDE * 0.65, -SIDE / 2, 60 * MM), dir: up });
            const vis = [layer.left.root.visible, layer.right.root.visible];
            layer.left.root.visible = layer.right.root.visible = true;
            r.compile(scene, camera);
            render();
            layer.left.setPose(pose.left);
            layer.right.setPose(pose.right);
            [layer.left.root.visible, layer.right.root.visible] = vis;
            render();
            canvas.style.display = display;
          },
          // points: world points ({x, y, z}); shown only with hands-dots in
          // location.hash.
          debugDots(points) {
            const on = /hands-dots/.test((root.location && root.location.hash) || '');
            const list = on ? Array.from(points || []) : [];
            while (dots.children.length < list.length) {
              const d = new T.Mesh(dotGeo, dotMat);
              d.renderOrder = 10;
              dots.add(d);
            }
            dots.children.forEach((d, i) => {
              d.visible = i < list.length;
              if (d.visible) d.position.set(list[i].x, list[i].y, list[i].z);
            });
          },
        };
        hide();
        layer.resize();
        if (root.ResizeObserver) new ResizeObserver(() => layer.resize()).observe(container);
        // A lost context (the GPU reset, too many contexts) draws nothing
        // until the browser restores it; three.js rebuilds its own state
        // then and uploads the textures again, the room's reflection is
        // rendered anew, and the next frame draws everything again.
        canvas.addEventListener('webglcontextlost', e => { e.preventDefault(); lost = true; });
        canvas.addEventListener('webglcontextrestored', () => {
          lost = false;
          environment();
          dirty = true; lastT = NaN;
          layer.resize();
        });
        return layer;
      });
    }).catch(e => {
      if (renderer) renderer.dispose();
      if (canvas && canvas.parentNode) canvas.parentNode.removeChild(canvas);
      throw e;
    });
  }

  const api = { create };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.HandLayer = api;
})(typeof window !== 'undefined' ? window : globalThis);
