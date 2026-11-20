"""Generate and check the Rev B main-board schematic in KiCad 10."""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCHEMATIC = HERE.parent / "schematic"
sys.path.insert(0, str(SCHEMATIC / "gen"))
import build as circuits
import sch
import rp2350_core as core
import assembly
import cv_gate
import microsd

OUT = HERE / "midi"
PROJECT = "midi-io"
TX = ["MIDI_TX", "MIDI_TX_B", "MIDI_TX_C", "MIDI_TX_D"]
DEVICES = (401, 403, 404, 405)
GPIO = (0, 23, 24, 29)


class MidiSection(sch.Design):
    def title_block(self, sheet):
        return ('(title_block (title %s) (rev "B") (company "ReBirth RP235xB") '
                '(comment 1 "Digital core, MIDI, microSD and CV/Gate") (comment 2 "ReBirth hardware / Rev B"))'
                % sch.q(sheet.title))


def generate():
    OUT.mkdir(exist_ok=True)
    sch.PROJECT = PROJECT
    design = MidiSection(str(OUT), str(SCHEMATIC / "rebirth.kicad_sym"), "2026-11-19", "B")
    root = design.sheet("ReBirth main board", PROJECT + ".kicad_sch",
                        "ReBirth processor, MIDI, microSD and CV/Gate", "A3", 1)
    first = circuits.sheet_midi(design)
    first.page, first.name = 2, "OUT A and IN"
    next(part for part in first.parts if part.ref == "J401").value = "MIDI OUT A"
    boxes = [(first, circuits.pins([TX[0]], "input", "left") +
                     circuits.pins(["MIDI_RX"], "output", "right"))]
    for i, port in enumerate("BCD", 1):
        child = design.sheet("OUT " + port, f"out-{port.lower()}.kicad_sch",
                             f"MIDI OUT {port} current-loop transmitter", "A4", i + 2)
        circuits.draw_midi_out(child, i)
        boxes.append((child, circuits.pins([TX[i]], "input", "left")))
    circuits.draw_root(design, boxes, x0=45.72, y0=50.80)
    cpu = core.processor(design)
    boot = core.boot_io(design)
    card = microsd.card(design)
    analog = cv_gate.sheet(design)
    for part in cpu.parts + boot.parts:
        if part.ref in assembly.OVERRIDES:
            part.variants[assembly.EXTERNAL_VARIANT] = assembly.OVERRIDES[part.ref]
        if part.ref in assembly.EXTERNAL_FLASH_PARTS:
            part.dnp = True
    cpu_pins = [(net, net, direction, "left") for _, net, direction in core.BOOT_PINS]
    cpu_pins += [(net, net, direction, "right")
                 for _, net, direction in core.MIDI_PINS + microsd.CPU_PINS + cv_gate.CPU_PINS]
    boot_pins = [(net, net, core.INVERSE[direction], "left") for _, net, direction in core.BOOT_PINS]
    card_pins = [(net, net, core.INVERSE[direction], "left") for _, net, direction in microsd.CPU_PINS]
    analog_pins = [(net, net, "input", "left") for _, net, _ in cv_gate.CPU_PINS]
    circuits.draw_root(design, [(cpu, cpu_pins), (boot, boot_pins), (card, card_pins), (analog, analog_pins)],
                       x0=180.34, y0=50.80, max_y=235.0)
    root.heading("DIN MIDI interface", (20.32, 30.48))
    root.heading("Processor, power and boot", (157.48, 30.48))
    root.heading("Signal test points", (292.10, 30.48))
    for i, (net, gpio) in enumerate(zip(TX + ["MIDI_RX"], (*GPIO, 1))):
        x, y = 316.23, 53.34 + i * 15.24
        circuits.place(root, "Connector:TestPoint", f"TP{401+i}", f"GP{gpio}", x, y,
                       fp=circuits.FP_TP, conn={"1": net})
        root.wire((x, y), (x - 6.35, y))
        root.label(net, (x - 6.35, y), 180, "right bottom")
    circuits.note(root, 20.32, 180.34,
                  "Port A uses UART0 TX. Ports B, C and D are assigned to separate PIO TX state machines.",
                  "Each port carries 16 MIDI channels. The H11L1 receiver returns input data to UART0 RX.")
    for x, ref, net in ((309.88, "TP406", "+3V3"), (342.9, "TP407", "GND")):
        circuits.place(root, "Connector:TestPoint", ref, net, x, 147.32, fp=circuits.FP_TP,
                       conn={"1": net}, ref_at=(2.54,-5.08,"left"), val_at=(2.54,-2.54,"left"))
        root.wire((x, 147.32), (x, 151.13))
        if net == "GND":
            circuits.gnd(root, x, 151.13)
        else:
            root.wire((x,151.13), (x-6.35,151.13), (x-6.35,143.51))
            circuits.rail(root, net, x-6.35,143.51)
    circuits.write_project(design, str(OUT), PROJECT, "${KIPRJMOD}/../../schematic/rebirth.kicad_sym",
                           "${KIPRJMOD}/../../pcb/rebirth.pretty",
                           variants=[(assembly.EXTERNAL_VARIANT, assembly.EXTERNAL_DESCRIPTION)])
    design.write()
    return design


def verify(design, reference=None):
    cli = os.environ.get("KICAD_CLI", circuits.KICAD_CLI)
    source = str(OUT / (PROJECT + ".kicad_sch"))
    with tempfile.TemporaryDirectory() as tmp:
        exported = Path(tmp) / "midi.net"
        subprocess.run([cli, "sch", "export", "netlist", "--format", "kicadsexpr",
                        "-o", str(exported), source], check=True)
        actual = circuits.parse_netlist(exported)
        # Compare complete sets of pin numbers. This detects any two shorted
        # ports, crossed DIN pins or an input connected across the optical barrier.
        by_pin = {pin: nodes for nodes in actual.values() for pin in nodes}
        for name, nodes in design.declared_nets().items():
            assert all(by_pin.get(pin) == nodes for pin in nodes), (name, nodes)
        for i, dev in enumerate(DEVICES):
            off = 10 * i
            assert by_pin[f"J{dev}", "4"] == {(f"J{dev}", "4"), (f"FB{402+off}", "2")}
            assert by_pin[f"J{dev}", "5"] == {(f"J{dev}", "5"), (f"FB{401+off}", "2")}
            assert by_pin[f"TP{401+i}", "1"] == {
                (f"TP{401+i}", "1"), (f"U{dev}", "2"), (f"R{401+off}", "2"),
                ("U301", ("77","23","25","37")[i])}
        assert by_pin["TP405", "1"] == {("TP405","1"), ("U402","4"), ("R405","2"), ("U301","78")}
        for supply, numbers in core.SUPPLIES.items():
            nets = [by_pin["U301",str(p)] for p in numbers]
            assert all(n == nets[0] for n in nets), supply
        assert len({frozenset(by_pin["U301",str(numbers[0])]) for numbers in core.SUPPLIES.values()}) == 5
        for cpu_pin, flash_pin in ((71,6), (72,5), (74,2), (73,3), (70,7)):
            assert by_pin["U301",str(cpu_pin)] == {("U301",str(cpu_pin)), ("U303",str(flash_pin))}
        assert by_pin["U301", "75"] == {("U301", "75"), ("R310", "1"), ("R306", "1")}
        assert by_pin["R306", "2"] == {("R306", "2"), ("SW301", "1")}
        assert ("SW301", "2") in by_pin["TP407", "1"]
        for cpu_pin, socket_pin in (("40", "7"), ("42", "2"), ("44", "3"), ("46", "9")):
            assert ("J302", socket_pin) in by_pin["U301", cpu_pin]
        assert by_pin["U301", "43"] == {("U301", "43"), ("R311", "1")}
        assert by_pin["J302", "5"] == {("J302", "5"), ("R311", "2"), ("D302", "11")}
        assert by_pin["U301", "45"] == {("U301", "45"), ("U304", "3"), ("R312", "1")}
        card_supply = {("U304", "5"), ("J302", "4"), ("C323", "1"), ("C324", "1")}
        assert by_pin["J302", "4"] == card_supply | {(f"R{i}", "1") for i in range(314, 319)}
        assert ("U304", "1") in by_pin["J301", "1"]
        assert {("J302", p) for p in ("6", "10", "SH")} <= by_pin["TP407", "1"]
        for i, (cpu_pin, _, _) in enumerate(cv_gate.CPU_PINS[:-1]):
            assert ("U504", str(3+i)) in by_pin["U301", str(cpu_pin)]
        for i, dac_pins in enumerate((
                (("U501", "3"), ("U502", "3")),
                (("U501", "4"), ("U502", "4")),
                (("U501", "2"),), (("U502", "2"),))):
            ref = f"R{561+i}"
            assert by_pin["U504", str(21-i)] == {("U504", str(21-i)), (ref, "1")}
            assert set(dac_pins) <= by_pin[ref, "2"]
        assert by_pin["U504", "22"] == {("U301", "47"), ("U504", "22"), ("R573", "2")}
        assert {("U504", "1"), ("U504", "2"), ("R573", "1")} <= by_pin["TP406", "1"]
        assert {("U501", "1"), ("U502", "1"), ("U503", "4"), ("U504", "23"), ("U504", "24")} <= by_pin["J301", "1"]
        for n in range(1, 5):
            cv, gate = f"J{500+n}", f"J{504+n}"
            assert by_pin[cv, "T"] == {(cv, "T"), (f"D{500+n}", "1"), (f"R51{n}", "2"), (f"R52{n}", "2")}
            assert by_pin[gate, "T"] == {
                (gate, "T"), (f"D{504+n}", "1"), (f"R54{n}", "2"), (f"R55{n}", "1")}
            assert by_pin["U504", str(18-n)] == {("U504", str(18-n)), (f"R54{n}", "1")}
            assert {(cv, "S"), (gate, "S")} <= by_pin["TP407", "1"]
            assert len(by_pin[cv, "R"]) == len(by_pin[gate, "R"]) == 1
        assert by_pin["L301","2"] == {("L301","2"), ("U301","63")}
        assert ("U301","65") in by_pin["L301","1"]
        assert ("U302","5") in by_pin["TP406","1"]
        for jack, tvs, resistor, chip in [("3", ("1","6"), "R307", "67"), ("2", ("3","4"), "R308", "66")]:
            assert by_pin["J301",jack] == {("J301",jack),("D301",tvs[0]),("D301",tvs[1]),(resistor,"1")}
            assert by_pin["U301",chip] == {("U301",chip),(resistor,"2")}
        if reference is not None:
            verify_reference(circuits.parse_netlist(reference), by_pin)
        for pin in ("1", "2", "3"):
            assert len(by_pin.get(("J402", pin), ())) <= 1
        erc = Path(tmp) / "erc.json"
        result = subprocess.run([cli, "sch", "erc", "--severity-all", "--format", "json",
                                 "--exit-code-violations", "-o", str(erc), source])
        report = json.loads(erc.read_text())
        violations = [v for sheet in report["sheets"] for v in sheet["violations"]]
        if violations:
            raise RuntimeError([(v["type"], v["description"]) for v in violations])
        result.check_returncode()
    print("Processor, boot, USB, MIDI, microSD and CV/Gate: KiCad netlist matches all declared pins; ERC clean")


def verify_reference(reference, by_pin):
    """Compare only shared parts. R4 has breakout headers, optional PSRAM and a different 3V3 LDO."""
    refs = {"U1":"U301", "U3":"U303", "X1":"Y301", "L1":"L301"}
    refs.update({f"C{i}":f"C{300+i}" for i in range(1,22)})
    refs.update({f"R{i}":f"R{300+i}" for i in (1,2,3,4,6,7,8,10)})
    def ours(ref, pin):
        if ref in refs:
            return refs[ref], pin
        if ref == "J1" and pin in ("1","2","3","4","5"):
            return "J301", pin
        if ref == "J4" and pin in ("1","2","3"):
            return "J303", pin
        if ref in ("SW1","SW2"):
            return ("SW301" if ref == "SW1" else "SW302", "1" if pin in ("3","4") else "2")
        return None
    projected = [{ours(*p) for p in nodes} - {None} for nodes in reference.values()]
    shared = set().union(*projected)
    for expected in projected:
        for pin in expected:
            assert by_pin.get(pin, set()) & shared == expected, (pin, expected, by_pin.get(pin))
    for gpio, pin in zip((*GPIO,1), ("77","23","25","37","78")):
        assert ("U1",pin) in reference[f"/GPIO{gpio}"]
    print(f"RP2350B Minimal R4/S1: {len(shared)} common component pins checked against vendor netlist")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-netlist", type=Path, help="KiCad netlist of Raspberry Pi RP2350B Minimal R4/S1")
    verify(generate(), parser.parse_args().reference_netlist)
