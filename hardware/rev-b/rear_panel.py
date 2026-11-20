"""Build the rear wall from the saved Rev B PCB. Run with FreeCAD's Python."""

import csv
import math
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from itertools import combinations
from pathlib import Path
from xml.etree import ElementTree as ET

from FreeCAD import Vector as V
import Import
import Part

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "pcb" / "gen"))
from geometry import OX, OY

# Enclosure and board stack from hardware/panel/build_internals.py, in mm.
WIDTH, HEIGHT, WALL = 260.0, 50.0, 3.0
OUTSIDE_Y = 135.0
PCB_TOP = 3.0 + 5.0 + 1.6


@dataclass(frozen=True)
class Port:
    ref: str
    name: str
    x: float  # From the left edge as seen by a person facing the rear sockets.
    z: float  # From the bottom of the enclosure.
    width: float
    height: float
    radius: float

    @property
    def circular(self):
        return self.width == 2 * self.radius == self.height


# A footprint's relative coordinates use KiCad local X and Y (down).
# Values: footprint angle, local mating axis X, local front Y, opening centre
# above PCB, designed cut width/height/corner radius, nominal recess of front.
# The microSD and USB windows allow access around the nominal shell height.
GEOMETRY = {
    "DIN-5_180deg_SameSky_SDS-50J_Horizontal":
        (180, 0.0, 12.5, 10.0, 18.0, 18.0, 9.0, 3.5),
    "Jack_3.5mm_CUI_SJ1-3533NG_Horizontal":
        (0, 0.90, -5.20, 7.0, 7.0, 7.0, 3.5, -0.5),
    "USB_Micro-B_Amphenol_10104110_Horizontal":
        (180, 0.0, 4.05, 1.25, 12.0, 6.5, 1.2, 2.2),
    "microSD_HC_Hirose_DM3AT-SF-PEJM5":
        (180, -0.425, 8.325, 0.85, 16.0, 10.0, 3.0, 4.0),
}
GROUPS = [
    ("CV 1–4", ["CV1", "CV2", "CV3", "CV4"]),
    ("GATE 1–4", ["GATE1", "GATE2", "GATE3", "GATE4"]),
    ("microSD", ["microSD"]),
    ("USB Micro-B", ["USB Micro-B"]),
    ("MIDI IN, OUT A–D", ["MIDI IN", "MIDI OUT A", "MIDI OUT B", "MIDI OUT C", "MIDI OUT D"]),
]


def board_ports():
    cli = os.environ.get("KICAD_CLI") or shutil.which("kicad-cli") or (
        "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
    with tempfile.TemporaryDirectory() as temporary:
        positions = Path(temporary) / "main.csv"
        subprocess.run([cli, "pcb", "export", "pos", "--format", "csv", "--units", "mm",
                        "--side", "front", "-o", str(positions),
                        str(HERE / "main-placement.kicad_pcb")], check=True)
        with positions.open(newline="") as handle:
            rows = list(csv.DictReader(handle))
    ports = []
    for row in rows:
        if row["Package"] not in GEOMETRY:
            continue
        angle, dx, dy, h, width, height, radius, recess = GEOMETRY[row["Package"]]
        assert float(row["Rot"]) % 360 == angle and row["Side"] == "top", row["Ref"]
        sign = 1 if angle == 0 else -1
        case_x = float(row["PosX"]) - OX + sign * dx
        case_y = float(row["PosY"]) + OY - sign * dy
        assert math.isclose(OUTSIDE_Y - case_y, recess, abs_tol=0.001), row["Ref"]
        ports.append(Port(row["Ref"], row["Val"], round(WIDTH / 2 - case_x, 3),
                          round(PCB_TOP + h, 3), width, height, radius))
    ports.sort(key=lambda p: p.x)
    assert [p.name for p in ports] == [n for _, names in GROUPS for n in names]
    return ports


def rectangle(x0, y0, x1, y1, r=0):
    if not r:
        return Part.makePolygon([V(x0,y0,0), V(x1,y0,0), V(x1,y1,0),
                                 V(x0,y1,0), V(x0,y0,0)])
    # Four true quarter circles and straight tangents, in counterclockwise order.
    edges = []
    for cx, cy, start in [(x1-r, y0+r, 270), (x1-r, y1-r, 0),
                          (x0+r, y1-r, 90), (x0+r, y0+r, 180)]:
        arc = Part.makeCircle(r, V(cx,cy,0), V(0,0,1), start, start + 90)
        if edges:
            edges.append(Part.makeLine(edges[-1].Vertexes[-1].Point, arc.Vertexes[0].Point))
        edges.append(arc)
    edges.append(Part.makeLine(edges[-1].Vertexes[-1].Point, edges[0].Vertexes[0].Point))
    return Part.Wire(edges)


def opening(p):
    if p.circular:
        return Part.Wire([Part.makeCircle(p.radius, V(p.x,p.z,0))])
    return rectangle(p.x - p.width/2, p.z - p.height/2,
                     p.x + p.width/2, p.z + p.height/2, p.radius)


def build_cad(ports):
    outline = rectangle(0, 0, WIDTH, HEIGHT)
    holes = [opening(p) for p in ports]
    pairs = [(a.distToShape(b)[0], p.name, q.name)
             for (p, a), (q, b) in combinations(zip(ports, holes), 2)]
    gap, first, second = min(pairs)
    assert gap > 0, f"overlapping rear openings: {first}, {second}"
    assert all(w.isClosed() and outline.distToShape(w)[0] > 0 for w in holes)
    face = Part.makeFace([outline, *holes], "Part::FaceMakerBullseye")
    assert len(face.Faces) == 1 and len(face.Wires) == len(ports) + 1
    # XY of DXF is outside-view (X, Z). STEP is in the 3-D enclosure frame.
    Import.writeDXFShape([outline, *holes], str(HERE / "rear-panel.dxf"), 14, True)
    solid = face.extrude(V(0,0,WALL))
    solid.rotate(V(0,0,0), V(1,0,0), 90)
    solid.rotate(V(0,0,0), V(0,0,1), 180)
    solid.translate(V(WIDTH/2, OUTSIDE_Y - WALL, 0))
    filename = str(HERE / "rear-panel.step")
    solid.exportStep(filename)
    saved = Part.read(filename)
    bounds = saved.BoundBox
    actual = (bounds.XMin, bounds.XMax, bounds.YMin, bounds.YMax, bounds.ZMin, bounds.ZMax)
    expected = (-WIDTH/2, WIDTH/2, OUTSIDE_Y - WALL, OUTSIDE_Y, 0, HEIGHT)
    assert all(abs(a - b) < 1e-5 for a, b in zip(actual, expected))
    area = WIDTH * HEIGHT - sum(p.width * p.height - (4 - math.pi) * p.radius**2 for p in ports)
    assert saved.isValid() and len(saved.Solids) == 1
    assert math.isclose(saved.Volume, area * WALL, abs_tol=1e-5)
    for y in (OUTSIDE_Y - WALL, OUTSIDE_Y):
        end, = [f for f in saved.Faces if f.BoundBox.YLength < 1e-5 and abs(f.CenterOfMass.y - y) < 1e-5]
        assert len(end.Wires) == len(ports) + 1
    for p in ports:
        centre = V(WIDTH/2 - p.x, OUTSIDE_Y - WALL/2, p.z)
        assert not saved.isInside(centre, 1e-6, True), p.ref
        assert saved.isInside(centre + V(0,0,p.height/2 + 0.25), 1e-6, True), p.ref
    print(f"{len(holes)} rear openings; smallest web {gap:.3f} mm ({first}, {second}); "
          f"STEP volume {saved.Volume:.3f} mm³")


def number(value):
    return f"{value:.3f}".rstrip("0").rstrip(".").replace(".", ",")


def drawing(ports):
    svg = ET.Element("svg", xmlns="http://www.w3.org/2000/svg", width="297mm", height="210mm",
                     viewBox="0 0 297 210")
    def element(tag, **attrs):
        return ET.SubElement(svg, tag, {k.replace("_", "-"): str(v) for k, v in attrs.items()})
    def label(x, y, string, size=3.1, weight="normal", anchor="start", color="#20392f"):
        element("text", x=x, y=y, font_family="Arial, sans-serif", font_size=size,
                font_weight=weight, text_anchor=anchor, fill=color).text = string
    def line(x1, y1, x2, y2, color="#70867d", width=0.22):
        element("line", x1=x1, y1=y1, x2=x2, y2=y2, stroke=color, stroke_width=width)
    def box(x, y, w, h, fill, stroke="none", radius=0):
        element("rect", x=x, y=y, width=w, height=h, rx=radius, fill=fill,
                stroke=stroke, stroke_width=0.3)

    ink, green = "#20392f", "#426354"
    left, bottom = (297-WIDTH)/2, 108
    top, right = bottom-HEIGHT, left+WIDTH
    box(0, 0, 297, 210, "#fff")
    label(left, 18, "ReBirth", 5.0, "bold")
    label(right, 18, "КОРПУС   /   REV B", 3.5, "bold", "end", green)
    line(left, 23, right, 23)
    label(left, 34, "Задняя стенка", 8.0, "bold")
    label(left, 42, "Вид снаружи, со стороны штекеров. Все размеры в миллиметрах.", 3.9)
    for x in (left, right):
        line(x, top-1, x, top-9)
        line(x-1, top-5, x+1, top-7)
    line(left, top-6, right, top-6)
    box(140.5, top-9, 16, 5, "white")
    label(148.5, top-5, number(WIDTH), 3.6, "bold", "middle")
    for y in (top, bottom):
        line(left-8, y, left-1, y)
        line(left-7, y+1, left-5, y-1)
    line(left-6, top, left-6, bottom)
    box(left-10, top+22.5, 8, 5, "white")
    label(left-6, top+26.3, number(HEIGHT), 3.4, "bold", "middle")
    box(left, top, WIDTH, HEIGHT, "#eff3ef", ink)
    for group, names in GROUPS:
        subset = [p for p in ports if p.name in names]
        if len(subset) > 1:
            title = "MIDI" if group.startswith("MIDI") else group.split()[0]
            label(left + (subset[0].x + subset[-1].x)/2, top+8, title, 3.5, "bold", "middle", green)
    for p in ports:
        x, y = left+p.x, bottom-p.z
        if p.circular:
            element("circle", cx=x, cy=y, r=p.radius, fill="white", stroke=ink, stroke_width=0.35)
        else:
            box(x-p.width/2, y-p.height/2, p.width, p.height, "white", ink, p.radius)
        line(x-1, y, x+1, y)
        line(x, y-1, x, y+1)
        short = p.name.replace("MIDI ", "").replace("CV", "").replace("GATE", "").replace("USB Micro-B", "USB")
        label(x, y-p.height/2-2.4, short, 3.0, "bold", "middle")
    label(left, 117, "Начало координат — левый нижний угол стенки. X вправо, Z вверх.", 3.5)
    columns = (left+2, 68, 190, 224)
    box(left, 124, WIDTH, 8, "#e7eee8")
    for x, heading in zip(columns, ("Разъём", "X центров слева направо", "Z центра", "Сквозной вырез")):
        label(x, 129.4, heading, 3.3, "bold")
    for index, (group, names) in enumerate(GROUPS):
        row_y = 138 + index * 7
        subset = [p for p in ports if p.name in names]
        p = subset[0]
        cut = "Ø" + number(p.width) if p.circular else (
              f"{number(p.width)} × {number(p.height)}   R{number(p.radius)}")
        for x, value in zip(columns, (group, "  ·  ".join(number(q.x) for q in subset), number(p.z), cut)):
            label(x, row_y, value, 3.45)
        line(left, row_y+2.1, right, row_y+2.1, "#d8e1d9", 0.18)
    label(left, 181, "Толщина стенки 3 мм. Шаблон для примерки, М 1:1 на A4.", 3.8, "bold")
    label(left, 189, "Печать 100 % без подгонки. После печати измерьте контрольный отрезок.", 3.4)
    line(right-50, 187, right, 187, ink, 0.35)
    for x in (right-50, right):
        line(x, 185, x, 189, ink, 0.35)
    label(right-25, 183.5, "50 мм", 3.3, "bold", "middle")
    ET.indent(svg)
    ET.ElementTree(svg).write(HERE / "rear-panel.svg", encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    ports = board_ports()
    build_cad(ports)
    drawing(ports)
