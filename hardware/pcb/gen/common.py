"""Shared helpers for the two board generators (run with KiCad's own Python, pcbnew).

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import json
import math
import os
import re
import subprocess

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.dirname(HERE)
HW = os.path.dirname(PCB)
KICAD_CLI = os.environ.get("KICAD_CLI", "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
FP_DIR = os.environ.get("KICAD_FOOTPRINT_DIR", "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints")
PROJECT_FP = {"rebirth": os.path.join(PCB, "rebirth.pretty")}

MM = pcbnew.FromMM

# JLCPCB 2-layer standard process, used with margin (capabilities: 0.127 mm track/space,
# 0.3 mm via drill, 0.25 mm annular ring... see hardware/pcb/README.md)
RULES = {
    "min_clearance": 0.15, "min_track_width": 0.15, "min_via_diameter": 0.6, "min_via_annular_width": 0.15,
    "min_through_hole_diameter": 0.3, "min_hole_to_hole": 0.5, "min_hole_clearance": 0.25,
    "min_copper_edge_clearance": 0.25, "min_silk_clearance": 0.0, "min_text_height": 0.8,
    "min_text_thickness": 0.12, "solder_mask_to_copper_clearance": 0.0, "min_microvia_diameter": 0.2,
    "min_microvia_drill": 0.1, "min_resolved_spokes": 1, "min_groove_width": 0.0,
    "max_error": 0.005, "min_connection": 0.0, "use_height_for_length_calcs": True,
}
NETCLASSES = [
    # name, track, clearance, via diameter, via drill, nets (patterns)
    ("Default", 0.2, 0.2, 0.6, 0.3, []),
    ("Power", 0.5, 0.2, 0.8, 0.4, ["+3V3", "VBUS", "GND", "/Power/LED_EN"]),
    ("LED", 0.8, 0.2, 0.8, 0.4, ["+5V_LED"]),
]


# ------------------------------------------------------------------ netlist

def sexp(text):
    tok = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+')
    stack = [[]]
    for m in tok.finditer(text):
        t = m.group(0)
        if t == "(":
            stack.append([])
        elif t == ")":
            done = stack.pop()
            stack[-1].append(done)
        elif t.startswith('"'):
            stack[-1].append(t[1:-1].replace('\\"', '"'))
        else:
            stack[-1].append(t)
    return stack[0][0]


def find(node, key):
    for c in node[1:]:
        if isinstance(c, list) and c and c[0] == key:
            return c
    return None


def find_all(node, key):
    return [c for c in node[1:] if isinstance(c, list) and c and c[0] == key]


def export_netlist(sch_path, out_path):
    subprocess.run([KICAD_CLI, "sch", "export", "netlist", "--format", "kicadsexpr", "-o", out_path, sch_path],
                   check=True, capture_output=True)
    tree = sexp(open(out_path, encoding="utf-8").read())
    comps = {}
    for c in find_all(find(tree, "components"), "comp"):
        ref = find(c, "ref")[1]
        sheet = find(c, "sheetpath")
        tstamps = find(c, "tstamps")
        props = {p[1][1]: p[2][1] for p in find_all(c, "property") if len(p) > 2 and isinstance(p[2], list)}
        fields = {}
        fl = find(c, "fields")
        if fl:
            for f in find_all(fl, "field"):
                fname = find(f, "name")[1]
                fields[fname] = f[2] if len(f) > 2 and isinstance(f[2], str) else ""
        prop_names = {p[1][1] for p in find_all(c, "property") if isinstance(p[1], list)}
        comps[ref] = {
            "exclude_from_bom": "exclude_from_bom" in prop_names,
            "fields": fields,
            "value": find(c, "value")[1],
            "footprint": (find(c, "footprint") or [None, ""])[1],
            "path": find(sheet, "tstamps")[1] + tstamps[1],
            "sheetname": find(sheet, "names")[1],
            "sheetfile": props.get("Sheetfile", ""),
            "dnp": find(c, "property") is not None and any(
                p[1][1] == "dnp" for p in find_all(c, "property") if isinstance(p[1], list)),
        }
    nets = {}
    pin_net = {}
    for n in find_all(find(tree, "nets"), "net"):
        name = find(n, "name")[1]
        nodes = [(find(x, "ref")[1], find(x, "pin")[1]) for x in find_all(n, "node")]
        nets[name] = nodes
        for node in nodes:
            pin_net[node] = name
    return comps, nets, pin_net


# ------------------------------------------------------------------ board

def load_fp(lib_name):
    lib, name = lib_name.split(":", 1)
    path = PROJECT_FP.get(lib) or os.path.join(FP_DIR, lib + ".pretty")
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None:
        raise RuntimeError("footprint %s not found in %s" % (lib_name, path))
    return fp


class Board:
    """A board being generated: footprints by reference, nets by name, helpers for copper."""

    def __init__(self, comps, nets, pin_net):
        self.b = pcbnew.CreateEmptyBoard()
        self.b.SetCopperLayerCount(2)
        self.comps, self.nets_by_name, self.pin_net = comps, nets, pin_net
        self.net = {}
        for name in sorted(nets):
            ni = pcbnew.NETINFO_ITEM(self.b, name)
            self.b.Add(ni)
            self.net[name] = ni
        self.fp = {}
        for ref, c in sorted(comps.items()):
            fp = load_fp(c["footprint"])
            lib, name = c["footprint"].split(":", 1)
            fp.SetFPID(pcbnew.LIB_ID(lib, name))
            for fname in ("Datasheet", "Description"):
                if fname in c["fields"]:
                    fp.SetField(fname, c["fields"][fname])
            fp.SetReference(ref)
            fp.SetValue(c["value"])
            fp.SetPath(pcbnew.KIID_PATH(c["path"]))
            fp.SetSheetname(c["sheetname"])
            fp.SetSheetfile(c["sheetfile"])
            self.b.Add(fp)
            for pad in fp.Pads():
                name = pin_net.get((ref, pad.GetNumber()))
                if name:
                    pad.SetNet(self.net[name])
            self.fp[ref] = fp

    # ---- placement
    def place(self, ref, x, y, rot=0.0, bottom=False):
        """Footprint origin at (x, y) mm (KiCad coordinates), rotation in degrees as seen from the top."""
        fp = self.fp[ref]
        if bottom and not fp.IsFlipped():
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        fp.SetOrientationDegrees(rot)
        fp.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
        return fp

    def place_center(self, ref, x, y, rot=0.0, bottom=False):
        """Place so that the centre of the pads' bounding box lands on (x, y)."""
        fp = self.place(ref, x, y, rot, bottom)
        cx, cy = self.pads_center(ref)
        fp.SetPosition(pcbnew.VECTOR2I(MM(2 * x - cx), MM(2 * y - cy)))
        return fp

    def pads_center(self, ref):
        pads = [p for p in self.fp[ref].Pads() if p.GetNumber()]
        xs = [pcbnew.ToMM(p.GetPosition().x) for p in pads]
        ys = [pcbnew.ToMM(p.GetPosition().y) for p in pads]
        return (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2

    def place_two(self, ref, p1, p2, bottom=False):
        """Two-pad part: pad '1' near p1, pad '2' near p2 (chooses the rotation, centres between)."""
        ang = math.degrees(math.atan2(-(p2[1] - p1[1]), p2[0] - p1[0]))
        best = None
        for rot in (0, 90, 180, 270):
            self.place(ref, (p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2, rot, bottom)
            a, b = self.pad(ref, "1"), self.pad(ref, "2")
            d = math.hypot(a[0] - p1[0], a[1] - p1[1]) + math.hypot(b[0] - p2[0], b[1] - p2[1])
            if best is None or d < best[0]:
                best = (d, rot)
        self.place(ref, (p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2, best[1], bottom)
        cx, cy = self.pads_center(ref)
        fp = self.fp[ref]
        pos = fp.GetPosition()
        fp.SetPosition(pcbnew.VECTOR2I(pos.x + MM((p1[0] + p2[0]) / 2 - cx), pos.y + MM((p1[1] + p2[1]) / 2 - cy)))
        return ang

    def pad(self, ref, number):
        for p in self.fp[ref].Pads():
            if p.GetNumber() == number:
                q = p.GetPosition()
                return pcbnew.ToMM(q.x), pcbnew.ToMM(q.y)
        raise KeyError("%s pad %s" % (ref, number))

    def pad_net(self, ref, number):
        return self.pin_net.get((ref, number))

    # ---- copper
    def track(self, net, pts, layer, width):
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            if abs(x1 - x2) < 1e-6 and abs(y1 - y2) < 1e-6:
                continue
            t = pcbnew.PCB_TRACK(self.b)
            t.SetStart(pcbnew.VECTOR2I(MM(x1), MM(y1)))
            t.SetEnd(pcbnew.VECTOR2I(MM(x2), MM(y2)))
            t.SetLayer(layer)
            t.SetWidth(MM(width))
            t.SetNet(self.net[net])
            t.SetLocked(True)
            self.b.Add(t)

    def via(self, net, x, y, dia=0.6, drill=0.3):
        v = pcbnew.PCB_VIA(self.b)
        v.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
        v.SetWidth(MM(dia))
        v.SetDrill(MM(drill))
        v.SetNet(self.net[net])
        v.SetLocked(True)
        self.b.Add(v)

    def path(self, net, pts, width=0.25):
        """pts: (x, y, layer) waypoints; a via is added wherever the layer changes."""
        run = [pts[0]]
        for p in pts[1:]:
            if p[2] != run[-1][2]:
                self.track(net, [(q[0], q[1]) for q in run], run[-1][2], width)
                if (p[0], p[1]) != (run[-1][0], run[-1][1]):
                    raise ValueError("layer change must stay in place: %s -> %s" % (run[-1], p))
                self.via(net, p[0], p[1])
                run = [p]
            else:
                run.append(p)
        self.track(net, [(q[0], q[1]) for q in run], run[-1][2], width)

    # ---- outline, holes, zones, text
    def outline(self, x0, y0, x1, y1, r=3.0):
        def seg(a, b):
            s = pcbnew.PCB_SHAPE(self.b)
            s.SetShape(pcbnew.SHAPE_T_SEGMENT)
            s.SetStart(pcbnew.VECTOR2I(MM(a[0]), MM(a[1])))
            s.SetEnd(pcbnew.VECTOR2I(MM(b[0]), MM(b[1])))
            s.SetLayer(pcbnew.Edge_Cuts)
            s.SetWidth(MM(0.05))
            self.b.Add(s)

        def arc(c, start, end):
            s = pcbnew.PCB_SHAPE(self.b)
            s.SetShape(pcbnew.SHAPE_T_ARC)
            s.SetCenter(pcbnew.VECTOR2I(MM(c[0]), MM(c[1])))
            s.SetStart(pcbnew.VECTOR2I(MM(start[0]), MM(start[1])))
            s.SetEnd(pcbnew.VECTOR2I(MM(end[0]), MM(end[1])))
            s.SetLayer(pcbnew.Edge_Cuts)
            s.SetWidth(MM(0.05))
            self.b.Add(s)
        seg((x0 + r, y0), (x1 - r, y0))
        seg((x1, y0 + r), (x1, y1 - r))
        seg((x1 - r, y1), (x0 + r, y1))
        seg((x0, y1 - r), (x0, y0 + r))
        arc((x1 - r, y0 + r), (x1 - r, y0), (x1, y0 + r))
        arc((x1 - r, y1 - r), (x1, y1 - r), (x1 - r, y1))
        arc((x0 + r, y1 - r), (x0 + r, y1), (x0, y1 - r))
        arc((x0 + r, y0 + r), (x0, y0 + r), (x0 + r, y0))
        self.box = (x0, y0, x1, y1)

    def board_only(self, lib_name, ref, x, y, net=None, bottom=False, rot=0.0):
        fp = load_fp(lib_name)
        fp.SetReference(ref)
        fp.SetValue(lib_name.split(":")[1])
        fp.SetBoardOnly(True)
        fp.SetExcludedFromBOM(True)
        fp.SetExcludedFromPosFiles(True)
        self.b.Add(fp)
        if bottom:
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
        fp.SetOrientationDegrees(rot)
        fp.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
        if net:
            for p in fp.Pads():
                p.SetNet(self.net[net])
        fp.Reference().SetVisible(False)
        self.fp[ref] = fp
        return fp

    def zone(self, net, layer, inset=0.5, priority=0, clearance=0.25, min_width=0.25):
        x0, y0, x1, y1 = self.box
        z = pcbnew.ZONE(self.b)
        z.SetLayer(layer)
        z.SetNet(self.net[net])
        ol = z.Outline()
        ol.NewOutline()
        for px, py in ((x0 + inset, y0 + inset), (x1 - inset, y0 + inset), (x1 - inset, y1 - inset),
                       (x0 + inset, y1 - inset)):
            ol.Append(MM(px), MM(py))
        z.SetLocalClearance(MM(clearance))
        z.SetMinThickness(MM(min_width))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetThermalReliefGap(MM(0.3))
        z.SetThermalReliefSpokeWidth(MM(0.4))
        z.SetAssignedPriority(priority)
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        self.b.Add(z)
        return z

    def text(self, s, x, y, layer=pcbnew.F_SilkS, size=1.5, thick=0.2, rot=0.0, mirror=None):
        t = pcbnew.PCB_TEXT(self.b)
        t.SetText(s)
        t.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
        t.SetLayer(layer)
        t.SetTextSize(pcbnew.VECTOR2I(MM(size), MM(size)))
        t.SetTextThickness(MM(thick))
        t.SetTextAngleDegrees(rot)
        if mirror is None:
            mirror = layer in (pcbnew.B_SilkS, pcbnew.B_Cu, pcbnew.B_Fab)
        t.SetMirrored(mirror)
        self.b.Add(t)
        return t

    def refs_to_fab(self, refs):
        """Reference designators of dense parts go to the fab layer (assembly drawing), not the silk."""
        for ref in refs:
            fp = self.fp[ref]
            r = fp.Reference()
            r.SetLayer(pcbnew.B_Fab if fp.IsFlipped() else pcbnew.F_Fab)

    def save(self, path):
        self.b.SetFileName(path)
        pcbnew.SaveBoard(path, self.b)


# ------------------------------------------------------------------ project settings

def write_project_rules(pro_path, extra_classes=()):
    """Board rules and net classes into the .kicad_pro, keeping what the schematic generator wrote."""
    pro = json.load(open(pro_path)) if os.path.exists(pro_path) else {}
    board = pro.setdefault("board", {})
    ds = board.setdefault("design_settings", {})
    ds["rules"] = dict(RULES)
    ds.setdefault("defaults", {})
    ds["track_widths"] = [0.0, 0.2, 0.25, 0.3, 0.5, 0.8, 1.0]
    ds["via_dimensions"] = [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.6, "drill": 0.3},
                            {"diameter": 0.8, "drill": 0.4}]
    ds["rule_severities"] = {
        # footprint library checks are about the installed KiCad library, not this board
        "lib_footprint_issues": "ignore", "lib_footprint_mismatch": "ignore",
    }
    classes, patterns = [], []
    for name, track, clr, vd, vdr, nets in list(NETCLASSES) + list(extra_classes):
        classes.append({"name": name, "track_width": track, "clearance": clr, "via_diameter": vd,
                        "via_drill": vdr, "microvia_diameter": 0.3, "microvia_drill": 0.1,
                        "diff_pair_width": 0.2, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25,
                        "wire_width": 6, "bus_width": 12, "line_style": 0, "pcb_color": "rgba(0, 0, 0, 0.000)",
                        "schematic_color": "rgba(0, 0, 0, 0.000)", "priority": 2147483647 if name == "Default" else 0})
        for n in nets:
            patterns.append({"netclass": name, "pattern": n})
    pro["net_settings"] = {"classes": classes, "meta": {"version": 4}, "net_colors": None,
                           "netclass_assignments": None, "netclass_patterns": patterns}
    with open(pro_path, "w") as f:
        json.dump(pro, f, indent=2)
        f.write("\n")


DRU = """(version 1)

# The reverse-mount SK6812MINI-E footprint puts its pads 0.2-0.25 mm from its own light cutout;
# JLCPCB routes to 0.2 mm from an outline. Everything else keeps the board-wide 0.25 mm.
# Copper is routed at 0.2 mm (net classes, Freerouting and gen/maze.py); the check holds the
# board to 0.15 mm, still above JLCPCB's 0.127 mm minimum, so a router's rounding by a few
# micrometres is not reported.
(rule "fab clearance"
  (constraint clearance (min 0.15mm)))

(rule "SK6812MINI-E pads at their cutout"
  (constraint edge_clearance (min 0.2mm))
  (condition "A.memberOfFootprint('D9*')"))
"""


def write_dru(pro_path):
    with open(pro_path[:-len(".kicad_pro")] + ".kicad_dru", "w") as f:
        f.write(DRU)


def run(cmd, **kw):
    print("$", " ".join(cmd))
    return subprocess.run(cmd, **kw)


# ------------------------------------------------------------------ finishing a routed board

def _bbox_mm(item):
    bb = item.GetBoundingBox()
    return (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()), pcbnew.ToMM(bb.GetRight()),
            pcbnew.ToMM(bb.GetBottom()))


class Obstacles:
    """Copper, holes, cutouts and part bodies, bucketed on a coarse grid for quick hit tests."""

    def __init__(self, board, cell=5.0):
        self.cell = cell
        self.grid = {}
        items = []
        for t in board.GetTracks():
            items.append(("item", t))
        for fp in board.GetFootprints():
            for p in fp.Pads():
                items.append(("item", p))
            for g in fp.GraphicalItems():
                if g.GetLayer() == pcbnew.Edge_Cuts:
                    items.append(("item", g))
            for side in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
                poly = fp.GetCourtyard(side)
                if poly.OutlineCount():
                    bb = poly.BBox()
                    items.append(("box", (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                                          pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()))))
        for d in board.GetDrawings():
            if d.GetLayer() == pcbnew.Edge_Cuts:
                items.append(("item", d))
        for kind, it in items:
            x0, y0, x1, y1 = it if kind == "box" else _bbox_mm(it)
            for gx in range(int(x0 // cell) - 1, int(x1 // cell) + 2):
                for gy in range(int(y0 // cell) - 1, int(y1 // cell) + 2):
                    self.grid.setdefault((gx, gy), []).append((kind, it))

    def free(self, x, y, r):
        pos = pcbnew.VECTOR2I(MM(x), MM(y))
        acc = MM(r)
        for kind, it in self.grid.get((int(x // self.cell), int(y // self.cell)), ()):
            if kind == "box":
                x0, y0, x1, y1 = it
                if x0 - r < x < x1 + r and y0 - r < y < y1 + r:
                    return False
            elif it.HitTest(pos, acc):
                return False
        return True


def stitch(board, net_name, box, pitch=6.0, margin=2.0, via=0.6, drill=0.3, clearance=0.35, gap=0.0):
    """GND vias on a grid wherever they keep clear of everything else. With `gap`, only where no
    via of the net is nearer than `gap` mm: a finer pass that fills the holes a coarser one left."""
    net = board.FindNet(net_name)
    cell = max(gap, 1.0)
    near = {}

    def remember(x, y):
        near.setdefault((int(x // cell), int(y // cell)), []).append((x, y))

    def far(x, y):
        if not gap:
            return True
        i, j = int(x // cell), int(y // cell)
        for di in (-1, 0, 1):
            for dj in (-1, 0, 1):
                for vx, vy in near.get((i + di, j + dj), ()):
                    if (vx - x) ** 2 + (vy - y) ** 2 < gap * gap:
                        return False
        return True
    if gap:
        for t in board.GetTracks():
            if t.Type() == pcbnew.PCB_VIA_T and t.GetNetname() == net_name:
                remember(pcbnew.ToMM(t.GetPosition().x), pcbnew.ToMM(t.GetPosition().y))
    obs = Obstacles(board)
    fills = [z.GetFilledPolysList(z.GetLayer()) for z in board.Zones()
             if z.GetNetname() == net_name and not z.GetIsRuleArea()]
    fills = [f for f in fills if f.OutlineCount()]
    layers_filled = {z.GetLayer() for z in board.Zones()
                     if z.GetNetname() == net_name and z.GetFilledPolysList(z.GetLayer()).OutlineCount()}

    def in_fill_both(x, y):
        """With fills from an earlier pass, a via must land in GND copper on both layers."""
        if len(layers_filled) < 2:
            return True
        pt = pcbnew.VECTOR2I(MM(x), MM(y))
        hit = set()
        for z in board.Zones():
            if z.GetNetname() == net_name and z.GetFilledPolysList(z.GetLayer()).Contains(pt):
                hit.add(z.GetLayer())
        return len(hit) >= 2
    x0, y0, x1, y1 = box
    n = 0
    y = y0 + margin
    row = 0
    while y < y1 - margin:
        x = x0 + margin + (pitch / 2 if row % 2 else 0)
        while x < x1 - margin:
            if far(x, y) and obs.free(x, y, via / 2 + clearance) and in_fill_both(x, y):
                remember(x, y)
                v = pcbnew.PCB_VIA(board)
                v.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
                v.SetWidth(MM(via))
                v.SetDrill(MM(drill))
                v.SetNet(net)
                board.Add(v)
                n += 1
            x += pitch
        y += pitch * 0.866
        row += 1
    return n


def add_zone(board, net_name, layer, box, inset=0.5, clearance=0.3, min_width=0.25):
    x0, y0, x1, y1 = box
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(board.FindNet(net_name))
    ol = z.Outline()
    ol.NewOutline()
    for px, py in ((x0 + inset, y0 + inset), (x1 - inset, y0 + inset), (x1 - inset, y1 - inset),
                   (x0 + inset, y1 - inset)):
        ol.Append(MM(px), MM(py))
    z.SetLocalClearance(MM(clearance))
    z.SetMinThickness(MM(min_width))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(MM(0.3))
    z.SetThermalReliefSpokeWidth(MM(0.45))
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    z.SetZoneName("GND_" + board.GetLayerName(layer))
    board.Add(z)
    return z


def board_box(board):
    bb = board.GetBoardEdgesBoundingBox()
    return (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()), pcbnew.ToMM(bb.GetRight()),
            pcbnew.ToMM(bb.GetBottom()))


def has_zone(board, name):
    return any(z.GetZoneName() == name for z in board.Zones())


def clear_zones(board):
    """Kept for compatibility: zones are added once (see add_zone_once); removing them through
    SWIG corrupts the footprint list of the board object in KiCad 10's Python."""
    return None


def add_zone_once(board, net_name, layer, box):
    if not has_zone(board, "GND_" + board.GetLayerName(layer)):
        add_zone(board, net_name, layer, box)
    for z in board.Zones():
        if z.GetZoneName() == "GND_" + board.GetLayerName(layer):
            z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)


def stitch_these(board, refpads, net_name="GND"):
    """A via (or a track to nearby GND copper) for each (reference, pad number) given."""
    import maze
    g = maze.Grid(board)
    net = board.FindNet(net_name)
    code = net.GetNetCode()
    done = 0
    for ref, num in refpads:
        fp = board.FindFootprintByReference(ref)
        pad = [p for p in fp.Pads() if p.GetNumber() == num][0]
        if _stitch_one(board, g, pad, net, code) or \
                _join_nearby(board, g, pad, net, code, None, lambda i, q: False):
            done += 1
    return done


def stitch_pads(board, pairs_net="GND", log=print):
    """A GND pad left on an island of the pour gets a short track and a via to the other layer."""
    import maze
    g = maze.Grid(board)
    net = board.FindNet(pairs_net)
    code = net.GetNetCode()
    done = 0
    stuck = []
    for item in _unconnected_items(board, pairs_net):
        starts = g.item_cells(item, code)
        if not starts:
            continue
        p = item.GetPosition()
        cx, cy = g.cell(pcbnew.ToMM(p.x), pcbnew.ToMM(p.y))
        goals = set()
        rad = int(8.0 / g.res)
        for j in range(cy - rad, cy + rad + 1):
            for i in range(cx - rad, cx + rad + 1):
                if not (0 <= i < g.nx and 0 <= j < g.ny):
                    continue
                if (i - cx) ** 2 + (j - cy) ** 2 < (1.2 / g.res) ** 2:
                    continue
                k = g.idx(i, j)
                if g.free_via(k, code):
                    for li in (0, 1):
                        if g.free_track(li, k, code):
                            goals.add((li, k))
        path = g.route(code, starts, goals)
        if not path:
            log("  no stitch for %s" % item.GetParentFootprint().GetReference() if item.GetParentFootprint() else item)
            stuck.append(item)
            continue
        g.commit(net, path, g.w)
        li, k = path[-1]
        x, y = g.pos(k % g.nx, k // g.nx)
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
        v.SetWidth(MM(0.6))
        v.SetDrill(MM(0.3))
        v.SetNet(net)
        board.Add(v)
        g.add_disc(x, y, 0.3, maze.LAYERS, code)
        done += 1
    board._stuck_pads = stuck
    return done


def _unconnected_items(board, net_name):
    """Pads of a net that a DRC report lists as unconnected (the last report of this board)."""
    path = os.path.join(os.path.dirname(board.GetFileName()), ".check", "drc.json")
    if not os.path.exists(path):
        return []
    rep = json.load(open(path))
    by_id = {p.m_Uuid.AsString(): p for fp in board.GetFootprints() for p in fp.Pads()}
    out = []
    for u in rep.get("unconnected_items", []):
        for i in u["items"]:
            item = by_id.get(i["uuid"])
            if item is not None and item.GetNetname() == net_name and item not in out:
                out.append(item)
    return out


def sync_fields(board, comps):
    """Library link and Datasheet/Description fields from the schematic, for the parity check."""
    for fp in board.GetFootprints():
        c = comps.get(fp.GetReference())
        if not c or fp.IsBoardOnly():
            continue
        lib, name = c["footprint"].split(":", 1)
        fp.SetFPID(pcbnew.LIB_ID(lib, name))
        for fname in ("Datasheet", "Description"):
            if fname in c["fields"]:
                fp.SetField(fname, c["fields"][fname])
        fp.SetExcludedFromBOM(c["exclude_from_bom"])


def silk_inside(board):
    """Footprint silk that runs past the board outline goes to the fab layer (it would be cut off)."""
    x0, y0, x1, y1 = board_box(board)
    for fp in board.GetFootprints():
        for g in fp.GraphicalItems():
            if g.GetLayer() not in (pcbnew.F_SilkS, pcbnew.B_SilkS):
                continue
            bx0, by0, bx1, by1 = _bbox_mm(g)
            if bx0 < x0 + 0.2 or by0 < y0 + 0.2 or bx1 > x1 - 0.2 or by1 > y1 - 0.2:
                g.SetLayer(pcbnew.F_Fab if g.GetLayer() == pcbnew.F_SilkS else pcbnew.B_Fab)


def stitch_islands(board, net_name="GND", step=0.5, log=print):
    """Join every island of the GND pours to the main one with a via where both layers have fill.

    Islands are the filled outlines of the GND zones on each layer; GND vias and plated GND pads
    join the islands they sit in. Each island outside the largest group gets a via at a free spot
    it shares with a fill of the main group on the other layer."""
    import maze
    zones = [z for z in board.Zones() if z.GetNetname() == net_name and not z.GetIsRuleArea()]
    islands = []
    for z in zones:
        lay = z.GetLayer()
        polys = z.GetFilledPolysList(lay)
        for k in range(polys.OutlineCount()):
            islands.append((lay, polys, k))
    if not islands:
        return 0
    parent = list(range(len(islands)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def inside(i, pt):
        lay, polys, k = islands[i]
        return polys.Contains(pt, k)

    links = [v.GetPosition() for v in board.GetTracks()
             if v.Type() == pcbnew.PCB_VIA_T and v.GetNetname() == net_name]
    links += [p.GetPosition() for fp in board.GetFootprints() for p in fp.Pads()
              if p.GetNetname() == net_name and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH]
    for pt in links:
        hit = [i for i in range(len(islands)) if inside(i, pt)]
        for i in hit[1:]:
            parent[find(i)] = find(hit[0])
    groups = {}
    for i in range(len(islands)):
        groups.setdefault(find(i), []).append(i)
    main = max(groups.values(), key=lambda g: sum(islands[i][1].Outline(islands[i][2]).Area() for i in g))
    main_root = find(main[0])
    g = maze.Grid(board)
    net = board.FindNet(net_name)
    code = net.GetNetCode()
    added = 0
    for i in range(len(islands)):
        if find(i) == main_root:
            continue
        lay, polys, k = islands[i]
        bb = polys.Outline(k).BBox()
        x0, y0, x1, y1 = pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()), pcbnew.ToMM(bb.GetRight()), \
            pcbnew.ToMM(bb.GetBottom())
        others = [j for j in range(len(islands)) if islands[j][0] != lay and find(j) == main_root]
        placed = False
        y = y0 + step / 2
        while y < y1 and not placed:
            x = x0 + step / 2
            while x < x1:
                pt = pcbnew.VECTOR2I(MM(x), MM(y))
                ci, cj = g.cell(x, y)
                if 0 <= ci < g.nx and 0 <= cj < g.ny and g.free_via(g.idx(ci, cj), code) and inside(i, pt):
                    hit = [j for j in others if inside(j, pt)]
                    if hit:
                        v = pcbnew.PCB_VIA(board)
                        v.SetPosition(pt)
                        v.SetWidth(MM(0.6))
                        v.SetDrill(MM(0.3))
                        v.SetNet(net)
                        board.Add(v)
                        g.add_disc(x, y, 0.3, maze.LAYERS, code)
                        parent[find(i)] = main_root
                        added += 1
                        placed = True
                        break
                x += step
            y += step
        pads = []
        if not placed:
            # no spot shared with the main pour: run a track from a GND pad of the island to a via
            pads = [p for fp in board.GetFootprints() for p in fp.Pads()
                    if p.GetNetname() == net_name and p.IsOnLayer(lay) and inside(i, p.GetPosition())]
            for pad in pads:
                if _stitch_one(board, g, pad, net, code) or _join_nearby(board, g, pad, net, code, i, inside):
                    parent[find(i)] = main_root
                    added += 1
                    placed = True
                    break
        if not placed:
            log("  island on %s at (%.1f, %.1f) could not be joined" % (board.GetLayerName(lay), x0, y0))
            stuck = getattr(board, "_stuck_pads", [])
            board._stuck_pads = stuck + [p for p in pads if p not in stuck]
    return added


def _join_nearby(board, g, pad, net, code, island, inside, radius=15.0):
    """Track from a pad of an island to another GND pad or via outside that island."""
    starts = g.item_cells(pad, code)
    if not starts:
        return False
    p0 = pad.GetPosition()
    goals = set()
    cands = [t for t in board.GetTracks() if t.Type() == pcbnew.PCB_VIA_T and t.GetNetCode() == code]
    cands += [q for fp in board.GetFootprints() for q in fp.Pads() if q.GetNetCode() == code and q is not pad]
    for c in cands:
        q = c.GetPosition()
        if math.hypot(pcbnew.ToMM(q.x - p0.x), pcbnew.ToMM(q.y - p0.y)) > radius or inside(island, q):
            continue
        goals |= g.item_cells(c, code)
    path = g.route(code, starts, goals)
    if not path:
        return False
    g.commit(net, path, g.w)
    return True


def _stitch_one(board, g, pad, net, code, radius=8.0):
    """Track from a pad to the nearest free via spot more than 1.2 mm away, and the via."""
    import maze
    starts = g.item_cells(pad, code)
    if not starts:
        return False
    p = pad.GetPosition()
    cx, cy = g.cell(pcbnew.ToMM(p.x), pcbnew.ToMM(p.y))
    goals = set()
    rad = int(radius / g.res)
    for j in range(cy - rad, cy + rad + 1):
        for i in range(cx - rad, cx + rad + 1):
            if not (0 <= i < g.nx and 0 <= j < g.ny) or (i - cx) ** 2 + (j - cy) ** 2 < (1.2 / g.res) ** 2:
                continue
            k = g.idx(i, j)
            if g.free_via(k, code):
                for li in (0, 1):
                    if g.free_track(li, k, code):
                        goals.add((li, k))
    path = g.route(code, starts, goals)
    if not path:
        return False
    g.commit(net, path, g.w)
    li, k = path[-1]
    x, y = g.pos(k % g.nx, k // g.nx)
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    v.SetWidth(MM(0.6))
    v.SetDrill(MM(0.3))
    v.SetNet(net)
    board.Add(v)
    g.add_disc(x, y, 0.3, maze.LAYERS, code)
    return True


def remove_dangling_vias_text(pcb_path):
    """Delete GND vias that the last DRC found connected on one layer only, in the board file
    itself (removing items through KiCad 10's Python leaves the board object unusable)."""
    import re
    rep_path = os.path.join(os.path.dirname(pcb_path), ".check", "drc.json")
    if not os.path.exists(rep_path):
        return 0
    rep = json.load(open(rep_path))
    ids = {i["uuid"] for v in rep.get("violations", []) if v.get("type") in ("via_dangling", "track_dangling")
           for i in v["items"] if "[GND]" in i.get("description", "")}
    if not ids:
        return 0
    text = open(pcb_path).read()
    n = 0
    for uid in ids:
        pat = re.compile(r'\n\t\((?:via|segment)\n(?:\t\t[^\n]*\n)*?\t\t\(uuid "%s"\)\n\t\)' % re.escape(uid))
        text, k = pat.subn("", text)
        n += k
    open(pcb_path, "w").write(text)
    return n


def drop_dangling_vias(board):
    """Stitching vias that the last fill left connected on one layer only are removed."""
    path = os.path.join(os.path.dirname(board.GetFileName()), ".check", "drc.json")
    if not os.path.exists(path):
        return 0
    rep = json.load(open(path))
    ids = {i["uuid"] for v in rep.get("violations", []) if v.get("type") == "via_dangling" for i in v["items"]}
    n = 0
    for t in list(board.GetTracks()):
        if t.Type() == pcbnew.PCB_VIA_T and t.m_Uuid.AsString() in ids and t.GetNetname() == "GND":
            board.Remove(t)
            n += 1
    return n


def silk_to_fab(board, prefixes, keep=()):
    """Reference text of dense parts goes to the fab layer; the silkscreen keeps connectors, ICs, holes."""
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref in keep or not ref.startswith(tuple(prefixes)):
            continue
        r = fp.Reference()
        r.SetLayer(pcbnew.B_Fab if fp.IsFlipped() else pcbnew.F_Fab)
