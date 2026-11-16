"""Panel board: placement from the front-panel geometry and the regular part of the routing.

All SMD parts sit on the bottom (one assembly side); MX switches, tactiles, step LEDs,
encoders and the OLED socket are through-hole on the top. Copper plan inside the grid:
B.Cu runs horizontally (ROW lines, LED chain, +5V_LED bus per row), F.Cu vertically
(COL lines); GND is poured on both layers.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import pcbnew

from geometry import (BEAT_C, ENCODERS, K, MUTE_Y, OLED_C, PANEL_X0, PANEL_X1, PANEL_Y0, PANEL_Y1, PLAY_C,
                      SCENE_X, STEP_LED_Y, STEP_Y, col_x, row_y, step_x)

F, B = pcbnew.F_Cu, pcbnew.B_Cu

# offsets inside a key cell, KiCad coordinates relative to the switch centre (+y = toward the front)
LED_DY = 5.08          # SK6812MINI-E under the MX LED window
DIODE_DY = -7.6        # BAT54J row, anode 3.8 / cathode 6.0 to the right of the centre
COL_DX = 7.0           # COL line (F.Cu) to the right of each switch column
ROW_DY = -2.75         # ROW line (B.Cu), through switch pin 2
BUS_DY = 8.6           # +5V_LED bus (B.Cu) under each row of LEDs
CHAIN_DY = 3.5         # chain trace from DOUT to the next LED, above the LED pads
CAP_DX = 4.6           # 100 nF beside each LED, vertical


def keys():
    """(kind, index, key number, panel x, panel y, LED number) for every MX key."""
    out = []
    for r in range(8):
        for c in range(8):
            out.append(("pad", (c, r), 10 * r + c + 1, col_x(c), row_y(r), r * 8 + c + 1))
    for r in range(8):
        out.append(("scene", r, 10 * r + 9, SCENE_X, row_y(r), 65 + r))
    for t in range(8):
        out.append(("mute", t, 10 * t + 10, col_x(t), MUTE_Y, 73 + t))
    return out


def place_key(bd, sw, led, cap, diode, x, y):
    cx, cy = K(x, y)
    bd.place(sw, cx + 2.54, cy - 5.08)
    bd.place(led, cx, cy + LED_DY, 0, bottom=True)
    bd.place_two(cap, (cx + CAP_DX, cy + LED_DY + 1.52), (cx + CAP_DX, cy + LED_DY - 0.03), bottom=True)
    if diode:
        bd.place_two(diode, (cx + 6.0, cy + DIODE_DY), (cx + 3.8, cy + DIODE_DY), bottom=True)
    return cx, cy


def place(bd):
    x0, y0 = K(PANEL_X0, PANEL_Y1)
    x1, y1 = K(PANEL_X1, PANEL_Y0)
    bd.outline(x0, y0, x1, y1, r=4.0)
    cells = {}
    for kind, idx, k, x, y, n in keys():
        cells[(kind, idx)] = place_key(bd, "SW8%02d" % k, "D9%02d" % n, "C9%02d" % n, "D8%02d" % k, x, y)
    cells[("play", 0)] = place_key(bd, "SW603", "D981", "C981", None, *PLAY_C)
    bx, by = K(*BEAT_C)
    bd.place("D982", bx, by, 0, bottom=True)
    bd.place_two("C982", (bx + CAP_DX, by + 1.52), (bx + CAP_DX, by - 0.03), bottom=True)

    # step row: tall 6 x 6 tactiles, 3 mm LEDs standing up to the plate, 470 R under each LED
    for i in range(16):
        sx, sy = K(step_x(i), STEP_Y)
        bd.place_center("SW7%02d" % (i + 1), sx, sy)
        lx, ly = K(step_x(i), STEP_LED_Y)
        bd.place_center("D7%02d" % (i + 1), lx, ly)
        bd.place_center("R7%02d" % (i + 1), lx + 4.2, ly, 90, bottom=True)

    # step-row expanders: 0x20 (U701) and 0x24 (U703) at the left, 0x21 (U702, LEDs) at the right
    # (their reset pull-ups sit right of pin 18, out of the way of the step-row buses)
    for ref, (x, y), rref, cref in (("U701", (-110.0, -62.0), "R717", "C701"),
                                    ("U703", (-110.0, -32.0), "R719", "C703"),
                                    ("U702", (110.0, -62.0), "R718", "C702")):
        kx, ky = K(x, y)
        bd.place_center(ref, kx, ky, 0, bottom=True)
        bd.place_center(cref, kx, ky - 12.0, 0, bottom=True)
        rst = bd.pad(ref, "18")
        near = "1" if bd.pad_net(rref, "1") == bd.pad_net(ref, "18") else "2"
        far = "2" if near == "1" else "1"
        pads = {near: (rst[0] + 2.95, rst[1]), far: (rst[0] + 4.55, rst[1])}
        bd.place_two(rref, pads["1"], pads["2"], bottom=True)

    # key scanner at the right margin, next to the ROW lines and the COL bus
    for ref, (x, y), rref, cref in (("U801", (110.0, 38.0), "R801", "C801"),
                                    ("U802", (110.0, 8.0), "R802", "C802")):
        kx, ky = K(x, y)
        bd.place_center(ref, kx, ky, 0, bottom=True)
        bd.place_center(cref, kx, ky - 12.0, 0, bottom=True)
        bd.place_center(rref, kx, ky + 12.0, 0, bottom=True)

    # top strip
    for i, (ref, (x, y)) in enumerate((("SW601", ENCODERS[0]), ("SW602", ENCODERS[1]))):
        kx, ky = K(x, y)
        bd.place_center(ref, kx, ky)
        base = 601 + 4 * i
        for j, r in enumerate(range(base, base + 4)):
            bd.place_center("R%d" % r, kx - 6.0 + 4.0 * j, ky + 11.0, 90, bottom=True)
        for j, c in enumerate((603 + 2 * i, 604 + 2 * i)):
            bd.place_center("C%d" % c, kx + 10.0, ky - 2.0 + 4.0 * j, 0, bottom=True)
    ox, oy = K(*OLED_C)
    bd.place_center("J601", ox, oy - 19.5, 90)            # socket under the module's header (check the module)
    bd.place_center("C601", ox + 12.0, oy - 15.0, 0, bottom=True)
    bd.place_center("C602", ox + 16.0, oy - 15.0, 0, bottom=True)

    # cable entry, its +3V3 decoupling and pull-ups, the +5V_LED bulk and the LED data buffer
    jx, jy = K(-61.0, 88.0)
    bd.place_center("J1201", jx, jy, 270, bottom=True)
    px, py = K(-36.0, 86.0)
    for i, ref in enumerate(("C1201", "C1202", "R1201", "R1202", "R1203")):
        bd.place_center(ref, px, py - 6.0 + 3.0 * i, 0, bottom=True)
    cx, cy = K(-28.0, 81.0)
    bd.place_center("C984", cx, cy, 0, bottom=True)
    ux, uy = K(-16.0, 79.0)
    bd.place_center("U901", ux, uy, 0, bottom=True)
    bd.place_center("R901", ux - 4.0, uy - 3.0, 0, bottom=True)
    bd.place_center("C983", ux - 4.0, uy + 3.0, 0, bottom=True)
    bd.place_center("R902", ux + 4.5, uy, 90, bottom=True)
    bd.place_center("R903", ux + 7.5, uy, 90, bottom=True)
    return cells


HOLES = [(-120.0, 72.0), (-40.0, 118.0), (35.0, 118.0), (119.0, 118.0), (0.0, 80.0), (40.0, 80.0),
         (-112.0, 30.0), (-112.0, -12.0), (-112.0, -92.0), (112.0, 70.0), (112.0, -18.0), (112.0, -92.0),
         (-119.0, -123.0), (-57.0, -123.5), (0.0, -123.5), (57.0, -123.5), (119.0, -123.0)]


def holes(bd):
    for i, (x, y) in enumerate(HOLES):
        kx, ky = K(x, y)
        bd.board_only("MountingHole:MountingHole_3.2mm_M3_Pad_Via", "H%d" % (i + 1), kx, ky, net="GND")
    for i, (x, y) in enumerate(((-121.0, 52.0), (121.0, 52.0), (-121.0, -110.0))):
        kx, ky = K(x, y)
        bd.board_only("Fiducial:Fiducial_1mm_Mask2mm", "FID%d" % (i + 1), kx, ky, bottom=True)


# ------------------------------------------------------------------ regular routing of the key grid

W_SIG = 0.25
W_BUS = 0.6
W_STUB = 0.4


def cell_power(bd, led, cap, cx, cy):
    """LED supply and decoupling of one key: VDD and C+ down to the row bus, both GNDs to vias."""
    vdd = bd.pad(led, "3")
    gnd = bd.pad(led, "1")
    cp, cg = bd.pad(cap, "1"), bd.pad(cap, "2")
    y_bus = cy + BUS_DY
    bd.track("+5V_LED", [vdd, (vdd[0], y_bus)], B, W_STUB)
    bd.track("+5V_LED", [cp, (cp[0], y_bus)], B, W_STUB)
    bd.track("GND", [gnd, (gnd[0], cy + 2.9)], B, W_STUB)
    bd.via("GND", gnd[0], cy + 2.9)
    bd.track("GND", [cg, (cx + 5.9, cg[1])], B, W_STUB)
    bd.via("GND", cx + 5.9, cg[1])
    return [vdd[0], cp[0]]


def chain_link(bd, led_a, led_b):
    """DOUT of one LED to DIN of its right-hand neighbour: up over the cap, across, down to DIN."""
    dout = bd.pad(led_a, "4")
    din = bd.pad(led_b, "2")
    net = bd.pad_net(led_a, "4")
    y_top = dout[1] - 0.83
    pts = [dout, (dout[0] + 0.83, y_top), (din[0] - 3.27, y_top), (din[0] - 1.77, din[1]), din]
    bd.track(net, pts, B, W_SIG)


def key_matrix_cell(bd, sw, diode):
    """Diode anode to switch pin 1 (B.Cu); cathode to a via on the column line."""
    a = bd.pad(diode, "2")
    k = bd.pad(diode, "1")
    p1 = bd.pad(sw, "1")
    bd.track(bd.pad_net(diode, "2"), [a, (p1[0], a[1] + (a[0] - p1[0])), p1], B, W_SIG)
    return k


def prerouted(bd):
    led_supply(bd)
    step_buses(bd)
    rows = {}
    for kind, idx, k, x, y, n in keys():
        r = idx[1] if kind == "pad" else (idx if kind == "scene" else 8)
        rows.setdefault(r, []).append((kind, idx, k, n) + K(x, y))
    for r, cells in rows.items():
        cells.sort(key=lambda c: c[4])
        bus_x = []
        row_pts = []
        mute_col = []
        for kind, idx, k, n, cx, cy in cells:
            sw, led, cap, diode = "SW8%02d" % k, "D9%02d" % n, "C9%02d" % n, "D8%02d" % k
            bus_x += cell_power(bd, led, cap, cx, cy)
            cathode = key_matrix_cell(bd, sw, diode)
            col_net = bd.pad_net(diode, "1")
            if kind == "mute":
                # COL9 runs along the mute row on B.Cu, above the diodes
                bd.track(col_net, [cathode, (cathode[0], cy - 9.0)], B, W_SIG)
                mute_col.append((cathode[0], cy - 9.0))
            else:
                bd.track(col_net, [cathode, (cx + COL_DX, cathode[1])], B, W_SIG)
                bd.via(col_net, cx + COL_DX, cathode[1])
                row_pts.append(bd.pad(sw, "2"))
        # +5V_LED bus of the row, from the first LED to past the last cap
        y_bus = cells[0][5] + BUS_DY
        xs = sorted(set(bus_x))
        # pad rows end on the +5V_LED spine via (led_supply), the mute row just past its last cap
        x_end = K(SPINE_X, 0)[0] if r < 8 else xs[-1] + 3.0
        bd.track("+5V_LED", [(xs[0], y_bus), (x_end, y_bus)], B, W_BUS)
        # chain inside the row: pads 1-8 (and mutes 1-8) left to right
        leds = [("D9%02d" % c[3]) for c in cells if c[0] in ("pad", "mute")]
        for a, b in zip(leds, leds[1:]):
            chain_link(bd, a, b)
        # ROW line through switch pin 2 of the pads and the scene key of the row
        if row_pts:
            net = bd.pad_net("SW8%02d" % cells[0][2], "2")
            bd.track(net, row_pts, B, W_SIG)
        if mute_col:
            bd.track(bd.pad_net("D8%02d" % cells[0][2], "1"), mute_col, B, W_SIG)
    # column lines on F.Cu: via to via, one per pad column and the scene column
    cols = {}
    for kind, idx, k, x, y, n in keys():
        if kind == "mute":
            continue
        cx, cy = K(x, y)
        cols.setdefault(round(cx, 3), []).append((cx + COL_DX, cy + DIODE_DY, "D8%02d" % k))
    for xc, pts in cols.items():
        pts.sort(key=lambda p: p[1])
        net = bd.pad_net(pts[0][2], "1")
        bd.track(net, [(p[0], p[1]) for p in pts], F, W_SIG)
    # PLAY and BEAT: local GND vias for the LED and its capacitor
    for led, cap, (x, y) in (("D981", "C981", PLAY_C), ("D982", "C982", BEAT_C)):
        cx, cy = K(x, y)
        if led == "D982":
            cy -= LED_DY
        g = bd.pad(led, "1")
        bd.track("GND", [g, (g[0], g[1] - 1.43)], B, W_STUB)
        bd.via("GND", g[0], g[1] - 1.43)
        cg = bd.pad(cap, "2")
        bd.track("GND", [cg, (cg[0] + 1.3, cg[1])], B, W_STUB)
        bd.via("GND", cg[0] + 1.3, cg[1])
        v, cp = bd.pad(led, "3"), bd.pad(cap, "1")
        bd.track("+5V_LED", [v, (v[0], cp[1]), cp], B, W_STUB)


KEEP_SILK = ("J1201", "J601", "U701", "U702", "U703", "U801", "U802", "U901", "SW601", "SW602", "SW603", "C984")


def finish(board):
    import common
    common.clear_zones(board)
    common.silk_inside(board)
    common.silk_to_fab(board, ("C", "D", "R", "SW", "U", "J", "H", "FID"), keep=KEEP_SILK)
    u = board.FindFootprintByReference("U901")     # reference clear of its own pin-1 mark
    u.Reference().SetPosition(pcbnew.VECTOR2I(u.GetPosition().x, u.GetPosition().y + pcbnew.FromMM(3.0)))
    n_pads = common.stitch_pads(board)
    print("panel: %d GND islands joined" % n_pads)
    box = common.board_box(board)
    n = common.stitch(board, "GND", box)
    n += common.stitch(board, "GND", box, pitch=3.0, gap=4.5)     # into the gaps the first pass left
    print("GND islands joined by vias: %d" % common.stitch_islands(board))
    common.add_zone_once(board, "GND", F, box)
    common.add_zone_once(board, "GND", B, box)
    print("panel: %d stitching vias" % n)


# ------------------------------------------------------------------ +5V_LED distribution

SPINE_X = 95.5          # panel x of the F.Cu spine down the right side of the grid
TOP_Y = 74.5            # panel y of the B.Cu feed along the top of the grid
W_FEED = 1.0


def led_supply(bd):
    """J1201 -> C984 -> along the top of the grid -> spine down the right side -> every row bus.

    Up to 306 mA (U201's limit) flows here; the row buses (0.6 mm) take it across each row."""
    j1, j2 = bd.pad("J1201", "1"), bd.pad("J1201", "2")
    cplus = bd.pad("C984", "1")
    # both cable pins, then on F.Cu along under the header body (9 mm clear of the pin rows, so
    # the pins can leave downward on both layers) to a via left of C984 and into C984+ on B.Cu
    y_feed = K(0, 77.4)[1]
    xv = cplus[0] - 2.75
    bd.track("+5V_LED", [j1, j2, (j2[0], y_feed), (xv, y_feed)], F, W_FEED)
    bd.via("+5V_LED", xv, y_feed, 0.8, 0.4)
    bd.track("+5V_LED", [(xv, y_feed), (xv, cplus[1]), cplus], B, W_FEED)
    # from C984+ down to the top feed and right to the spine
    top_y = K(0, TOP_Y)[1]
    sx = K(SPINE_X, 0)[0]
    bd.track("+5V_LED", [cplus, (cplus[0], top_y), (sx, top_y)], B, W_FEED)
    bd.via("+5V_LED", sx, top_y, 0.8, 0.4)
    # U901 and its decoupling hang on the feed
    for ref, pad in (("U901", "5"), ("C983", "1")):
        p = bd.pad(ref, pad)
        bd.track("+5V_LED", [p, (p[0], top_y)], B, W_STUB)
    # spine: via onto each row bus end, the mute bus is extended to the spine
    ys = []
    for r in range(8):
        y = K(0, row_y(r))[1] + BUS_DY
        ys.append(y)
        bd.via("+5V_LED", sx, y, 0.8, 0.4)
    my = K(0, MUTE_Y)[1] + BUS_DY
    mute_end = bd.pad("C980", "1")[0] + 3.0
    bd.track("+5V_LED", [(mute_end, my), (sx, my)], B, W_BUS)
    bd.via("+5V_LED", sx, my, 0.8, 0.4)
    bd.track("+5V_LED", [(sx, top_y)] + [(sx, y) for y in ys] + [(sx, my)], F, W_FEED)


# ------------------------------------------------------------------ step row: LED and button buses
#
# Between the step LEDs (and their 470 R) and the step buttons runs a channel ~8 mm high: B.Cu
# carries U702's 16 LED lines, F.Cu the 16 button lines from U701/U703. On both expanders the
# outputs come out in the reverse order of the row on one pin column, which no router untangles
# in that space, so the lines are drawn here, 0.5 mm apart, with planned vias.

CH_TOP = 254.75         # first row of the channel (KiCad y), 0.33 mm below the 470 R pads
CH_PITCH = 0.5
TACT_X0 = 46.9          # button lines come down between D701 and D702 from here, 0.5 mm apart
TACT_VIA_DX = 0.7       # ... and 0.7 mm apart past their vias, before they close up
JOG_Y = 227.0


def ch_row(r):
    return CH_TOP + r * CH_PITCH


def step_buses(bd):
    led_bus(bd)
    button_bus(bd)
    for i in range(16):         # 470 R to the LED anode, above the channel
        r2, a = bd.pad("R7%02d" % (i + 1), "2"), bd.pad("D7%02d" % (i + 1), "2")
        bd.track(bd.pad_net("D7%02d" % (i + 1), "2"), [r2, (a[0], r2[1]), a], B, W_SIG)


def led_bus(bd):
    """U702 GPA0..7 -> R701..R708, GPB0..7 -> R709..R716 (pad 1, from below), all on B.Cu.

    GPB0..6 leave U702's left column and come down beside it, GPA0..7 leave the right column and
    come down right of mounting hole H12; in the channel R714's line is the top row, R701's the
    bottom one. GPB6 enters R715 from the side; GPB7 would have to cross the others and takes
    F.Cu round H12 instead."""
    r1 = [bd.pad("R7%02d" % (i + 1), "1") for i in range(16)]
    x_gpb = r1[14][0] + 0.85                 # first line right of R715 (pad edge + 0.33 mm)
    for p in range(1, 8):                    # GPB0..GPB6
        i = 7 + p
        pin, net = bd.pad("U702", str(p)), bd.pad_net("U702", str(p))
        x = x_gpb + (7 - p) * CH_PITCH
        if p == 7:
            pts = [pin, (x, pin[1]), (x, r1[i][1]), r1[i]]
        else:
            y = ch_row(13 - i)
            pts = [pin, (x, pin[1]), (x, y), (r1[i][0], y), r1[i]]
        bd.track(net, pts, B, W_SIG)
    pin, net = bd.pad("U702", "8"), bd.pad_net("U702", "8")     # GPB7 -> R716
    xv, xe = pin[0] - 2.35, r1[15][0] + 1.65
    yh = K(112.0, -92.0)[1] + 4.6            # below H12
    bd.track(net, [pin, (xv, pin[1])], B, W_SIG)
    bd.via(net, xv, pin[1])
    bd.track(net, [(xv, pin[1]), (xv, yh), (xe, yh), (xe, r1[15][1])], F, W_SIG)
    bd.via(net, xe, r1[15][1])
    bd.track(net, [(xe, r1[15][1]), r1[15]], B, W_SIG)
    for p in range(21, 29):                  # GPA0..GPA7
        i = p - 21
        pin, net = bd.pad("U702", str(p)), bd.pad_net("U702", str(p))
        x = pin[0] + 1.35 + (28 - p) * CH_PITCH
        y = ch_row(13 - i)
        bd.track(net, [pin, (x, pin[1]), (x, y), (r1[i][0], y), r1[i]], B, W_SIG)


def button_bus(bd):
    """U701 GPA0..6 -> SW701..707, U703 GPA0 -> SW708, U701 GPB0..6 -> SW709..715,
    U703 GPA1 -> SW716 (pin 1, either pad).

    All sixteen come down on F.Cu between D701 and D702, SW701 leftmost, and turn right into the
    channel, SW716 in the top row. U701's right column reaches F.Cu through vias in a diagonal;
    its left column runs out to the left, down and back under the chip on B.Cu, the lowest pin
    turning first, to vias in a staircase, which puts those lines in the right order."""
    def xv(k):
        return TACT_X0 + (k - 1) * TACT_VIA_DX

    def xt(k):
        return TACT_X0 + (k - 1) * CH_PITCH
    start = {}
    for p in range(21, 28):                  # SW701..SW707
        k = p - 20
        pin, net = bd.pad("U701", str(p)), bd.pad_net("U701", str(p))
        bd.track(net, [pin, (xv(k), pin[1])], B, W_SIG)
        bd.via(net, xv(k), pin[1])
        start[k] = (net, (xv(k), pin[1]))
    pin, net = bd.pad("U703", "21"), bd.pad_net("U703", "21")       # SW708
    bd.track(net, [pin, (xv(1), pin[1])], B, W_SIG)
    bd.via(net, xv(1), pin[1])
    bd.track(net, [(xv(1), pin[1]), (xv(8), pin[1])], F, W_SIG)
    start[8] = (net, (xv(8), pin[1]))
    pin, net = bd.pad("U703", "22"), bd.pad_net("U703", "22")       # SW716
    bd.track(net, [pin, (xv(16), pin[1])], B, W_SIG)
    bd.via(net, xv(16), pin[1])
    start[16] = (net, (xv(16), pin[1]))
    y0 = bd.pad("U701", "1")[1] + 1.75
    for p in range(1, 8):                    # SW709..SW715
        k = 8 + p
        pin, net = bd.pad("U701", str(p)), bd.pad_net("U701", str(p))
        xw = pin[0] - 1.35 - (p - 1) * CH_PITCH
        yt = y0 + (p - 1) * TACT_VIA_DX
        bd.track(net, [pin, (xw, pin[1]), (xw, yt), (xv(k), yt)], B, W_SIG)
        bd.via(net, xv(k), yt)
        start[k] = (net, (xv(k), yt))
    for k in range(1, 17):
        net, s0 = start[k]
        sx, sy = K(step_x(k - 1), STEP_Y)
        d = xv(k) - xt(k)
        pts = [s0, (xv(k), JOG_Y), (xt(k), JOG_Y + d)]
        if k == 1:
            pts.append((xt(k), sy - 2.25))                 # straight into its right-hand pad
        else:
            y = ch_row(16 - k)
            pts += [(xt(k), y), (sx - 3.25, y), (sx - 3.25, sy - 2.25)]
        bd.track(net, pts, F, W_SIG)
