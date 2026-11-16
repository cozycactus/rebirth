"""What is inside the case: the two boards, the cable and the stack-up, animated.

A look-and-check model for the PCB step (hardware/pcb/README.md), built on the
panel geometry of build_panel.py. It is not a manufacturing model, but every
height below is a real constraint of the chosen parts:

  - MX switches soldered to the panel board, keycaps through windows in the
    2 mm top plate, plate on 15 mm standoffs above the board;
  - SK6812MINI-E reverse mounted on the bottom of the panel board, lighting
    through a cutout into the switch window;
  - main board on the floor at the rear wall: Pico (micro-USB at the wall),
    DACs, MIDI, right-angle 3.5 mm jacks and DIN-5 sockets;
  - one 26-wire IDC ribbon between the boards.

The case is taller than the look-and-feel mockup: MX switches need about
27 mm at the front edge (mockup: 20 mm), see FRONT/REAR below.

Inside a running Blender (adds the scene "Sequencer Internals", other scenes stay):
    p = "/path/to/build_internals.py"
    g = {"__file__": p, "__name__": "internals"}
    exec(compile(open(p).read(), p, "exec"), g); g["build_in_new_scene"]()

Headless video:
    Blender --background --factory-startup --python build_internals.py -- \
        --video out.mp4 [--res 1280 720] [--blend out.blend]

Sizes are millimetres; Blender units are metres.
"""

import math
import os
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
_g = {"__file__": os.path.join(HERE, "build_panel.py"), "__name__": "build_panel_geometry"}
exec(compile(open(_g["__file__"]).read(), _g["__file__"], "exec"), _g)
P = type("PanelGeometry", (), _g)  # constants and helpers of the mockup

SCENE_NAME = "Sequencer Internals"
MM = 0.001
FPS = 30
FRAMES = 420

# ------------------------------------------------------------------ the case
PANEL_W = P.PANEL_W          # 260, between the cheeks
DEPTH = P.DEPTH              # 270
FRONT = 28.0                 # outer height of the tray at the front (mockup 20)
REAR = 50.0                  # ... and at the rear (mockup 45)
TILT = math.atan2(REAR - FRONT, DEPTH)
PANEL_L = math.hypot(DEPTH, REAR - FRONT)
PLATE_T = 2.0
WALL = 3.0
FLOOR = 3.0
CHEEK_T = P.CHEEK_T
CHEEK_RISE = P.CHEEK_RISE

# ------------------------------------------------------------------ stack-up (panel frame)
# Panel frame: x across, y along the slope (+y to the rear), z up from the
# bottom of the top plate. The panel board hangs under the plate.
STANDOFF = 15.0              # plate bottom -> panel board top
PCB_T = 1.6
ZP = -STANDOFF               # panel board top
ZB = ZP - PCB_T              # panel board bottom
MX_LOWER, MX_TOP, MX_STEM = 5.0, 11.6, 16.6     # above the board top (Cherry MX)
CAP_TOP = 20.1               # unpressed DSA-height cap top above the board top
PANEL_X0, PANEL_X1 = -124.0, 124.0
PANEL_Y0, PANEL_Y1 = -128.0, 123.0

# ------------------------------------------------------------------ main board (world)
MAIN_X0, MAIN_X1 = -122.0, 122.0
MAIN_Y0, MAIN_Y1 = 74.0, DEPTH / 2 - WALL - 0.5
MAIN_STANDOFF = 5.0
MAIN_Z = FLOOR + MAIN_STANDOFF + PCB_T          # top of the main board
PICO_X = -100.0
JACKS = [("CV %d" % (i + 1), 112.0 - 14.0 * i) for i in range(4)] + \
        [("GATE %d" % (i + 1), 50.0 - 14.0 * i) for i in range(4)]
DINS = [("MIDI IN", -30.0), ("MIDI OUT", -62.0)]
J_MAIN = (-62.0, 90.5)       # right-angle 2x13 IDC header on the main board, opening to the front
J_PANEL = (-61.0, 83.0)      # right-angle 2x13 IDC header under the panel board, opening to the front
IDC_DEPTH = 15.0             # header body from the pins to the opening

STATE_PLAYING_ROW = P.PLAYING


# ================================================================== helpers

class Materials:
    def __init__(self):
        self.cache = {}

    def get(self, name, base, rough=0.5, metal=0.0, emit=None, strength=0.0, alpha=1.0,
            transmission=0.0, coat=0.0):
        if name in self.cache:
            return self.cache[name]
        m = bpy.data.materials.get("SI_" + name) or bpy.data.materials.new("SI_" + name)
        try:
            m.use_nodes = True
        except Exception:
            pass
        b = m.node_tree.nodes["Principled BSDF"]
        b.inputs["Base Color"].default_value = (*base, 1.0)
        b.inputs["Roughness"].default_value = rough
        b.inputs["Metallic"].default_value = metal
        if coat:
            b.inputs["Coat Weight"].default_value = coat
        if transmission:
            b.inputs["Transmission Weight"].default_value = transmission
        if alpha < 1.0:
            b.inputs["Alpha"].default_value = alpha
            for attr, val in (("surface_render_method", "BLENDED"), ("blend_method", "BLEND")):
                try:
                    setattr(m, attr, val)
                except Exception:
                    pass
        if emit is not None and strength > 0:
            b.inputs["Emission Color"].default_value = (*emit, 1.0)
            b.inputs["Emission Strength"].default_value = strength
        m.diffuse_color = (*(emit if (emit is not None and strength > 0) else base), alpha)
        self.cache[name] = m
        return m


class Batch:
    """Many simple solids in one mesh object (one material slot per material)."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.mats = []

    def _slot(self, mat):
        if mat not in self.mats:
            self.mats.append(mat)
        return self.mats.index(mat)

    def _add(self, geom_verts, faces, slot):
        for f in faces:
            f.material_index = slot

    def box(self, size, center, mat, rz=0.0):
        sx, sy, sz = size
        cx, cy, cz = center
        res = bmesh.ops.create_cube(self.bm, size=1.0)
        m = Matrix.Translation(Vector((cx, cy, cz)) * MM) @ Matrix.Rotation(rz, 4, "Z") @ \
            Matrix.Diagonal(Vector((sx * MM, sy * MM, sz * MM, 1.0)))
        bmesh.ops.transform(self.bm, matrix=m, verts=res["verts"])
        slot = self._slot(mat)
        for f in {f for v in res["verts"] for f in v.link_faces}:
            f.material_index = slot

    def cyl(self, r, h, center, mat, axis="Z", segments=24, r2=None):
        res = bmesh.ops.create_cone(self.bm, cap_ends=True, cap_tris=False, segments=segments,
                                    radius1=r * MM, radius2=(r if r2 is None else r2) * MM, depth=h * MM)
        rot = Matrix.Identity(4) if axis == "Z" else Matrix.Rotation(-math.pi / 2, 4, "X")
        m = Matrix.Translation(Vector(center) * MM) @ rot
        bmesh.ops.transform(self.bm, matrix=m, verts=res["verts"])
        slot = self._slot(mat)
        for f in {f for v in res["verts"] for f in v.link_faces}:
            f.material_index = slot

    def finish(self, coll, parent=None, bevel=0.0):
        me = bpy.data.meshes.new("SI_" + self.name)
        self.bm.to_mesh(me)
        self.bm.free()
        for m in self.mats:
            me.materials.append(m)
        ob = bpy.data.objects.new("SI_" + self.name, me)
        coll.objects.link(ob)
        ob.parent = parent
        if bevel > 0:
            mod = ob.modifiers.new("Bevel", "BEVEL")
            mod.width = bevel * MM
            mod.segments = 2
            mod.limit_method = "ANGLE"
        return ob


def empty(coll, name, loc=(0, 0, 0), rot=(0, 0, 0), parent=None, size=0.01):
    ob = bpy.data.objects.new("SI_" + name, None)
    coll.objects.link(ob)
    ob.parent = parent
    ob.location = Vector(loc) * MM
    ob.rotation_euler = rot
    ob.empty_display_size = size
    return ob


def apply_boolean(ob, cutter):
    mod = ob.modifiers.new("cut", "BOOLEAN")
    mod.operation = "DIFFERENCE"
    mod.solver = "EXACT"
    mod.object = cutter
    dg = bpy.context.evaluated_depsgraph_get()
    dg.update()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    old = ob.data
    ob.modifiers.remove(mod)
    ob.data = me
    bpy.data.meshes.remove(old)
    bpy.data.objects.remove(cutter)


def text(coll, name, body, loc, size, mat, parent=None, rot=(math.radians(90), 0, 0), align="CENTER"):
    cu = bpy.data.curves.new("SI_" + name, "FONT")
    cu.body = body
    cu.size = size * MM
    cu.align_x = align
    cu.align_y = "CENTER"
    cu.materials.append(mat)
    ob = bpy.data.objects.new("SI_" + name, cu)
    coll.objects.link(ob)
    ob.parent = parent
    ob.location = Vector(loc) * MM
    ob.rotation_euler = rot
    return ob


def plate_z_world(y):
    """World height of the plate bottom above y (the tray top)."""
    return FRONT + (REAR - FRONT) * (y + DEPTH / 2) / DEPTH


# ================================================================== panel content

def pad_state(c, r):
    has_clip = r in P.CLIPS.get(c, [])
    if P.QUEUED.get(c) == r:
        return "queued"
    if P.PLAYING.get(c) == r:
        return "playing" if has_clip else "stopped_here"
    return "clip" if has_clip else "empty"


def key_positions():
    """(kind, x, y, cap_w, cap_h, chain_state) for every MX key of the panel."""
    keys = []
    for r in range(8):
        for c in range(8):
            keys.append(("pad", P.col_x(c), P.row_y(r), 17.0, 17.0, (c, r)))
    for r in range(8):
        keys.append(("scene", P.SCENE_X, P.row_y(r), 14.0, 14.0, r))
    for c in range(8):
        keys.append(("mute", P.col_x(c), P.MUTE_Y, 17.0, 8.0, c))
    keys.append(("play", P.PLAY_C[0], P.PLAY_C[1], 14.0, 14.0, None))
    return keys


def cap_material(m, kind, info):
    dark = m.get("cap_dark", (0.10, 0.10, 0.105), rough=0.6)
    if kind == "pad":
        c, r = info
        color = P.TRACK_COLORS[c]
        st = pad_state(c, r)
        if st == "playing":
            return m.get("cap_play_%d" % c, color, rough=0.6, emit=color, strength=7.0)
        if st == "queued":
            return m.get("cap_queue_%d" % c, color, rough=0.6, emit=color, strength=3.0)
        if st == "clip":
            return m.get("cap_clip_%d" % c, tuple(0.4 * k for k in color), rough=0.6,
                         emit=color, strength=0.9)
        return dark
    if kind == "scene":
        used = {r for rows in P.CLIPS.values() for r in rows}
        if info == P.PLAYING_SCENE:
            return m.get("cap_scene_on", (0.2, 0.9, 0.3), emit=(0.2, 1.0, 0.3), strength=4.0)
        return m.get("cap_scene_has", (0.55, 0.55, 0.55), emit=(1, 1, 1), strength=0.35) \
            if info in used else dark
    if kind == "mute":
        color = P.TRACK_COLORS[info]
        if info in P.MUTED:
            return dark
        return m.get("cap_mute_%d" % info, tuple(0.5 * k for k in color), rough=0.6,
                     emit=color, strength=1.2)
    return m.get("cap_playkey", (0.2, 0.9, 0.3), emit=(0.2, 1.0, 0.3), strength=5.0)


def build_plate(coll, m, parent):
    alu = m.get("graphite_alu", (0.045, 0.045, 0.05), rough=0.36, metal=0.9)
    plate = Batch("plate")
    plate.box((PANEL_W - 0.4, PANEL_L, PLATE_T), (0, 0, PLATE_T / 2), alu)
    ob = plate.finish(coll, parent)
    cut = Batch("plate_cutter")
    for kind, x, y, w, h, _ in key_positions():
        cut.box((w + 0.8, h + 0.8, 6), (x, y, 1), alu)
    for i in range(16):
        cut.box((P.STEP_W + 0.8, P.STEP_H + 0.8, 6), (P.step_x(i), P.STEP_Y, 1), alu)
        cut.cyl(1.7, 6, (P.step_x(i), P.STEP_LED_Y, 1), alu, segments=16)
    for (ex, ey), _, _ in P.ENCODERS:
        cut.cyl(4.0, 6, (ex, ey, 1), alu, segments=24)
    cut.box((57.0, 29.0, 6), (P.OLED_C[0], P.OLED_C[1], 1), alu)
    cut.cyl(1.7, 6, (P.BEAT_C[0], P.BEAT_C[1], 1), alu, segments=16)
    cutter = cut.finish(coll, parent)
    apply_boolean(ob, cutter)
    bev = ob.modifiers.new("Bevel", "BEVEL")
    bev.width = 0.4 * MM
    bev.segments = 2
    bev.limit_method = "ANGLE"
    ink = m.get("print", (0.78, 0.78, 0.76), rough=0.55)
    for label, (x, y) in (("SCENE", (P.SCENE_X, P.row_y(0) + 15.0)),
                          ("MUTE", (P.col_x(0) - 19.5, P.MUTE_Y))):
        text(coll, "plate_%s" % label, label, (x, y, PLATE_T + 0.03), 3.0, ink, parent=parent,
             rot=(0, 0, 0))
    return ob


def build_caps(coll, m, parent):
    caps = Batch("caps")
    for kind, x, y, w, h, info in key_positions():
        top = ZP + CAP_TOP
        hh = 7.6 if kind != "mute" else 7.0
        caps.box((w, h, hh), (x, y, top - hh / 2), cap_material(m, kind, info))
    step_light = m.get("stepcap_light", (0.62, 0.62, 0.60), rough=0.5)
    step_dark = m.get("stepcap_dark", (0.20, 0.20, 0.21), rough=0.5)
    for i in range(16):
        caps.box((P.STEP_W, P.STEP_H, 6.0), (P.step_x(i), P.STEP_Y, ZP + CAP_TOP - 3.0),
                 step_light if (i // 4) % 2 == 0 else step_dark)
    knob = m.get("knob", (0.03, 0.03, 0.032), rough=0.4, metal=0.2)
    knob_cap = m.get("knob_cap", (0.55, 0.55, 0.57), rough=0.25, metal=1.0)
    for (ex, ey), _, _ in P.ENCODERS:
        caps.cyl(10.0, 13.0, (ex, ey, PLATE_T + 1.0 + 6.5), knob, segments=48)
        caps.cyl(8.3, 0.6, (ex, ey, PLATE_T + 7.8 + 6.5), knob_cap, segments=48)
    return caps.finish(coll, parent, bevel=0.8)


def build_panel_board(coll, m, parent):
    fr4 = m.get("mask_green", (0.02, 0.16, 0.07), rough=0.45, coat=0.3)
    edge = m.get("fr4_edge", (0.55, 0.50, 0.30), rough=0.7)
    b = Batch("panel_board")
    cx, cy = (PANEL_X0 + PANEL_X1) / 2, (PANEL_Y0 + PANEL_Y1) / 2
    b.box((PANEL_X1 - PANEL_X0, PANEL_Y1 - PANEL_Y0, PCB_T), (cx, cy, ZP - PCB_T / 2), fr4)
    board = b.finish(coll, parent, bevel=0.2)

    top = Batch("panel_top_parts")
    bot = Batch("panel_bottom_parts")
    lower = m.get("mx_lower", (0.05, 0.05, 0.055), rough=0.5)
    clear = m.get("mx_clear", (0.85, 0.88, 0.90), rough=0.15, alpha=0.45, transmission=0.6)
    stem = m.get("mx_stem", (0.55, 0.05, 0.04), rough=0.4)
    led_white = m.get("sk6812", (0.92, 0.92, 0.90), rough=0.4)
    cap0603 = m.get("mlcc", (0.62, 0.50, 0.33), rough=0.5)
    blk = m.get("ic_black", (0.025, 0.025, 0.028), rough=0.45)
    res = m.get("res", (0.05, 0.05, 0.05), rough=0.5)
    pin = m.get("tin", (0.75, 0.75, 0.78), rough=0.3, metal=1.0)

    for kind, x, y, w, h, info in key_positions():
        top.box((14.0, 14.0, MX_LOWER), (x, y, ZP + MX_LOWER / 2), lower)
        top.box((15.6, 15.6, MX_TOP - MX_LOWER), (x, y, ZP + (MX_LOWER + MX_TOP) / 2), clear)
        top.box((4.2, 1.3, MX_STEM - MX_TOP), (x, y, ZP + (MX_TOP + MX_STEM) / 2), stem)
        top.box((1.3, 4.2, MX_STEM - MX_TOP), (x, y, ZP + (MX_TOP + MX_STEM) / 2), stem)
        if kind == "pad":
            col = P.TRACK_COLORS[info[0]]
            st = pad_state(*info)
            glow = {"playing": 30.0, "queued": 14.0, "clip": 5.0}.get(st, 0.0)
        elif kind == "mute":
            col, glow = P.TRACK_COLORS[info], (0.0 if info in P.MUTED else 6.0)
        else:
            col, glow = (0.2, 1.0, 0.3), (12.0 if kind == "play" or info == P.PLAYING_SCENE else 2.0)
        lit = m.get("ledglow_%d_%d_%d_%d" % (int(col[0] * 99), int(col[1] * 99), int(col[2] * 99), glow),
                    col, emit=col, strength=glow) if glow else m.get("led_off", (0.2, 0.2, 0.2), rough=0.3)
        top.box((3.0, 2.4, 0.4), (x, y - 5.08, ZP + 0.25), lit)       # light through the cutout
        # bottom side: reverse-mount SK6812MINI-E, its 100 nF, the key diode, two switch pins
        bot.box((3.2, 2.8, 0.9), (x, y - 5.08, ZB - 0.45), led_white)
        bot.box((1.6, 0.8, 0.8), (x + 3.6, y - 5.08, ZB - 0.4), cap0603)
        bot.box((1.7, 1.25, 0.9), (x + 5.0, y + 3.8, ZB - 0.45), blk)
        for px, py in ((x + 2.54, y + 5.08), (x - 3.81, y + 2.54)):
            bot.cyl(0.6, 2.0, (px, py, ZB - 1.0), pin, segments=8)

    # step row: tall 6 x 6 tactiles, 3 mm LEDs standing up to the plate
    tact = m.get("tact", (0.08, 0.08, 0.09), rough=0.5)
    plunger = m.get("plunger", (0.15, 0.15, 0.16), rough=0.5)
    red_on = m.get("stepled_on", (1.0, 0.1, 0.06), emit=(1.0, 0.1, 0.06), strength=10.0)
    red_off = m.get("stepled_off", (0.25, 0.03, 0.02), rough=0.3)
    for i in range(16):
        x = P.step_x(i)
        top.box((6.0, 6.0, 3.5), (x, P.STEP_Y, ZP + 1.75), tact)
        top.cyl(1.8, CAP_TOP - 3.5 - 6.0, (x, P.STEP_Y, ZP + 3.5 + (CAP_TOP - 9.5) / 2), plunger)
        on = i in P.STEPS_ON or i == P.PLAYHEAD
        top.cyl(1.5, 5.0, (x, P.STEP_LED_Y, -0.5), red_on if on else red_off, segments=16)
        for dx in (-1.27, 1.27):
            top.cyl(0.25, STANDOFF - 3.0, (x + dx, P.STEP_LED_Y, ZP + (STANDOFF - 3.0) / 2), pin,
                    segments=6)
        bot.box((1.6, 0.8, 0.5), (x + 3.0, P.STEP_LED_Y, ZB - 0.25), res)

    # top strip: encoders with 30 mm shafts, OLED module on spacers, BEAT light pipe
    enc = m.get("enc_body", (0.55, 0.55, 0.58), rough=0.3, metal=1.0)
    for (ex, ey), _, _ in P.ENCODERS:
        top.box((12.0, 13.0, 6.5), (ex, ey, ZP + 3.25), enc)
        top.cyl(3.5, 5.0, (ex, ey, ZP + 6.5 + 2.5), enc)
        top.cyl(3.0, 30.0 - 11.5, (ex, ey, ZP + 11.5 + (30.0 - 11.5) / 2), enc)
    oled_pcb = m.get("oled_pcb", (0.02, 0.08, 0.30), rough=0.5)
    glass = m.get("oled_glass", (0.004, 0.006, 0.01), rough=0.2)
    pixels = m.get("oled_pixels", (0.6, 0.85, 1.0), emit=(0.55, 0.85, 1.0), strength=3.0)
    ox, oy = P.OLED_C
    top.box((70.0, 42.0, 1.6), (ox, oy, -4.2), oled_pcb)
    top.box((60.5, 34.0, 1.6), (ox, oy, -2.6), glass)
    for i in range(3):
        top.box((48.0 - 10 * i, 2.2, 0.1), (ox - 4 + 5 * i, oy + 8.5 - 8.5 * i, -1.75), pixels)
    for sx in (-31.0, 31.0):
        for sy in (-17.0, 17.0):
            top.cyl(2.5, STANDOFF - 5.0, (ox + sx, oy + sy, ZP + (STANDOFF - 5.0) / 2), pin)
    top.box((17.8, 2.5, 8.5), (ox, oy + 19.5, ZP + 4.25), blk)                 # J601 header
    pipe = m.get("lightpipe", (0.9, 0.95, 1.0), emit=(0.3, 0.5, 1.0), strength=6.0)
    top.cyl(1.5, STANDOFF + PLATE_T + 0.4, (P.BEAT_C[0], P.BEAT_C[1], ZP + (STANDOFF + PLATE_T + 0.4) / 2), pipe)
    bot.box((3.2, 2.8, 0.9), (P.BEAT_C[0], P.BEAT_C[1], ZB - 0.45), led_white)

    # bottom side: expanders, LED buffer, bulk capacitor, the cable header
    for x, y in ((-110.0, -62.0), (-110.0, -32.0), (110.0, -62.0), (110.0, 38.0), (110.0, 8.0)):
        bot.box((7.5, 17.9, 2.4), (x, y, ZB - 1.2), blk)
        for k in range(14):
            for side in (-1, 1):
                bot.box((1.0, 0.45, 0.3), (x + side * 4.6, y - 8.26 + 1.27 * k, ZB - 0.3), pin)
        for k in range(4):
            bot.box((1.6, 0.8, 0.5), (x - 7.5, y - 6 + 4 * k, ZB - 0.25), res)
    bot.box((2.9, 1.6, 1.1), (-16.0, 79.0, ZB - 0.55), blk)                       # U901
    alu_can = m.get("alu_can", (0.7, 0.72, 0.75), rough=0.25, metal=1.0)
    bot.cyl(4.0, 10.0, (-28.0, 81.0, ZB - 5.0), alu_can)                         # C984 470 uF
    for k in range(10):                                                          # encoder filters
        bot.box((1.6, 0.8, 0.5), (-12.0 + 3.0 * k, 88.0, ZB - 0.25), res if k % 3 else cap0603)
    hdr = m.get("idc", (0.06, 0.06, 0.07), rough=0.5)
    jx, jy = J_PANEL
    bot.box((42.0, IDC_DEPTH, 9.0), (jx, jy, ZB - 4.5), hdr)                     # right angle, opening to -y
    # mounting holes: standoffs to the plate, screws
    brass = m.get("standoff", (0.72, 0.62, 0.35), rough=0.3, metal=1.0)
    for x, y in panel_holes():
        top.cyl(2.75, STANDOFF, (x, y, ZP + STANDOFF / 2), brass, segments=6)
        bot.cyl(2.75, 2.0, (x, y, ZB - 1.0), pin, segments=16)
    top_ob = top.finish(coll, parent)
    bot_ob = bot.finish(coll, parent)
    return board, top_ob, bot_ob


def panel_holes():
    # hardware/pcb/gen/panel.py HOLES
    return [(-120.0, 72.0), (-40.0, 118.0), (35.0, 118.0), (119.0, 118.0), (0.0, 80.0), (40.0, 80.0),
            (-112.0, 30.0), (-112.0, -12.0), (-112.0, -92.0), (112.0, 70.0), (112.0, -18.0), (112.0, -92.0),
            (-119.0, -123.0), (-57.0, -123.5), (0.0, -123.5), (57.0, -123.5), (119.0, -123.0)]


# ================================================================== main board and case

def build_main_board(coll, m):
    fr4 = m.get("mask_green", (0.02, 0.16, 0.07), rough=0.45, coat=0.3)
    blk = m.get("ic_black", (0.025, 0.025, 0.028), rough=0.45)
    res = m.get("res", (0.05, 0.05, 0.05), rough=0.5)
    cap0603 = m.get("mlcc", (0.62, 0.50, 0.33), rough=0.5)
    pin = m.get("tin", (0.75, 0.75, 0.78), rough=0.3, metal=1.0)
    brass = m.get("standoff", (0.72, 0.62, 0.35), rough=0.3, metal=1.0)
    b = Batch("main_board")
    zb = MAIN_Z - PCB_T
    b.box((MAIN_X1 - MAIN_X0, MAIN_Y1 - MAIN_Y0, PCB_T),
          ((MAIN_X0 + MAIN_X1) / 2, (MAIN_Y0 + MAIN_Y1) / 2, zb + PCB_T / 2), fr4)
    for x, y in ((-118.0, 84.0), (-118.0, 127.0), (118.0, 84.0), (117.5, 101.0), (-46.0, 126.0), (-10.0, 84.0),
                 (60.0, 84.0)):
        b.cyl(2.75, MAIN_STANDOFF, (x, y, FLOOR + MAIN_STANDOFF / 2), brass, segments=6)
    board = b.finish(coll, None, bevel=0.2)

    p = Batch("main_parts")
    # Pico on 8.5 mm sockets, micro-USB at the wall
    pico_green = m.get("pico", (0.03, 0.30, 0.10), rough=0.45)
    usb = m.get("usb_metal", (0.8, 0.8, 0.82), rough=0.2, metal=1.0)
    py0, py1 = MAIN_Y1 - 51.0, MAIN_Y1
    pyc = (py0 + py1) / 2
    for dx in (-8.89, 8.89):
        p.box((2.5, 50.8, 8.5), (PICO_X + dx, pyc, MAIN_Z + 4.25), blk)
    zpico = MAIN_Z + 8.5
    p.box((21.0, 51.0, 1.0), (PICO_X, pyc, zpico + 0.5), pico_green)
    p.box((7.0, 7.0, 0.9), (PICO_X, pyc - 2.0, zpico + 1.45), blk)
    p.box((5.0, 6.0, 1.0), (PICO_X - 4.0, pyc - 13.0, zpico + 1.5), blk)
    p.box((8.0, 5.6, 2.8), (PICO_X, py1 - 1.5, zpico + 2.4), usb)
    p.box((4.0, 3.0, 1.6), (PICO_X - 5.0, pyc + 12.0, zpico + 1.8), m.get("btn", (0.9, 0.9, 0.9), rough=0.4))
    # right-angle jacks and DIN-5 at the rear wall
    jack_body = m.get("jack", (0.04, 0.04, 0.045), rough=0.5)
    nut = m.get("nut", (0.55, 0.55, 0.57), rough=0.25, metal=1.0)
    for _, x in JACKS:
        p.box((11.0, 14.0, 10.0), (x, MAIN_Y1 - 7.0, MAIN_Z + 5.0), jack_body)
        p.cyl(3.0, 4.0, (x, MAIN_Y1 + 1.0, MAIN_Z + 5.0), nut, axis="Y")
    for _, x in DINS:
        p.box((20.5, 20.0, 20.0), (x, MAIN_Y1 - 10.0, MAIN_Z + 10.0), jack_body)
        p.cyl(8.0, 1.0, (x, MAIN_Y1 + 0.5, MAIN_Z + 10.5), nut, axis="Y", segments=40)
    # ICs: DACs, op-amp, gate buffer, opto, MIDI driver, LED switch
    for x, y, w, h in ((106.0, 90.0, 4.9, 3.9), (76.0, 90.0, 4.9, 3.9), (91.0, 101.5, 8.7, 3.9),
                       (29.0, 101.5, 8.7, 3.9)):
        p.box((w, h, 1.6), (x, y, MAIN_Z + 0.8), blk)
    p.box((8.8, 6.4, 3.5), (-27.0, 101.0, MAIN_Z + 1.75), m.get("dip", (0.1, 0.1, 0.11), rough=0.5))
    for x, y in ((-70.0, 104.0), (-78.0, 104.0), (-85.0, 125.0)):
        p.box((2.9, 1.6, 1.1), (x, y, MAIN_Z + 0.55), blk)
    for k in range(48):
        x = 5.0 + (k % 24) * 4.5
        y = 86.0 + (k // 24) * 4.0
        p.box((1.6, 0.8, 0.5), (x, y, MAIN_Z + 0.25), res if k % 2 else cap0603)
    for k in range(14):
        p.box((1.6, 0.8, 0.5), (-50.0 + (k % 7) * 4.0, 108.0 - (k // 7) * 4.0, MAIN_Z + 0.25),
              res if k % 2 else cap0603)
    hdr = m.get("idc", (0.06, 0.06, 0.07), rough=0.5)
    jx, jy = J_MAIN
    p.box((42.0, IDC_DEPTH, 9.0), (jx, jy, MAIN_Z + 4.5), hdr)
    parts = p.finish(coll, None)
    return board, parts


def build_case(coll, m):
    tray = m.get("tray", (0.012, 0.012, 0.014), rough=0.6)
    c = Batch("case")
    c.box((PANEL_W, DEPTH, FLOOR), (0, 0, FLOOR / 2), tray)
    c.box((PANEL_W, WALL, FRONT), (0, -DEPTH / 2 + WALL / 2, FRONT / 2), tray)
    case = c.finish(coll, None, bevel=0.5)
    r = Batch("rear_wall")
    r.box((PANEL_W, WALL, REAR), (0, DEPTH / 2 - WALL / 2, REAR / 2), tray)
    rear = r.finish(coll, None)
    cut = Batch("rear_cutter")
    for _, x in JACKS:
        cut.cyl(3.2, 10.0, (x, DEPTH / 2, MAIN_Z + 5.0), tray, axis="Y", segments=24)
    for _, x in DINS:
        cut.cyl(8.2, 10.0, (x, DEPTH / 2, MAIN_Z + 10.5), tray, axis="Y", segments=40)
    usb_z = MAIN_Z + 8.5 + 1.0 + 2.4
    cut.box((12.0, 10.0, 7.0), (PICO_X, DEPTH / 2, usb_z), tray)
    apply_boolean(rear, cut.finish(coll, None))
    ink = m.get("print", (0.78, 0.78, 0.76), rough=0.55)
    face = (math.radians(90), 0, math.radians(180))
    for label, x in JACKS:
        text(coll, "rear_%s" % label, label, (x, DEPTH / 2 + 0.05, MAIN_Z + 13.0), 2.4, ink, rot=face)
    for label, x in DINS:
        text(coll, "rear_%s" % label, label, (x, DEPTH / 2 + 0.05, MAIN_Z + 22.5), 2.4, ink, rot=face)
    text(coll, "rear_usb", "USB", (PICO_X, DEPTH / 2 + 0.05, usb_z + 7.0), 2.4, ink, rot=face)
    wood = m.get("wood", (0.30, 0.14, 0.06), rough=0.42, coat=0.35)
    rise = PLATE_T + CHEEK_RISE
    ch = []
    for side in (-1, 1):
        x_in, x_out = side * PANEL_W / 2, side * (PANEL_W / 2 + CHEEK_T)
        x0, x1 = min(x_in, x_out), max(x_in, x_out)
        v = [(x0, -DEPTH / 2, 0), (x1, -DEPTH / 2, 0), (x1, DEPTH / 2, 0), (x0, DEPTH / 2, 0),
             (x0, -DEPTH / 2, FRONT + rise), (x1, -DEPTH / 2, FRONT + rise),
             (x1, DEPTH / 2, REAR + rise), (x0, DEPTH / 2, REAR + rise)]
        f = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
        me = bpy.data.meshes.new("SI_cheek")
        me.from_pydata([tuple(k * MM for k in pt) for pt in v], [], f)
        me.materials.append(wood)
        ob = bpy.data.objects.new("SI_cheek_%s" % ("L" if side < 0 else "R"), me)
        coll.objects.link(ob)
        mod = ob.modifiers.new("Bevel", "BEVEL")
        mod.width = 2.0 * MM
        mod.segments = 3
        ch.append(ob)
    return case, rear, ch


def build_ribbon(coll, m, j_panel_empty, j_main_empty, steps=28, half_width=16.5):
    """Flat 26-wire ribbon: both headers open toward the front, so it leaves the panel plug forward,
    folds down and goes back into the main-board plug underneath. A mesh strip, width along x,
    hooked to both plugs with blended weights so it follows the lid when it opens."""
    pw = j_panel_empty.matrix_world.translation / MM
    mw = j_main_empty.matrix_world.translation / MM
    c = [pw, pw + Vector((0, -12, 0)), mw + Vector((0, -12, 0)), mw]
    verts, faces, weights = [], [], []
    for i in range(steps + 1):
        t = i / steps
        p = ((1 - t) ** 3) * c[0] + 3 * ((1 - t) ** 2) * t * c[1] + 3 * (1 - t) * t * t * c[2] + (t ** 3) * c[3]
        for dx in (-half_width, half_width):
            verts.append(tuple((p + Vector((dx, 0, 0))) * MM))
            weights.append(1.0 - (3 * t * t - 2 * t * t * t))
        if i:
            k = 2 * i
            faces.append((k - 2, k - 1, k + 1, k))
    me = bpy.data.meshes.new("SI_ribbon")
    me.from_pydata(verts, [], faces)
    me.materials.append(m.get("ribbon", (0.62, 0.63, 0.66), rough=0.6))
    ob = bpy.data.objects.new("SI_ribbon", me)
    coll.objects.link(ob)
    vg = ob.vertex_groups.new(name="panel")
    for i, w in enumerate(weights):
        vg.add([i], w, "REPLACE")
    h = ob.modifiers.new("hook_panel", "HOOK")
    h.object = j_panel_empty
    h.vertex_group = "panel"
    h.matrix_inverse = j_panel_empty.matrix_world.inverted()
    sol = ob.modifiers.new("thick", "SOLIDIFY")
    sol.thickness = 0.9 * MM
    return ob


# ================================================================== scene, animation

def key(ob, path, frame, value, index=-1):
    setattr(ob, path, value) if index < 0 else getattr(ob, path).__setitem__(index, value)
    ob.keyframe_insert(data_path=path, index=index, frame=frame)


def smooth_all(obs):
    for ob in obs:
        ad = ob.animation_data
        if not ad or not ad.action:
            continue
        fcurves = getattr(ad.action, "fcurves", None)
        if fcurves is None:  # Blender 5 layered actions
            fcurves = [fc for layer in ad.action.layers for strip in layer.strips
                       for bag in strip.channelbags for fc in bag.fcurves]
        for fc in fcurves:
            for kp in fc.keyframe_points:
                kp.interpolation = "BEZIER"
                kp.easing = "AUTO"
                kp.handle_left_type = kp.handle_right_type = "AUTO_CLAMPED"


def build(scene):
    scene.render.fps = FPS
    scene.frame_start, scene.frame_end = 1, FRAMES
    coll = bpy.data.collections.new("SI_internals")
    scene.collection.children.link(coll)
    m = Materials()

    case, rear, cheeks = build_case(coll, m)
    main_board, main_parts = build_main_board(coll, m)

    # lid: lift -> hinge at the rear edge of the plate -> tilted panel frame
    origin = Vector((0.0, 0.0, (FRONT + REAR) / 2))
    rot = Matrix.Rotation(TILT, 4, "X")
    pivot = origin + rot.to_3x3() @ Vector((0.0, PANEL_L / 2, 0.0))
    lift = empty(coll, "lid_lift")
    hinge = empty(coll, "lid_hinge", loc=pivot, parent=lift)
    panel = empty(coll, "panel_frame", loc=origin - pivot, rot=(TILT, 0, 0), parent=hinge)
    plate_ctl = empty(coll, "plate_explode", parent=panel)
    caps_ctl = empty(coll, "caps_explode", parent=panel)

    build_plate(coll, m, plate_ctl)
    build_caps(coll, m, caps_ctl)
    build_panel_board(coll, m, panel)
    jx, jy = J_PANEL
    j_panel = empty(coll, "j_panel_exit", loc=(jx, jy - IDC_DEPTH / 2 - 7.0, ZB - 4.5), parent=panel)
    j_main = empty(coll, "j_main_entry", loc=(J_MAIN[0], J_MAIN[1] - IDC_DEPTH / 2 - 7.0, MAIN_Z + 4.5))
    bpy.context.view_layer.update()
    ribbon = build_ribbon(coll, m, j_panel, j_main)

    stage(scene, coll, m)
    cam = camera(scene, coll)
    labels = build_labels(coll, m, cam[0])
    animate(scene, lift, hinge, plate_ctl, caps_ctl, cam, labels)
    return cam


CAPTIONS = [  # (first frame, last frame, text)
    (1, 40, "Корпус в сборе: высота 28 мм спереди, 50 мм сзади"),
    (45, 108, "Пластина 2 мм и колпачки 17 мм снимаются"),
    (110, 190, "MX-переключатели на панельной плате, стойки 15 мм"),
    (205, 275, "Крышка с панельной платой открывается"),
    (280, 325, "Основная плата на дне: Pico, ЦАП, MIDI, гнёзда"),
    (328, 372, "Низ панели: SK6812MINI-E, диоды, MCP23017, шлейф"),
]


def build_labels(coll, m, cam_ob):
    """Captions in front of the camera, one at a time."""
    ink = m.get("caption", (1.0, 0.86, 0.45), emit=(1.0, 0.86, 0.45), strength=1.5)
    out = []
    shade = m.get("caption_back", (0.0, 0.0, 0.0), rough=1.0, alpha=0.55)
    for i, (a, b, body) in enumerate(CAPTIONS):
        ob = text(coll, "caption_%d" % i, body, (0, -25.5, -150.0), 2.7, ink, parent=cam_ob, rot=(0, 0, 0))
        out.append((ob, a, b))
        width = 0.62 * 2.7 * len(body) + 6.0          # rough text width, mm
        back = Batch("caption_back_%d" % i)
        back.box((width, 5.2, 0.05), (0, 0.4, -0.2), shade)
        out.append((back.finish(coll, ob), a, b))
    return out


def stage(scene, coll, m):
    floor = m.get("floor", (0.035, 0.035, 0.038), rough=0.55)
    f = Batch("floor")
    f.box((4000, 4000, 2), (0, 0, -1.0), floor)
    f.finish(coll, None)
    world = bpy.data.worlds.get("SI_world") or bpy.data.worlds.new("SI_world")
    try:
        world.use_nodes = True
    except Exception:
        pass
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.02, 0.02, 0.025, 1.0)
    scene.world = world
    target = Vector((0.0, 0.0, 0.03))
    for name, loc, size, power in (("key", (-0.45, -0.55, 0.75), 0.6, 26.0),
                                   ("fill", (0.70, -0.30, 0.40), 0.8, 9.0),
                                   ("rim", (0.10, 0.80, 0.50), 0.5, 22.0),
                                   ("top", (0.0, 0.05, 0.60), 0.9, 14.0)):
        light = bpy.data.lights.new("SI_" + name, "AREA")
        light.size = size
        light.energy = power
        ob = bpy.data.objects.new("SI_" + name, light)
        coll.objects.link(ob)
        ob.location = loc
        ob.rotation_euler = (target - Vector(loc)).to_track_quat("-Z", "Y").to_euler()


def camera(scene, coll):
    cam = bpy.data.cameras.new("SI_cam")
    cam.lens = 40.0
    cam.clip_start = 0.005
    ob = bpy.data.objects.new("SI_cam", cam)
    coll.objects.link(ob)
    tgt = empty(coll, "cam_target")
    con = ob.constraints.new("TRACK_TO")
    con.target = tgt
    con.track_axis = "TRACK_NEGATIVE_Z"
    con.up_axis = "UP_Y"
    scene.camera = ob
    return ob, tgt


def animate(scene, lift, hinge, plate_ctl, caps_ctl, cam, labels):
    cam_ob, tgt = cam
    n = Vector((0.0, -math.sin(TILT), math.cos(TILT)))
    # 1 assembled -> 40..110 explode plate and caps -> 150..190 back -> 210..270 lid opens
    # -> 270..360 look inside -> 380..420 close
    for f in (1, 40):
        key(plate_ctl, "location", f, (0, 0, 0))
        key(caps_ctl, "location", f, (0, 0, 0))
    key(plate_ctl, "location", 110, (0, 0, 0.075))
    key(caps_ctl, "location", 110, (0, 0, 0.035))
    key(plate_ctl, "location", 150, (0, 0, 0.075))
    key(caps_ctl, "location", 150, (0, 0, 0.035))
    key(plate_ctl, "location", 190, (0, 0, 0))
    key(caps_ctl, "location", 190, (0, 0, 0))
    for f in (1, 205):
        key(lift, "location", f, (0, 0, 0))
        key(hinge, "rotation_euler", f, (0, 0, 0))
    key(lift, "location", 235, (0, 0.0, 0.05))
    key(hinge, "rotation_euler", 235, (math.radians(-8), 0, 0))
    key(lift, "location", 275, (0, 0.01, 0.09))
    key(hinge, "rotation_euler", 275, (math.radians(-50), 0, 0))
    key(lift, "location", 365, (0, 0.01, 0.09))
    key(hinge, "rotation_euler", 365, (math.radians(-50), 0, 0))
    key(lift, "location", 400, (0, 0.0, 0.05))
    key(hinge, "rotation_euler", 400, (math.radians(-8), 0, 0))
    key(lift, "location", FRAMES, (0, 0, 0))
    key(hinge, "rotation_euler", FRAMES, (0, 0, 0))

    shots = [(1, (-0.42, -0.56, 0.40), (0.0, 0.0, 0.025), 50.0),
             (45, (-0.40, -0.52, 0.37), (0.0, 0.0, 0.03), 50.0),
             (110, (-0.40, -0.52, 0.36), (0.0, 0.0, 0.06), 50.0),
             (150, (0.38, -0.52, 0.32), (0.0, 0.0, 0.05), 50.0),
             (205, (0.44, -0.52, 0.34), (0.0, 0.02, 0.03), 50.0),
             (275, (0.30, -0.58, 0.22), (0.0, 0.05, 0.11), 32.0),
             (320, (0.02, -0.62, 0.18), (0.0, 0.06, 0.12), 32.0),
             (365, (-0.30, -0.54, 0.20), (-0.02, 0.07, 0.11), 32.0),
             (400, (-0.40, -0.54, 0.36), (0.0, 0.02, 0.04), 50.0),
             (FRAMES, (-0.42, -0.56, 0.40), (0.0, 0.0, 0.025), 50.0)]
    for f, loc, t, lens in shots:
        key(cam_ob, "location", f, loc)
        key(tgt, "location", f, t)
        cam_ob.data.lens = lens
        cam_ob.data.keyframe_insert("lens", frame=f)

    for ob, a, b in labels:
        for f, hidden in sorted({(1, a > 1), (max(a - 1, 1), a > 1), (a, False), (b, False), (b + 1, True)}):
            ob.hide_viewport = hidden
            ob.hide_render = hidden
            ob.keyframe_insert("hide_viewport", frame=f)
            ob.keyframe_insert("hide_render", frame=f)
    smooth_all([lift, hinge, plate_ctl, caps_ctl, cam_ob, tgt])
    try:
        smooth_all([cam_ob.data])
    except Exception:
        pass


def setup_render(scene, res=(1280, 720)):
    for eng in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        try:
            scene.render.engine = eng
            break
        except TypeError:
            continue
    scene.render.resolution_x, scene.render.resolution_y = res
    scene.render.resolution_percentage = 100
    for attr, value in (("view_transform", "AgX"), ("look", "AgX - Medium High Contrast")):
        try:
            setattr(scene.view_settings, attr, value)
        except Exception:
            pass
    for attr, value in (("taa_render_samples", 16), ("use_raytracing", False), ("use_shadows", True)):
        try:
            setattr(scene.eevee, attr, value)
        except Exception:
            pass
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.length_unit = "MILLIMETERS"


def build_in_new_scene():
    old = bpy.data.scenes.get(SCENE_NAME)
    if old is not None:
        for ob in list(old.collection.all_objects):
            bpy.data.objects.remove(ob)
        for coll in list(old.collection.children_recursive):
            bpy.data.collections.remove(coll)
        bpy.data.scenes.remove(old)
    scene = bpy.data.scenes.new(SCENE_NAME)
    window = bpy.context.window or bpy.context.window_manager.windows[0]
    window.scene = scene
    build(scene)
    setup_render(scene)
    scene.frame_set(1)
    for area in window.screen.areas:
        if area.type == "VIEW_3D":
            space = area.spaces.active
            space.shading.type = "RENDERED"
            space.overlay.show_overlays = False
            space.clip_start = 0.001
            space.region_3d.view_perspective = "CAMERA"
    return scene


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    res = (1280, 720)
    if "--res" in argv:
        i = argv.index("--res")
        res = (int(argv[i + 1]), int(argv[i + 2]))
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.name = SCENE_NAME
    build(scene)
    setup_render(scene, res)
    if "--blend" in argv:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(argv[argv.index("--blend") + 1]))
    if "--video" in argv:
        out = os.path.abspath(argv[argv.index("--video") + 1])
        if hasattr(scene.render.image_settings, "media_type"):  # Blender 5
            scene.render.image_settings.media_type = "VIDEO"
        scene.render.image_settings.file_format = "FFMPEG"
        scene.render.ffmpeg.format = "MPEG4"
        scene.render.ffmpeg.codec = "H264"
        scene.render.ffmpeg.constant_rate_factor = "HIGH"
        scene.render.filepath = out
        bpy.ops.render.render(animation=True)


if __name__ == "__main__":
    main()
