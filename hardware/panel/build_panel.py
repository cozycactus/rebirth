"""Look-and-feel model of the session-style front panel (layout B).

An 8x8 clip grid (columns = tracks, rows = pattern slots of the shown bank)
with a scene-launch column, a mute row, a row of 16 step buttons with LEDs,
the OLED, two encoders, PLAY and a beat LED on top; CV/Gate, MIDI and USB on
the rear. A visual mockup for choosing the layout, not a manufacturing model.

Headless (writes panel.blend and renders/*.jpg next to this file):
    Blender --background --factory-startup --python build_panel.py -- [--no-render] [--samples N]

Inside a running Blender (adds the scene "Sequencer Panel", other scenes stay):
    p = "/path/to/build_panel.py"
    g = {"__file__": p, "__name__": "panel"}
    exec(compile(open(p).read(), p, "exec"), g); g["build_in_new_scene"]()

Sizes below are millimetres; Blender units are metres.
"""

import math
import os
import sys

import bmesh
import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
SCENE_NAME = "Sequencer Panel"
MM = 0.001

# Body
PANEL_W = 260.0          # aluminium top plate, between the wooden cheeks
CHEEK_T = 10.0
CHEEK_RISE = 3.0         # cheeks stand this much above the plate
DEPTH = 270.0
H_FRONT = 20.0
H_REAR = 45.0
PLATE_T = 2.0
TILT = math.atan2(H_REAR - H_FRONT, DEPTH)
PANEL_L = math.hypot(DEPTH, H_REAR - H_FRONT)
SURF = PLATE_T           # panel-local z of the top surface

# Clip grid and scene column
PAD = 17.0
PITCH = 19.5
SCENE_GAP = 6.0
BLOCK_W = 7 * PITCH + PAD + SCENE_GAP + PAD
GRID_X0 = -BLOCK_W / 2 + PAD / 2
GRID_Y0 = 62.5
SCENE_X = GRID_X0 + 7 * PITCH + PAD + SCENE_GAP
MUTE_Y = GRID_Y0 - 7 * PITCH - PAD / 2 - 10.0

# Steps
STEP_LED_Y = -103.0
STEP_Y = -116.0
STEP_W, STEP_H = 11.0, 13.0
STEP_PITCH = 13.5
STEP_GROUP_GAP = 3.0
STEP_X0 = -(15 * STEP_PITCH + 3 * STEP_GROUP_GAP) / 2

# Top strip
OLED_C = (-84.0, 100.0)
ENCODERS = [((-25.0, 100.0), "TRACK", -40.0), ((15.0, 100.0), "VALUE", 30.0)]
PLAY_C = (62.0, 100.0)
BEAT_C = (90.0, 104.0)

TRACK_COLORS = [
    (1.00, 0.10, 0.06), (1.00, 0.42, 0.04), (1.00, 0.82, 0.08), (0.30, 1.00, 0.12),
    (0.08, 0.90, 0.85), (0.12, 0.32, 1.00), (0.55, 0.18, 1.00), (1.00, 0.22, 0.55),
]

# Demo state shown on the panel: scene 2 playing, track 8 has a clip queued
# for the next bar, track 6 muted, track 1 selected for step editing.
CLIPS = {0: [0, 1, 2, 3], 1: [0, 1, 4], 2: [0, 1, 2], 3: [1, 2, 5],
         4: [0, 2], 5: [1, 3, 6], 6: [0, 1], 7: [1, 2, 3]}
PLAYING = {0: 1, 1: 1, 2: 1, 3: 1, 4: 1, 5: 1, 6: 1, 7: 1}  # track 5's row-2 cell is empty
QUEUED = {7: 3}
MUTED = {5}
SELECTED_TRACK = 0
PLAYING_SCENE = 1
STEPS_ON = {0, 4, 8, 10, 12}
PLAYHEAD = 6

OLED_LINES = ["BANK A      SCENE 2", "TRK 1  KICK     A2", "128.0 BPM   Q 1 BAR"]


def col_x(c):
    return GRID_X0 + c * PITCH


def row_y(r):
    return GRID_Y0 - r * PITCH


def step_x(i):
    return STEP_X0 + i * STEP_PITCH + (i // 4) * STEP_GROUP_GAP


# ---------------------------------------------------------------- materials

class Materials:
    def __init__(self):
        self._cache = {}

    def get(self, name, base, rough=0.5, metal=0.0, emit=None, strength=0.0, coat=0.0):
        if name in self._cache:
            return self._cache[name]
        m = bpy.data.materials.new("SP_" + name)
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
        lit = emit is not None and strength > 0
        if lit:
            b.inputs["Emission Color"].default_value = (*emit, 1.0)
            b.inputs["Emission Strength"].default_value = strength
        m.diffuse_color = (*(emit if lit else base), 1.0)
        self._cache[name] = m
        return m

    def wood(self):
        m = self.get("wood", (0.30, 0.14, 0.06), rough=0.42, coat=0.35)
        nt = m.node_tree
        if "SP_grain" in nt.nodes:
            return m
        coord = nt.nodes.new("ShaderNodeTexCoord")
        mapping = nt.nodes.new("ShaderNodeMapping")
        mapping.inputs["Scale"].default_value = (2.0, 2.0, 14.0)
        wave = nt.nodes.new("ShaderNodeTexWave")
        wave.name = "SP_grain"
        wave.wave_type = "BANDS"
        wave.bands_direction = "Z"
        wave.inputs["Scale"].default_value = 6.0
        wave.inputs["Distortion"].default_value = 7.0
        wave.inputs["Detail"].default_value = 4.0
        ramp = nt.nodes.new("ShaderNodeValToRGB")
        ramp.color_ramp.elements[0].color = (0.13, 0.055, 0.022, 1.0)
        ramp.color_ramp.elements[1].color = (0.40, 0.19, 0.08, 1.0)
        nt.links.new(coord.outputs["Object"], mapping.inputs["Vector"])
        nt.links.new(mapping.outputs["Vector"], wave.inputs["Vector"])
        nt.links.new(wave.outputs["Fac"], ramp.inputs["Fac"])
        nt.links.new(ramp.outputs["Color"], nt.nodes["Principled BSDF"].inputs["Base Color"])
        return m


# ------------------------------------------------------------------ geometry

class Builder:
    def __init__(self, scene):
        self.scene = scene
        self.coll = bpy.data.collections.new("SP_panel")
        scene.collection.children.link(self.coll)
        self.panel = bpy.data.objects.new("SP_panel_surface", None)
        self.coll.objects.link(self.panel)
        self.panel.location = (0.0, 0.0, (H_FRONT + H_REAR) / 2 * MM)
        self.panel.rotation_euler = (TILT, 0.0, 0.0)
        self.panel.empty_display_size = 0.02
        self.m = Materials()

    def _place(self, ob, loc, rot, on_panel):
        self.coll.objects.link(ob)
        if on_panel:
            ob.parent = self.panel
        ob.location = Vector(loc) * MM
        ob.rotation_euler = rot
        return ob

    @staticmethod
    def _bevel(ob, width, segments, limit="NONE"):
        if width <= 0:
            return
        mod = ob.modifiers.new("Bevel", "BEVEL")
        mod.width = width * MM
        mod.segments = segments
        mod.limit_method = limit
        if limit == "ANGLE":
            mod.angle_limit = math.radians(30)
        try:
            mod.harden_normals = True
        except Exception:
            pass
        ob.data.shade_smooth()

    def box(self, name, size, loc, mat, bevel=1.0, segments=3, rot=(0, 0, 0), on_panel=True):
        me = bpy.data.meshes.new("SP_" + name)
        bm = bmesh.new()
        bmesh.ops.create_cube(bm, size=1.0)
        for v in bm.verts:
            v.co.x *= size[0] * MM
            v.co.y *= size[1] * MM
            v.co.z *= size[2] * MM
        bm.to_mesh(me)
        bm.free()
        me.materials.append(mat)
        ob = self._place(bpy.data.objects.new("SP_" + name, me), loc, rot, on_panel)
        self._bevel(ob, min(bevel, min(size) / 2 - 0.05), segments)
        return ob

    def cyl(self, name, r, h, loc, mat, bevel=0.3, axis="Z", on_panel=True, segments=48):
        me = bpy.data.meshes.new("SP_" + name)
        bm = bmesh.new()
        bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segments,
                              radius1=r * MM, radius2=r * MM, depth=h * MM)
        bm.to_mesh(me)
        bm.free()
        me.materials.append(mat)
        rot = (0, 0, 0) if axis == "Z" else (-math.pi / 2, 0, 0)  # "Y": axis along +Y
        ob = self._place(bpy.data.objects.new("SP_" + name, me), loc, rot, on_panel)
        self._bevel(ob, min(bevel, h / 2 - 0.05), 2, limit="ANGLE")
        return ob

    def wedge(self, name, x0, x1, y0, y1, z_front, z_rear, mat, bevel=1.5):
        v = [(x0, y0, 0), (x1, y0, 0), (x1, y1, 0), (x0, y1, 0),
             (x0, y0, z_front), (x1, y0, z_front), (x1, y1, z_rear), (x0, y1, z_rear)]
        f = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
        me = bpy.data.meshes.new("SP_" + name)
        me.from_pydata([tuple(c * MM for c in p) for p in v], [], f)
        me.materials.append(mat)
        ob = self._place(bpy.data.objects.new("SP_" + name, me), (0, 0, 0), (0, 0, 0), False)
        self._bevel(ob, bevel, 3)
        return ob

    def text(self, name, body, loc, size, mat, align="CENTER", rot=(0, 0, 0), on_panel=True):
        cu = bpy.data.curves.new("SP_" + name, "FONT")
        cu.body = body
        cu.size = size * MM
        cu.align_x = align
        cu.align_y = "CENTER"
        cu.materials.append(mat)
        return self._place(bpy.data.objects.new("SP_" + name, cu), loc, rot, on_panel)


# --------------------------------------------------------------- the device

def build_body(b):
    m = b.m
    alu = m.get("graphite_alu", (0.045, 0.045, 0.05), rough=0.36, metal=0.9)
    tray = m.get("tray", (0.012, 0.012, 0.014), rough=0.6)
    b.wedge("tray", -PANEL_W / 2, PANEL_W / 2, -DEPTH / 2, DEPTH / 2, H_FRONT, H_REAR, tray)
    b.box("plate", (PANEL_W - 0.4, PANEL_L, PLATE_T), (0, 0, PLATE_T / 2), alu, bevel=0.6)
    rise = PLATE_T + CHEEK_RISE
    for side in (-1, 1):
        x_in, x_out = side * PANEL_W / 2, side * (PANEL_W / 2 + CHEEK_T)
        b.wedge("cheek_%s" % ("L" if side < 0 else "R"), min(x_in, x_out), max(x_in, x_out),
                -DEPTH / 2, DEPTH / 2, H_FRONT + rise, H_REAR + rise, m.wood(), bevel=2.0)


def build_top_strip(b):
    m = b.m
    ink = m.get("print", (0.78, 0.78, 0.76), rough=0.55)
    bezel = m.get("oled_bezel", (0.01, 0.01, 0.012), rough=0.3)
    screen = m.get("oled_glass", (0.004, 0.006, 0.01), rough=0.45)
    pixel = m.get("oled_pixels", (0.6, 0.85, 1.0), emit=(0.55, 0.85, 1.0), strength=4.0)
    x, y = OLED_C
    b.box("oled_bezel", (62, 34, 1.2), (x, y, SURF + 0.6), bezel, bevel=0.8)
    b.box("oled_screen", (56, 28, 0.2), (x, y, SURF + 1.25), screen, bevel=0)
    for i, line in enumerate(OLED_LINES):
        b.text("oled_line%d" % i, line, (x - 26.0, y + 8.5 - i * 8.5, SURF + 1.4), 3.7, pixel,
               align="LEFT")
    b.box("oled_bar", (52.0 * 7 / 16, 1.0, 0.05), (x - 26.0 + 52.0 * 7 / 32, y - 12.0, SURF + 1.4),
          pixel, bevel=0)

    knob = m.get("knob", (0.02, 0.02, 0.022), rough=0.45)
    cap = m.get("knob_cap", (0.75, 0.75, 0.76), rough=0.25, metal=1.0)
    nut = m.get("nut", (0.6, 0.6, 0.62), rough=0.3, metal=1.0)
    for i, ((ex, ey), label, angle) in enumerate(ENCODERS):
        b.cyl("enc%d_nut" % i, 5.5, 1.5, (ex, ey, SURF + 0.75), nut)
        b.cyl("enc%d_knob" % i, 10.0, 13.0, (ex, ey, SURF + 8.0), knob, bevel=1.2, segments=64)
        b.cyl("enc%d_cap" % i, 8.3, 0.6, (ex, ey, SURF + 14.8), cap, bevel=0.2, segments=64)
        a = math.radians(angle)
        b.box("enc%d_mark" % i, (1.2, 6.0, 0.3),
              (ex - 4.6 * math.sin(a), ey + 4.6 * math.cos(a), SURF + 15.2), ink, bevel=0,
              rot=(0, 0, a))
        b.text("enc%d_label" % i, label, (ex, ey - 15.5, SURF + 0.03), 3.0, ink)

    px, py = PLAY_C
    play = m.get("play_lit", (0.2, 0.9, 0.3), emit=(0.2, 1.0, 0.3), strength=5.0)
    b.box("play", (14, 14, 5), (px, py, SURF + 2.5), play, bevel=2.0)
    b.text("play_label", "PLAY", (px, py - 15.5, SURF + 0.03), 3.0, ink)

    bx, by = BEAT_C
    beat = m.get("beat_led", (1.0, 0.1, 0.05), emit=(1.0, 0.08, 0.04), strength=12.0)
    b.cyl("beat_led", 1.8, 1.5, (bx, by, SURF + 0.75), beat, bevel=0.5)
    b.text("beat_label", "BEAT", (bx, by - 11.5, SURF + 0.03), 3.0, ink)


def pad_material(m, c, state):
    color = TRACK_COLORS[c]
    if state == "empty":
        return m.get("pad_empty", (0.10, 0.10, 0.105), rough=0.6)
    if state == "stopped_here":  # the track plays this empty cell: it is silent
        return m.get("pad_stopped", (0.35, 0.35, 0.35), rough=0.6, emit=(1.0, 1.0, 1.0),
                     strength=0.5)
    if state == "stored":
        return m.get("pad_stored_%d" % c, tuple(0.45 * k for k in color), rough=0.6,
                     emit=color, strength=0.9)
    if state == "playing":
        return m.get("pad_playing_%d" % c, color, rough=0.6, emit=color, strength=7.0)
    whiter = tuple(0.45 * k + 0.55 for k in color)
    return m.get("pad_queued_%d" % c, whiter, rough=0.6, emit=whiter, strength=3.5)


def build_grid(b):
    m = b.m
    ink = m.get("print", (0.78, 0.78, 0.76), rough=0.55)
    bed = m.get("grid_bed", (0.008, 0.008, 0.009), rough=0.7)
    grid_h = 7 * PITCH + PAD
    b.box("grid_bed", (BLOCK_W + 8, grid_h + 8, 0.6),
          (0, (row_y(0) + row_y(7)) / 2, SURF + 0.3), bed, bevel=1.5)

    for c in range(8):
        for r in range(8):
            has_clip = r in CLIPS.get(c, [])
            if QUEUED.get(c) == r:
                state = "queued"
            elif PLAYING.get(c) == r:
                state = "playing" if has_clip else "stopped_here"
            elif has_clip:
                state = "stored"
            else:
                state = "empty"
            b.box("pad_%d_%d" % (c, r), (PAD, PAD, 5), (col_x(c), row_y(r), SURF + 3.1),
                  pad_material(m, c, state), bevel=2.0)

    scene_on = m.get("scene_playing", (0.2, 0.9, 0.3), emit=(0.2, 1.0, 0.3), strength=4.0)
    scene_has = m.get("scene_ready", (0.55, 0.55, 0.55), emit=(1.0, 1.0, 1.0), strength=0.35)
    scene_off = m.get("pad_empty", (0.10, 0.10, 0.105), rough=0.6)
    used_rows = {r for rows in CLIPS.values() for r in rows}
    for r in range(8):
        mat = scene_on if r == PLAYING_SCENE else scene_has if r in used_rows else scene_off
        b.box("scene_%d" % r, (14, 14, 4), (SCENE_X, row_y(r), SURF + 2.6), mat, bevel=1.8)
        b.text("row_%d" % r, str(r + 1), (col_x(0) - PAD / 2 - 11.0, row_y(r), SURF + 0.03),
               3.0, ink)
    b.text("scene_label", "SCENE", (SCENE_X, row_y(0) + PAD / 2 + 6.5, SURF + 0.03), 3.0, ink)

    for c in range(8):
        color = TRACK_COLORS[c]
        if c in MUTED:
            mat = m.get("pad_empty", (0.10, 0.10, 0.105), rough=0.6)
        elif c == SELECTED_TRACK:
            mat = m.get("mute_sel_%d" % c, color, rough=0.6, emit=color, strength=6.0)
        else:
            mat = m.get("mute_on_%d" % c, tuple(0.5 * k for k in color), rough=0.6,
                        emit=color, strength=1.8)
        b.box("mute_%d" % c, (PAD, 8, 3.5), (col_x(c), MUTE_Y, SURF + 1.75), mat, bevel=1.5)
    b.text("mute_label", "MUTE", (col_x(0) - PAD / 2 - 11.0, MUTE_Y, SURF + 0.03), 3.0, ink)


def build_steps(b):
    m = b.m
    ink = m.get("print", (0.78, 0.78, 0.76), rough=0.55)
    color = TRACK_COLORS[SELECTED_TRACK]
    led_on = m.get("step_led_on", color, emit=color, strength=10.0)
    led_head = m.get("step_led_head", (1, 1, 1), emit=(1.0, 0.95, 0.9), strength=12.0)
    led_off = m.get("step_led_off", (0.06, 0.02, 0.02), rough=0.3)
    cap_light = m.get("step_cap_light", (0.62, 0.62, 0.60), rough=0.5)
    cap_dark = m.get("step_cap_dark", (0.20, 0.20, 0.21), rough=0.5)
    for i in range(16):
        x = step_x(i)
        led = led_head if i == PLAYHEAD else led_on if i in STEPS_ON else led_off
        b.cyl("step_led_%d" % i, 1.5, 1.2, (x, STEP_LED_Y, SURF + 0.6), led, bevel=0.4)
        cap = cap_light if (i // 4) % 2 == 0 else cap_dark
        b.box("step_%d" % i, (STEP_W, STEP_H, 4), (x, STEP_Y, SURF + 2.0), cap, bevel=1.5)
        b.text("step_num_%d" % i, str(i + 1), (x, STEP_Y - 10.0, SURF + 0.03), 2.6, ink)


def build_rear(b):
    m = b.m
    ink = m.get("print", (0.78, 0.78, 0.76), rough=0.55)
    nut = m.get("nut", (0.6, 0.6, 0.62), rough=0.3, metal=1.0)
    hole = m.get("hole", (0.0, 0.0, 0.0), rough=0.9)
    y = DEPTH / 2
    face = (math.pi / 2, 0, math.pi)   # text facing +Y, readable from behind
    jack_z, label_z = 20.0, 33.0
    # Seen from behind, +X is on the left: CV 1..4, GATE 1..4, MIDI IN, MIDI OUT, USB.
    jacks = [("CV %d" % (i + 1), 112.0 - 14.0 * i) for i in range(4)]
    jacks += [("GATE %d" % (i + 1), 50.0 - 14.0 * i) for i in range(4)]
    for i, (label, x) in enumerate(jacks):
        b.cyl("jack_%d" % i, 4.0, 2.0, (x, y + 1.0, jack_z), nut, axis="Y", on_panel=False)
        b.cyl("jack_hole_%d" % i, 1.8, 2.2, (x, y + 1.1, jack_z), hole, bevel=0, axis="Y",
              on_panel=False)
        b.text("jack_label_%d" % i, label, (x, y + 0.05, label_z), 2.6, ink, rot=face,
               on_panel=False)
    for i, (label, x) in enumerate((("MIDI IN", -30.0), ("MIDI OUT", -62.0))):
        b.cyl("din_%d" % i, 8.0, 1.5, (x, y + 0.75, jack_z), nut, axis="Y", on_panel=False)
        b.cyl("din_hole_%d" % i, 6.6, 1.7, (x, y + 0.85, jack_z), hole, bevel=0, axis="Y",
              on_panel=False)
        for k in range(5):
            a = math.radians(-90 + 45 * k)
            b.cyl("din_pin_%d_%d" % (i, k), 0.5, 1.9,
                  (x + 3.6 * math.cos(a), y + 0.95, jack_z - 3.6 * math.sin(a)), nut, bevel=0,
                  axis="Y", on_panel=False, segments=12)
        b.text("din_label_%d" % i, label, (x, y + 0.05, label_z), 2.6, ink, rot=face,
               on_panel=False)
    b.box("usb", (9.0, 1.5, 3.4), (-100.0, y + 0.6, jack_z), hole, bevel=1.4, on_panel=False)
    b.text("usb_label", "USB", (-100.0, y + 0.05, label_z), 2.6, ink, rot=face, on_panel=False)


def build_stage(b):
    scene = b.scene
    floor = b.m.get("floor", (0.035, 0.035, 0.038), rough=0.55)
    b.box("floor", (4000, 4000, 2), (0, 0, -1.0), floor, bevel=0, on_panel=False)

    world = bpy.data.worlds.new("SP_world")
    try:
        world.use_nodes = True
    except Exception:
        pass
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.012, 0.012, 0.015, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world

    target = Vector((0.0, 0.0, 0.03))
    for name, loc, size, power in (("key", (-0.45, -0.55, 0.75), 0.6, 20.0),
                                   ("fill", (0.70, -0.30, 0.40), 0.8, 7.0),
                                   ("rim", (0.10, 0.80, 0.50), 0.5, 18.0)):
        light = bpy.data.lights.new("SP_" + name, "AREA")
        light.size = size
        light.energy = power
        ob = bpy.data.objects.new("SP_" + name, light)
        b.coll.objects.link(ob)
        ob.location = loc
        ob.rotation_euler = (target - Vector(loc)).to_track_quat("-Z", "Y").to_euler()


def build_cameras(b):
    def camera(name, loc, target=None, lens=50.0, ortho=None, rot=None):
        cam = bpy.data.cameras.new("SP_" + name)
        cam.lens = lens
        if ortho:
            cam.type = "ORTHO"
            cam.ortho_scale = ortho
        ob = bpy.data.objects.new("SP_cam_" + name, cam)
        b.coll.objects.link(ob)
        ob.location = loc
        ob.rotation_euler = rot or (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
        return ob

    centre = Vector((0.0, 0.0, (H_FRONT + H_REAR) / 2 * MM))
    normal = Vector((0.0, -math.sin(TILT), math.cos(TILT)))
    return [
        ("hero", camera("hero", (-0.36, -0.48, 0.40), (0.01, 0.015, 0.02)), (1600, 1100)),
        ("top", camera("top", centre + normal * 0.9, ortho=0.30, rot=(TILT, 0, 0)), (1400, 1400)),
        ("rear", camera("rear", (-0.30, 0.50, 0.20), (0.0, 0.12, 0.022)), (1600, 900)),
    ]


def setup_glow(scene):
    try:
        ng = bpy.data.node_groups.new("SP_compositor", "CompositorNodeTree")
        ng.interface.new_socket(name="Image", in_out="OUTPUT", socket_type="NodeSocketColor")
        rl = ng.nodes.new("CompositorNodeRLayers")
        rl.scene = scene
        glare = ng.nodes.new("CompositorNodeGlare")
        for kind in ("Bloom", "Fog Glow"):
            try:
                glare.inputs["Type"].default_value = kind
                break
            except Exception:
                continue
        glare.inputs["Threshold"].default_value = 1.0
        glare.inputs["Strength"].default_value = 0.3
        glare.inputs["Size"].default_value = 0.35
        out = ng.nodes.new("NodeGroupOutput")
        ng.links.new(rl.outputs["Image"], glare.inputs["Image"])
        ng.links.new(glare.outputs["Image"], out.inputs["Image"])
        scene.compositing_node_group = ng
        scene.render.use_compositing = True
    except Exception as e:  # the glow is decoration only
        print("panel: glow skipped:", e)


def setup_render(scene, samples, use_gpu):
    scene.render.engine = "CYCLES"
    scene.cycles.samples = samples
    scene.cycles.use_denoising = True
    if use_gpu:
        try:
            prefs = bpy.context.preferences.addons["cycles"].preferences
            prefs.compute_device_type = "METAL"
            prefs.get_devices()
            for d in prefs.devices:
                d.use = True
            scene.cycles.device = "GPU"
        except Exception as e:
            print("panel: GPU not used:", e)
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "JPEG"
    scene.render.image_settings.quality = 90
    for attr, value in (("view_transform", "AgX"), ("look", "AgX - Medium High Contrast")):
        try:
            setattr(scene.view_settings, attr, value)
        except Exception:
            pass
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.length_unit = "MILLIMETERS"
    setup_glow(scene)


def build(scene):
    b = Builder(scene)
    build_body(b)
    build_top_strip(b)
    build_grid(b)
    build_steps(b)
    build_rear(b)
    build_stage(b)
    cams = build_cameras(b)
    scene.camera = cams[0][1]
    scene.render.resolution_x, scene.render.resolution_y = cams[0][2]
    return cams


def build_in_new_scene():
    """For a running Blender: (re)build the panel in its own scene and show it."""
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
    setup_render(scene, 64, use_gpu=False)
    for area in window.screen.areas:
        if area.type == "VIEW_3D":
            space = area.spaces.active
            space.shading.type = "MATERIAL"
            space.shading.use_scene_lights = True
            space.shading.use_scene_world = True
            space.region_3d.view_perspective = "CAMERA"
            space.clip_start = 0.001
    return scene


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    samples = int(argv[argv.index("--samples") + 1]) if "--samples" in argv else 128
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.name = SCENE_NAME
    cams = build(scene)
    setup_render(scene, samples, use_gpu=True)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, "panel.blend"))
    if "--no-render" in argv:
        return
    os.makedirs(os.path.join(HERE, "renders"), exist_ok=True)
    for name, cam, (w, h) in cams:
        scene.camera = cam
        scene.render.resolution_x, scene.render.resolution_y = w, h
        scene.render.filepath = os.path.join(HERE, "renders", name + ".jpg")
        bpy.ops.render.render(write_still=True)
    scene.camera = cams[0][1]


if __name__ == "__main__":
    main()
