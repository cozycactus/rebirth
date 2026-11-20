"""Rev B digital core. Pin and passive numbering follows the RP2350B Minimal R4/S1."""

import build as c
import assembly
import cv_gate
import microsd

MCU_FP = "rebirth:RP2350B_QFN80_Minimal_R4"
USB_FP = "rebirth:USB_Micro-B_Amphenol_10104110_Horizontal"
FLASH_FP = "Package_SO:SOIC-8_5.3x5.3mm_P1.27mm"
XTAL_FP = "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm"
SWITCH_FP = "Button_Switch_SMD:SW_Push_1P1T_NO_Vertical_Wuerth_434133025816"
L_FP = "rebirth:L_Abracon_AOTA-B201610S3R3-101-T"
C_VREG = "rebirth:C_0402_RP2350_VREG"
C0402 = "Capacitor_SMD:C_0402_1005Metric"
R0402 = "Resistor_SMD:R_0402_1005Metric"

# Physical QFN-80 pins, including each stacked supply pin and the exposed pad.
SUPPLIES = {
    "+3V3": (5, 15, 24, 29, 41, 50, 59, 60, 64, 68, 69, 76),
    "+1V1": (10, 32, 51, 65),
    "GND": (62, 81),
    "VREG_AVDD": (61,),
    "VREG_LX": (63,),
}
# Pin, signal and direction as seen from the processor.
BOOT_PINS = [
    (75, "QSPI_SS", "output"), (71, "QSPI_SCLK", "output"),
    (72, "QSPI_SD0", "bidirectional"), (74, "QSPI_SD1", "bidirectional"),
    (73, "QSPI_SD2", "bidirectional"), (70, "QSPI_SD3", "bidirectional"),
    (30, "XIN", "input"), (31, "XOUT", "output"),
    (33, "SWCLK", "input"), (34, "SWDIO", "bidirectional"), (35, "RUN", "input"),
    (66, "USB_DM", "bidirectional"), (67, "USB_DP", "bidirectional"),
]
MIDI_PINS = [(77, "MIDI_TX", "output"), (78, "MIDI_RX", "input"),
             (23, "MIDI_TX_B", "output"), (25, "MIDI_TX_C", "output"),
             (37, "MIDI_TX_D", "output")]
FLASH_PINS = {"1": "FLASH_SS", "2": "QSPI_SD1", "3": "QSPI_SD2", "4": "GND",
              "5": "QSPI_SD0", "6": "QSPI_SCLK", "7": "QSPI_SD3", "8": "+3V3"}
INVERSE = {"input": "output", "output": "input", "bidirectional": "bidirectional"}


def bank(sh, refs, x, y, supply):
    """Identical bypass capacitors on continuous supply and ground buses."""
    last = x + 10.16 * (len(refs) - 1)
    sh.wire((x, y-7.62), (last, y-7.62))
    sh.wire((x, y+7.62), (last, y+7.62))
    c.rail(sh, supply, x, y-7.62)
    c.gnd(sh, last, y+7.62)
    for i, ref in enumerate(refs):
        px = x + i * 10.16
        c.cap(sh, ref, "100n", px, y, {"1": supply, "2": "GND"}, fp=C0402)
        sh.wire((px, y-7.62), (px, y-3.81))
        sh.wire((px, y+3.81), (px, y+7.62))


def processor(design):
    sh = design.sheet("Processor", "processor.kicad_sch", "Processor, core converter and bypass", "A3", 6)
    x, y = 134.62, 149.86
    conn = {str(p): net for net, numbers in SUPPLIES.items() for p in numbers}
    signals = BOOT_PINS + MIDI_PINS + microsd.CPU_PINS + cv_gate.CPU_PINS
    conn.update({str(p): net for p, net, _ in signals})
    cpu = c.place(sh, "MCU_RaspberryPi:RP2354B", "U301", assembly.DEFAULT_MCU, x, y,
                  fp=MCU_FP, conn=conn, props={"MPN": assembly.DEFAULT_MCU},
                  ref_at=(30.48, -59.69, "left"), val_at=(30.48, -56.515, "left"))
    sh.heading("Processor and MIDI connections", (20.32, 25.4))
    c.note(sh, 20.32, 33.02, "QFN-80. MIDI OUT A / IN are UART0. OUT B, C and D have individual PIO transmit pins.",
           "GPIOs left open on this sheet are available for the remaining sequencer circuits.")
    for pin in cpu.sym.pins:
        if pin.number not in conn:
            assert pin.name.startswith("GPIO"), pin.name
            sh.nc(sh.pin("U301", pin.number))
    for p, net, shape in signals:
        px, py = sh.pin("U301", str(p))
        left = px < x
        end = (px - 7.62 if left else px + 7.62, py)
        sh.wire((px, py), end)
        sh.hlabel(net, end, shape, "left" if left else "right")
    # The power pins stacked in the KiCad symbol connect through the same wire.
    pins = sorted({sh.pin("U301", str(p)) for p in SUPPLIES["+3V3"]})
    bus = pins[0][1] - 8.89
    sh.wire((pins[0][0], bus), (pins[-1][0], bus))
    for px, py in pins:
        sh.wire((px, py), (px, bus))
    c.rail(sh, "+3V3", pins[0][0], bus)
    (fb, py), (dvdd, _) = sorted({sh.pin("U301", str(p)) for p in SUPPLIES["+1V1"]})
    sh.wire((fb, py), (fb, py-5.08), (dvdd, py-5.08), (dvdd, py))
    c.rail(sh, "+1V1", dvdd, py-5.08)
    for pin, name, dx, rise in [(61, "VREG_AVDD", -12.7, 7.62), (63, "VREG_LX", 0, 20.32)]:
        px, py = sh.pin("U301", str(pin))
        end = (px + dx, py-rise)
        sh.wire((px, py), (px, py-rise), end)
        sh.label(name, end, 180 if dx else 0, "right bottom" if dx else "left bottom")
    (pg, py), (ep, _) = sorted(sh.pin("U301", str(p)) for p in SUPPLIES["GND"])
    sh.wire((pg, py), (pg, py+3.81), (ep, py+3.81), (ep, py))
    c.gnd(sh, ep, py+3.81)

    sh.heading("On-chip 1.1 V buck converter", (220.98, 43.18))
    c.decap(sh, "C306", "4.7u", 228.6, 66.04, "+3V3", fp=C_VREG)
    c.place(sh, "Device:L_Polarized", "L301", "3.3uH", 287.02, 60.96, rot=270, fp=L_FP,
            conn={"1": "+1V1", "2": "VREG_LX"}, ref_at=(0, -3.81, "center"),
            val_at=(0, 3.81, "center"), props={"MPN": "AOTA-B201610S3R3-101-T"})
    sh.wire(sh.pin("L301", "2"), (271.78, 60.96))
    sh.label("VREG_LX", (271.78, 60.96), 180, "right bottom")
    sh.wire(sh.pin("L301", "1"), (320.04, 60.96))
    c.rail(sh, "+1V1", 320.04, 60.96)
    c.cap(sh, "C307", "4.7u", 304.8, 68.58, {"1": "+1V1", "2": "GND"}, fp=C_VREG)
    sh.wire((304.8, 60.96), (304.8, 64.77))
    c.gnd(sh, 304.8, 72.39)
    sh.pwr("PWR_FLAG", (298.45, 60.96))  # Regulated DC downstream of the inductor.
    c.res(sh, "R303", "33R", 287.02, 88.9, {"1": "+3V3", "2": "VREG_AVDD"}, horiz=True, fp=R0402)
    sh.wire(sh.pin("R303", "1"), (271.78, 88.9))
    c.rail(sh, "+3V3", 271.78, 88.9)
    sh.wire(sh.pin("R303", "2"), (322.58, 88.9))
    sh.label("VREG_AVDD", (322.58, 88.9))
    c.cap(sh, "C309", "4.7u", 304.8, 96.52, {"1": "VREG_AVDD", "2": "GND"}, fp=C0402)
    sh.wire((304.8, 88.9), (304.8, 92.71))
    c.gnd(sh, 304.8, 100.33)
    sh.pwr("PWR_FLAG", (298.45, 88.9))  # DC feed through R303.
    c.note(sh, 220.98, 110.49,
           "L301 marked terminal 1 to +1V1; terminal 2 to VREG_LX. C306/C307 use R4 wide lands.",
           "Follow the RP2350B Minimal R4 switching loop, ground return, vias and inductor orientation.")
    sh.heading("Local supply bypass", (220.98, 132.08))
    bank(sh, ["C308", "C310", "C311"], 228.6, 154.94, "+1V1")
    c.decap(sh, "C319", "10u", 299.72, 154.94, "+3V3", fp=c.FP_C0805)
    bank(sh, ["C312", "C313", "C314", "C315", "C316", "C317", "C318", "C320", "C321"],
         228.6, 190.5, "+3V3")
    c.note(sh, 220.98, 210.82, "Place the bypass capacitors at their MCU supply pins.",
           "USB_OTP_VDD and QSPI_IOVDD share the nearby 100 nF part in the reference layout.")
    return sh


def boot_io(design):
    sh = design.sheet("USB, clock and storage", "boot-usb.kicad_sch",
                      "USB power and data, primary flash, oscillator and debug", "A3", 7)
    sh.heading("Micro-B device and 3.3 V logic supply", (20.32, 25.4))
    c.place(sh, "Connector:USB_B_Micro", "J301", "USB Micro-B", 43.18, 58.42, fp=USB_FP,
            conn={"1": "VBUS", "2": "USB_D-", "3": "USB_D+", "5": "GND", "SH": "GND"},
            ref_at=(-1.27, -13.97, "center"), val_at=(-1.27, -11.43, "center"),
            props={"MPN": "10104110-0001LF"})
    sh.nc(sh.pin("J301", "4"))
    sh.wire(sh.pin("J301", "1"), (55.88, 53.34))
    c.rail(sh, "VBUS", 55.88, 53.34)
    sh.wire(sh.pin("J301", "SH"), (40.64, 72.39), (43.18, 72.39), sh.pin("J301", "5"))
    c.gnd(sh, 43.18, 72.39)
    c.place(sh, "Power_Protection:USBLC6-2SC6", "D301", "USBLC6-2SC6", 96.52, 58.42,
            fp=c.FP_SOT236, ref_at=(10.16,-10.16,"left"), val_at=(10.16,-7.62,"left"),
            conn={"1": "USB_D+", "6": "USB_D+", "3": "USB_D-", "4": "USB_D-", "5": "VBUS", "2": "GND"})
    sh.wire(sh.pin("D301", "5"), (96.52, 48.26))
    c.rail(sh, "VBUS", 96.52, 48.26)
    c.gnd(sh, *sh.pin("D301", "2"))
    # Label both sides of the TVS through paths so the PCB net includes all four lands.
    for jack, a, b, ref, net, cpu_net, y, rx in [
            ("3", "1", "6", "R307", "USB_D+", "USB_DP", 58.42, 152.4),
            ("2", "3", "4", "R308", "USB_D-", "USB_DM", 60.96, 185.42)]:
        sh.wire(sh.pin("J301", jack), sh.pin("D301", a))
        sh.label(net, (66.04,y))
        c.res(sh, ref, "27R", rx, y, {"1": net, "2": cpu_net}, horiz=True, fp=R0402,
              fields=((0,-5.08,"center"),(0,-2.54,"center")) if jack == "3" else
                     ((0,2.54,"center"),(0,5.08,"center")))
        sh.wire(sh.pin("D301", b), sh.pin(ref, "1"))
        sh.label(net, (127,y))
        sh.wire(sh.pin(ref, "2"), (210.82, y))
        sh.hlabel(cpu_net, (210.82, y), "bidirectional", "right")
    c.place(sh, "Regulator_Linear:TLV75533PDBV", "U302", "TLV75533PDBVR", 307.34, 60.96,
            fp=c.FP_SOT235, ref_at=(0,-10.16,"center"), val_at=(0,-7.62,"center"),
            conn={"1": "VBUS", "2": "GND", "3": "VBUS", "5": "+3V3"})
    sh.nc(sh.pin("U302", "4"))
    sh.wire(sh.pin("U302", "1"), (274.32,58.42))
    sh.wire(sh.pin("U302", "3"), (293.37,60.96), (293.37,58.42))
    c.rail(sh, "VBUS", 274.32, 58.42)
    c.gnd(sh, *sh.pin("U302","2"))
    sh.wire(sh.pin("U302", "5"), (345.44,58.42))
    c.rail(sh, "+3V3", 345.44, 58.42)
    for ref, supply, x in [("C301", "VBUS", 281.94), ("C305", "+3V3", 330.2)]:
        c.cap(sh, ref, "4.7u", x, 68.58, {"1": supply, "2": "GND"}, fp=c.FP_C0805,
              props={"Rating": "10 V X7R"})
        sh.wire((x,58.42), sh.pin(ref,"1"))
        c.gnd(sh, *sh.pin(ref,"2"))
    c.note(sh, 20.32, 82.55, "Place D301 at J301 and R307/R308 at the MCU. Impedance target: 90 ohm differential.",
           "The LED chain and TFT backlight require their own current and startup budget.")

    sh.heading("Optional primary flash and BOOTSEL", (20.32,101.6))
    c.place(sh, "Memory_Flash:W25Q128JVS", "U303", "W25Q128JVSIQ", 129.54, 154.94,
            conn=FLASH_PINS, fp=FLASH_FP, props={"MPN": "W25Q128JVSIQ"},
            ref_at=(11.43,-12.7,"left"), val_at=(11.43,-10.16,"left"))
    c.rail(sh, "+3V3", *sh.pin("U303","8"))
    c.gnd(sh, *sh.pin("U303","4"))
    c.decap(sh, "C302", "100n", 177.8, 154.94, "+3V3", fp=C0402)
    for pad in ("2", "3", "5", "6", "7"):
        px, py = sh.pin("U303", pad)
        sh.wire((104.14, py), (px, py))
        sh.hlabel(FLASH_PINS[pad], (104.14, py), "input" if pad == "6" else "bidirectional", "left")
    cs_y = sh.pin("U303","1")[1]
    c.res(sh, "R310", "0R", 77.47, cs_y, {"1": "QSPI_SS", "2": "FLASH_SS"}, horiz=True, fp=R0402)
    sh.wire((41.91,cs_y), sh.pin("R310","1"))
    sh.hlabel("QSPI_SS", (41.91,cs_y), "input", "left")
    sh.wire(sh.pin("R310","2"), sh.pin("U303","1"))
    sh.label("FLASH_SS", (104.14,cs_y))
    c.res(sh, "R301", "10k DNP", 91.44, cs_y-11.43, {"1": "+3V3", "2": "FLASH_SS"}, fp=R0402, dnp=True)
    sh.wire(sh.pin("R301","2"), (91.44,cs_y))
    c.rail(sh, "+3V3", *sh.pin("R301","1"))
    c.res(sh, "R306", "1k", 55.88, 161.29, {"1": "QSPI_SS", "2": "BOOT_BUTTON"}, fp=R0402)
    sh.wire((55.88,cs_y), sh.pin("R306","1"))
    c.place(sh, "Switch:SW_Push", "SW301", "BOOTSEL", 66.04, 176.53, fp=SWITCH_FP,
            ref_at=(0,-6.35,"center"), val_at=(0,-3.81,"center"),
            conn={"1": "BOOT_BUTTON", "2": "GND"})
    sh.wire(sh.pin("R306","2"), (55.88,176.53), sh.pin("SW301","1"))
    sh.wire(sh.pin("SW301","2"), (76.2,176.53))
    c.gnd(sh, 76.2, 176.53)
    c.note(sh, 20.32, 191.77,
           "Default: RP2354B with 2 MiB in-package. Leave U303, C302 and R310 unpopulated.",
           "External-16MiB: fit RP2350B, U303, C302 and R310. Leave R301 unpopulated.",
           "BOOTSEL pulls QSPI_SS low through R306 in both builds. Hold it during reset for USB boot.")

    sh.heading("12 MHz crystal", (226.06,101.6))
    c.place(sh, "Device:Crystal_GND24", "Y301", "ABM8-272-T3", 290.83, 129.54,
            fp=XTAL_FP, ref_at=(0,-8.89,"center"), val_at=(0,-6.35,"center"),
            conn={"1": "XIN", "2": "GND", "3": "XTAL_OUT", "4": "GND"})
    sh.wire(sh.pin("Y301","1"), (246.38,129.54))
    sh.hlabel("XIN",(246.38,129.54),"output","left")
    c.gnd(sh, *sh.pin("Y301","2"))
    c.place(sh, "Device:R", "R302", "1k", 335.28, 129.54, rot=270, fp=R0402,
            ref_at=(0,-3.81,"center"), val_at=(0,3.81,"center"), conn={"1":"XOUT","2":"XTAL_OUT"})
    sh.wire(sh.pin("Y301","3"), sh.pin("R302","2"))
    sh.wire(sh.pin("R302","1"), (346.71,129.54))
    sh.hlabel("XOUT", (346.71,129.54), "input","right")
    for ref, x, net in [("C303",269.24,"XIN"), ("C304",304.8,"XTAL_OUT")]:
        c.cap(sh, ref, "15p", x,142.24, {"1":net,"2":"GND"}, fp=C0402, props={"Dielectric":"C0G"})
        sh.wire((x,129.54), sh.pin(ref,"1"))
        c.gnd(sh, *sh.pin(ref,"2"))

    sh.heading("Internal reset and debug", (226.06,172.72))
    c.place(sh,"Switch:SW_Push","SW302","RUN",274.32,193.04, fp=SWITCH_FP,
            ref_at=(0,-6.35,"center"), val_at=(0,-3.81,"center"), conn={"1":"RUN","2":"RESET_RETURN"})
    sh.wire((255.27,193.04), sh.pin("SW302","1"))
    sh.hlabel("RUN",(255.27,193.04),"output","left")
    c.res(sh,"R304","1k",299.72,193.04,{"1":"RESET_RETURN","2":"GND"}, horiz=True, fp=R0402)
    sh.wire(sh.pin("SW302","2"),sh.pin("R304","1"))
    sh.wire(sh.pin("R304","2"),(307.34,193.04))
    c.gnd(sh,307.34,193.04)
    c.place(sh,"Connector_Generic:Conn_01x03","J303","SWD",350.52,204.47,
            fp="Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical",
            ref_at=(5.08,-2.54,"left"),val_at=(5.08,0,"left"),conn={"1":"SWCLK","2":"GND","3":"SWDIO"})
    for pad, signal, direction in [("1","SWCLK","output"), ("3","SWDIO","bidirectional")]:
        _, py = sh.pin("J303",pad)
        sh.wire(sh.pin("J303",pad), (326.39,py))
        sh.hlabel(signal, (326.39,py),direction,"left")
    sh.wire(sh.pin("J303","2"), (316.23,204.47), (316.23,210.82))
    c.gnd(sh,316.23,210.82)
    return sh
