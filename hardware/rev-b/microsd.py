"""Rear microSD socket, SPI interface and switched card supply."""

import build as c

SOCKET_FP = "Connector_Card:microSD_HC_Hirose_DM3AT-SF-PEJM5"
R0402 = "Resistor_SMD:R_0402_1005Metric"
C0402 = "Capacitor_SMD:C_0402_1005Metric"
CPU_PINS = [
    (40, "SD_MISO", "input"), (42, "SD_CS_N", "output"), (43, "SD_SCK", "output"),
    (44, "SD_MOSI", "output"), (45, "SD_PWR_EN", "output"), (46, "SD_DETECT_N", "input"),
]
SOCKET_PINS = {
    "1": "SD_DAT2", "2": "SD_CS_N", "3": "SD_MOSI", "4": "SD_VDD",
    "5": "SD_SCK_CARD", "6": "GND", "7": "SD_MISO", "8": "SD_DAT1",
    "9": "SD_DETECT_N", "10": "GND", "SH": "GND",
}
TVS_PINS = {
    "14": "SD_DAT2", "13": "SD_CS_N", "12": "SD_MOSI", "11": "SD_SCK_CARD",
    "9": "SD_MISO", "8": "SD_DAT1", "5": "GND", "10": "GND",
}


def card(design):
    sh = design.sheet("microSD", "microsd.kicad_sch", "Rear microSD socket and card power", "A3", 8)
    sh.heading("microSD socket in SPI mode", (20.32, 25.4))
    c.place(sh, "Connector:Micro_SD_Card_Det_Hirose_DM3AT", "J302", "microSD",
            144.78, 66.04, fp=SOCKET_FP, conn=SOCKET_PINS,
            props={"MPN": "DM3AT-SF-PEJM5"},
            ref_at=(0, -17.78, "center"), val_at=(0, -15.24, "center"))
    port_direction = {"2": "input", "3": "input", "7": "output", "9": "output"}
    for number, net in SOCKET_PINS.items():
        x, y = sh.pin("J302", number)
        if number == "5":
            c.res(sh, "R311", "33R", 86.36, y, {"1": "SD_SCK", "2": net},
                  horiz=True, fp=R0402)
            sh.wire((50.8, y), sh.pin("R311", "1"))
            sh.hlabel("SD_SCK", (50.8, y), "input", "left")
            sh.wire(sh.pin("R311", "2"), (x, y))
            sh.label(net, (96.52, y))
        elif net == "GND":
            stub_x = {"6": 111.76, "10": 114.3, "SH": 172.72}[number]
            sh.wire((x, y), (stub_x, y))
            c.gnd(sh, stub_x, y)
        else:
            stub = (50.8, y)
            sh.wire(stub, (x, y))
            if number in port_direction:
                sh.hlabel(net, stub, port_direction[number], "left")
            else:
                sh.label(net, stub)
    c.note(sh, 20.32, 93.98, "DAT1 and DAT2 are pulled up. The detect switch closes when a card is fully inserted.",
           "Use a dedicated PIO SPI bus on GP32–GP35. Place the clock series resistor near U301.")

    sh.heading("Card data pull-ups and insertion detect", (205.74, 25.4))
    positions = (213.36, 241.3, 269.24, 297.18, 325.12)
    sh.wire((positions[0], 45.72), (positions[-1], 45.72))
    sh.label("SD_VDD", (positions[0], 45.72))
    pullups = ("SD_CS_N", "SD_MOSI", "SD_MISO", "SD_DAT1", "SD_DAT2")
    for i, (x, net) in enumerate(zip(positions, pullups), 314):
        ref = f"R{i}"
        c.res(sh, ref, "47k", x, 55.88, {"1": "SD_VDD", "2": net}, fp=R0402)
        sh.wire((x, 45.72), sh.pin(ref, "1"))
        sh.wire(sh.pin(ref, "2"), (x, 69.85))
        sh.label(net, (x, 69.85))
    c.res(sh, "R313", "47k", 363.22, 55.88, {"1": "+3V3", "2": "SD_DETECT_N"}, fp=R0402)
    sh.wire((363.22, 45.72), sh.pin("R313", "1"))
    c.rail(sh, "+3V3", 363.22, 45.72)
    sh.wire(sh.pin("R313", "2"), (363.22, 69.85))
    sh.label("SD_DETECT_N", (363.22, 69.85))
    c.note(sh, 205.74, 85.09, "Card bus pull-ups use switched SD_VDD. Detect stays on the logic rail.")

    sh.heading("ESD protection at the socket", (20.32, 115.57))
    tvs = c.place(sh, "Power_Protection:TPD6E05U06RVZ", "D302", "TPD6E05U06RVZ",
                  104.14, 172.72, fp="Package_SON:Texas_R-PUSON-N14", conn=TVS_PINS,
                  props={"MPN": "TPD6E05U06RVZR"},
                  ref_at=(29.21, -7.62, "left"), val_at=(29.21, -5.08, "left"))
    for pin in tvs.sym.pins:
        x, y = sh.pin("D302", pin.number)
        if pin.number not in TVS_PINS:
            sh.nc((x, y))
        elif TVS_PINS[pin.number] != "GND":
            sh.wire((x, y), (x, 147.32))
            sh.label(TVS_PINS[pin.number], (x, 147.32), 90)
    sh.wire(sh.pin("D302", "5"), (139.7, 172.72))
    c.gnd(sh, 139.7, 172.72)
    c.note(sh, 20.32, 205.74, "Tie both TVS grounds and the socket shield to board ground.",
           "Keep the protection stubs and their ground return short.")

    sh.heading("Switched 3.3 V for the card", (205.74, 115.57))
    power(sh)
    return sh


def power(sh):
    c.place(sh, "Regulator_Linear:TLV75533PDBV", "U304", "TLV75533PDBVR",
            281.94, 166.37, fp=c.FP_SOT235,
            conn={"1": "VBUS", "2": "GND", "3": "SD_PWR_EN", "5": "SD_VDD"},
            ref_at=(0, -10.16, "center"), val_at=(0, -7.62, "center"))
    sh.nc(sh.pin("U304", "4"))
    c.gnd(sh, *sh.pin("U304", "2"))
    sh.wire((231.14, 163.83), sh.pin("U304", "1"))
    c.rail(sh, "VBUS", 231.14, 163.83)
    sh.wire((210.82, 166.37), (210.82, 175.26), (266.7, 175.26),
            (266.7, 166.37), sh.pin("U304", "3"))
    sh.hlabel("SD_PWR_EN", (210.82, 166.37), "input", "left")
    c.res(sh, "R312", "100k", 256.54, 184.15, {"1": "SD_PWR_EN", "2": "GND"}, fp=R0402)
    sh.wire((256.54, 175.26), sh.pin("R312", "1"))
    c.gnd(sh, *sh.pin("R312", "2"))
    c.cap(sh, "C322", "2.2u", 243.84, 143.51, {"1": "VBUS", "2": "GND"},
          fp=c.FP_C0805, props={"Rating": "10 V X7R"})
    # Keep the independent input bypass above the enable path.
    c.rail(sh, "VBUS", *sh.pin("C322", "1"))
    c.gnd(sh, *sh.pin("C322", "2"))
    sh.wire(sh.pin("U304", "5"), (340.36, 163.83))
    sh.label("SD_VDD", (314.96, 163.83))
    for ref, value, x, fp in [("C323", "10u", 312.42, c.FP_C0805),
                              ("C324", "100n", 340.36, C0402)]:
        c.cap(sh, ref, value, x, 177.8, {"1": "SD_VDD", "2": "GND"},
              fp=fp, props={"Rating": "10 V X7R"})
        sh.wire((x, 163.83), sh.pin(ref, "1"))
        c.gnd(sh, *sh.pin(ref, "2"))
    c.note(sh, 205.74, 205.74, "SD power defaults off before GPIO setup. Keep SPI pins high-impedance while off.",
           "Enable the card after the USB power budget permits it. Allow VDD to settle before SPI init.")
