"""Main board: on the case floor against the rear wall, all SMD parts on the top.

Left: the Pico with its micro-USB at the wall, the LED-rail switch next to its VBUS pin and the
cable header J1101 in front of it, so the LED current returns over the cable to the Pico's GND
at this end. Middle: MIDI OUT/IN. Right: DACs, op-amp, gate buffer and the eight jacks, as far
from the LED return path as the board allows.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import pcbnew

from geometry import DINS, JACKS, K, MAIN_X0, MAIN_X1, MAIN_Y0, MAIN_Y1, PICO_X

F, B = pcbnew.F_Cu, pcbnew.B_Cu
REAR = MAIN_Y1
PICO_PIN1 = (PICO_X - 8.89, REAR - 2.11)      # pin 1 next to the USB end, Pico edge 0.5 mm inside the board edge


def pico_pin(n):
    """Case-frame position of Pico pin n (1-20 down the left, 21-40 up the right)."""
    x0, y0 = PICO_PIN1
    if n <= 20:
        return x0, y0 - 2.54 * (n - 1)
    return x0 + 17.78, y0 - 2.54 * (40 - n)


def at(bd, ref, x, y, rot=0.0):
    kx, ky = K(x, y)
    bd.place_center(ref, kx, ky, rot)


def place(bd):
    x0, y0 = K(MAIN_X0, MAIN_Y1)
    x1, y1 = K(MAIN_X1, MAIN_Y0)
    bd.outline(x0, y0, x1, y1, r=2.0)
    kx, ky = K(*PICO_PIN1)
    bd.place("U301", kx, ky, 0)

    # source terminations right at the GPIO pins (GPIO 2-6 and 10-13 on the left, 27 on the right)
    for ref, pin in (("R1101", 4), ("R1102", 5), ("R1104", 6), ("R1103", 7), ("R1105", 9),
                     ("R561", 14), ("R562", 15), ("R563", 16), ("R564", 17)):
        x, y = pico_pin(pin)
        at(bd, ref, x - 5.0, y, 0)
    x, y = pico_pin(32)
    at(bd, "R1106", x + 5.0, y, 0)
    x, y = pico_pin(10)
    at(bd, "TP301", x - 6.5, y, 0)
    at(bd, "TP302", x - 6.5, y - 4.5, 0)
    x, y = pico_pin(33)
    at(bd, "R303", x + 5.0, y, 0)

    # LED rail switch beside VBUS (pin 40)
    vx, vy = pico_pin(40)
    at(bd, "U201", vx + 9.0, vy - 4.0, 0)
    at(bd, "C201", vx + 5.0, vy - 1.0, 90)
    at(bd, "C202", vx + 13.5, vy - 4.0, 90)
    at(bd, "R201", vx + 9.0, vy - 8.5, 0)
    at(bd, "R202", vx + 13.5, vy - 8.5, 0)
    at(bd, "TP201", vx + 13.5, vy - 13.0, 0)
    at(bd, "R203", vx + 5.0, vy - 8.5, 90)
    at(bd, "JP201", vx + 5.0, vy - 13.0, 0)

    # cable header in front of the Pico, opening toward the front edge; I2C pull-ups beside it
    at(bd, "J1101", -62.0, 95.5, 270)
    at(bd, "R301", -37.0, 93.0, 90)
    at(bd, "R302", -34.5, 93.0, 90)

    # MIDI OUT (J401) and IN (J402), DIN-5 at the rear edge, face at the board edge
    dins = dict((ref, x) for ref, _, x in DINS)
    for ref, x in dins.items():
        kx, ky = K(x, REAR - 12.5)
        bd.place(ref, kx, ky, 180)
    xo = dins["J401"]
    at(bd, "U401", xo - 6.0, 108.0, 0)
    at(bd, "C401", xo - 6.0, 104.0, 0)
    at(bd, "R401", xo - 10.5, 108.0, 90)
    at(bd, "R406", xo - 1.5, 108.0, 0)
    at(bd, "R407", xo - 1.5, 105.0, 0)
    at(bd, "Q401", xo + 2.5, 107.0, 0)
    at(bd, "R402", xo + 7.0, 107.0, 90)
    at(bd, "R403", xo + 10.0, 104.0, 90)
    at(bd, "FB401", xo + 6.0, 101.0, 0)
    at(bd, "FB402", xo + 10.0, 108.5, 90)
    xi = dins["J402"]
    at(bd, "FB403", xi - 8.0, 109.0, 90)
    at(bd, "FB404", xi - 5.5, 109.0, 90)
    at(bd, "R404", xi - 8.0, 104.5, 90)
    at(bd, "D401", xi - 5.0, 104.0, 90)
    at(bd, "U402", xi + 3.0, 101.0, 90)
    at(bd, "R405", xi + 9.5, 106.0, 90)
    at(bd, "C402", xi + 9.5, 101.0, 90)

    # jacks: right angle, face at the rear edge (rotation 270 puts the nose toward the wall)
    for i, (_, x) in enumerate(JACKS):
        ref = "J%d" % (501 + i)
        kx, ky = K(x, REAR - 1.8)
        bd.place(ref, kx, ky, 270)
        # TVS right in front of its jack
        at(bd, "D%d" % (501 + i), x, 114.5, 0)

    # CV outputs: per channel output resistor, feedback R and C, input pull-down, in front of the jack
    for n in range(1, 5):
        x = JACKS[n - 1][1]
        at(bd, "R51%d" % n, x - 2.5, 111.0, 90)      # 1 k inside the loop
        at(bd, "R52%d" % n, x + 2.5, 111.0, 90)      # 10 k feedback from the jack side
        at(bd, "C51%d" % n, x + 5.0, 111.0, 90)      # 1 nF C0G
        at(bd, "R51%d" % (n + 4), x - 5.0, 111.0, 90)  # 100 k at the + input
    # op-amp centred between the CV jacks, DACs in front of it
    at(bd, "U503", 91.0, 101.5, 90)
    at(bd, "C504", 91.0, 106.5, 0)
    at(bd, "U501", 106.0, 90.0, 90)
    at(bd, "C501", 106.0, 94.5, 0)
    at(bd, "C503", 111.0, 94.5, 0)
    at(bd, "U502", 76.0, 90.0, 90)
    at(bd, "C502", 76.0, 94.5, 0)
    at(bd, "C506", 71.0, 94.5, 0)

    # gates: buffer in the middle of the gate jacks
    at(bd, "U504", 29.0, 101.5, 90)
    at(bd, "C505", 29.0, 106.5, 0)
    at(bd, "C507", 35.0, 106.5, 0)
    for n in range(1, 5):
        x = JACKS[3 + n][1]
        at(bd, "R54%d" % n, x - 2.5, 111.0, 90)       # 1 k series
        at(bd, "R55%d" % n, x + 2.5, 111.0, 90)       # 100 k at the jack
        at(bd, "R53%d" % n, 13.0 + 8.0 * (n - 1), 92.0, 90)   # 100 k at the buffer input


HOLES = [(-118.0, 84.0), (-118.0, 127.0), (118.0, 84.0), (117.5, 101.0), (-46.0, 126.0), (-10.0, 84.0),
         (60.0, 84.0)]


def holes(bd):
    for i, (x, y) in enumerate(HOLES):
        kx, ky = K(x, y)
        bd.board_only("MountingHole:MountingHole_3.2mm_M3_Pad_Via", "H%d" % (i + 1), kx, ky, net="GND")
    for i, (x, y) in enumerate(((-114.0, 77.0), (-2.0, 126.0), (110.0, 78.0))):
        kx, ky = K(x, y)
        bd.board_only("Fiducial:Fiducial_1mm_Mask2mm", "FID%d" % (i + 1), kx, ky)


def finish(board):
    import common
    common.clear_zones(board)
    common.silk_inside(board)
    common.silk_to_fab(board, ("H", "FID", "R", "C", "D", "FB", "Q", "TP", "U201", "U401"))
    n_pads = common.stitch_pads(board)
    box = common.board_box(board)
    n = common.stitch(board, "GND", box)
    n += common.stitch(board, "GND", box, pitch=3.0)     # dense where both pours are present
    print("GND islands joined by vias: %d" % common.stitch_islands(board))
    print("main: %d GND islands joined" % n_pads)
    common.add_zone_once(board, "GND", F, box)
    common.add_zone_once(board, "GND", B, box)
    print("main: %d stitching vias" % n)
