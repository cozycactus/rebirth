#!/usr/bin/env python3
"""Builds sim/web/handmodel.js: the two hands of Godot XR Tools (CC0), as the
script that sets HAND_MODEL (see the header it writes).

    python3 sim/vendor/build-handmodel.py [FOLDER]

Without FOLDER the five source files are downloaded at the pinned commit; with
it they are read from FOLDER, by file name. Either way each must have the
SHA-256 recorded below. macOS only: the textures are converted with `sips`.
Python 3.9 or later, standard library only.
"""
import base64
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import urllib.request

REPO = 'https://github.com/GodotVR/godot-xr-tools'
COMMIT = '6fc5361db295c12b5e51ee5b9fee09751a4458e7'
FOLDER = 'addons/godot-xr-tools/hands'
RAW = 'https://raw.githubusercontent.com/GodotVR/godot-xr-tools/%s/%s/' % (COMMIT, FOLDER)
SOURCES = {  # path under FOLDER: SHA-256
    'model/Hand_Nails_L.gltf': 'cbb72c5fa29ce50c3289a4394161ab8a8bb732a0e8427f0a345906340e1f9311',
    'model/Hand_Nails_R.gltf': '88e6d161e997ffb9f8c98d56496671c3811c2e94abaa8d154708ad506db4d96e',
    'textures/caucasian_realistic_baseColor.png': '9979cc51739541c3b89b888194c919d106675c56e78aca125b6c0e6fb25792e6',
    'textures/hands_normal.png': '29b8dbcc738ec7de54596bddb3a1745cea2dce56a7472bd7e3f816e215af4498',
    'textures/hands_occlusionRoughnessMetallic.png': '8f8e7d7fed3985359731086726371dae6d64cb81530281e891d925bd216daac2',
}
TEXTURES = [  # key in HAND_MODEL.textures, source, size in px, JPEG quality
    ('albedo', 'textures/caucasian_realistic_baseColor.png', 1024, 82),
    ('normal', 'textures/hands_normal.png', 512, 88),
    ('orm', 'textures/hands_occlusionRoughnessMetallic.png', 512, 88),
]
OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web', 'handmodel.js'))

COMPONENT_SIZE = {5121: 1, 5123: 2, 5125: 4, 5126: 4}
COMPONENT_FORMAT = {5121: 'B', 5123: 'H', 5125: 'I'}
TYPE_SIZE = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def fail(message):
    sys.exit('build-handmodel: ' + message)


def read_sources(folder):
    data = {}
    for path, want in SOURCES.items():
        if folder:
            with open(os.path.join(folder, os.path.basename(path)), 'rb') as f:
                data[path] = f.read()
        else:
            with urllib.request.urlopen(RAW + path) as r:
                data[path] = r.read()
        got = hashlib.sha256(data[path]).hexdigest()
        print('%s  %s' % (got, path))
        if got != want:
            fail('%s has SHA-256 %s, want %s' % (path, got, want))
    return data


def accessor_bytes(gltf, buf, index):
    """The accessor's elements as raw little-endian bytes, and its component type."""
    a = gltf['accessors'][index]
    if 'sparse' in a or 'bufferView' not in a or a['componentType'] not in COMPONENT_SIZE:
        fail('accessor %d: sparse, without a buffer view or of signed components' % index)
    view = gltf['bufferViews'][a['bufferView']]
    size = COMPONENT_SIZE[a['componentType']] * TYPE_SIZE[a['type']]
    if view.get('byteStride', size) != size:
        fail('accessor %d: interleaved' % index)
    start = view.get('byteOffset', 0) + a.get('byteOffset', 0)
    return buf[start:start + a['count'] * size], a


def floats(gltf, buf, index, kind):
    raw, a = accessor_bytes(gltf, buf, index)
    if a['componentType'] != 5126 or a['type'] != kind:
        fail('accessor %d: want float %s' % (index, kind))
    return raw, a['count']


def uint16(gltf, buf, index):
    """Integer components widened or narrowed to little-endian Uint16."""
    raw, a = accessor_bytes(gltf, buf, index)
    form = COMPONENT_FORMAT.get(a['componentType'])
    if form is None:
        fail('accessor %d: not unsigned integers' % index)
    n = len(raw) // COMPONENT_SIZE[a['componentType']]
    values = struct.unpack('<%d%s' % (n, form), raw)
    if values and max(values) > 0xffff:
        fail('accessor %d: values above 65535' % index)
    return struct.pack('<%dH' % n, *values), a['count']


def identity(node):
    return ('matrix' not in node and node.get('translation', [0, 0, 0]) == [0, 0, 0]
            and node.get('rotation', [0, 0, 0, 1]) == [0, 0, 0, 1] and node.get('scale', [1, 1, 1]) == [1, 1, 1])


def b64(data):
    return base64.b64encode(data).decode('ascii')


def hand(text, suffix):
    gltf = json.loads(text)
    uri = gltf['buffers'][0]['uri']
    prefix = 'data:application/octet-stream;base64,'
    if len(gltf['buffers']) != 1 or not uri.startswith(prefix):
        fail('want one embedded buffer')
    buf = base64.b64decode(uri[len(prefix):])
    nodes = gltf['nodes']

    # The scene is the Armature, holding the mesh node and the wrist; both the
    # Armature and the mesh node are identity, so the Armature is folded away.
    roots = gltf['scenes'][gltf.get('scene', 0)]['nodes']
    if len(roots) != 1 or nodes[roots[0]]['name'] != 'Armature' or not identity(nodes[roots[0]]):
        fail('the scene is not one identity Armature')
    armature = nodes[roots[0]]
    mesh_nodes = [i for i in armature['children'] if 'mesh' in nodes[i]]
    if len(mesh_nodes) != 1 or not identity(nodes[mesh_nodes[0]]) or nodes[mesh_nodes[0]].get('skin') != 0:
        fail('want one identity mesh node with skin 0 under the Armature')
    mesh_node = nodes[mesh_nodes[0]]

    # Bones parent first (the file lists them children first).
    bones, bone_of = [], {}

    def visit(i, parent):
        node = nodes[i]
        if 'matrix' in node or 'mesh' in node:
            fail('bone node %s: matrix or mesh' % node['name'])
        bone_of[i] = len(bones)
        bones.append({'name': node['name'], 'parent': parent,
                      't': node.get('translation', [0, 0, 0]), 'r': node.get('rotation', [0, 0, 0, 1]),
                      's': node.get('scale', [1, 1, 1])})
        for child in node.get('children', []):
            visit(child, bone_of[i])

    for i in armature['children']:
        if i != mesh_nodes[0]:
            visit(i, -1)
    if not all(b['name'].endswith(suffix) for b in bones):
        fail('bone names without the suffix %s' % suffix)

    skin = gltf['skins'][0]
    joints = [bone_of.get(j, -1) for j in skin['joints']]
    if sorted(joints) != list(range(len(bones))):
        fail('the skin joints are not exactly the bones')
    inverse_bind, count = floats(gltf, buf, skin['inverseBindMatrices'], 'MAT4')
    if count != len(joints):
        fail('want an inverse bind matrix per joint')

    primitives = gltf['meshes'][mesh_node['mesh']]['primitives']
    if len(primitives) != 1 or primitives[0].get('mode', 4) != 4 or 'targets' in primitives[0]:
        fail('want one triangle primitive without morph targets')
    attrs = primitives[0]['attributes']
    position, count = floats(gltf, buf, attrs['POSITION'], 'VEC3')
    normal, _ = floats(gltf, buf, attrs['NORMAL'], 'VEC3')
    uv, _ = floats(gltf, buf, attrs['TEXCOORD_0'], 'VEC2')
    skin_index, _ = uint16(gltf, buf, attrs['JOINTS_0'])
    skin_weight, _ = floats(gltf, buf, attrs['WEIGHTS_0'], 'VEC4')
    index, _ = uint16(gltf, buf, primitives[0]['indices'])
    for name in ('POSITION', 'NORMAL', 'TEXCOORD_0', 'JOINTS_0', 'WEIGHTS_0'):
        if gltf['accessors'][attrs[name]]['count'] != count:
            fail('%s has another vertex count' % name)
    weights = struct.unpack('<%df' % (4 * count), skin_weight)
    worst = max(abs(sum(weights[4 * v:4 * v + 4]) - 1) for v in range(count))
    if worst > 1e-3:
        fail('skin weights sum to 1 within %g only' % worst)

    return {
        'bones': bones,
        'joints': joints,
        'inverseBind': b64(inverse_bind),
        'mesh': {'position': b64(position), 'normal': b64(normal), 'uv': b64(uv), 'skinWeight': b64(skin_weight),
                 'index': b64(index), 'skinIndex': b64(skin_index), 'count': count},
    }


def jpeg(png, px, quality, tmp):
    src, dst = os.path.join(tmp, 'in.png'), os.path.join(tmp, 'out.jpg')
    with open(src, 'wb') as f:
        f.write(png)
    subprocess.run(['sips', '-s', 'format', 'jpeg', '-s', 'formatOptions', str(quality), '-Z', str(px), src,
                    '--out', dst], check=True, stdout=subprocess.DEVNULL)
    with open(dst, 'rb') as f:
        return 'data:image/jpeg;base64,' + b64(f.read())


def main():
    if len(sys.argv) > 2:
        fail('usage: build-handmodel.py [FOLDER]')
    data = read_sources(sys.argv[1] if len(sys.argv) == 2 else None)
    model = {
        'right': hand(data['model/Hand_Nails_R.gltf'], '_R'),
        'left': hand(data['model/Hand_Nails_L.gltf'], '_L'),
    }
    with tempfile.TemporaryDirectory() as tmp:
        model['textures'] = {key: jpeg(data[path], px, q, tmp) for key, path, px, q in TEXTURES}

    def compact(value):
        return json.dumps(value, separators=(',', ':'))

    def side(name):
        m = model[name]
        bones = ''.join('        %s,\n' % compact(b) for b in m['bones'])
        rest = ''.join('      %s: %s,\n' % (k, compact(m[k])) for k in ('joints', 'inverseBind', 'mesh'))
        return '    %s: {\n      bones: [\n%s      ],\n%s    },' % (name, bones, rest)

    lines = [
        '// handmodel.js: the hand model of the simulator page\'s hands, as HAND_MODEL.',
        '// Generated by sim/vendor/build-handmodel.py; do not edit.',
        '//',
        '// Source: Godot XR Tools, %s,' % REPO,
        '// commit %s, %s/' % (COMMIT, FOLDER),
    ] + ['//   %s' % path for path in SOURCES] + [
        '// Licence: CC0 1.0 (DigitalN8m4r3; the mesh is MakeHuman output, CC0); see notices.txt.',
        '//',
        '// HAND_MODEL = { right, left, textures: { albedo, normal, orm } }. Per hand:',
        '// bones [{ name, parent (index in bones, or -1), t, r (quaternion x y z w), s }],',
        '// parents first, rest pose as in the glTF (metres); joints: the skin\'s joints',
        '// as indices in bones (skinIndex counts in joints); inverseBind: base64',
        '// Float32, 16 per joint, column-major; mesh: base64 little-endian position,',
        '// normal, uv, skinWeight (Float32), index, skinIndex (Uint16), and the vertex',
        '// count. UVs are as in the glTF (v down), so the textures need flipY = false.',
        '// Textures: JPEG data URLs, albedo 1024 px (sRGB), normal and ORM 512 px.',
        '(function (root) {',
        '  \'use strict\';',
        '  const api = {',
        side('right'),
        side('left'),
        '    textures: {',
    ] + ['      %s: %s,' % (k, compact(v)) for k, v in model['textures'].items()] + [
        '    },',
        '  };',
        '  if (typeof module !== \'undefined\' && module.exports) module.exports = api;',
        '  else root.HAND_MODEL = api;',
        '})(typeof window !== \'undefined\' ? window : globalThis);',
        '',
    ]
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(lines))
    with open(OUT, 'rb') as f:
        out = f.read()
    print('%s: %d bytes, sha256 %s' % (os.path.relpath(OUT), len(out), hashlib.sha256(out).hexdigest()))


if __name__ == '__main__':
    main()
