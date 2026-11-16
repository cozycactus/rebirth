#!/usr/bin/env python3
"""Generate the KiCad schematic of the ReBirth RP2040 sequencer with its
session-style live panel.

    python3 hardware/schematic/gen/build.py           # write the project
    python3 hardware/schematic/gen/build.py --check   # write, then check the
                                                      # exported netlist and ERC

Edit this file, never the generated .kicad_sch files. Every placed part
declares the net of each pin (conn=...); --check exports the netlist with
kicad-cli and fails if the drawing connects anything differently.

Sources: docs/superpowers/specs/2026-11-16-session-panel-design.md,
ARCHITECTURE.md ("Hardware Interfaces", "MIDI Circuit", "CV Circuit") and the
pin/address constants in include/*.h.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sch  # noqa: E402
from sch import Design, mm  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.dirname(HERE)
KICAD_CLI = os.environ.get("KICAD_CLI", "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
DATE = "2026-11-16"
REV = "A"

# ------------------------------------------------------------------ footprints
FP_R = "Resistor_SMD:R_0603_1608Metric"
FP_R1210 = "Resistor_SMD:R_1210_3225Metric"
FP_C = "Capacitor_SMD:C_0603_1608Metric"
FP_C0805 = "Capacitor_SMD:C_0805_2012Metric"
FP_CP = "Capacitor_SMD:CP_Elec_6.3x7.7"
FP_FB = "Inductor_SMD:L_0603_1608Metric"
FP_D = "Diode_SMD:D_SOD-123"
FP_LED3 = "LED_THT:LED_D3.0mm"
FP_TP = "TestPoint:TestPoint_Pad_D1.5mm"
FP_SOT235 = "Package_TO_SOT_SMD:SOT-23-5"
FP_SOT236 = "Package_TO_SOT_SMD:SOT-23-6"
FP_SO8 = "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"
FP_SO14 = "Package_SO:SOIC-14_3.9x8.7mm_P1.27mm"
FP_SO28W = "Package_SO:SOIC-28W_7.5x17.9mm_P1.27mm"
FP_DIP6 = "Package_DIP:DIP-6_W7.62mm"
FP_JACK = "Connector_Audio:Jack_3.5mm_QingPu_WQP-PJ398SM_Vertical_CircularHoles"
FP_MX = "Button_Switch_Keyboard:SW_Cherry_MX_1.00u_PCB"
FP_TACT = "Button_Switch_THT:SW_PUSH_6mm"
FP_ENC = "Rotary_Encoder:RotaryEncoder_Alps_EC11E-Switch_Vertical_H20mm"
FP_HDR7 = "Connector_PinHeader_2.54mm:PinHeader_1x07_P2.54mm_Vertical"
FP_PICO = "Module:RaspberryPi_Pico_Common_THT"
FP_RGB = "LED_SMD:LED_SK6812MINI-E_3.2x2.8mm_P1.5mm_ReverseMount"


# ------------------------------------------------------------------ project symbols

def _pin(etype, x, y, ang, length, name, num):
    return ('(pin %s line (at %s %s %d) (length %s) (name "%s" (effects (font (size 1.27 1.27)))) '
            '(number "%s" (effects (font (size 1.27 1.27)))))' % (etype, x, y, ang, length, name, num))


def _props(ref, value, fp, ds, desc, ref_at, val_at):
    def p(k, v, at, hide=False, just=""):
        return ('(property "%s" "%s" (at %s %s 0) (effects (font (size 1.27 1.27))%s%s))'
                % (k, v, at[0], at[1], " (justify %s)" % just if just else "", " (hide yes)" if hide else ""))
    return " ".join([p("Reference", ref, ref_at, just="left bottom"), p("Value", value, val_at, just="left top"),
                     p("Footprint", fp, (0, 0), True), p("Datasheet", ds, (0, 0), True),
                     p("Description", desc, (0, 0), True)])


def project_symbols(lib):
    head = '(pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes) (in_pos_files yes) (duplicate_pin_numbers_are_jumpers no)'
    out = []
    # TCA8418, pins grouped by function: bus on the left, matrix on the right.
    pins = []
    for r in range(8):
        pins.append(_pin("bidirectional", 15.24, 22.86 - 2.54 * r, 180, 5.08, "ROW%d" % r, 8 - r))
    for c in range(10):
        pins.append(_pin("bidirectional", 15.24, -2.54 * c, 180, 5.08, "COL%d" % c, 9 + c))
    pins += [_pin("input", -15.24, 20.32, 0, 5.08, "~{RESET}", 20),
             _pin("open_collector", -15.24, 17.78, 0, 5.08, "~{INT}", 24),
             _pin("input", -15.24, 10.16, 0, 5.08, "SCL", 23),
             _pin("bidirectional", -15.24, 7.62, 0, 5.08, "SDA", 22),
             _pin("power_in", 0, 33.02, 270, 5.08, "VCC", 21),
             _pin("power_in", -2.54, -33.02, 90, 5.08, "GND", 19),
             _pin("passive", 2.54, -33.02, 90, 5.08, "EP", 25)]
    out.append('(symbol "TCA8418RTW" %s %s (symbol "TCA8418RTW_0_1" (rectangle (start -10.16 27.94) (end 10.16 -27.94) '
               '(stroke (width 0.254) (type default)) (fill (type background)))) (symbol "TCA8418RTW_1_1" %s) (embedded_fonts no))'
               % (head, _props("U", "TCA8418RTW", "Package_DFN_QFN:QFN-24-1EP_4x4mm_P0.5mm_EP2.5x2.5mm",
                               "https://www.ti.com/lit/ds/symlink/tca8418.pdf",
                               "I2C keypad scan controller, up to 8x10 keys, 1.65-3.6 V, address 0x34, WQFN-24 (RTW). Pin numbers per TI SCPS215G.",
                               (-10.16, 29.21), (-10.16, -29.21)), " ".join(pins)))
    # TPS2553 current-limited load switch (DBV, SOT-23-6), active-high enable.
    pins = [_pin("power_in", -15.24, 2.54, 0, 5.08, "IN", 1),
            _pin("input", -15.24, -2.54, 0, 5.08, "EN", 3),
            _pin("power_out", 15.24, 2.54, 180, 5.08, "OUT", 6),
            _pin("open_collector", 15.24, -2.54, 180, 5.08, "~{FAULT}", 4),
            _pin("power_in", -2.54, -15.24, 90, 5.08, "GND", 2),
            _pin("passive", 2.54, -15.24, 90, 5.08, "ILIM", 5)]
    out.append('(symbol "TPS2553DBV" %s %s (symbol "TPS2553DBV_0_1" (rectangle (start -10.16 5.08) (end 10.16 -10.16) '
               '(stroke (width 0.254) (type default)) (fill (type background)))) (symbol "TPS2553DBV_1_1" %s) (embedded_fonts no))'
               % (head, _props("U", "TPS2553DBV", FP_SOT236, "https://www.ti.com/lit/ds/symlink/tps2553.pdf",
                               "Current-limited power switch, adjustable limit, active-high enable, auto-retry, SOT-23-6. Pin numbers per TI SLVS841.",
                               (-7.62, 6.35), (-7.62, -6.35)), " ".join(pins)))
    # +5V_LED: the switched 5 V rail of the RGB chain, a copy of power:+5V.
    p5 = sch.dump(lib.get("power:+5V").sexpr)
    p5 = p5.replace('(symbol "power:+5V"', '(symbol "+5V_LED"').replace('"+5V_0_1"', '"+5V_LED_0_1"') \
        .replace('"+5V_1_1"', '"+5V_LED_1_1"').replace('(property "Value" "+5V"', '(property "Value" "+5V_LED"') \
        .replace('global label with name \\"+5V\\"', 'global label with name \\"+5V_LED\\"')
    out.append(p5)
    return out


# ------------------------------------------------------------------ placement helpers

def place(sh, lib_id, ref, value, x, y, rot=0, mirror=None, unit=1, conn=None, fp="",
          ref_at=None, val_at=None, hide_val=False, hide_ref=False, props=None, dnp=False):
    f = {}
    if ref_at:
        f["ref"] = (mm(x + ref_at[0]), mm(y + ref_at[1]), ref_at[2])
    if val_at:
        f["val"] = (mm(x + val_at[0]), mm(y + val_at[1]), val_at[2])
    if hide_val:
        f["hide_val"] = True
    if hide_ref:
        f["hide_ref"] = True
    return sh.part(lib_id, ref, value, (x, y), rot, mirror, unit, conn, fp, f, dnp, props)


RIGHT = ((2.54, -1.27, "left"), (2.54, 1.27, "left"))
LEFT = ((-2.54, -1.27, "right"), (-2.54, 1.27, "right"))
ABOVE = ((0, -2.54, "center"), (0, 2.54, "center"))


def res(sh, ref, val, x, y, conn, horiz=False, fields=None, fp=FP_R, **kw):
    """Device:R. Vertical: pin 1 at y-3.81 (top). Horizontal: pin 1 at x-3.81 (left)."""
    fr, fv = fields or (ABOVE if horiz else RIGHT)
    return place(sh, "Device:R", ref, val, x, y, 90 if horiz else 0, conn=conn, fp=fp,
                 ref_at=fr, val_at=fv, **kw)


def cap(sh, ref, val, x, y, conn, horiz=False, fields=None, fp=FP_C, polar=False, **kw):
    fr, fv = fields or (((0, -3.175, "center"), (0, 3.175, "center")) if horiz else
                        ((2.54, -1.27, "left"), (2.54, 1.27, "left")))
    return place(sh, "Device:C_Polarized" if polar else "Device:C", ref, val, x, y, 90 if horiz else 0,
                 conn=conn, fp=fp, ref_at=fr, val_at=fv, **kw)


def gnd(sh, x, y):
    sh.pwr("GND", (x, y))


def rail(sh, name, x, y):
    sh.pwr(name, (x, y))


def decap(sh, ref, val, x, y, railname, fp=FP_C, polar=False):
    """Decoupling capacitor between its own rail symbol above and ground below."""
    cap(sh, ref, val, x, y, {"1": railname, "2": "GND"}, fp=fp, polar=polar)
    sh.wire((x, y - 3.81), (x, y - 6.35))
    rail(sh, railname, x, y - 6.35)
    sh.wire((x, y + 3.81), (x, y + 6.35))
    gnd(sh, x, y + 6.35)


def pull_up(sh, ref, val, x, y_node, railname, conn_node, fields=None):
    """Resistor standing on a horizontal wire at (x, y_node), rail above."""
    res(sh, ref, val, x, y_node - 6.35, {"1": railname, "2": conn_node}, fields=fields)
    sh.wire((x, y_node - 2.54), (x, y_node))
    sh.wire((x, y_node - 10.16), (x, y_node - 12.7))
    rail(sh, railname, x, y_node - 12.7)


def pull_down(sh, ref, val, x, y_node, conn_node, fields=None):
    res(sh, ref, val, x, y_node + 6.35, {"1": conn_node, "2": "GND"}, fields=fields)
    sh.wire((x, y_node), (x, y_node + 2.54))
    sh.wire((x, y_node + 10.16), (x, y_node + 12.7))
    gnd(sh, x, y_node + 12.7)


def note(sh, x, y, *lines, size=1.27):
    """Note anchored at its top-left corner, one text item per line at a 2.54 mm pitch."""
    for i, line in enumerate(lines):
        if line:
            sh.text(line, (x, y + 2.54 * i), size=size, justify="left top")


# ------------------------------------------------------------------ sheet 2: power

def sheet_power(d):
    sh = d.sheet("Power", "power.kicad_sch", "Power: VBUS, +3V3 and the switched LED rail +5V_LED", "A4", 2)
    sh.heading("LED supply: +5V_LED = VBUS through a current-limited switch", (20.32, 38.1))
    note(sh, 20.32, 44.45,
         "Feeds only the 82 SK6812MINI-E and their data buffer U901 (sheet 9). Limit 234-303 mA, firmware budget 200 mA.")
    y = 60.96
    yg = 91.44
    rail(sh, "VBUS", 40.64, y)
    sh.wire((40.64, y), (76.2, y))
    note(sh, 20.32, 49.53, "VBUS = (4.75, 5.0, 5.25) V @ (, 300, 500) mA from the laptop USB port,",
         "through the Pico's micro-USB socket (U301 pin 40, sheet 3).")
    # input capacitor
    sh.wire((55.88, y), (55.88, 81.28))
    cap(sh, "C201", "1u", 55.88, 85.09, {"1": "VBUS", "2": "GND"}, fp=FP_C0805)
    sh.wire((55.88, 88.9), (55.88, yg))
    gnd(sh, 55.88, yg)
    # switch
    place(sh, "rebirth:TPS2553DBV", "U201", "TPS2553DBV", 91.44, 63.5, fp=FP_SOT236,
          ref_at=(-10.16, -6.35, "left"), val_at=(-2.54, -6.35, "left"),
          conn={"1": "VBUS", "2": "GND", "3": "+3V3", "4": "LED_FAULT_N", "5": "ILIM", "6": "+5V_LED"})
    sh.wire((76.2, 66.04), (71.12, 66.04))
    rail(sh, "+3V3", 71.12, 66.04)
    sh.wire((88.9, 78.74), (88.9, yg))
    gnd(sh, 88.9, yg)
    sh.wire((93.98, 78.74), (93.98, 81.28))
    res(sh, "R201", "100k 1%", 93.98, 85.09, {"1": "ILIM", "2": "GND"})
    sh.wire((93.98, 88.9), (93.98, yg))
    gnd(sh, 93.98, yg)
    # output
    sh.wire((106.68, y), (147.32, y))
    rail(sh, "+5V_LED", 147.32, y)
    sh.wire((134.62, y), (134.62, 81.28))
    cap(sh, "C202", "10u", 134.62, 85.09, {"1": "+5V_LED", "2": "GND"}, fp=FP_C0805)
    sh.wire((134.62, 88.9), (134.62, yg))
    gnd(sh, 134.62, yg)
    # fault flag: test point with pull-up
    sh.wire((106.68, 66.04), (115.57, 66.04))
    sh.wire((111.76, 66.04), (111.76, 71.12))
    place(sh, "Connector:TestPoint", "TP201", "LED_FAULT_N", 111.76, 71.12, rot=180, fp=FP_TP,
          conn={"1": "LED_FAULT_N"}, ref_at=(2.54, 3.81, "left"), val_at=(2.54, 6.35, "left"))
    res(sh, "R202", "10k", 119.38, 66.04, {"1": "LED_FAULT_N", "2": "+3V3"}, horiz=True,
        fields=((0, -2.286, "center"), (0, 2.794, "center")))
    sh.wire((123.19, 66.04), (125.73, 66.04))
    rail(sh, "+3V3", 125.73, 66.04)
    note(sh, 139.7, 64.77, "+5V_LED = (4.7, 4.95, 5.25) V", "@ (, 200, 306) mA")
    note(sh, 20.32, 104.14,
         "U201 TPS2553DBV (active-high EN, auto-retry). Why a switch here:",
         " - inrush: USB allows 10 uF at plug-in; the chain has 82 x 100 nF + 470 uF bulk (C984), so it is charged at the current limit;",
         " - backstop: a firmware error that lights the whole grid at full white (82 x 60 mA) cannot pull more than 306 mA;",
         " - power-up: EN follows +3V3 (threshold 0.66-1.1 V), so the rail comes up with the Pico's regulator, before the firmware runs.",
         "R201 100 kOhm 1% sets the limit, IOS = (232, 267, 306) mA with the 1 % tolerance (TPS2553 eq. 1-3). Firmware budget 200 mA;",
         "keep R201 >= 82 kOhm (IOS max 369 mA). The 5 V budget below is an estimate until the OLED is chosen and the board measured.",
         "TP201: low while U201 limits current or is too hot. Keep large capacitors behind U201: C201 is the only one on VBUS.",
         "+5V_LED min 4.7 V is VBUS min minus the switch (~30 mV at 200 mA); it falls lower only while U201 limits current.")
    sh.heading("Rails and budget", (20.32, 139.7))
    note(sh, 20.32, 146.05,
         "VBUS    5 V from USB, Pico pin 40. Loads: U201 (LED chain), U503 MCP6004, U504 74AHCT125, Pico VSYS (through its own diode).",
         "+3V3    Pico's RT6150B buck-boost, pin 36; external load <= 300 mA (Pico datasheet), this board ~100 mA:",
         "        OLED ~25 mA, step LEDs 16 x 3 mA, MIDI OUT 7 mA, expanders, TCA8418, DACs, pull-ups.",
         "+5V_LED switched VBUS for the RGB chain only (sheet 9).",
         "GND     one ground. On the board, return the LED-chain current to the Pico's GND pins on its own path,",
         "        apart from the DAC/op-amp ground of sheet 5 (CV noise check in HARDWARE_STATUS.md).",
         "Budget at 5 V (estimate): RGB chain <= 200 mA (firmware) / 306 mA (U201), Pico + 3.3 V loads 100-130 mA through the",
         "buck-boost, gates <= 20 mA => about 450 mA worst case. Idle: 82 x ~1 mA for the chain plus the Pico, above the 100 mA",
         "an unconfigured USB device may draw; most laptop ports allow it (design: bench on two laptops).")
    return sh


# ------------------------------------------------------------------ sheet 3: MCU

PICO_GPIO = {  # gpio -> (pin number, net, shape seen from the Pico)
    0: (1, "MIDI_TX", "output"), 1: (2, "MIDI_RX", "input"),
    2: (4, "OLED_SCK", "output"), 3: (5, "OLED_MOSI", "output"), 4: (6, "OLED_CS_N", "output"),
    5: (7, "OLED_DC", "output"), 6: (9, "OLED_RST_N", "output"), 7: (10, "JITTER_PROBE", None),
    8: (11, "I2C_SDA", "bidirectional"), 9: (12, "I2C_SCL", "output"),
    10: (14, "DAC_SCK", "output"), 11: (15, "DAC_MOSI", "output"), 12: (16, "DAC_CS1_N", "output"),
    13: (17, "DAC_CS2_N", "output"), 14: (19, "GATE1", "output"), 15: (20, "GATE2", "output"),
    16: (21, "GATE3", "output"), 17: (22, "GATE4", "output"), 18: (24, "ENC1_A", "input"),
    19: (25, "ENC1_B", "input"), 20: (26, "ENC1_SW_N", "input"), 21: (27, "ENC2_A", "input"),
    22: (29, "ENC2_B", "input"), 26: (31, "ENC2_SW_N", "input"), 27: (32, "LED_DATA", "output"),
    28: (34, "PLAY_N", "input"),
}


def sheet_mcu(d):
    sh = d.sheet("MCU", "mcu.kicad_sch", "Raspberry Pi Pico and the I2C bus", "A4", 3)
    X, Y = 139.7, 104.14
    conn = {str(pin): net for (pin, net, _) in PICO_GPIO.values()}
    for g in ("3", "8", "13", "18", "23", "28", "38"):
        conn[g] = "GND"
    conn["33"] = "AGND"
    conn.update({"36": "+3V3", "40": "VBUS", "30": None, "37": None, "35": None, "39": None})
    place(sh, "MCU_Module:RaspberryPi_Pico", "U301", "Raspberry Pi Pico", X, Y, fp=FP_PICO, conn=conn,
          ref_at=(10.16, -40.64, "left"), val_at=(10.16, -38.1, "left"))
    for gpio, (pin, net, shape) in PICO_GPIO.items():
        px, py = sh.pin("U301", str(pin))
        left = px < X
        if gpio in (8, 9):
            sh.wire((px, py), (63.5, py))
            sh.hlabel(net, (63.5, py), shape, "left")
        elif gpio == 7:
            sh.wire((px, py), (px - 17.78, py))
            place(sh, "Connector:TestPoint", "TP301", "JITTER", px - 17.78, py, rot=90, fp=FP_TP,
                  conn={"1": "JITTER_PROBE"}, ref_at=(-8.89, 0, "center"), val_at=(-8.89, 2.54, "center"),
                  hide_val=True)
        elif left:
            sh.wire((px, py), (px - 5.08, py))
            sh.hlabel(net, (px - 5.08, py), shape, "left")
        else:
            sh.wire((px, py), (px + 5.08, py))
            sh.hlabel(net, (px + 5.08, py), shape, "right")
    # I2C pull-ups
    sda_y = sh.pin("U301", "11")[1]
    scl_y = sh.pin("U301", "12")[1]
    pull_up(sh, "R301", "2.2k", 71.12, sda_y, "+3V3", "I2C_SDA")
    res(sh, "R302", "2.2k", 81.28, sda_y - 6.35, {"1": "+3V3", "2": "I2C_SCL"})
    sh.wire((81.28, sda_y - 2.54), (81.28, scl_y))
    sh.wire((81.28, sda_y - 10.16), (81.28, sda_y - 12.7))
    rail(sh, "+3V3", 81.28, sda_y - 12.7)
    # unused pins
    for pin in ("30", "37", "35", "39"):
        sh.nc(sh.pin("U301", pin))
    ax, ay = sh.pin("U301", "33")
    sh.wire((ax, ay), (ax + 5.08, ay), (ax + 5.08, ay + 2.54))
    res(sh, "R303", "0R", ax + 5.08, ay + 6.35, {"1": "AGND", "2": "GND"})
    sh.wire((ax + 5.08, ay + 10.16), (ax + 5.08, ay + 12.7))
    gnd(sh, ax + 5.08, ay + 12.7)
    # rails
    vx, vy = sh.pin("U301", "40")
    sh.wire((vx, vy), (vx, vy - 7.62))
    rail(sh, "VBUS", vx, vy - 7.62)
    tx, ty = sh.pin("U301", "36")
    sh.wire((tx, ty), (tx, ty - 5.08))
    rail(sh, "+3V3", tx, ty - 5.08)
    gx, gy = sh.pin("U301", "3")
    sh.wire((gx, gy), (gx, gy + 2.54))
    gnd(sh, gx, gy + 2.54)
    # test points
    place(sh, "Connector:TestPoint", "TP302", "GND", 50.8, 152.4, fp=FP_TP, conn={"1": "GND"},
          ref_at=(2.54, -5.08, "left"), val_at=(2.54, -2.54, "left"))
    sh.wire((50.8, 152.4), (50.8, 154.94))
    gnd(sh, 50.8, 154.94)
    sh.heading("Raspberry Pi Pico (RP2040) and the I2C bus", (20.32, 33.02))
    note(sh, 20.32, 40.64,
         "U301 plugs in on headers. All 26 header GPIOs are used; further controls go on I2C. Flags lead to sheets 4-9.",
         "VBUS (pin 40) and 3V3 (pin 36) are the board's 5 V and 3.3 V sources (power-output pins); sheet 2 has the budget.")
    note(sh, 20.32, 76.2,
         "I2C0 400 kHz: 0x20 + 0x24 step buttons, 0x21 step LEDs",
         "(sheet 7), 0x34 TCA8418 key matrix (sheet 8).",
         "R301/R302: external pull-ups, the firmware turns the",
         "internal ones off. 2.2 k suits up to ~150 pF; fit up",
         "to 4.7 k when the measured bus allows it.")
    note(sh, 20.32, 165.1,
         "TP301 (JITTER, on GPIO 7): high while the sequencer timer ISR runs (JITTER_MEASUREMENT.md); TP302 is the probe ground.",
         "Left open on purpose: RUN and 3V3_EN (pulled up on the Pico), VSYS (5 V comes from VBUS),",
         "ADC_VREF (GPIO 26-28 are digital here). R303 0 R joins AGND to GND at one point. GPIO 25 (on-board LED) is not on the header.")
    return sh


# ------------------------------------------------------------------ sheet 4: MIDI

def sheet_midi(d):
    sh = d.sheet("MIDI", "midi.kicad_sch", "MIDI OUT (3.3 V, CA-033) and MIDI IN (reserved)", "A4", 4)
    sh.heading("MIDI OUT: 5 mA current loop at 3.3 V (MIDI CA-033)", (20.32, 30.48))
    y = 68.58     # TX, inverter and base drive
    yj = 58.42    # loop: collector -> RC -> pin 5
    sh.hlabel("MIDI_TX", (30.48, y), "input", "left")
    sh.wire((30.48, y), (60.96, y))
    pull_up(sh, "R401", "10k", 45.72, y, "+3V3", "MIDI_TX")
    place(sh, "74xGxx:74LVC1G14", "U401", "74LVC1G14", 76.2, y, fp=FP_SOT235,
          conn={"2": "MIDI_TX", "4": "MIDI_TX_N", "5": "+3V3", "3": "GND", "1": None},
          ref_at=(2.54, -8.89, "left"), val_at=(2.54, 8.89, "left"))
    sh.wire((71.12, y - 10.16), (71.12, y - 12.7))
    rail(sh, "+3V3", 71.12, y - 12.7)
    sh.wire((71.12, y + 10.16), (71.12, y + 12.7))
    gnd(sh, 71.12, y + 12.7)
    decap(sh, "C401", "100n", 83.82, 45.72, "+3V3")
    sh.wire((88.9, y), (92.71, y))
    res(sh, "R406", "470", 96.52, y, {"1": "MIDI_TX_N", "2": "MIDI_BASE"}, horiz=True)
    sh.wire((100.33, y), (106.68, y))
    pull_down(sh, "R407", "10k", 104.14, y, "MIDI_BASE", fields=LEFT)
    place(sh, "Transistor_BJT:MMBT3904", "Q401", "MMBT3904", 111.76, y, fp="Package_TO_SOT_SMD:SOT-23",
          conn={"1": "MIDI_BASE", "2": "GND", "3": "MIDI_SINK"},
          ref_at=(5.08, -1.27, "left"), val_at=(5.08, 1.27, "left"))
    sh.wire((114.3, y + 5.08), (114.3, y + 7.62))
    gnd(sh, 114.3, y + 7.62)
    sh.wire((114.3, y - 5.08), (114.3, yj), (118.11, yj))
    res(sh, "R402", "10R 0.25W", 121.92, yj, {"1": "MIDI_SINK", "2": "MIDI_OUT_5"}, horiz=True, fp="Resistor_SMD:R_1206_3216Metric")
    sh.wire((125.73, yj), (132.08, yj))
    place(sh, "Device:FerriteBead_Small", "FB401", "1k@100MHz", 134.62, yj, rot=90, fp=FP_FB,
          conn={"1": "MIDI_OUT_5", "2": "MIDI_OUT_5F"}, ref_at=(0, -3.175, "center"), val_at=(0, 3.81, "center"))
    sh.wire((137.16, yj), (147.32, yj))
    place(sh, "Connector:DIN-5_180degree", "J401", "MIDI OUT", 154.94, yj - 2.54, rot=180,
          conn={"5": "MIDI_OUT_5F", "4": "MIDI_OUT_4F", "2": "GND", "1": None, "3": None},
          ref_at=(-5.08, -8.89, "left"), val_at=(5.08, 7.62, "left"))
    sh.nc((147.32, yj - 2.54))
    sh.nc((162.56, yj - 2.54))
    sh.wire((154.94, yj + 5.08), (154.94, yj + 7.62))
    gnd(sh, 154.94, yj + 7.62)
    sh.wire((162.56, yj), (167.64, yj), (167.64, yj - 2.54))
    place(sh, "Device:FerriteBead_Small", "FB402", "1k@100MHz", 167.64, yj - 5.08, fp=FP_FB,
          conn={"1": "MIDI_OUT_4", "2": "MIDI_OUT_4F"}, ref_at=(2.54, -1.27, "left"), val_at=(2.54, 1.27, "left"))
    sh.wire((167.64, yj - 7.62), (167.64, yj - 11.43))
    res(sh, "R403", "33R 0.5W", 167.64, yj - 15.24, {"1": "+3V3", "2": "MIDI_OUT_4"}, fp=FP_R1210)
    sh.wire((167.64, yj - 19.05), (167.64, yj - 21.59))
    rail(sh, "+3V3", 167.64, yj - 21.59)
    note(sh, 180.34, 35.56,
         "J401 MIDI OUT, rear panel: DIN-5 180 deg female.",
         "Pin 4 = +3.3 V through RA = R403 33 R 0.5 W,",
         "pin 5 = data through RC = R402 10 R 0.25 W",
         "(CA-033, 3.3 V). Pin 2 to GND (transmitter side",
         "only); pins 1/3 and the shell are not connected.")
    note(sh, 20.32, 86.36,
         "Open collector as CA-033 assumes at 3.3 V: U401 inverts GPIO 0 (UART0 TX), Q401 sinks ~7 mA.",
         "TX high/idle = Q401 off. R401 idles the line before the UART runs, R407 keeps Q401 off unpowered.",
         "Pin 5 to GND: no driver current; 4-5 shorted: <= 85 mA in Q401 (200 mA max). FB401/402 optional.")

    sh.heading("MIDI IN: opto-isolated receiver (reserved in firmware)", (20.32, 99.06))
    yi = 114.3
    place(sh, "Connector:DIN-5_180degree", "J402", "MIDI IN", 40.64, 116.84, mirror="y",
          conn={"4": "MIDI_IN_4", "5": "MIDI_IN_5", "1": None, "2": None, "3": None},
          ref_at=(-5.08, -11.43, "left"), val_at=(-5.08, 11.43, "left"))
    for pin in ("1", "2", "3"):
        sh.nc(sh.pin("J402", pin))
    sh.wire((48.26, yi), (53.34, yi))
    place(sh, "Device:FerriteBead_Small", "FB403", "1k@100MHz", 55.88, yi, rot=90, fp=FP_FB,
          conn={"1": "MIDI_IN_4", "2": "MIDI_IN_4F"}, ref_at=(0, -3.175, "center"), val_at=(0, 3.81, "center"))
    sh.wire((58.42, yi), (64.77, yi))
    res(sh, "R404", "220R", 68.58, yi, {"1": "MIDI_IN_4F", "2": "OPTO_A"}, horiz=True)
    sh.wire((72.39, yi), (88.9, yi))
    place(sh, "Diode:1N4148W", "D401", "1N4148W", 81.28, 118.11, rot=270, fp=FP_D,
          conn={"1": "OPTO_A", "2": "OPTO_K"}, ref_at=(-2.54, -1.27, "right"), val_at=(-2.54, 1.27, "right"))
    place(sh, "Isolator:H11L1", "U402", "H11L1", 96.52, 116.84, fp=FP_DIP6,
          conn={"1": "OPTO_A", "2": "OPTO_K", "4": "MIDI_RX", "5": "GND", "6": "+3V3", "3": None},
          ref_at=(2.54, -11.43, "left"), val_at=(2.54, -8.89, "left"))
    sh.wire((88.9, 119.38), (86.36, 119.38), (86.36, 121.92), (81.28, 121.92), (81.28, 134.62), (63.5, 134.62))
    place(sh, "Device:FerriteBead_Small", "FB404", "1k@100MHz", 60.96, 134.62, rot=90, fp=FP_FB,
          conn={"1": "MIDI_IN_5", "2": "OPTO_K"}, ref_at=(0, -3.175, "center"), val_at=(0, 3.81, "center"))
    sh.wire((58.42, 134.62), (25.4, 134.62), (25.4, yi), (33.02, yi))
    sh.wire((96.52, 109.22), (96.52, 106.68))
    rail(sh, "+3V3", 96.52, 106.68)
    sh.wire((96.52, 124.46), (96.52, 127))
    gnd(sh, 96.52, 127)
    sh.wire((104.14, 116.84), (127, 116.84))
    pull_up(sh, "R405", "1k", 111.76, 116.84, "+3V3", "MIDI_RX")
    sh.hlabel("MIDI_RX", (127, 116.84), "output", "right")
    decap(sh, "C402", "100n", 132.08, 101.6, "+3V3")
    note(sh, 152.4, 111.76,
         "J402 MIDI IN, rear panel: DIN-5 180 deg female.",
         "No jack pin touches GND: pin 2 and the shell stay",
         "open (CA-033); pins 1/3 not used.",
         "R404 220 R limits the loop current, D401 protects",
         "the opto LED against reversed polarity.")
    note(sh, 20.32, 146.05,
         "U402 H11L1: Schmitt-trigger opto, 3-15 V, turns on below 1.6 mA, open-collector output pulled up by R405.",
         "Idle loop (no current) = output high = UART idle. GPIO 1 is UART0 RX; the firmware keeps MIDI IN reserved.")
    return sh


# ------------------------------------------------------------------ sheet 5: CV and gates

def dac_block(sh, ref, X, Y, cs_net, outs, cap_refs):
    place(sh, "Analog_DAC:MCP4822", ref, "MCP4822", X, Y, fp=FP_SO8,
          conn={"1": "+3V3", "2": cs_net, "3": "DAC_SCK", "4": "DAC_MOSI", "5": "GND", "6": outs[1],
                "7": "GND", "8": outs[0]},
          ref_at=(2.54, -13.97, "left"), val_at=(2.54, -11.43, "left"))
    sh.wire((X, Y - 10.16), (X, Y - 12.7))
    rail(sh, "+3V3", X, Y - 12.7)
    sh.wire((X, Y + 12.7), (X, Y + 15.24))
    gnd(sh, X, Y + 15.24)
    lx = X - 35.56
    sh.wire((X - 12.7, Y - 2.54), (lx, Y - 2.54))
    sh.hlabel("DAC_SCK", (lx, Y - 2.54), "input", "left")
    sh.wire((X - 12.7, Y), (X - 22.86, Y))
    gnd(sh, X - 22.86, Y)
    sh.wire((X - 12.7, Y + 2.54), (X - 17.78, Y + 2.54), (X - 17.78, Y + 7.62), (lx, Y + 7.62))
    sh.hlabel("DAC_MOSI", (lx, Y + 7.62), "input", "left")
    sh.wire((X - 12.7, Y + 5.08), (X - 15.24, Y + 5.08), (X - 15.24, Y + 10.16), (lx, Y + 10.16))
    sh.hlabel(cs_net, (lx, Y + 10.16), "input", "left")
    decap(sh, cap_refs[0], "100n", X + 20.32, Y - 17.78, "+3V3")
    if len(cap_refs) > 1:
        decap(sh, cap_refs[1], "10u", X + 30.48, Y - 17.78, "+3V3", fp=FP_C0805)


def cv_stage(sh, unit, Xo, Yo, n, dac_net, refs):
    """Unity buffer with the output resistor inside the loop."""
    pins = {1: ("3", "2", "1"), 2: ("5", "6", "7"), 3: ("10", "9", "8"), 4: ("12", "13", "14")}[unit]
    inv = "CV%d_FB" % n
    out = "CV%d_AMP" % n
    jack = "CV%d" % n
    place(sh, "Amplifier_Operational:MCP6004", "U503", "MCP6004", Xo, Yo, unit=unit, fp=FP_SO14,
          conn={pins[0]: dac_net, pins[1]: inv, pins[2]: out},
          ref_at=(2.54, -7.62, "left"), val_at=(2.54, -5.08, "left"))
    rcf, rrs, rrf, rpd = refs
    # pull-down on the + input: defined 0 V while the DAC is unpowered or in shutdown
    pull_down(sh, rpd, "100k", Xo - 20.32, Yo - 2.54, dac_net)
    sh.wire((Xo - 7.62, Yo + 2.54), (Xo - 10.16, Yo + 2.54), (Xo - 10.16, Yo + 15.24), (Xo + 1.27, Yo + 15.24))
    sh.wire((Xo - 10.16, Yo + 7.62), (Xo - 3.81, Yo + 7.62))
    cap(sh, rcf, "100p", Xo, Yo + 7.62, {"1": inv, "2": out}, horiz=True,
        fields=((0, 3.048, "center"), (0, 5.08, "center")), hide_val=True)
    sh.wire((Xo + 3.81, Yo + 7.62), (Xo + 10.16, Yo + 7.62), (Xo + 10.16, Yo))
    sh.wire((Xo + 7.62, Yo), (Xo + 11.43, Yo))
    res(sh, rrs, "1k", Xo + 15.24, Yo, {"1": out, "2": jack}, horiz=True)
    res(sh, rrf, "10k", Xo + 5.08, Yo + 15.24, {"1": inv, "2": jack}, horiz=True,
        fields=((0, -2.54, "center"), (0, 2.794, "center")))
    sh.wire((Xo + 8.89, Yo + 15.24), (Xo + 22.86, Yo + 15.24), (Xo + 22.86, Yo))
    sh.wire((Xo + 19.05, Yo), (Xo + 30.48, Yo))
    sh.label(jack, (Xo + 24.13, Yo))
    jack_out(sh, "J%d" % (500 + n), "CV %d" % n, Xo + 35.56, Yo, jack)


def jack_out(sh, ref, label, x, y, net):
    place(sh, "Connector_Audio:AudioJack2", ref, "3.5mm TS", x, y, rot=180, fp=FP_JACK,
          conn={"T": net, "S": "GND"}, ref_at=(6.35, -3.175, "left"), val_at=(6.35, 3.81, "left"),
          hide_val=True)
    sh.wire((x - 5.08, y + 2.54), (x - 7.62, y + 2.54), (x - 7.62, y + 5.08))
    gnd(sh, x - 7.62, y + 5.08)
    sh.text(label, (x + 6.35, y + 0.635), size=1.524, bold=True)


def sheet_cv(d):
    sh = d.sheet("CV_Gate", "cv_gate.kicad_sch", "CV 1-4 and gate 1-4 outputs", "A3", 5)
    sh.heading("CV outputs: two MCP4822 (internal 2.048 V reference, gain 1x) and unity buffers", (20.32, 30.48))
    note(sh, 180.34, 33.02,
         "CV = (0, , 2.048) V, 1 V/oct, MIDI note 24 = 0 V, note 36 = 1 V, note 48 = 2 V; higher notes saturate (ARCHITECTURE.md, CV Circuit).",
         "The DACs run from +3V3, so only the default gain 1x is possible: -DREBIRTH_CV_DAC_GAIN=2 (0-4.096 V) would need the DACs on",
         "VBUS and 3.3 V -> 5 V buffers on DAC_SCK/DAC_MOSI/DAC_CSx_N. The buffers already run from VBUS and have the headroom.")
    dac_block(sh, "U501", 76.2, 63.5, "DAC_CS1_N", ("CV1_DAC", "CV2_DAC"), ("C501", "C503"))
    dac_block(sh, "U502", 76.2, 127.0, "DAC_CS2_N", ("CV3_DAC", "CV4_DAC"), ("C502", "C506"))
    Xo = 121.92
    xin = Xo - 7.62
    rows = [(1, 63.5, "CV1_DAC", 1), (2, 91.44, "CV2_DAC", 2), (3, 127.0, "CV3_DAC", 3), (4, 154.94, "CV4_DAC", 4)]
    for unit, Yo, net, n in rows:
        cv_stage(sh, unit, Xo, Yo, n, net,
                 ("C5%d" % (10 + n), "R5%d" % (10 + n), "R5%d" % (20 + n), "R5%d" % (14 + n)))
    # DAC outputs to the buffers
    sh.wire((88.9, 60.96), (xin, 60.96))
    sh.wire((88.9, 68.58), (96.52, 68.58), (96.52, 88.9), (xin, 88.9))
    sh.wire((88.9, 124.46), (xin, 124.46))
    sh.wire((88.9, 132.08), (96.52, 132.08), (96.52, 152.4), (xin, 152.4))
    sh.label("CV1_DAC", (90.17, 60.96))
    sh.label("CV2_DAC", (104.14, 88.9))
    sh.label("CV3_DAC", (90.17, 124.46))
    sh.label("CV4_DAC", (104.14, 152.4))
    note(sh, 180.34, 50.8,
         "U503 MCP6004 (rail-to-rail, VBUS): unity gain at DC. R511-R514 (1 k) sit inside",
         "the loop: R521-R524 (10 k) take the feedback from the jack side, C511-C514",
         "(100 pF) close the loop at high frequency, so the load does not shift the pitch",
         "and cable capacitance does not upset the stage. R515-R518 (100 k) hold the inputs",
         "at 0 V while the DACs are unpowered or in their power-on shutdown.",
         "The output cannot reach 0 V exactly (MCP6004 swing, ~25 mV at light load = up to",
         "30 cents sharp at note 24). DAC gain/offset errors (about +-2 % FSR) and op-amp",
         "offset need a per-channel calibration in firmware; this board has no trimmers.",
         "R511-R514 limit a short to GND to ~5 mA; external voltages are not clamped.",
         "J501-J504: rear 3.5 mm TS jacks (PJ398SM type) to a synth's pitch CV input.")

    sh.heading("Gate outputs: 3.3 V GPIO -> 5 V gates", (20.32, 181.61))
    xb = 76.2
    gate_units = [(1, "2", "3", "1"), (2, "5", "6", "4"), (3, "9", "8", "10"), (4, "12", "11", "13")]
    for i, (unit, pa, py_, poe) in enumerate(gate_units):
        n = i + 1
        y = 193.04 + 17.78 * i
        net = "GATE%d" % n
        out = "GATE%d_BUF" % n
        jack = "GATE%d_OUT" % n
        sh.hlabel(net, (40.64, y), "input", "left")
        sh.wire((40.64, y), (68.58, y))
        res(sh, "R5%d" % (30 + n), "100k", 55.88, y + 5.08, {"1": net, "2": "GND"})
        sh.wire((55.88, y), (55.88, y + 1.27))
        sh.wire((55.88, y + 8.89), (55.88, y + 11.43))
        gnd(sh, 55.88, y + 11.43)
        place(sh, "74xx:74AHCT125", "U504", "74AHCT125", xb, y, unit=unit, fp=FP_SO14,
              conn={pa: net, py_: out, poe: "GND"}, ref_at=(5.08, -3.175, "left"), val_at=(6.35, -2.54, "left"),
              hide_val=True)
        sh.wire((xb, y + 6.35), (xb, y + 8.89))
        gnd(sh, xb, y + 8.89)
        sh.wire((83.82, y), (90.17, y))
        res(sh, "R5%d" % (40 + n), "1k", 93.98, y, {"1": out, "2": jack}, horiz=True)
        jx = Xo + 35.56
        sh.wire((97.79, y), (jx - 5.08, y))
        sh.label(jack, (101.6, y))
        res(sh, "R5%d" % (50 + n), "100k", 127.0, y + 5.08, {"1": jack, "2": "GND"})
        sh.wire((127.0, y), (127.0, y + 1.27))
        sh.wire((127.0, y + 8.89), (127.0, y + 11.43))
        gnd(sh, 127.0, y + 11.43)
        jack_out(sh, "J%d" % (504 + n), "GATE %d" % n, jx, y, jack)
    note(sh, 180.34, 193.04,
         "U504 74AHCT125 on VBUS: TTL-level inputs accept the 3.3 V GPIO (VIH 2.0 V),",
         "outputs swing to VBUS, so a gate is (0, 5, 5.25) V. R531-R534 hold the",
         "inputs low until the firmware drives GPIO 14-17; R551-R554 hold the jacks low",
         "with the device unplugged. R541-R544 (1 k) limit a short to GND to ~5 mA;",
         "external voltages are not clamped.",
         "J505-J508: rear 3.5 mm TS jacks to a synth's gate / trigger input.",
         "Use one active source track per CV/gate pair (ARCHITECTURE.md).")
    # supply units
    sh.heading("Buffer supplies", (241.3, 101.6))
    place(sh, "Amplifier_Operational:MCP6004", "U503", "MCP6004", 254.0, 124.46, unit=5, fp=FP_SO14,
          conn={"4": "VBUS", "11": "GND"}, ref_at=(1.27, -2.54, "left"), val_at=(1.27, 0, "left"), hide_val=True)
    sh.wire((251.46, 116.84), (251.46, 114.3))
    rail(sh, "VBUS", 251.46, 114.3)
    sh.wire((251.46, 132.08), (251.46, 134.62))
    gnd(sh, 251.46, 134.62)
    decap(sh, "C504", "100n", 264.16, 124.46, "VBUS")
    place(sh, "74xx:74AHCT125", "U504", "74AHCT125", 287.02, 124.46, unit=5, fp=FP_SO14,
          conn={"14": "VBUS", "7": "GND"}, ref_at=(6.35, -2.54, "left"), val_at=(6.35, 0, "left"), hide_val=True)
    sh.wire((287.02, 111.76), (287.02, 109.22))
    rail(sh, "VBUS", 287.02, 109.22)
    sh.wire((287.02, 137.16), (287.02, 139.7))
    gnd(sh, 287.02, 139.7)
    decap(sh, "C505", "100n", 299.72, 124.46, "VBUS")
    decap(sh, "C507", "10u", 312.42, 124.46, "VBUS", fp=FP_C0805)
    note(sh, 241.3, 147.32,
         "C507: local bulk for the buffers on VBUS, which also feeds",
         "the LED rail; place it between U503 and U504.")
    return sh


# ------------------------------------------------------------------ sheet 6: top strip

def encoder_block(sh, ref, label, X, Y, nets, refs):
    a_net, b_net, sw_net = nets
    place(sh, "Device:RotaryEncoder_Switch", ref, "EC11 + switch", X, Y, mirror="y", fp=FP_ENC,
          conn={"A": a_net + "_RAW", "B": b_net + "_RAW", "C": "GND", "S1": sw_net, "S2": "GND"},
          ref_at=(-5.08, -7.62, "left"), val_at=(-5.08, 7.62, "left"))
    sh.text(label, (X - 5.08, Y + 11.43), size=1.524, bold=True)
    # common to ground
    sh.wire((X + 7.62, Y), (X + 15.24, Y))
    gnd(sh, X + 15.24, Y)
    # switch: S2 to ground, S1 up and over to its flag
    sh.wire((X - 7.62, Y + 2.54), (X - 10.16, Y + 2.54))
    gnd(sh, X - 10.16, Y + 2.54)
    x1, x2 = X + 20.32, X + 39.37
    xl = X + 49.53
    sh.wire((X - 7.62, Y - 2.54), (X - 10.16, Y - 2.54), (X - 10.16, Y - 27.94), (xl, Y - 27.94))
    sh.hlabel(sw_net, (xl, Y - 27.94), "output", "right")
    rows = [(a_net, "A", Y - 2.54, Y - 10.16), (b_net, "B", Y + 2.54, Y + 10.16)]
    for k, (net, pin, ypin, yr) in enumerate(rows):
        raw = net + "_RAW"
        sh.wire((X + 7.62, ypin), (X + 10.16, ypin), (X + 10.16, yr), (x1 + 6.35, yr))
        pull_up(sh, refs[k][0], "10k", x1, yr, "+3V3", raw)
        res(sh, refs[k][1], "10k", x1 + 10.16, yr, {"1": raw, "2": net}, horiz=True)
        sh.wire((x1 + 13.97, yr), (xl, yr))
        res_c = refs[k][2]
        cap(sh, res_c, "10n", x2, yr + 6.35, {"1": net, "2": "GND"})
        sh.wire((x2, yr), (x2, yr + 2.54))
        sh.wire((x2, yr + 10.16), (x2, yr + 12.7))
        gnd(sh, x2, yr + 12.7)
        sh.hlabel(net, (xl, yr), "output", "right")


def sheet_top(d):
    sh = d.sheet("Top_strip", "top_strip.kicad_sch", "Top strip: OLED, TRACK/VALUE encoders, PLAY", "A4", 6)
    sh.heading("OLED: SSD1309 128x64 (2.42 in), 4-wire SPI0", (20.32, 33.02))
    X, Y = 114.3, 60.96
    place(sh, "Connector_Generic:Conn_01x07", "J601", "OLED module", X, Y, mirror="x", fp=FP_HDR7,
          conn={"1": "GND", "2": "+3V3", "3": "OLED_SCK", "4": "OLED_MOSI", "5": "OLED_RST_N",
                "6": "OLED_DC", "7": "OLED_CS_N"},
          ref_at=(-1.27, -10.16, "left"), val_at=(-1.27, 11.43, "left"))
    pins = {"7": ("OLED_CS_N",), "6": ("OLED_DC",), "5": ("OLED_RST_N",), "4": ("OLED_MOSI",), "3": ("OLED_SCK",)}
    for pin, (net,) in pins.items():
        px, py = sh.pin("J601", pin)
        sh.wire((px, py), (px - 5.08, py))
        sh.hlabel(net, (px - 5.08, py), "input", "left")
    vx, vy = sh.pin("J601", "2")
    sh.wire((vx, vy), (71.12, vy))
    rail(sh, "+3V3", 71.12, vy)
    for ref, val, x, fp in (("C601", "10u", 81.28, FP_C0805), ("C602", "100n", 91.44, FP_C)):
        sh.wire((x, vy), (x, vy + 2.54))
        cap(sh, ref, val, x, vy + 6.35, {"1": "+3V3", "2": "GND"}, fp=fp)
        sh.wire((x, vy + 10.16), (x, vy + 12.7))
        gnd(sh, x, vy + 12.7)
    gx, gy = sh.pin("J601", "1")
    sh.wire((gx, gy), (gx - 2.54, gy), (gx - 2.54, gy + 2.54))
    gnd(sh, gx - 2.54, gy + 2.54)
    note(sh, 124.46, 52.07,
         "J601: 7-pin header to the OLED module",
         "(pin order GND VCC SCK SDA RES DC CS as",
         "printed on the common 2.42 in modules;",
         "check the module before wiring).",
         "The module makes its panel voltage itself;",
         "~25 mA from +3V3.")

    sh.heading("PLAY", (190.5, 33.02))
    place(sh, "Switch:SW_Push", "SW603", "PLAY (MX)", 205.74, 50.8, fp=FP_MX, conn={"1": "GND", "2": "PLAY_N"},
          ref_at=(0, -5.08, "center"), val_at=(0, 2.54, "center"))
    sh.wire((200.66, 50.8), (198.12, 50.8))
    gnd(sh, 198.12, 50.8)
    sh.wire((210.82, 50.8), (226.06, 50.8))
    sh.hlabel("PLAY_N", (226.06, 50.8), "output", "right")
    note(sh, 190.5, 63.5,
         "Pressed = low. GPIO 28 uses its internal pull-up,",
         "20 ms firmware debounce. Its RGB light is D981",
         "in the chain (sheet 9), BEAT is D982.")

    sh.heading("Encoders: TRACK (enc 1) and VALUE (enc 2)", (20.32, 96.52))
    note(sh, 20.32, 102.87,
         "A/B contacts close to GND. R pull-up 10 k + R 10 k + C 10 nF per contact: the encoder maker's filter (tau 0.1 ms).",
         "The GPIOs also enable their internal pull-ups. Push switch = low when pressed, firmware debounce 20 ms.")
    encoder_block(sh, "SW601", "TRACK", 35.56, 137.16, ("ENC1_A", "ENC1_B", "ENC1_SW_N"),
                  (("R601", "R602", "C603"), ("R603", "R604", "C604")))
    encoder_block(sh, "SW602", "VALUE", 139.7, 137.16, ("ENC2_A", "ENC2_B", "ENC2_SW_N"),
                  (("R605", "R606", "C605"), ("R607", "R608", "C606")))
    return sh


# ------------------------------------------------------------------ sheet 7: step row

def mcp_block(sh, ref, X, Y, addr_a0_high, kind, refs, skip=()):
    """MCP23017 with its 16 pins fanned out to 7.62 mm rows. skip: (port, bit) pins left open."""
    conn = {"9": "+3V3", "10": "GND", "12": "I2C_SCL", "13": "I2C_SDA", "18": ref + "_RESET_N",
            "15": "+3V3" if addr_a0_high else "GND", "16": "GND", "17": "GND", "19": None, "20": None,
            "11": None, "14": None}
    for k in range(8):
        conn[str(21 + k)] = None if ("A", k) in skip else "%s_GPA%d" % (ref, k)
        conn[str(1 + k)] = None if ("B", k) in skip else "%s_GPB%d" % (ref, k)
    place(sh, "Interface_Expansion:MCP23017x-x-SO", ref, "MCP23017", X, Y, fp=FP_SO28W, conn=conn,
          ref_at=(2.54, -26.67, "left"), val_at=(2.54, 26.67, "left"))
    # bus
    for pin, net, shape in (("12", "I2C_SCL", "input"), ("13", "I2C_SDA", "bidirectional")):
        px, py = sh.pin(ref, pin)
        sh.wire((px, py), (X - 27.94, py))
        sh.hlabel(net, (X - 27.94, py), shape, "left")
    for pin in ("19", "20"):
        sh.nc(sh.pin(ref, pin))
    # reset pull-up
    rx, ry = sh.pin(ref, "18")
    sh.wire((rx, ry), (X - 17.78, ry), (X - 17.78, ry - 2.54))
    res(sh, refs["reset"], "10k", X - 17.78, ry - 6.35, {"1": "+3V3", "2": ref + "_RESET_N"}, fields=LEFT)
    sh.wire((X - 17.78, ry - 10.16), (X - 17.78, ry - 12.7))
    rail(sh, "+3V3", X - 17.78, ry - 12.7)
    # address pins
    a0 = sh.pin(ref, "15")
    a1 = sh.pin(ref, "16")
    a2 = sh.pin(ref, "17")
    if addr_a0_high:
        sh.wire(a0, (X - 20.32, a0[1]), (X - 20.32, a0[1] - 2.54))
        rail(sh, "+3V3", X - 20.32, a0[1] - 2.54)
        sh.wire(a1, (X - 15.24, a1[1]), (X - 15.24, a2[1] + 2.54))
        sh.wire(a2, (X - 15.24, a2[1]))
    else:
        sh.wire(a0, (X - 15.24, a0[1]), (X - 15.24, a2[1] + 2.54))
        sh.wire(a1, (X - 15.24, a1[1]))
        sh.wire(a2, (X - 15.24, a2[1]))
    gnd(sh, X - 15.24, a2[1] + 2.54)
    # supply
    sh.wire((X, Y - 25.4), (X, Y - 27.94))
    rail(sh, "+3V3", X, Y - 27.94)
    sh.wire((X, Y + 25.4), (X, Y + 27.94))
    gnd(sh, X, Y + 27.94)
    decap(sh, refs["cap"], "100n", X - 22.86, Y - 33.02, "+3V3")
    # fan-out to 7.62 mm rows
    xp = X + 12.7
    xs = xp + 33.02
    lines = []
    for k in range(8):
        lines.append(("21", k, "A"))
    for k in range(8):
        lines.append(("1", k, "B"))
    for idx, (base, k, port) in enumerate(lines):
        pin = str(int(base) + k)
        px, py = sh.pin(ref, pin)
        target = Y - 57.15 + 7.62 * idx
        if port == "A":
            xj = xp + 2.54 * (k + 1)
        else:
            xj = xp + 2.54 * (8 - k)
        net = "%s_GP%s%d" % (ref, port, k)
        step = idx + 1
        if (port, k) in skip:
            sh.nc((px, py))
            sh.text("STEP %d: SW%d on U703 (0x24), below" % (step, 700 + step), (xs, target + 0.635))
            continue
        sh.wire((px, py), (xj, py), (xj, target), (xs, target))
        if kind == "buttons":
            sw = "SW%d" % (700 + step)
            place(sh, "Switch:SW_Push", sw, "STEP %d" % step, xs + 5.08, target, fp=FP_TACT,
                  conn={"1": net, "2": "GND"}, ref_at=(0, -4.445, "center"), val_at=(0, 2.54, "center"),
                  hide_val=True)
            sh.wire((xs + 10.16, target), (xs + 12.7, target))
            gnd(sh, xs + 12.7, target)
            sh.text("STEP %d" % step, (xs + 16.51, target + 0.635))
        else:
            r = "R%d" % (700 + step)
            dd = "D%d" % (700 + step)
            res(sh, r, "470", xs + 3.81, target, {"1": net, "2": net + "_A"}, horiz=True,
                fields=((0, -2.159, "center"), (0, 2.159, "center")))
            sh.wire((xs + 7.62, target), (xs + 10.16, target))
            place(sh, "Device:LED", dd, "RED", xs + 13.97, target, rot=180, fp=FP_LED3,
                  conn={"2": net + "_A", "1": "GND"}, ref_at=(0, 2.54, "center"), val_at=(0, 4.445, "center"),
                  hide_val=True)
            sh.wire((xs + 17.78, target), (xs + 20.32, target))
            gnd(sh, xs + 20.32, target)
            sh.text("STEP %d" % step, (xs + 24.13, target + 0.635))


def extra_buttons_block(sh, X, Y):
    """U703 at 0x24: steps 8 and 16, kept off the output-only GPA7/GPB7 of U701."""
    ref = "U703"
    conn = {"9": "+3V3", "10": "GND", "12": "I2C_SCL", "13": "I2C_SDA", "18": "U703_RESET_N",
            "15": "GND", "16": "GND", "17": "+3V3", "19": None, "20": None, "11": None, "14": None,
            "21": "U703_GPA0", "22": "U703_GPA1"}
    unused = [str(p) for p in list(range(23, 29)) + list(range(1, 9))]
    for p in unused:
        conn[p] = None
    place(sh, "Interface_Expansion:MCP23017x-x-SO", ref, "MCP23017", X, Y, fp=FP_SO28W, conn=conn,
          ref_at=(2.54, -26.67, "left"), val_at=(2.54, 26.67, "left"))
    for pin, net, shape in (("12", "I2C_SCL", "input"), ("13", "I2C_SDA", "bidirectional")):
        px, py = sh.pin(ref, pin)
        sh.wire((px, py), (X - 27.94, py))
        sh.hlabel(net, (X - 27.94, py), shape, "left")
    for pin in ["19", "20"] + unused:
        sh.nc(sh.pin(ref, pin))
    rx, ry = sh.pin(ref, "18")
    sh.wire((rx, ry), (X - 17.78, ry), (X - 17.78, ry - 2.54))
    res(sh, "R719", "10k", X - 17.78, ry - 6.35, {"1": "+3V3", "2": "U703_RESET_N"}, fields=LEFT)
    sh.wire((X - 17.78, ry - 10.16), (X - 17.78, ry - 12.7))
    rail(sh, "+3V3", X - 17.78, ry - 12.7)
    # address 0x24: A2 = 1, A1 = A0 = 0
    a0 = sh.pin(ref, "15")
    a1 = sh.pin(ref, "16")
    a2 = sh.pin(ref, "17")
    sh.wire(a0, (X - 15.24, a0[1]), (X - 15.24, a1[1]))
    sh.wire(a1, (X - 15.24, a1[1]))
    sh.wire((X - 15.24, a0[1]), (X - 30.48, a0[1]), (X - 30.48, a2[1] + 5.08))
    gnd(sh, X - 30.48, a2[1] + 5.08)
    sh.wire(a2, (X - 20.32, a2[1]))
    rail(sh, "+3V3", X - 20.32, a2[1])
    sh.wire((X, Y - 25.4), (X, Y - 27.94))
    rail(sh, "+3V3", X, Y - 27.94)
    sh.wire((X, Y + 25.4), (X, Y + 27.94))
    gnd(sh, X, Y + 27.94)
    decap(sh, "C703", "100n", X - 22.86, Y - 33.02, "+3V3")
    xp = X + 12.7
    xs = xp + 33.02
    p0 = sh.pin(ref, "21")
    p1 = sh.pin(ref, "22")
    rows = [(8, "U703_GPA0", [p0, (xs, p0[1])], p0[1]),
            (16, "U703_GPA1", [p1, (xp + 2.54, p1[1]), (xp + 2.54, p1[1] + 5.08), (xs, p1[1] + 5.08)],
             p1[1] + 5.08)]
    for step, net, path, target in rows:
        sh.wire(*path)
        sw = "SW%d" % (700 + step)
        place(sh, "Switch:SW_Push", sw, "STEP %d" % step, xs + 5.08, target, fp=FP_TACT,
              conn={"1": net, "2": "GND"}, ref_at=(0, -4.445, "center"), val_at=(0, 2.54, "center"),
              hide_val=True)
        sh.wire((xs + 10.16, target), (xs + 12.7, target))
        gnd(sh, xs + 12.7, target)
        sh.text("STEP %d" % step, (xs + 16.51, target + 0.635))


def sheet_steps(d):
    sh = d.sheet("Step_row", "step_row.kicad_sch",
                 "Step row: 16 buttons and 16 LEDs on MCP23017", "A3", 7)
    sh.heading("Step buttons: MCP23017 at 0x20 (A2 A1 A0 = 000)", (20.32, 30.48))
    sh.heading("Step LEDs: MCP23017 at 0x21 (A0 = 1)", (215.9, 30.48))
    mcp_block(sh, "U701", 76.2, 106.68, False, "buttons", {"reset": "R717", "cap": "C701"},
              skip={("A", 7), ("B", 7)})
    mcp_block(sh, "U702", 271.78, 106.68, True, "leds", {"reset": "R718", "cap": "C702"})
    sh.heading("Steps 8 and 16: MCP23017 at 0x24 (A2 = 1)", (20.32, 187.96))
    extra_buttons_block(sh, 76.2, 236.22)
    note(sh, 215.9, 180.34,
         "Buttons: SW701-SW716, 6x6 mm tactile, pressed = low, internal pull-ups, inverted polarity.",
         "U701 (0x20): GPA0-6 = steps 1-7, GPB0-6 = steps 9-15 (bit n = STEP n+1, as before).",
         "U703 (0x24): GPA0 = step 8, GPA1 = step 16; its other pins are unused.",
         "GPA7/GPB7 of U701 and U703 are output-only in the current MCP23017 datasheet",
         "(DS20001952 rev. D+; a level change during a read can corrupt SDA): left open,",
         "the firmware must configure them as outputs. INTA/INTB unused (polled).",
         "",
         "U702 (0x21): all 16 pins outputs, bit n high = STEP n+1 lit (GPA0-7 = 1-8,",
         "GPB0-7 = 9-16); GPA7/GPB7 as outputs are fine. D701-D716: red 3 mm LEDs, 470 R,",
         "about 1.5-3 mA each from the expander's 3.3 V outputs.")
    return sh


# ------------------------------------------------------------------ sheet 8: key matrix

def sheet_keys(d):
    sh = d.sheet("Key_matrix", "key_matrix.kicad_sch", "Key matrix: 64 pads, 8 SCENE, 8 MUTE on a TCA8418", "A3", 8)
    sh.heading("Key matrix: TCA8418 (0x34) scans 8 rows x 10 columns = 80 keys", (20.32, 27.94))
    Xi, Yi = 66.04, 215.9
    conn = {"20": "TCA_RESET_N", "24": "TCA_INT_N", "23": "I2C_SCL", "22": "I2C_SDA", "21": "+3V3", "19": "GND",
            "25": "GND"}
    for r in range(8):
        conn[str(8 - r)] = "ROW%d" % r
    for c in range(10):
        conn[str(9 + c)] = "COL%d" % c
    place(sh, "rebirth:TCA8418RTW", "U801", "TCA8418RTW", Xi, Yi, conn=conn,
          fp="Package_DFN_QFN:QFN-24-1EP_4x4mm_P0.5mm_EP2.5x2.5mm",
          ref_at=(-10.16, -29.21, "left"), val_at=(2.54, -29.21, "left"))
    # left side
    for pin, net, shape in (("23", "I2C_SCL", "input"), ("22", "I2C_SDA", "bidirectional")):
        px, py = sh.pin("U801", pin)
        sh.wire((px, py), (px - 10.16, py))
        sh.hlabel(net, (px - 10.16, py), shape, "left")
    ix, iy = sh.pin("U801", "24")
    sh.wire((ix, iy), (35.56, iy))
    pull_up(sh, "R802", "10k", 35.56, iy, "+3V3", "TCA_INT_N", fields=LEFT)
    rx, ry = sh.pin("U801", "20")
    sh.wire((rx, ry), (rx - 5.08, ry), (rx - 5.08, ry - 3.81))
    res(sh, "R801", "10k", rx - 5.08, ry - 7.62, {"1": "+3V3", "2": "TCA_RESET_N"}, fields=LEFT)
    sh.wire((rx - 5.08, ry - 11.43), (rx - 5.08, ry - 13.97))
    rail(sh, "+3V3", rx - 5.08, ry - 13.97)
    vx, vy = sh.pin("U801", "21")
    sh.wire((vx, vy), (vx, vy - 2.54))
    rail(sh, "+3V3", vx, vy - 2.54)
    decap(sh, "C801", "100n", 76.2, 172.72, "+3V3")
    decap(sh, "C802", "1u", 22.86, 172.72, "+3V3")
    for pin in ("19", "25"):
        px, py = sh.pin("U801", pin)
        sh.wire((px, py), (px, py + 2.54))
        gnd(sh, px, py + 2.54)
    # matrix geometry
    X0, Y0, PX, PY = 114.3, 50.8, 27.94, 20.32
    xr = [Xi + 15.24 + 2.54 * (r + 1) for r in range(8)]
    X = [X0 + PX * c for c in range(10)]
    col_x = [x + 15.24 for x in X]
    for r in range(8):
        px, py = sh.pin("U801", str(8 - r))
        yr = Y0 + PY * r
        sh.wire((px, py), (xr[r], py), (xr[r], yr), (X[9], yr))
        sh.text("SLOT %d" % (r + 1), (xr[r] + 2.54, yr - 0.635))
    for c in range(10):
        px, py = sh.pin("U801", str(9 + c))
        sh.wire((px, py), (col_x[c], py), (col_x[c], Y0 + 15.24))
        head = "TRACK %d" % (c + 1) if c < 8 else ("SCENE" if c == 8 else "MUTE")
        sh.text(head, (X[c] + 1.27, Y0 - 6.35), size=1.524, bold=True)
        sh.text("COL%d" % c, (X[c] + 1.27, Y0 - 3.81))
    for r in range(8):
        yr = Y0 + PY * r
        for c in range(10):
            k = r * 10 + c + 1
            x = X[c]
            sw, dd = "SW8%02d" % k, "D8%02d" % k
            sh.wire((x, yr), (x, yr + 2.54))
            place(sh, "Switch:SW_Push", sw, "MX", x, yr + 7.62, rot=90, fp=FP_MX,
                  conn={"2": "ROW%d" % r, "1": "K%02d" % k}, ref_at=(-7.62, 0, "center"),
                  val_at=(-7.62, 2.54, "center"), hide_val=True)
            sh.wire((x, yr + 12.7), (x, yr + 15.24), (x + 5.08, yr + 15.24))
            place(sh, "Diode:1N4148W", dd, "1N4148W", x + 8.89, yr + 15.24, rot=180, fp=FP_D,
                  conn={"2": "K%02d" % k, "1": "COL%d" % c}, ref_at=(0, -3.302, "center"),
                  val_at=(0, 3.302, "center"), hide_val=True)
            sh.wire((x + 12.7, yr + 15.24), (col_x[c], yr + 15.24))
    note(sh, 111.76, 243.84,
         "Scan (TCA8418 datasheet 8.4.2): columns are driven low one at a time, rows are inputs with pull-ups. Each key is",
         "row -> switch -> diode anode -> cathode -> column, so a held key can never feed a row back through another column: no ghost presses.",
         "Key event code k = 10 x row + column + 1 (1..80) = the last two digits of SW8kk and D8kk. Firmware table (row, col):",
         "col 0-7 = pad (track col+1, slot row+1), col 8 = SCENE row+1, col 9 = MUTE track row+1. Chip debounce off, firmware 5-10 ms.",
         "Latency: the datasheet (8.6.2.15) scans the keypad every 25 ms and reports a press after 25-40 ms, with debounce disabled",
         "too; the design asks for <= 15 ms. Open point, see README. Keys: MX-type switches (assumed). Diodes: 1N4148W.",
         "INT is not read by the firmware (polled); R802 pulls the open-drain output up as the datasheet asks.")
    return sh


# ------------------------------------------------------------------ sheet 9: RGB chain

GROUPS = [  # (first chain index, count, title)
    (0, 8, "GRID SLOT 1, tracks 1-8"), (8, 8, "GRID SLOT 2, tracks 1-8"),
    (16, 8, "GRID SLOT 3"), (24, 8, "GRID SLOT 4"), (32, 8, "GRID SLOT 5"), (40, 8, "GRID SLOT 6"),
    (48, 8, "GRID SLOT 7"), (56, 8, "GRID SLOT 8"), (64, 8, "SCENE 1-8"), (72, 8, "MUTE tracks 1-8"),
    (80, 1, "PLAY"), (81, 1, "BEAT"),
]


def sheet_rgb(d):
    sh = d.sheet("RGB_chain", "rgb_chain.kicad_sch", "RGB light chain: 82 x SK6812MINI-E", "A2", 9)
    sh.heading("RGB lights: 82 x SK6812MINI-E in one chain, data from GPIO 27 (PIO), 5 V from +5V_LED", (20.32, 30.48))
    note(sh, 20.32, 36.83,
         "Chain order = firmware logical index + 1: D9nn is LED_PAD/SCENE/MUTE index nn-1 (LED_PAD(track,row) = row*8 + track,",
         "LED_SCENE(row) = 64 + row, LED_MUTE(track) = 72 + track, LED_PLAY = 80, LED_BEAT = 81), so chain_order[] stays the identity.",
         "Every D9nn has its own C9nn 100 nF X7R 0603 at its VDD pin. Route +5V_LED and GND as wide traces (a bus) along each row, not through",
         "the LEDs. SK6812MINI-E: about 1 mA idle, 12 mA per channel nominal (20 mA until measured); firmware caps the chain at 200 mA.")
    Y1 = 76.2
    # driver
    sh.hlabel("LED_DATA", (30.48, Y1), "input", "left")
    sh.wire((30.48, Y1), (48.26, Y1))
    pull_down(sh, "R901", "10k", 40.64, Y1, "LED_DATA")
    X = 63.5
    place(sh, "74xGxx:74AHCT1G125", "U901", "74AHCT1G125", X, Y1, fp=FP_SOT235,
          conn={"2": "LED_DATA", "4": "LED_DIN_5V", "1": "GND", "5": "+5V_LED", "3": "GND"},
          ref_at=(-2.54, 7.62, "left"), val_at=(-2.54, 10.16, "left"))
    sh.wire((X, Y1 - 10.16), (X, Y1 - 12.7), (X + 10.16, Y1 - 12.7), (X + 10.16, Y1 - 7.62))
    gnd(sh, X + 10.16, Y1 - 7.62)
    sh.wire((X - 5.08, Y1 - 10.16), (X - 5.08, Y1 - 15.24))
    rail(sh, "+5V_LED", X - 5.08, Y1 - 15.24)
    sh.wire((X - 5.08, Y1 + 10.16), (X - 5.08, Y1 + 12.7))
    gnd(sh, X - 5.08, Y1 + 12.7)
    decap(sh, "C983", "100n", 45.72, Y1 - 19.05, "+5V_LED")
    sh.wire((X + 12.7, Y1), (X + 16.51, Y1))
    res(sh, "R902", "390", X + 20.32, Y1, {"1": "LED_DIN_5V", "2": "CHAIN_IN"}, horiz=True)
    x0 = 104.14
    sh.wire((X + 24.13, Y1), (x0 - 7.62, Y1))
    pull_down(sh, "R903", "10k", 88.9, Y1, "CHAIN_IN", fields=LEFT)
    decap(sh, "C984", "470u 10V", 81.28, Y1 + 25.4, "+5V_LED", fp="Capacitor_SMD:CP_Elec_8x10", polar=True)
    note(sh, 20.32, Y1 + 38.1,
         "U901 74AHCT1G125 on +5V_LED: TTL input takes the",
         "3.3 V GPIO, output swings to the LEDs' own 5 V",
         "(SK6812 needs VIH 0.7 x VDD = 3.5 V). OE tied low.",
         "R901 holds the buffer input low until the PIO runs,",
         "R903 holds the first DIN low while U901 is still",
         "unpowered. R902 390 R at the first LED damps the line.",
         "C984 470 uF 10 V low-ESR: bulk at the chain input",
         "for the PWM current peaks; final value from a scope",
         "check of +5V_LED (keep >= 4.5 V for U901).")
    # LED rows
    per_row = 16
    pitch = 27.94
    rowp = 50.8
    for i in range(82):
        j, k = divmod(i, per_row)
        Y = Y1 + rowp * j
        x = x0 + pitch * k + (5.08 if k >= 8 else 0)
        n = i + 1
        led, cref = "D9%02d" % n, "C9%02d" % n
        din = "CHAIN_IN" if i == 0 else "CHAIN_%02d" % i
        dout = "CHAIN_%02d" % n if i < 81 else None
        place(sh, "LED:SK6812MINI-E", led, "SK6812MINI-E", x, Y, fp=FP_RGB,
              conn={"2": din, "4": dout, "3": "+5V_LED", "1": "GND"},
              ref_at=(2.54, -8.89, "left"), val_at=(2.54, 6.35, "left"), hide_val=True)
        sh.wire((x, Y - 7.62), (x, Y - 10.16))
        rail(sh, "+5V_LED", x, Y - 10.16)
        sh.wire((x, Y + 7.62), (x, Y + 10.16))
        gnd(sh, x, Y + 10.16)
        xc = x + 12.7
        rail(sh, "+5V_LED", xc, Y + 7.62)
        sh.wire((xc, Y + 7.62), (xc, Y + 10.16))
        cap(sh, cref, "100n", xc, Y + 13.97, {"1": "+5V_LED", "2": "GND"},
            fields=((2.54, 0, "left"), (2.54, 2.54, "left")), hide_val=True)
        sh.wire((xc, Y + 17.78), (xc, Y + 20.32))
        gnd(sh, xc, Y + 20.32)
        if i == 81:
            sh.nc((x + 7.62, Y))
            sh.text("end of chain", (x + 10.16, Y - 1.27))
            continue
        if k == per_row - 1:
            # return wire to the start of the next row
            xr = x + 12.7 + 10.16
            sh.wire((x + 7.62, Y), (xr, Y), (xr, Y + 30.48), (x0 - 12.7, Y + 30.48), (x0 - 12.7, Y + rowp),
                    (x0 - 7.62, Y + rowp))
        else:
            nx = x0 + pitch * (k + 1) + (5.08 if k + 1 >= 8 else 0)
            sh.wire((x + 7.62, Y), (nx - 7.62, Y))
    for first, count, title in GROUPS:
        j, k = divmod(first, per_row)
        Y = Y1 + rowp * j
        x = x0 + pitch * k + (5.08 if k >= 8 else 0)
        last = first + count
        span = "D9%02d" % (first + 1) if count == 1 else "D9%02d-D9%02d" % (first + 1, last)
        sh.text("%s: %s" % (title, span), (x - 7.62, Y - 17.78), size=1.524, bold=True)
    return sh


# ------------------------------------------------------------------ sheet 1: root

ROOT_GROUPS = [
    ("MIDI", [("MIDI_TX", "output", "input"), ("MIDI_RX", "input", "output")]),
    ("CV_Gate", [("DAC_SCK", "output", "input"), ("DAC_MOSI", "output", "input"),
                 ("DAC_CS1_N", "output", "input"), ("DAC_CS2_N", "output", "input"),
                 ("GATE1", "output", "input"), ("GATE2", "output", "input"),
                 ("GATE3", "output", "input"), ("GATE4", "output", "input")]),
    ("Top_strip", [("OLED_SCK", "output", "input"), ("OLED_MOSI", "output", "input"),
                   ("OLED_CS_N", "output", "input"), ("OLED_DC", "output", "input"),
                   ("OLED_RST_N", "output", "input"), ("ENC1_A", "input", "output"),
                   ("ENC1_B", "input", "output"), ("ENC1_SW_N", "input", "output"),
                   ("ENC2_A", "input", "output"), ("ENC2_B", "input", "output"),
                   ("ENC2_SW_N", "input", "output"), ("PLAY_N", "input", "output")]),
    ("RGB_chain", [("LED_DATA", "output", "input")]),
    ("Step_row", [("I2C_SDA", "bidirectional", "bidirectional"), ("I2C_SCL", "output", "input")]),
]


def root_sheet(d, sheets):
    root = d.root
    by = {s.name: s for s in sheets}
    xm, wm = 60.96, 50.8
    xd, wd = 175.26, 55.88
    y = 48.26
    mcu_pins = []
    boxes = []
    for name, pins in ROOT_GROUPS:
        top = y
        dpins = []
        for net, mshape, dshape in pins:
            mcu_pins.append((net, mshape, (xm + wm, y), "right"))
            dpins.append((net, dshape, (xd, y), "left"))
            root.wire((xm + wm, y), (xd, y))
            y += 2.54
        boxes.append((name, top - 5.08, y - top + 7.62, dpins))
        y += 17.78
    # key matrix shares the I2C pins of the step row
    sda_y = [p[2][1] for p in mcu_pins if p[0] == "I2C_SDA"][0]
    scl_y = sda_y + 2.54
    ky = scl_y + 25.4
    kpins = [("I2C_SDA", "bidirectional", (xd, ky), "left"), ("I2C_SCL", "input", (xd, ky + 2.54), "left")]
    root.wire((152.4, sda_y), (152.4, ky), (xd, ky))
    root.wire((157.48, scl_y), (157.48, ky + 2.54), (xd, ky + 2.54))
    boxes.append(("Key_matrix", ky - 5.08, 15.24, kpins))
    top_m = 40.64
    d.add_sheet_symbol(by["MCU"], (xm, top_m), (wm, y - top_m - 12.7), mcu_pins)
    for name, top, h, dpins in boxes:
        d.add_sheet_symbol(by[name], (xd, top), (wd, h), dpins)
    d.add_sheet_symbol(by["Power"], (xm, 215.9), (wm, 15.24), [])
    root.heading("ReBirth RP2040 sequencer with the session-style live panel", (20.32, 22.86))
    note(root, 20.32, 30.48,
         "Hierarchy: sheet boxes are drawn in signal order; the Pico (sheet 3) drives every other sheet. Rails VBUS, +3V3, +5V_LED and GND",
         "are global power symbols (sheet 2). Design: docs/superpowers/specs/2026-11-16-session-panel-design.md; parts and open points: hardware/schematic/README.md.")
    note(root, 20.32, 240.03,
         "Power (sheet 2): USB VBUS from the laptop, +3V3 from the Pico,",
         "+5V_LED for the RGB chain through a 234-303 mA switch.",
         "Whole device within 500 mA at 5 V.")
    note(root, 254.0, 50.8,
         "GPIO map (all 26 header GPIOs used)",
         "0  MIDI_TX  (UART0)        14-17  GATE1-4",
         "1  MIDI_RX  (reserved)     18 19 20  ENC1 A B SW (TRACK)",
         "2  OLED_SCK (SPI0)         21 22 26  ENC2 A B SW (VALUE)",
         "3  OLED_MOSI               27  LED_DATA (PIO, SK6812 chain)",
         "4  OLED_CS_N               28  PLAY_N",
         "5  OLED_DC",
         "6  OLED_RST_N              I2C0 400 kHz, external pull-ups:",
         "7  JITTER_PROBE (TP301)      0x20  MCP23017 step buttons 1-7, 9-15",
         "8  I2C_SDA                   0x21  MCP23017 step LEDs",
         "9  I2C_SCL                   0x24  MCP23017 step buttons 8, 16",
         "10 DAC_SCK (SPI1)            0x34  TCA8418 key matrix",
         "11 DAC_MOSI",
         "12 DAC_CS1_N (U501: CV1 CV2)",
         "13 DAC_CS2_N (U502: CV3 CV4)")
    note(root, 254.0, 104.14,
         "Reference designators: the hundreds digit is the sheet",
         "(U2xx power, U3xx Pico, ... D9nn RGB LED number nn in the chain).",
         "Key matrix: SW8kk/D8kk = TCA8418 key event code kk.")


# ------------------------------------------------------------------ project files

def write_project(d):
    pro = {
        "meta": {"filename": sch.PROJECT + ".kicad_pro", "version": 3},
        "schematic": {
            "drawing": {"intersheets_ref_show": True, "intersheets_ref_short": False,
                        "intersheets_ref_own_page": False, "intersheets_ref_prefix": "",
                        "intersheets_ref_suffix": ""},
            "legacy_lib_dir": "", "legacy_lib_list": [],
        },
        "sheets": [[d.root.uuid, "Root"]] + [[s.sheet_uuid, s.name] for s in d.sheets],
        "text_variables": {},
    }
    with open(os.path.join(OUT, sch.PROJECT + ".kicad_pro"), "w") as f:
        json.dump(pro, f, indent=2)
        f.write("\n")
    with open(os.path.join(OUT, "sym-lib-table"), "w") as f:
        f.write('(sym_lib_table\n  (version 7)\n  (lib (name "rebirth") (type "KiCad") (uri "${KIPRJMOD}/rebirth.kicad_sym") '
                '(options "") (descr "ReBirth project symbols: TCA8418RTW, TPS2553DBV, +5V_LED"))\n)\n')


def build():
    project_lib = os.path.join(OUT, "rebirth.kicad_sym")
    d = Design(OUT, project_lib, DATE, REV)
    sch.write_symbol_lib(project_lib, project_symbols(d.lib))
    d.lib = sch.Library(project_lib)
    d.sheet("Root", sch.PROJECT + ".kicad_sch", "ReBirth RP2040 sequencer with session-style live panel", "A3", 1)
    sheets = [sheet_power(d), sheet_mcu(d), sheet_midi(d), sheet_cv(d), sheet_top(d), sheet_steps(d),
              sheet_keys(d), sheet_rgb(d)]
    root_sheet(d, sheets)
    write_project(d)
    d.write()
    return d


# ------------------------------------------------------------------ checks

def parse_netlist(path):
    tree = sch.parse(open(path, encoding="utf-8").read())[0]
    nets = {}
    for net in sch.find(tree, "nets")[1:]:
        name = str(sch.find(net, "name")[1])
        nodes = {(str(sch.find(n, "ref")[1]), str(sch.find(n, "pin")[1])) for n in sch.find_all(net, "node")}
        nets[name] = nodes
    return nets


def check(d):
    tmp = os.path.join(OUT, ".check")
    os.makedirs(tmp, exist_ok=True)
    root = os.path.join(OUT, sch.PROJECT + ".kicad_sch")
    net_path = os.path.join(tmp, "netlist.net")
    subprocess.run([KICAD_CLI, "sch", "export", "netlist", "--format", "kicadsexpr", "-o", net_path, root],
                   check=True, capture_output=True)
    got = parse_netlist(net_path)
    want = d.declared_nets()
    pin_got = {}
    for name, nodes in got.items():
        for n in nodes:
            pin_got[n] = name
    errors = []
    for name, nodes in sorted(want.items()):
        names = {pin_got.get(n) for n in nodes}
        if None in names:
            errors.append("%s: pins missing from export: %s" % (name, sorted(n for n in nodes if n not in pin_got)))
            continue
        if len(names) != 1:
            groups = {}
            for n in nodes:
                groups.setdefault(pin_got[n], []).append(n)
            errors.append("%s: split over %s" % (name, {k: sorted(v) for k, v in groups.items()}))
            continue
        exported = got[names.pop()]
        extra = exported - nodes
        if extra:
            errors.append("%s: also joined to %s" % (name, sorted(extra)))
    declared_pins = {n for nodes in want.values() for n in nodes}
    nc_pins = {(p.ref, pin) for s in [d.root] + d.sheets for p in s.parts for pin, net in p.conn.items() if net is None}
    for name, nodes in got.items():
        for n in nodes:
            if n not in declared_pins and n not in nc_pins:
                errors.append("undeclared pin %s on net %s" % (n, name))
            if n in nc_pins and len(nodes) > 1:
                errors.append("pin %s declared unconnected but on net %s with %s" % (n, name, sorted(nodes)))
    # every pin of every placed unit must be declared
    for s in [d.root] + d.sheets:
        for p in s.parts:
            for pin in p.sym.unit_pins(p.unit):
                if pin.number not in p.conn and pin.etype != "no_connect":
                    errors.append("%s pin %s (%s) has no declared net" % (p.ref, pin.number, pin.name))
    erc_path = os.path.join(tmp, "erc.json")
    subprocess.run([KICAD_CLI, "sch", "erc", "--severity-all", "--format", "json", "-o", erc_path, root],
                   capture_output=True)
    erc = json.load(open(erc_path))
    viol = []
    for sheet in erc.get("sheets", []):
        for v in sheet.get("violations", []):
            viol.append((sheet.get("path"), v.get("severity"), v.get("type"), v.get("description"),
                         [i.get("description") for i in v.get("items", [])]))
    print("netlist: %d declared nets, %d exported nets, %d problems" % (len(want), len(got), len(errors)))
    for e in errors:
        print("  NET", e)
    print("ERC: %d violations" % len(viol))
    for v in viol:
        print("  ERC", v)
    return not errors


if __name__ == "__main__":
    design = build()
    if "--check" in sys.argv:
        sys.exit(0 if check(design) else 1)
