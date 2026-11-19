"""Place the connected sections of the ReBirth Rev B main board."""

import argparse
import tempfile
from pathlib import Path

import pcbnew as pcb

import placement as layout
import common
import assembly

PROJECT = layout.HERE / "midi" / "midi-io"

# Offsets in KiCad board axes (positive Y down) from the QFN centre and rotations.
# These positions are from Raspberry Pi RP2350B Minimal R4; see Raspberry-Pi-LICENSE.txt.
CORE_OFFSETS = {
    "C306": (3, -6.06, 0), "C307": (3, -7, 0), "L301": (3, -8.65, 0),
    "C309": (5.2, -6.5, -90), "R303": (5.2, -8.35, -90),
    "C308": (-8, -0.2, 180), "C310": (1.4, 8.3, -90), "C311": (8, -0.5, 0),
    "C312": (-1, -8.8, 90), "C313": (-1.3, 8.3, -90), "C314": (-5.8, -8.1, 180),
    "C315": (-8, -2.9, 180), "C316": (8.005, 0.5, 0), "C317": (-8, 2.2, 180),
    "C318": (7.5, 4.5, 0), "C320": (7.5, -4.3, 0), "C321": (-4.7, 6.7, 180),
    "R307": (0, -8.8, -90), "R308": (1, -8.8, -90),
    "C303": (-1.375, 10.42, 180), "C304": (1.595, 10.42, 0), "R302": (0.2, 8.305, -90),
    "Y301": (0, 14, 0),
}

# Enclosure coordinates, positive Y rearwards. The rear ports that have
# schematic symbols already and the MCU inherit their poses from main-placement.
DIGITAL = {
    "C301": (10.7, 122.6, 90), "C305": (12.7, 114.8, 0), "U302": (12.5, 118.5, 0),
    "D301": (6.5, 123.25, 180), "C319": (15, 106, 0),
    "U303": (-9.5, 111, 0), "C302": (-3.85, 113.55, 90), "R301": (-15.3, 111, 90),
    "R310": (-15.3, 108.4, 90), "R306": (-8.2, 106.4, 0),
    "SW301": (-12.4, 92.0, 0), "SW302": (15, 91, 0), "R304": (11, 89.3, 90),
    "J303": (23, 85, 90), "TP406": (14.5, 109.5, 0), "TP407": (18, 109.5, 0),
    "U402": (-18.5, 104.5, 0), "C402": (-8, 103.6, 90), "R405": (-7.8, 99.8, 90),
    "D401": (-22.5, 108.0, 90), "R404": (-17.5, 110.0, 90),
    "FB403": (-16, 113.3, 0), "FB404": (-1.2, 113.4, 0), "TP405": (-13.5, 96.4, 0),
    "R311": (9.5, 100.0, 0), "U304": (20, 106, 180), "R312": (23, 104, 90),
    "C322": (20.5, 102.4, -90), "C323": (21.05, 109.65, 90), "C324": (21.0, 112.7, 90),
    "D302": (24.3, 112.05, 90), "R313": (29.5, 111.5, 90),
    "R318": (24, 108, 0), "R314": (26.2, 108, 0), "R315": (28.4, 108, 0),
    "R316": (30.6, 108, 0), "R317": (32.8, 108, 0),
    "U501": (110.3, 100, 90), "U502": (86, 100, 90), "U503": (100, 105, 90),
    "U504": (56.5, 102.5, 90),
    "C501": (111.8, 104.6, 0), "C502": (85.0, 104.6, 0),
    "C504": (99.7, 100.0, 0), "C505": (50.8, 106.5, 0),
    "C506": (107.75, 105.6, 90), "C507": (62.8, 107.4, 0),
    "R573": (48.5, 101.4, 0),
}


def planned_positions(board):
    positions = dict(DIGITAL)
    centre = board.FindFootprintByReference("U301").GetPosition()
    cpu_x = pcb.ToMM(centre.x) - layout.case.OX
    cpu_y = layout.case.OY - pcb.ToMM(centre.y)
    for ref, (dx, dy, angle) in CORE_OFFSETS.items():
        positions[ref] = (cpu_x + dx, cpu_y - dy, angle)
    for bank, dev in enumerate((401, 403, 404, 405)):
        din = board.FindFootprintByReference(f"J{dev}")
        x = pcb.ToMM(din.GetPosition().x) - layout.case.OX
        for prefix, num, dx, y, angle in (
                ("FB", 401, 5, 113.2, 90), ("FB", 402, -5, 113.2, 90),
                ("R", 402, 5, 109.8, 0), ("R", 403, -5, 109.5, 0),
                ("R", 401, -5.3, 100.8, 90), ("C", 401, -4.6, 104.6, 90),
                ("R", 406, 0.7, 106, 0), ("R", 407, 6.8, 103.1, 90)):
            positions[f"{prefix}{num + 10 * bank}"] = (x + dx, y, angle)
        positions[f"U{dev}"] = (x - 1.8, 102.2, 0)
        positions[f"Q{dev}"] = (x + 3.3, 103.7, 0)
        positions[f"TP{401+bank}"] = (x + 1.5, 100, 0)
    for ref, _, x in layout.JACK_AXES:
        jack = int(ref[1:])
        positions[f"D{jack}"] = (x+0.4, 115.35, 0)
        if jack <= 504:
            n = jack - 500
            positions[f"R51{n}"] = (x-2.35, 112.7, 0)
            positions[f"R52{n}"] = (x+2.1, 112.7, 0)
            positions[f"C51{n}"] = (x+2.1, 110.9, 0)
            positions[f"R51{n+4}"] = (x-2.35, 110.9, 0)
        else:
            n = jack - 504
            positions[f"R54{n}"] = (x-2.35, 112.7, 0)
            positions[f"R55{n}"] = (x+2.1, 112.7, 0)
    for i in range(4):
        positions[f"R{531+i}"] = (47.0 + 3.1*i, 94.5, 0)
        positions[f"R{561+i}"] = (67.0, 100.0 + 2.0*i, 0)
        positions[f"R{565+i}"] = (47.0 + 3.1*i, 97.0, 0)
        positions[f"R{569+i}"] = (72.4, 100.0 + 2.0*i, 0)
    return positions


def check_board(board, mechanical, components, pins):
    assert layout.holes(board) == layout.holes(mechanical)
    assert layout.outline(board) == layout.outline(mechanical)
    active = {f.GetReference(): f for f in board.GetFootprints() if not f.IsBoardOnly()}
    assert set(active) == set(components)
    assert board.HasVariant(assembly.EXTERNAL_VARIANT)
    for ref, fp in active.items():
        component = components[ref]
        assert fp.GetPath().AsString() == component["path"]
        assert fp.GetFPIDAsString() == component["footprint"]
        assert fp.IsDNP() == component["dnp"]
        assert fp.IsExcludedFromBOM() == component["exclude_from_bom"]
        expected = assembly.OVERRIDES.get(ref, {})
        assert fp.GetDNPForVariant(assembly.EXTERNAL_VARIANT) == expected.get("dnp", fp.IsDNP())
        for field, value in expected.get("fields", {}).items():
            assert fp.GetFieldValueForVariant(assembly.EXTERNAL_VARIANT, field) == value
        for pad in fp.Pads():
            assert pad.GetNetname() == pins.get((ref, pad.GetNumber()), ""), (ref, pad.GetNumber())
    for fp in mechanical.GetFootprints():
        placed = board.FindFootprintByReference(fp.GetReference())
        assert (placed.GetPosition(), placed.GetOrientationDegrees()) == (
            fp.GetPosition(), fp.GetOrientationDegrees())


def generate(target):
    mechanical_file = layout.HERE / "main-placement.kicad_pcb"
    board = pcb.LoadBoard(str(mechanical_file))
    mechanical = pcb.LoadBoard(str(mechanical_file))
    board.AddVariant(assembly.EXTERNAL_VARIANT)
    board.SetVariantDescription(assembly.EXTERNAL_VARIANT, assembly.EXTERNAL_DESCRIPTION)
    with tempfile.TemporaryDirectory() as temp:
        components, nets, pins = common.export_netlist(str(PROJECT.with_suffix(".kicad_sch")),
                                                        str(Path(temp) / "main.net"))
    positions = planned_positions(board)
    signals = {}
    for name in sorted(nets):
        signals[name] = pcb.NETINFO_ITEM(board, name)
        board.Add(signals[name])
    for fp in board.GetFootprints():
        if fp.GetReference() not in components:
            fp.SetBoardOnly(True)  # Fixed mechanical positions of later circuit sections.
    for ref, component in components.items():
        fp = board.FindFootprintByReference(ref)
        if fp is None:
            fp = layout.from_library(board, component["footprint"], ref, component["value"],
                                     *positions.pop(ref))
        fp.SetValue(component["value"])
        fp.SetPath(pcb.KIID_PATH(component["path"]))
        fp.SetSheetname(component["sheetname"])
        fp.SetSheetfile(component["sheetfile"])
        fp.SetDNP(component["dnp"])
        fp.SetExcludedFromBOM(component["exclude_from_bom"])
        for key in ("Datasheet", "Description"):
            fp.SetField(key, component["fields"].get(key, ""))
        for key, value in component["fields"].items():
            if key not in ("Reference", "Value", "Footprint", "Datasheet", "Description") and value:
                fp.SetField(key, value)
                field = fp.GetField(key)
                field.SetVisible(False)
                layout.identity(field, f"{ref}/field/{key}")
        if ref in assembly.OVERRIDES:
            override = assembly.OVERRIDES[ref]
            variant = fp.AddVariant(assembly.EXTERNAL_VARIANT)
            if "dnp" in override:
                variant.SetDNP(override["dnp"])
            for field, value in override.get("fields", {}).items():
                variant.SetFieldValue(field, value)
            fp.SetVariant(variant)
        for pad in fp.Pads():
            net = pins.get((ref, pad.GetNumber()))
            if net:
                pad.SetNet(signals[net])
    assert not positions, sorted(positions)
    title = board.GetTitleBlock()
    title.SetTitle("ReBirth Rev B — main-board electrical placement")
    title.SetRevision("B")
    board.SetTitleBlock(title)
    check_board(board, mechanical, components, pins)
    # The input is the mechanical study. Its empty project would replace the real schematic settings.
    if not pcb.SaveBoard(str(target), board, True):
        raise OSError(f"Failed to write {target}")
    check_board(pcb.LoadBoard(str(target)), mechanical, components, pins)
    print(f"{len(components)} schematic footprints, {len(pins)} pin assignments; rear ports and mounting holes match")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=PROJECT.with_suffix(".kicad_pcb"))
    target = parser.parse_args().output
    if target.exists():
        existing = pcb.LoadBoard(str(target))
        if list(existing.GetTracks()) or list(existing.Zones()):
            parser.error("this board contains copper; specify a separate --output for the placement study")
    generate(target)
