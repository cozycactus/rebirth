"""Four Rev B pitch CV outputs and active-high Gate outputs."""

import build as c

JACK = "rebirth:Jack_3.5mm_CUI_SJ1-3533NG_Horizontal"
TRANSLATOR = "Package_SO:TSSOP-24_4.4x7.8mm_P0.65mm"
SPI_SIGNALS = tuple(net for net, _ in c.SPI_SERIES)
GATES = tuple(f"GATE{i}" for i in range(1, 5))
ENABLE = "CV_ENABLE_N"
SIGNALS = SPI_SIGNALS + GATES
CPU_PINS = [(pin, net, "output") for pin, net in zip((8, 9, 11, 12, 13, 14, 16, 17), SIGNALS)]
CPU_PINS.append((47, ENABLE, "output"))


def translator(sh):
    sh.heading("CV/Gate level translation", (20.32, 25.4))
    output_nets = [net + "_B" for net in SPI_SIGNALS] + [gate + "_BUF" for gate in GATES]
    pins = {"1": "+3V3", "2": "+3V3", "11": "GND", "12": "GND", "13": "GND",
            "22": ENABLE, "23": "VBUS", "24": "VBUS"}
    pins.update({str(3+i): net for i, net in enumerate(SIGNALS)})
    pins.update({str(21-i): net for i, net in enumerate(output_nets)})
    ref = "U504"
    c.place(sh, "Logic_LevelTranslator:SN74LVC8T245", ref, "SN74LVC8T245PWR", 76.2, 83.82,
            fp=TRANSLATOR, conn=pins, props={"MPN": "SN74LVC8T245PWR"},
            ref_at=(15.24, -31.75, "left"), val_at=(15.24, -29.21, "left"))
    for i, (net, out) in enumerate(zip(SIGNALS, output_nets)):
        x, y = sh.pin(ref, str(3+i))
        sh.wire((31.75, y), (x, y))
        sh.hlabel(net, (31.75, y), "input", "left")
        x, y = sh.pin(ref, str(21-i))
        if i < len(SPI_SIGNALS):
            r = c.SPI_SERIES[i][1]
            terminated = net + "_T"
            c.res(sh, r, "33R", 104.14, y, {"1": out, "2": terminated}, horiz=True,
                  fields=((0, -1.778, "center"), (0, 1.778, "center")), hide_val=True)
            sh.wire((x, y), sh.pin(r, "1"))
            sh.wire(sh.pin(r, "2"), (116.84, y))
            sh.label(terminated, (116.84, y))
        else:
            sh.wire((x, y), (105.41, y))
            sh.label(out, (105.41, y))
    for number, rail in (("1", "+3V3"), ("23", "VBUS"), ("2", "+3V3")):
        x, y = sh.pin(ref, number)
        if number == "2":
            stub = (x-8.89, y)
            sh.wire((x, y), stub)
        else:
            stub = (x + (-7.62 if number == "1" else 7.62), y-6.35)
            sh.wire((x, y), (x, y-6.35), stub)
        c.rail(sh, rail, *stub)
    x, y = sh.pin(ref, "22")
    sh.wire((31.75, y), (x, y))
    sh.hlabel(ENABLE, (31.75, y), "input", "left")
    c.gnd(sh, *sh.pin(ref, "11"))
    c.decap(sh, "C505", "100n", 106.68, 41.91, "+3V3")
    c.decap(sh, "C507", "100n", 127.0, 41.91, "VBUS")
    c.note(sh, 20.32, 121.92, "DIR is high. A takes 3.3 V logic, B drives the VBUS domain.",
           "OE stays high until firmware sets chip selects high and the four gates low.",
           "Place the 33 ohm SPI series resistors close to U504.")


def bias(sh):
    sh.heading("Reset levels", (20.32, 151.13))
    # Keep both ports defined with /OE high, including the two DAC chip selects.
    for index, net in enumerate(SPI_SIGNALS):
        for on_dac, y, supply, first_ref in ((False, 173.99, "+3V3", 565),
                                             (True, 214.63, "VBUS", 569)):
            x = 25.4 + 25.4 * index
            ref = f"R{first_ref+index}"
            label = net + "_T" if on_dac else net
            if index >= 2:
                c.pull_up(sh, ref, "10k", x, y, supply, label)
            else:
                c.pull_down(sh, ref, "47k", x, y, label)
            sh.wire((x, y), (x+5.08, y))
            sh.label(label, (x+5.08, y))
    c.pull_up(sh, "R573", "10k", 129.54, 173.99, "+3V3", ENABLE)
    sh.wire((129.54, 173.99), (134.62, 173.99))
    sh.label(ENABLE, (134.62, 173.99))
    for i, net in enumerate(GATES, 1):
        x = 25.4 * i
        c.pull_down(sh, f"R53{i}", "100k", x, 241.3, net)
        sh.wire((x, 241.3), (x+5.08, 241.3))
        sh.label(net, (x+5.08, 241.3))


def outputs(sh):
    sh.heading("Four pitch outputs, 1 V/octave", (152.4, 25.4))
    c.note(sh, 323.85, 35.56, "MCP4822 at gain 2x on VBUS. Nominal 1 mV/LSB at the jack.",
           "At 4.096 V full scale, allow supply and buffer headroom under load.",
           "Measure slope and offset at the jacks with the connected synth.")
    c.dac_block(sh, "U501", 180.34, 66.04, "DAC_CS1_N", ("CV1_DAC", "CV2_DAC"), ("C501",), "VBUS")
    c.dac_block(sh, "U502", 180.34, 129.54, "DAC_CS2_N", ("CV3_DAC", "CV4_DAC"), ("C502",), "VBUS")
    stage_x = 248.92
    for n, y in enumerate((66.04, 93.98, 129.54, 157.48), 1):
        net = f"CV{n}_DAC"
        refs = (f"C51{n}", f"R51{n}", f"R52{n}", f"R51{n+4}")
        c.cv_stage(sh, n, stage_x, y, n, net, refs, jack_fp=JACK, ring=True)
        pin = "8" if n % 2 else "6"
        px, py = sh.pin("U501" if n <= 2 else "U502", pin)
        dest = (stage_x-7.62, y-2.54)
        # The even-numbered channels turn down into the next buffer row.
        sh.wire((px, py), (205.74, py), (205.74, dest[1]), dest)
        sh.label(net, (205.74, dest[1]))

    c.place(sh, "Amplifier_Operational:MCP6004", "U503", "MCP6004", 353.06, 91.44,
            unit=5, fp=c.FP_SO14, conn={"4": "VBUS", "11": "GND"},
            ref_at=(1.27, -2.54, "left"), hide_val=True)
    c.rail(sh, "VBUS", *sh.pin("U503", "4", unit=5))
    c.gnd(sh, *sh.pin("U503", "11", unit=5))
    c.decap(sh, "C504", "100n", 374.65, 91.44, "VBUS")
    c.decap(sh, "C506", "1u", 383.54, 109.22, "VBUS", fp=c.FP_C0805)
    c.note(sh, 323.85, 144.78, "MCP6004 buffers. R511–R514 are inside the DC feedback loop.",
           "C511–C514 close local feedback at high frequency.", "D501–D508 are rear-jack TVS diodes.")

    sh.heading("Four active-high Gate outputs", (152.4, 179.07))
    for n, gate in enumerate(GATES, 1):
        y = 191.77 + (n-1)*17.78
        source, jack = gate + "_BUF", gate + "_OUT"
        resistor = f"R54{n}"
        c.res(sh, resistor, "1k", 215.9, y, {"1": source, "2": jack}, horiz=True)
        sh.wire((182.88, y), sh.pin(resistor, "1"))
        sh.label(source, (182.88, y))
        sh.wire(sh.pin(resistor, "2"), (279.4, y))
        sh.label(jack, (234.95, y))
        c.pull_down(sh, f"R55{n}", "100k", 251.46, y, jack)
        c.jack_out(sh, f"J{504+n}", f"GATE {n}", 284.48, y, jack,
                   tvs=f"D{504+n}", fp=JACK, ring=True)
    c.note(sh, 323.85, 193.04, "The jacks are held low while OE is high.",
           "Use the tip and sleeve of a TS cable. Ring is open.",
           "Check the high Gate level and CV settling under load.")


def sheet(design):
    sh = design.sheet("CV and Gate", "cv-gate.kicad_sch", "CV 1–4 and Gate 1–4", "A3", 9)
    translator(sh)
    bias(sh)
    outputs(sh)
    return sh
