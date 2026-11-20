"""Build and check the two Rev B mechanical studies with KiCad 10 pcbnew.

Coordinates in the design are millimetres in the enclosure frame, x to the
right, y to the rear. The panel uses its own sloping plane, like Rev A.
"""

import math
import os
import sys
import uuid
from itertools import combinations
from pathlib import Path

import pcbnew as pcb
from assembly import DEFAULT_MCU

HERE = Path(__file__).resolve().parent
PCB = HERE.parent / "pcb"
sys.path.insert(0, str(PCB / "gen"))
import geometry as case

LIBS = Path(os.environ.get("KICAD10_FOOTPRINT_DIR",
            "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"))
DIN = "rebirth:DIN-5_180deg_SameSky_SDS-50J_Horizontal"
JACK = "rebirth:Jack_3.5mm_CUI_SJ1-3533NG_Horizontal"
USB = "rebirth:USB_Micro-B_Amphenol_10104110_Horizontal"
SD = "Connector_Card:microSD_HC_Hirose_DM3AT-SF-PEJM5"
MCU = "rebirth:RP2350B_QFN80_Minimal_R4"
TACT = "Button_Switch_THT:SW_PUSH_6mm"

# Rear wall. Preserve the five connector identities across electrical
# sheets, panel engraving and the PCB. Positions refer to the mating axes.
DIN_AXES = [("J405", "OUT D", -103.53), ("J404", "OUT C", -82.00),
            ("J403", "OUT B", -60.47), ("J401", "OUT A", -31.25),
            ("J402", "IN", -9.50)]
JACK_AXES = [("J%d" % (508 - i), name, 37.0 + 11.4 * i)
             for i, name in enumerate(("GATE4", "GATE3", "GATE2", "GATE1",
                                       "CV4", "CV3", "CV2", "CV1"))]
LCD_X, LCD_Y = -86.25, 104.0
LCD_W, LCD_H = 83.0, 54.5  # ER-TFT035IPS-6, no touch, folded FPC
BUTTONS = [("SW604", "REC", 41.0, 100.0), ("SW605", "CYCLE", 80.0, 100.0)]


def point(x, y):
    kx, ky = case.K(x, y)
    return pcb.VECTOR2I(pcb.FromMM(kx), pcb.FromMM(ky))


def identity(item, key):
    item.SetUuid(pcb.KIID(str(uuid.uuid5(uuid.NAMESPACE_URL, "rebirth/rev-b/" + key))))


def from_library(board, lib_id, ref, value, x, y, angle=0):
    lib, name = lib_id.split(":")
    directory = PCB / "rebirth.pretty" if lib == "rebirth" else LIBS / (lib + ".pretty")
    fp = pcb.FootprintLoad(str(directory), name)
    if fp is None:
        raise ValueError(f"Missing footprint {lib_id} for {ref} in {directory}")
    fp.SetFPID(pcb.LIB_ID(lib, name))
    fp.SetReference(ref)
    fp.SetValue(value)
    # Library item UUIDs belong to the template. Each instance and its children
    # need distinct IDs for the board editor and stable regeneration.
    identity(fp, ref)
    for i, item in enumerate([*fp.GetFields(), *fp.Pads(), *fp.GraphicalItems(), *fp.Zones()]):
        identity(item, f"{ref}/{i}")
    fp.SetOrientationDegrees(angle)
    fp.SetPosition(point(x, y))
    fp.Reference().SetLayer(pcb.F_Fab)
    fp.Value().SetVisible(False)
    board.Add(fp)
    return fp


def copy_footprint(board, old):
    fp = pcb.FOOTPRINT(old)
    for pad in fp.Pads():
        pad.SetNetCode(0)
    fp.SetPath(pcb.KIID_PATH())
    fp.Reference().SetLayer(pcb.B_Fab if fp.IsFlipped() else pcb.F_Fab)
    fp.Value().SetVisible(False)
    identity(fp, old.GetReference())
    for i, item in enumerate([*fp.GetFields(), *fp.Pads(), *fp.GraphicalItems(), *fp.Zones()]):
        identity(item, f"{old.GetReference()}/{i}")
    board.Add(fp)
    return fp


def case_rect(board, key, x0, y0, x1, y1):
    drawing = pcb.PCB_SHAPE(board)
    drawing.SetShape(pcb.SHAPE_T_RECT)
    drawing.SetStart(point(x0, y1))
    drawing.SetEnd(point(x1, y0))
    drawing.SetLayer(pcb.Dwgs_User)
    drawing.SetWidth(pcb.FromMM(0.15))
    identity(drawing, key)
    board.Add(drawing)


def starting_board(name):
    old = pcb.LoadBoard(str(PCB / name / (name + ".kicad_pcb")))
    board = pcb.BOARD()
    title = pcb.TITLE_BLOCK()
    title.SetTitle("ReBirth " + name + " – mechanical placement study")
    title.SetRevision("B · layout study")
    board.SetTitleBlock(title)
    for shape in old.GetDrawings():
        if shape.GetLayer() == pcb.Edge_Cuts:
            dup = shape.Duplicate()
            identity(dup, name + "/" + shape.m_Uuid.AsString())
            board.Add(dup)
    for fp in old.GetFootprints():
        ref = fp.GetReference()
        if ref.startswith("H") or (name == "panel" and
                (ref.startswith("SW") or ref in {"D982", "J1201"})) or ref == "J1101":
            copy_footprint(board, fp)
    return old, board


def populate_main(board):
    rear = case.MAIN_Y1
    for ref, name, x in DIN_AXES:
        from_library(board, DIN, ref, "MIDI " + name, x, rear - 12.5, 180)
    for ref, name, x in JACK_AXES:
        # Same Sky's bushing axis is at x=+0.90 in the KiCad footprint.
        # Seat the housing at the PCB edge; its 4 mm bushing enters the wall.
        from_library(board, JACK, ref, name, x - 0.90, rear - 1.20)
    # The USB footprint marks its suggested PCB edge at local y=2.75.
    # The Hirose mouth is at +8.325. Set it back 0.5 for the rear shield lands.
    from_library(board, USB, "J301", "USB Micro-B", 7.0, rear - 2.75, 180)
    from_library(board, SD, "J302", "microSD", 23.0, rear - 8.325 - 0.5, 180)
    from_library(board, MCU, "U301", DEFAULT_MCU, 3.0, 103.0)


def populate_panel(board):
    for ref, name, x, y in BUTTONS:
        fp = from_library(board, TACT, ref, name, x, y)
        # The switch origin is a lead, not the centre of the actuator.
        box = fp.GetCourtyard(pcb.F_CrtYd).BBox()
        fp.Move(point(x, y) - box.GetCenter())
    case_rect(board, "lcd-glass", LCD_X - LCD_W / 2, LCD_Y - LCD_H / 2,
              LCD_X + LCD_W / 2, LCD_Y + LCD_H / 2)


def bounds(box):
    return tuple(map(pcb.ToMM, (box.GetLeft(), box.GetTop(), box.GetRight(), box.GetBottom())))


def clearance(a, b):
    # A lower bound for shape-to-shape separation of two disjoint bounding boxes.
    x = max(a[0] - b[2], b[0] - a[2], 0)
    y = max(a[1] - b[3], b[1] - a[3], 0)
    return math.hypot(x, y)


def holes(board):
    return {fp.GetReference(): (tuple(fp.GetPosition()),
                               sorted((tuple(p.GetPosition()), tuple(p.GetDrillSize()))
                                      for p in fp.Pads() if p.GetDrillSize().x))
            for fp in board.GetFootprints() if fp.GetReference().startswith("H")}


def outline(board):
    return sorted((s.GetShape(), tuple(s.GetStart()), tuple(s.GetEnd()),
                   tuple(s.GetArcMid()) if s.GetShape() == pcb.SHAPE_T_ARC else None)
                  for s in board.GetDrawings() if s.GetLayer() == pcb.Edge_Cuts)


def check(name, old, board):
    assert outline(board) == outline(old), f"{name} outline changed"
    assert holes(board) == holes(old), f"{name} mounting pattern changed"
    assert len(holes(board)) == (7 if name == "main" else 17)
    min_gap = math.inf
    closest = None
    all_boxes = {}
    for fp in board.GetFootprints():
        courtyard = fp.GetCourtyard(pcb.B_CrtYd if fp.IsFlipped() else pcb.F_CrtYd)
        assert courtyard.OutlineCount(), fp.GetReference()
        all_boxes[fp.GetReference()] = bounds(courtyard.BBox())
        if name == "main" and fp.GetReference().startswith(("J", "U")):
            left, top = point(case.MAIN_X0, case.MAIN_Y1)
            right, bottom = point(case.MAIN_X1, case.MAIN_Y0)
            for pad in fp.Pads():
                box = pad.GetBoundingBox()
                margin = min(box.GetLeft()-left, right-box.GetRight(),
                             box.GetTop()-top, bottom-box.GetBottom())
                assert margin >= pcb.FromMM(0.25), f"{fp.GetReference()}.{pad.GetNumber()} near edge"
    if name == "panel":
        corners = (point(LCD_X - LCD_W / 2, LCD_Y + LCD_H / 2),
                   point(LCD_X + LCD_W / 2, LCD_Y - LCD_H / 2))
        all_boxes["LCD"] = tuple(pcb.ToMM(v) for corner in corners for v in corner)
        assert LCD_X - LCD_W / 2 >= -130 and LCD_Y + LCD_H / 2 < math.hypot(270, 22) / 2
        tilt = math.atan2(22, 270)
        # Glass flush under plate, thickness 2.3 mm. Inner rear wall is y=132.
        rear_glass = (LCD_Y + LCD_H/2) * math.cos(tilt) + 2.3 * math.sin(tilt)
        assert rear_glass < 132
        # Top plate and cap travel, rather than the smaller switch courtyards.
        assert LCD_Y - LCD_H / 2 > case.GRID_Y0 + case.PAD / 2
    for (ref_a, a), (ref_b, b) in combinations(all_boxes.items(), 2):
        if name == "panel":
            if not ({ref_a, ref_b} & {"LCD", "SW604", "SW605"}):
                continue
            if "LCD" in (ref_a, ref_b) and "J1201" in (ref_a, ref_b):
                continue  # IDC below PCB; LCD sits immediately below top plate
        gap = clearance(a, b)
        assert gap > 0, f"{name}: projected interference {ref_a}, {ref_b}"
        if gap < min_gap:
            min_gap, closest = gap, (ref_a, ref_b)
    print(f"{name}: original outline and {len(holes(board))} mounts match; "
          f"minimum checked XY clearance {min_gap:.2f} mm at {' / '.join(sorted(closest))}")


def preview(boards):
    """Draw the upper controls and rear connector row from the placement data."""
    from xml.etree.ElementTree import Element, SubElement, indent, tostring
    svg = Element("svg", xmlns="http://www.w3.org/2000/svg", viewBox="0 0 1040 758")
    def shape(tag, **args):
        return SubElement(svg, tag, {k: str(v) for k, v in args.items()})
    def label(x, y, value, size=16, color="#24382f", weight=400, anchor="start"):
        shape("text", x=x, y=y, fill=color, **{"font-family":"Arial, sans-serif", "font-size":size,
              "font-weight":weight, "text-anchor":anchor}).text = value
    def rect(x, y, w, h, fill, stroke="none", radius=0):
        shape("rect", x=x, y=y, width=w, height=h, rx=radius, fill=fill, stroke=stroke)
    def circle(x, y, radius, fill, stroke):
        shape("circle", cx=x, cy=y, r=radius, fill=fill, stroke=stroke)
    def sx(x): return 520 + 3.62 * x
    def top(y): return 140 + 3.62 * (135.45 - y)
    def main_y(y): return 486 + 3.62 * (131.5 - y)
    def mounts(board, to_y, ymin):
        for ref in holes(board):
            p = board.FindFootprintByReference(ref).GetPosition()
            x, y = pcb.ToMM(p.x) - case.OX, case.OY - pcb.ToMM(p.y)
            if y >= ymin:
                circle(sx(x), to_y(y), 3.5 * 3.62, "#e4dfcd", "#948761")
                circle(sx(x), to_y(y), 1.6 * 3.62, "#fffdf6", "#948761")
    rect(0, 0, 1040, 758, "#faf9f4")
    label(48, 52, "ReBirth  /  Rev B", 34, weight=700)
    label(48, 83, "Все разъёмы и слот microSD выходят на заднюю стенку.", 19)
    label(48, 121, "ВЕРХНЯЯ ПАНЕЛЬ", 14, weight=700)
    rect(sx(-130), top(135.45), 260*3.62, 70.45*3.62, "#e7e8df", "#738374", 5)
    mounts(boards["panel"], top, 69)
    left, upper = sx(LCD_X - LCD_W/2), top(LCD_Y + LCD_H/2)
    rect(left, upper, LCD_W*3.62, LCD_H*3.62, "#28483e", "#172f28", 1.5)
    label(sx(LCD_X), top(LCD_Y)-3, "3,5″ TFT", 27, "#f3f4e9", 700, "middle")
    label(sx(LCD_X), top(LCD_Y)+24, "480 × 320", 18, "#c4d9c7", anchor="middle")
    for (x, y), name in zip(case.ENCODERS, ("TRACK", "VALUE")):
        circle(sx(x), top(y), 10*3.62, "#6a716b", "#414941")
        label(sx(x), top(y)+5, name, 12, "white", 700, "middle")
    for x, y, size, name, fill in [(case.PLAY_C[0], case.PLAY_C[1], 17, "PLAY", "#c5d9c0")] + [
            (x, y, 8, name, "#d4a993" if name == "REC" else "#bfcfd8") for _, name, x, y in BUTTONS]:
        rect(sx(x-size/2), top(y+size/2), size*3.62, size*3.62, fill, "#68786d", 3)
        label(sx(x), top(y)+47, name, 13, weight=700, anchor="middle")
    circle(sx(case.BEAT_C[0]), top(case.BEAT_C[1]), 5, "#91af8f", "#6d866b")
    for x in [case.col_x(c) for c in range(8)] + [case.SCENE_X]:
        rect(sx(x-8.5), top(case.GRID_Y0+8.5), 17*3.62, 13, "#cbd1c6", "#97a195", 2)
    label(48, 437, "ОСНОВНАЯ ПЛАТА", 14, weight=700)
    label(48, 464, "Вид сверху. Разъёмы направлены к задней стенке.", 16)
    rect(sx(-122), main_y(131.5), 244*3.62, 57.5*3.62, "#edf0e7", "#6e8876", 7)
    mounts(boards["main"], main_y, 0)
    ports = [(ref, name) for ref, name, _ in DIN_AXES]
    ports += [("J301", "USB"), ("J302", "SD")]
    ports += [(ref, name) for ref, name, _ in JACK_AXES]
    for ref, name in ports:
        fp = boards["main"].FindFootprintByReference(ref)
        x0, y0, x1, y1 = bounds(fp.GetCourtyard(pcb.F_CrtYd).BBox())
        x = sx(x0 - case.OX)
        y = main_y(case.OY - y0)
        w, h = (x1-x0)*3.62, (y1-y0)*3.62
        fill = "#afc8b9" if ref.startswith("J4") else "#d7c4a5" if ref.startswith("J3") else "#c5cfdb"
        rect(x, y, w, h, fill, "#66756d", 2)
        label(x+w/2, y+h/2+4, name.replace("GATE", "G"), 12, weight=700, anchor="middle")
    for ref, name in (("U301", "RP2350B"), ("J1101", "IDC")):
        fp = boards["main"].FindFootprintByReference(ref)
        x0, y0, x1, y1 = bounds(fp.GetCourtyard(pcb.F_CrtYd).BBox())
        x, y = sx(x0-case.OX), main_y(case.OY-y0)
        w, h = (x1-x0)*3.62, (y1-y0)*3.62
        rect(x, y, w, h, "#d7dfd1", "#7e8b79", 1)
        label(x+w/2, y+h+15, name, 12, weight=700, anchor="middle")
    label(48, 732, "244 × 57,5 мм  ·  7 креплений основной платы  ·  17 креплений панели", 16)
    indent(svg)
    (HERE / "layout.svg").write_bytes(tostring(svg, encoding="utf-8", xml_declaration=True))


def main():
    boards = {}
    for name in ("main", "panel"):
        old, board = starting_board(name)
        if name == "main":
            populate_main(board)
        else:
            populate_panel(board)
        check(name, old, board)
        path = HERE / (name + "-placement.kicad_pcb")
        pcb.SaveBoard(str(path), board)
        check(name, old, pcb.LoadBoard(str(path)))
        boards[name] = board
    preview(boards)


if __name__ == "__main__":
    main()
