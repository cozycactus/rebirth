# Vendored inputs of the simulator page's hands

Two files of `sim/web/` are built from other people's work and committed as
built, so the page needs no build step for them. Their notices are in
`sim/web/notices.txt`, published with the page.

| Output | What it is | Built by |
|---|---|---|
| `sim/web/three.js` | three.js 0.187.0 (MIT): the exports listed in `three-entry.js`, minified into one classic script that sets `THREE` on the global object; no GLTFLoader; loads in Node too | `build-three.sh`, with esbuild 0.25.0 |
| `sim/web/handmodel.js` | `HAND_MODEL`: the two hands of Godot XR Tools (CC0), mesh and skeleton as base64 typed arrays, textures as JPEG data URLs (albedo 1024 px, quality 82; normal and ORM 512 px, quality 88); the layout is in its header | `build-handmodel.py` |

To build them again:

```bash
sim/vendor/build-three.sh                     # needs Node, npm and a network
python3 sim/vendor/build-handmodel.py [DIR]   # macOS (sips), Python 3.9+; downloads the sources, or reads them from DIR
```

`build-three.sh` installs `three@0.187.0` and `esbuild@0.25.0` into a
temporary folder and bundles `three-entry.js` with `--bundle --minify
--format=iife --legal-comments=inline --target=es2020`; the MIT licence
comment of three.js stays in the output, and the script checks that it does.

`build-handmodel.py` takes five files of
https://github.com/GodotVR/godot-xr-tools at commit
`6fc5361db295c12b5e51ee5b9fee09751a4458e7`, folder
`addons/godot-xr-tools/hands` (its `License.md`: CC0 1.0, DigitalN8m4r3; its
`About.md`: the mesh is MakeHuman output, CC0). It prints the SHA-256 of
each and stops unless they are these:

| File | SHA-256 |
|---|---|
| `model/Hand_Nails_L.gltf` | `cbb72c5fa29ce50c3289a4394161ab8a8bb732a0e8427f0a345906340e1f9311` |
| `model/Hand_Nails_R.gltf` | `88e6d161e997ffb9f8c98d56496671c3811c2e94abaa8d154708ad506db4d96e` |
| `textures/caucasian_realistic_baseColor.png` | `9979cc51739541c3b89b888194c919d106675c56e78aca125b6c0e6fb25792e6` |
| `textures/hands_normal.png` | `29b8dbcc738ec7de54596bddb3a1745cea2dce56a7472bd7e3f816e215af4498` |
| `textures/hands_occlusionRoughnessMetallic.png` | `8f8e7d7fed3985359731086726371dae6d64cb81530281e891d925bd216daac2` |

`node sim/test/hands_unit.js` checks both outputs (section "assets").
