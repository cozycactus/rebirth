"""Front-panel geometry, millimetres, panel frame (x to the right, y toward the rear).

Copied from hardware/panel/build_panel.py (the look-and-feel mockup); keep the two in step.
KiCad coordinates: K(x, y) = (OX + x, OY - y).
"""

OX, OY = 150.0, 150.0

PAD = 17.0
PITCH = 19.5
SCENE_GAP = 6.0
BLOCK_W = 7 * PITCH + PAD + SCENE_GAP + PAD
GRID_X0 = -BLOCK_W / 2 + PAD / 2
GRID_Y0 = 62.5
SCENE_X = GRID_X0 + 7 * PITCH + PAD + SCENE_GAP
MUTE_Y = GRID_Y0 - 7 * PITCH - PAD / 2 - 10.0

STEP_LED_Y = -103.0
STEP_Y = -116.0
STEP_W, STEP_H = 11.0, 13.0
STEP_PITCH = 13.5
STEP_GROUP_GAP = 3.0
STEP_X0 = -(15 * STEP_PITCH + 3 * STEP_GROUP_GAP) / 2

OLED_C = (-84.0, 100.0)
ENCODERS = [(-25.0, 100.0), (15.0, 100.0)]
PLAY_C = (62.0, 100.0)
BEAT_C = (90.0, 104.0)

# panel board outline (panel frame)
PANEL_X0, PANEL_X1 = -124.0, 124.0
PANEL_Y0, PANEL_Y1 = -128.0, 123.0

# main board outline (case frame, seen from above; y toward the rear wall)
MAIN_X0, MAIN_X1 = -122.0, 122.0
MAIN_Y0, MAIN_Y1 = 74.0, 131.5
PICO_X = -100.0
JACKS = [("CV%d" % (i + 1), 112.0 - 14.0 * i) for i in range(4)] + \
        [("GATE%d" % (i + 1), 50.0 - 14.0 * i) for i in range(4)]
DINS = [("J402", "MIDI IN", -30.0), ("J401", "MIDI OUT", -62.0)]


def K(x, y):
    return OX + x, OY - y


def col_x(c):
    return GRID_X0 + c * PITCH


def row_y(r):
    return GRID_Y0 - r * PITCH


def step_x(i):
    return STEP_X0 + i * STEP_PITCH + (i // 4) * STEP_GROUP_GAP
