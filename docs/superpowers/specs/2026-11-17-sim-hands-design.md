# Hands on the Simulator Page — Design

Date: 2026-11-17. Status: the approach (3D hands over the drawn panel) and
the look of a one-hand prototype approved by the project owner in
conversation. Revised the same day after an independent review by three
models (see "Review changes"). This document is for the full feature.

## Intent

The simulator page (`sim/web/`, published at https://cozycactus.github.io/rebirth/)
shows a scripted Goa trance artist programming the device and playing a set.
Today the page only rings the control under the "finger" (`app.js`, CSS
classes `finger` and `down`). Whoever opens the page should instead see two
human hands at the device, pressing, holding and turning the controls the
artist uses, at the moments the firmware sees them. The panel must stay
readable: the device is read by its lights.

## Goals

1. Two realistic hands (skin, nails, knuckles, shadow on the panel) perform
   every finger action of the artist's score on the drawn panel: taps, long
   presses, several steps held by one hand while the other edits, two MUTE
   keys at once, a pad held while another is pressed (clip copy), the pad
   keyboard, encoder turns and encoder presses, PLAY.
2. At normal speed a fingertip is on the control when the firmware sees the
   press and leaves when it sees the release; a hand is never late.
3. Between gestures the hands get out of the way (lift off and leave the
   frame); during a busy stretch a hand waits lifted above its next control.
4. A checkbox hides the hands; the ring comes back.
5. The page keeps working from disk (`file://`) and on the public page;
   `sim/check.sh` stays green and also checks the hands' plan.
6. No audio crackle from the hands; the PR states the frame cost.

## Non-goals

- A hand for the user's own mouse, touch or keyboard input. The user sees
  their own pointer.
- Hands on the synth panels (`rig` moves) or on the MIDI keyboard card
  (`key` actions of the `keys` score): those happen beside the device. While
  the `keys` score plays a note, the editing hand stays lifted where it is.
- Physics, 3D models of the controls, VR, a free camera.
- Any change to the score: no new waits or actions in `artist.js`.
- Exact contacts at prep speeds x8 and x16 (see "Time").

## Anti-goals (a failure even if it works)

- At normal speed, a finger landing beside the control, or after the light
  already changed.
- Hands that sit on the pads and hide the lights while nothing happens.
- Rubber or plastic-looking hands (the first model tried looked like a
  mitten and was dropped).
- Any change in what the artist check sees: the hands only watch the score.
- Crackle in the 303s, or a page that no longer opens from disk.

## Constraints

- Classic scripts only, no modules, no `fetch`: the page also opens from
  `file://` (see `sim/README.md`). Assets are embedded as base64.
- `sim/publish.sh` copies `PAGE_FILES`, cache-busts `<script src="[a-z0-9_]+\.js">`
  and requires as many such tags in `index.html` as `PAGE_FILES` has `.js`
  files, less `LOADED_BY_APP` (12 tags since the merge of master's SH-101 and
  MIDI Song work). The hands add no tag: their scripts are listed in
  `PAGE_FILES` and in `LOADED_BY_APP`, and loaded from `app.js` with `?v=`
  added there.
- Global names `Hands`/`hands` are taken (`device.js`, `app.js`).
- The repository is GPL-3.0. Everything added is MIT or CC0 and keeps its
  notice, also on the published page.
- Real dimensions come from `hardware/panel/build_panel.py`: pad pitch 19.5
  mm = 91 page units, so the page's 1400-unit square is 300 mm (4.667 units
  per mm). Control tops above the plate: pads 5.6 mm, scene 4.6, mute 3.5,
  steps 4.0, PLAY 5.0, encoder knobs 20 mm across and 14.5 mm tall. Step
  buttons are 11 mm wide at a 13.5 mm pitch with a 3 mm gap between groups
  of four.

## Approaches considered

| Approach | Trade-off | Decision |
|---|---|---|
| 3D rigged hands in a transparent WebGL layer over the 2D panel | Real finger bending, shadows, any gesture; ~2 MB of scripts and a few ms of GPU per moving frame | **Chosen by the owner** |
| Pre-rendered 2D hand sprites in ~10 poses | Light, photo-like stills, but hands jump between poses and fingers never bend smoothly | Rejected |
| Drawn vector hands with bending fingers | Lightest, but cartoonish | Rejected |

Hand models considered: the WebXR input-profiles `generic-hand` (MIT,
2.3k triangles, untextured: looked like a mitten even subdivided and
shaded) and the hands of Godot XR Tools (CC0, made from MakeHuman output,
3.4k triangles with modelled nails, a realistic skin texture, a normal map
and an occlusion map). The prototype the owner approved uses the Godot XR
Tools hands.

## What the owner decided

- 3D hands, prototype first; the prototype's look is approved: real scale
  (an 18.7 cm hand covers about 4 x 5 pads), the Godot XR Tools skin
  ("caucasian realistic" texture, desaturated by a third), a dark sleeve,
  a soft shadow from a key light at the upper left, a press made with an
  extended index finger and the other fingers curled, the hand leaving the
  frame after the press.
- Accepted without objection when proposed: hands rest at the edges or lift
  away between gestures; at prep speeds x8 and x16 the hands run as in a
  fast-forwarded film, smoothed; at x40 and "instant" there are no hands and
  the ring is shown; a checkbox hides the hands.

## Design

### Files

| File | Role | Loads in Node |
|---|---|---|
| `sim/web/three.js` | three.js 0.187.0 (MIT): only the parts used (math, scene, skinned mesh, materials, lights, renderer), one IIFE setting `THREE` on the global object; no GLTFLoader | yes |
| `sim/web/handmodel.js` | `HAND_MODEL`: per hand the mesh as base64 typed arrays (positions, normals, UVs, indices, skin indices and weights) and the skeleton (bone names, parents, rest translations and rotations, inverse bind matrices); the three textures as base64 JPEG | yes |
| `sim/web/handrig.js` | `HandRig`: builds one hand (bones, skeleton, skinned mesh) from `HAND_MODEL` with three.js objects, poses it, FK, skinned positions of chosen vertices, and the fitting of fingertips to targets; no renderer, no DOM | yes |
| `sim/web/handplan.js` | `HandPlan`: the planner, pure functions from `show.actions` and the panel geometry to a plan per hand | yes |
| `sim/web/handlayer.js` | `HandLayer`: the WebGL canvas, camera, lights, materials, forearm and sleeve, rendering, and the page glue | browser only |
| `sim/web/notices.txt` | the third-party notices: three.js MIT text, the Godot XR Tools hands' CC0 statement and `About.md`, sources and revisions | — |
| `sim/vendor/` | `three-entry.js`, `build-three.sh` (pinned three 0.187.0 and esbuild 0.25.0), `build-handmodel.py` (reads the Godot XR Tools files at the pinned commit, writes `handmodel.js`), `README.md` | — |
| `sim/test/hands_check.js` | the hands' check, run by `sim/check.sh` | — |

The vendored outputs are committed: the page must work with no build step
beyond `sim/build.sh web`. The Godot XR Tools files are taken from commit
`6fc5361db295c12b5e51ee5b9fee09751a4458e7` of
https://github.com/GodotVR/godot-xr-tools (`addons/godot-xr-tools/hands`:
`model/Hand_Nails_L.gltf`, `model/Hand_Nails_R.gltf`,
`textures/caucasian_realistic_baseColor.png`, `textures/hands_normal.png`,
`textures/hands_occlusionRoughnessMetallic.png`; the folder's `License.md`
is CC0 1.0 by DigitalN8m4r3, its `About.md` says the mesh is MakeHuman
output, CC0). `build-handmodel.py` records each source file's SHA-256 in
`sim/vendor/README.md`. Textures go in as JPEG at 1024 px for the albedo and
512 px for the normal and ORM maps; the target for `handmodel.js` is about
1 MB. `publish.sh` copies `notices.txt` with the page and `sim/vendor/` into
`source/`, and the generated `source/README.md` names them.

### Loading, the toggle and the show's life cycle

- `index.html` gets no script tag for the hands. After the device is up, if
  the hands are enabled and WebGL2 is available, `app.js` loads `three.js`,
  `handmodel.js`, `handrig.js`, `handplan.js`, `handlayer.js` one after
  another (each script element's `onload` starts the next; `?v=` from
  `SIM_BUILD.short` when present), behind one promise. Nothing of the hands
  blocks the device or the audio from starting.
- When the layer is ready it compiles its shaders and uploads its textures
  once (`renderer.compile`) while the page is idle, so the first visible
  frame is not a long task.
- A checkbox "Показывать руки" in the artist card, on by default, its state
  kept in `localStorage` (reads and writes in try/catch).
- One function, `setHandsShown(on)`, owns the ring and the hands: when the
  hands are shown, `io.touch` still toggles `down` but not `finger`; when
  they are not (off, no WebGL, the load failed, prep at x40 or "instant"),
  `finger` works as today. On every switch it recomputes both classes for
  the controls held right now from the show's actions, so nothing is left
  ringed or pressed.
- The plan is built from the whole show in `startShow`, or when the scripts
  finish loading while a show runs; a show generation number makes a late
  load or a late callback for an old show do nothing. A control held at
  that moment is in the plan with its finger already on it.
- `endShow` (and so `takeOver`) and `reboot` drop the plan; the hands leave
  the frame over 300 ms of wall time.

### Time

- The hands are drawn at device time, the clock of the LEDs and of the
  `down` class. At each animation frame the layer takes `device.now()`.
  Free motion (travel, lifting, leaving) may be extrapolated by wall time
  times the speed, by at most one tick, but the draw time never passes the
  next down or up that the device has not processed yet: a finger never
  touches before its control shows `down`, nor leaves before it shows up.
- At normal speed (the live set, and prep at x1 if ever used) every
  contact is drawn as planned.
- At prep speeds x8 and x16 a 10 ms tick advances the device by 80 to 160
  ms and a tap can begin and end inside one tick, so exact contacts cannot
  be shown. There the hand's root follows a moving average of its planned
  root over a window centred on the draw time, about two frames of device
  time wide. The plan is known ahead, so the average does not lag. Fingers
  are fitted from that root to their targets when within reach. A tap
  shorter than a frame may show as a passing touch or not at all; the
  `down` class still shows it.

### The planner (`handplan.js`)

Input: `show.actions` and the panel geometry (control centres and top
heights in page units, from the same numbers `app.js` draws with). Output:
per hand a timeline of keyframes saying what each finger aims at and when
(free, approaching a target, in contact with a target, lifting, pinching a
knob). `handplan.js` decides which hand and finger, where and when; it does
not compute joint angles or the hand's root, which the driver fits from the
targets (see "Poses and fitting").

1. **Intervals.** Each control's down is paired with its next up: one
   interval per press. `key`, `rig` and `say` actions are not touches.
2. **Groups.** Intervals on controls of one kind (steps, mutes, pads) whose
   downs fall within 100 ms of the first and that overlap form a group,
   played by several fingers of one hand when reachable (see 4). A group
   or an interval during which other touches begin and end is a hold; those
   other touches go to the other hand.
3. **Encoder runs.** Edges of one encoder form a run; runs less than 150 ms
   apart are one turn (the grip is kept between the bursts of
   `turnNumber`, which are 126 ms apart). A turn ends 20 ms after its last
   edge. A press of the same
   encoder within 200 ms after a turn continues the same gesture: the pinch
   becomes a push by the index finger without the hand leaving.
4. **Hands.**
   - A hold group goes to the hand on the side of the group's centre
     relative to what the other hand will do during it: the holder takes the
     outer side, the editing hand comes from the other. With no editing in
     it, the side of the panel's centre decides.
   - A clip copy (a pad held, then a pad 1 to 3 rows nearer the player in
     the same column pressed) uses one hand: the index finger holds the
     source pad and the thumb presses the target. The hand turns (its yaw is
     free in the fit) so that the thumb comes below the index. Index and
     middle cannot do it (the rows run along the fingers: in a Node trial
     with the model the middle finger missed by 6 to 34 mm), and two hands in
     one column put the holding hand over the pad the other must press.
   - Two MUTE keys at once: one hand (index and middle, or index and ring)
     when at most two tracks apart, otherwise both hands.
   - Anything else: the free hand whose last position is nearer, with a
     penalty for crossing the other hand, and a mild preference for the
     right hand.
5. **Fingers.** A tap or an encoder press: the index finger. A clip copy:
   index and thumb (see 4). A hold group:
   up to four fingers in order across the buttons (for the left hand pinky,
   ring, middle, index from left to right; mirrored for the right). An
   encoder turn: thumb and index pinch the knob. A finger keeps its target
   from down to up; the others of the same hand come and go around it.
6. **Timing.** A fingertip touches at the down's `t` and leaves at the up's
   `t`. It arrives over the control 15 mm up about 80 ms before; when the
   hand's previous contact ends sooner, the approach shortens, down to a
   direct move. After a release the finger lifts 15 mm. Hand travel uses
   minimum-jerk interpolation.
7. **Idle.** A hand whose next gesture is more than 1.5 s away (device
   time) leaves the frame (down past the panel's front edge) and comes back
   in time. Otherwise it moves toward its next control and waits lifted 30
   mm above it, fingers relaxed. While a `key` note of the `keys` score
   sounds, the hand that would otherwise edit stays where it is, lifted.

### Poses and fitting (`handrig.js`)

- The skeleton is the model's (hierarchical, Godot naming `Wrist_R`,
  `Index_Proximal_R`, ...). A bone's local +Y runs along the bone, +X out of
  the back of the hand, Z across; a curl is a rotation about Z, a spread
  about X. The left hand is checked against these conventions at load and
  a sign that differs is taken from the model, not assumed.
- A fingertip touches with its contact point: in the current pose, the
  deepest vertex of its last two segments along the contact direction (down
  onto a control's top; toward the axis on a knob's side). The point moves
  over the fingertip as the finger tilts, as a real fingertip's does; the
  fit re-picks it and refits (at most 3 rounds).
- Pose library (curl per joint, degrees): `point` (pressing finger nearly
  straight, the others curled), `relax`, `fist` (where a free finger goes to
  clear the panel), `lift` (the thumb's way up), `pinch`.
- The hand points from a shoulder point in front of the panel (page
  x 350 for the left, 1050 for the right; page y 2000, that is 130 mm in
  front of the front edge; 350 mm above the plate) toward the control, and
  is pitched down about 20 degrees, as the prototype's press.
- One fingertip (seeded from `point`): the root is translated so the
  contact point is exactly on the target. The target is the control's centre raised to its top height
  along the eye ray, so it lands on the drawn spot (the camera has a
  shifted lens; see below).
- Several fingertips (held steps, a one-hand MUTE pair, a clip copy; seeded
  from nearly straight fingers, with the hand's yaw first turned to the best
  rigid alignment of the fingertips with the targets): each
  finger has three angles (spread at the knuckle, flex at the knuckle, flex
  at the middle joint with the last joint following at two thirds; for the
  thumb, two at its base and one shared by its other joints), solved by
  damped least squares within joint limits (spread ±20°, flex 0–95°)
  together with the root's position, yaw and pitch, with a light pull
  toward the seed pose so the solution stays natural. If fingertips still
  hook under, the descent of the last segment is capped. A target left more
  than 1 mm away is an error that the check reports (no silent pose).
- Clearance: about 60 vertices spread over the palm, knuckles and fingers
  are skinned each fitted frame; free fingers curl toward `fist` (the thumb
  toward `lift`) and, if that is not enough, the root rises, until no
  such vertex is below the plate or inside a control (a box up to its top;
  an encoder knob is a cylinder 20 mm across and 14.5 mm tall, so fingers
  on its side are allowed). A touching finger's last two segments may sink
  up to 1 mm into its own target (a fingertip pressed into silicone), never
  below the plate.
- Encoder turn: thumb and index contact points on opposite sides of the
  knob, 12 mm above the plate (the knob is 14.5 mm tall), the hand free to
  roll up to 20° thumb side up; the grip at a twist is the grip at twist 0
  turned about the knob's axis. The hand's twist follows the knob angle (15° a detent, as
  `app.js` draws the knob) within a window of ±20° (the grip at twist 0 is centred on the arm's direction, so the forearm never swings far); inside a fast burst the
  fingers slide on the knob and the twist eases back toward the middle of
  the window; a full lift and regrip happens only in a pause of 100 ms or
  more between bursts. Encoder press: the index contact point on the knob
  top (14.5 mm).

### Drawing (`handlayer.js`)

As in the approved prototype:

- A canvas over `#device` (same box, `pointer-events: none`), WebGL2,
  transparent, pixel ratio capped at 2.
- Camera: the eye above page point (700, 2100), that is centred across the
  panel and 700 units (150 mm) in front of its front edge, 4600 units
  (~1 m) above the plate. The camera's orientation is identity (it looks
  straight down, no `lookAt`); the frustum is shifted with
  `makePerspective(left, right, top, bottom)` so the panel plane maps
  exactly onto the drawing (no keystone), while the hands are seen slightly
  from the player's side.
- Lights: hemisphere (0.6), a key light from the upper left (2.2) casting a
  PCF shadow (2048 map, radius 5) onto a transparent shadow-catcher plane at
  the plate, a cool fill from the lower right (0.7); a dim room environment
  map for reflections. No tone mapping.
- Skin: physical material with the albedo (desaturated by a third in the
  shader), the normal map at 0.3, the ORM's occlusion, roughness 0.62, a
  little sheen, and a wrapped-diffuse red term standing in for subsurface
  scattering. Forearm: an elliptic tube from inside the wrist, the size of
  the mesh's wrist section, coloured from the texture at the wrist; a dark
  sleeve from 2 mm behind the wrist.
- Rendering on demand: a frame is drawn only while a hand moves or a
  transition is under way; a still page costs nothing.

### The check (`sim/test/hands_check.js`)

Run by `sim/check.sh` once (not per firmware score: the hands need only
`artist.js`), over every song in every arrangement in all four scores
(`base`, `fw2`, `keys`, `pads`). It loads `three.js`, `handmodel.js`,
`handrig.js` and `handplan.js` in Node, builds the model once, plans each
show and checks, against a table of touches it builds itself from
`show.actions` (not from the plan):

1. every press interval of a panel control is covered by one finger of one
   hand from down to up; every encoder run by a pinch on that encoder;
2. the hand rules: a hold and the touches inside it are on different
   hands; a clip copy is one hand, index and thumb; a MUTE pair follows the
   distance rule;
3. at the down, the middle and 1 ms before the up of every interval, the
   finger's contact point is within 1.5 mm of the target (FK through
   `handrig.js`);
   during a pinch, both pads are within 2 mm of the knob's rim;
4. at those moments no clearance vertex of either hand is below the plate
   or the top of the control under it;
5. in the live set, the two hands' palm boxes do not overlap, and the left
   hand's wrist is left of the right one when both are over the panel;
6. in the live set, a hand's peak speed, measured on the driver's poses,
   stays under 4 m/s.

The preparation is never drawn at normal speed (x8, x16, or not at all), and
its tempo sometimes leaves no assignment that avoids both a crossing and a
late hand (the kit loop: TRACK, steps on both sides of the knob 66–100 ms
later, TRACK again). There the check counts crossings and palm overlaps and
bounds the peak speed at 9 m/s (8 m/s until the merge of master's SH-101
songs; see "As built"), but does not fail on a crossing.

The poses of identical situations are cached. The check prints its running
time; if the whole run exceeds two minutes in CI, the exact fitting (3, 4)
runs on one song per arrangement and score and the rest on every show. The
artist check itself is untouched.

### First milestone: the hardest poses

Before the planner is built out, the rig fits and the layer draws the
hardest single moments, and the owner sees a frame of each: a clip copy 3
rows apart (index and thumb); steps 2, 4, 6 and 8 held while the other hand
turns TRACK; four steps spanning seven buttons with two of them adjacent;
a MUTE pair two tracks apart; the encoder pinch. A pose that cannot be fitted
within 1 mm, or that looks wrong, changes the rules above before anything
depends on them.

### Performance

- Rendering cost about 3.5 ms a frame at 1520 x 1520 pixels on an Apple M1
  with one hand in the prototype (measured with a forced GPU sync, so an
  upper bound); nothing while still.
- Built in from the start: while the 303s run on the main thread (the page
  opened from disk, where no AudioWorklet loads; `rig.js` then uses a
  1024-frame `ScriptProcessorNode`), the shadow map is 1024 and the pixel
  ratio 1.5.
- The test, which an agent can run: over the same 60 s live stretch (with a
  MUTE pair and a hold edit), once with the hands off and once on, from disk
  and over HTTP with the audio on, record the main-thread long tasks
  (`PerformanceObserver`), the frame times (median, p95, p99), and on the
  main-thread path the intervals between `onaudioprocess` calls. Pass: with
  the hands, no long task over 50 ms that is absent without them (the
  load and the warm-up are reported apart, before the show), and no more
  callback intervals over 1.5 buffer durations (32 ms at 48 kHz) than
  without. If it fails from disk, the hands are off by default there. The
  PR reports the numbers; the owner may also listen on the PR's page, but
  the merge does not wait on that.

### Documentation

`sim/README.md` gets a section "Hands": what they are, the toggle, where the
models come from and under which licence, how `three.js` and `handmodel.js`
are rebuilt, and the frame cost. The PR carries screenshots and a short
recording.

## Review changes

An independent review (Codex, Grok, DeepSeek; Gemini could not run) found,
and this revision fixes:

- Encoder gestures: `turnNumber` presses the same encoder 96 ms after its
  last edge and bursts are 126 ms apart, so a turn now ends 20 ms after its
  last edge, bursts closer than 150 ms are one turn, and a press right after
  a turn continues the gesture. A 120 ms regrip every 60° could not keep up
  with 25 ms detents: the twist now has a window, fingers slide inside a
  burst, regrips only in pauses.
- Grouping: the 50 ms chord window would have caught the steps a hold puts
  down 30 ms apart; touches are now paired down/up first, then grouped.
- Copies use both hands (rows run along the fingers); a hold's hand is
  chosen against what the other hand does during it, not by step 8.
- Two angles per finger cannot place a fingertip in three dimensions:
  three per finger plus an outer loop on the root; unreachable is an error.
- Clearance and the check looked at fingertip pads only: they now use
  skinned vertices over the whole hand.
- At x16 a 40 ms wall-time spring would have lagged 640 ms of device time:
  replaced by a centred average of the plan, and exact contacts are no
  longer promised at x8/x16. Extrapolation can no longer pass an
  unprocessed down or up.
- The check compared the plan with its own labels: it now builds its own
  table of touches from the score, samples whole holds and encoder runs,
  and runs all four scores regardless of the firmware.
- Node: the model is converted to plain arrays at build time, so the rig
  needs no GLTFLoader, images or DOM.
- Life cycle: sequential loading behind one promise, a show generation
  number, `setHandsShown` owning the ring, hooks in `startShow`, `endShow`
  and `reboot`, shader warm-up.
- Publishing: the hands add no script tag to `index.html` (`publish.sh`
  counts the tags from `PAGE_FILES` less `LOADED_BY_APP`); notices are
  published with the page and the vendor recipes with the source.

A second review, of the implementation plan, moved the turn threshold from
110 to 150 ms (the plan's own fixture had bursts 126 ms apart), made the
knob a cylinder for clearance, let the driver fit the root with the fingers
in one solve, and moved the hand speed check from the plan to the driver's
poses.

Building the fitting (plan Task 3) then showed that a contact point fixed
on the mesh at load cannot stay a fingertip's lowest point once the finger
tilts past about 43°: the fit reached every target, but the rest of the
fingertip sank 0.4–6 mm into the control. The contact point is now picked
per pose, the seeds and the yaw alignment are as above, and a fingertip may
sink 1 mm into its own target.

A builder's read of the first version then found that a one-hand copy with
index and middle cannot reach a pad straight below (now index and thumb,
with a first milestone that shows the hardest poses), and that "listen for
crackle" is not a test an agent can run (now a measured stand-in with a pass
rule, and the main-thread fallback built in).

## As built (after the final reviews)

The final whole-branch review and a three-model code review (Codex, Grok,
DeepSeek) led to one fix wave. What it changed against the text above:

- Time: the hands extrapolate with the clock the device ticks by (the audio
  clock when the synths sound), and their draw time never goes back within a
  show. Encoder edges are events too, so a pinch never turns ahead of the
  drawn knob. A contact holds on [down, up): at the up itself, and before the
  down, the fingertip hovers 0.3 mm above its control.
- Loading: the scripts may load at any time, but the layer is built and
  warmed only while the set is not live (the ring is shown until then). The
  renderer refuses a software context (`failIfMajorPerformanceCaveat`); a lost
  WebGL context brings the ring back until it is restored. The `down` and
  `finger` classes are rebuilt from the controls the artist holds, never from
  the plan. The hands are off by default, over HTTP and from disk (the
  owner's choice after the PR was opened; first only from disk, as the
  measurement decided): the box turns them on, the browser keeps it, and the
  scripts load only then.
- Clearance and the check: every fingertip vertex is a clearance vertex; the
  check tests the whole mesh against the controls' volumes, that a grip turns
  with the knob (never more than 20.5°), that a group's fingers are on one hand
  in order, that the panel geometry matches `app.js`, and samples the live set
  every 20 ms on 4 of the 32 posed shows (every show with `HANDS_FULL=1`). The
  default run takes about 70 s on an M1; `HANDS_FULL=1` 13–29 minutes.
- Known limitation: a pinch's knob axis is the drawn centre at the plate, so
  with the shifted lens the grip is drawn about 23 page units (12 px at the
  page's normal size) above the drawn knob. Moving the axis onto the eye ray at
  the grip's height put the approved grip's thumb 1.1–1.5 mm into the first
  pad row, and no variant fixed both; the owner approved the frames with this
  offset.
- Merge of master's SH-101 voices, 64-track sequencer and MIDI Song player:
  the score's `pad-midi` actions (the PAD phrases of the four SH-101 songs,
  MIDI straight to the synth) touch no control. A MIDI Song has no artist
  and gets no plan, and the hands are not built while one loads or plays
  (the 0.2–0.3 s task would stall the device's tick beyond the look-ahead).
  `index.html` has 12 script tags, the count `publish.sh` takes from
  `PAGE_FILES` less `LOADED_BY_APP` (the five hand scripts `app.js` loads).
  The check plans 240 shows; the preparation's speed bound is 9 m/s (two of
  the new songs reach 8.00 and 8.33 m/s there, the old ones 7.90), the live
  set's still 4 m/s.
- 2026-11-25, `2026-11-25-sham-live-mutes-design.md`: the hands also play
  the performed clips mode ("Клипы на падах · артист играет MUTE"): its
  loading and MUTE performance are one show at speed 1 with a plan, and the
  hands may be built while it loads, before anything is scheduled. The
  plain clips mode and a MIDI Song still get none, and the build still
  never runs while either plays. (`index.html` now has 13 script tags, the
  count `publish.sh` takes from `PAGE_FILES` less `LOADED_BY_APP`; the 12
  above was the count at the time.)

## Left to the builder

Exact pose angles, lead times within the limits above, the cost weights of
the hand choice, the twist window and regrip details, the averaging window,
the light intensities, the choice of clearance vertices, and file-internal
structure. Anything that changes something the owner has seen (the hand's
size, skin, light direction, the leave-the-frame behaviour) comes back to
the owner.
