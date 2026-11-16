"""Small KiCad 10 schematic writer used by build.py.

Standard library only. Coordinates are integers in micrometres internally, so
1.27 mm grid arithmetic is exact. Symbols are taken from the KiCad library
installed with KiCad (KICAD_SYMBOL_DIR) and from the project library
rebirth.kicad_sym, flattened and embedded into each sheet.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later, like the firmware.
"""

import copy
import math
import os
import re
import uuid

KICAD_SYMBOL_DIR = os.environ.get(
    "KICAD_SYMBOL_DIR",
    "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols")

SCH_VERSION = "20260306"
SYM_VERSION = "20251024"
PROJECT = "rebirth_panel"
NS = uuid.UUID("6f1d1f52-0d2b-4a8e-9a49-3c2f5e0b7a10")

U = 1000          # micrometres per millimetre
G = 1270          # 1.27 mm grid in micrometres


def mm(v):
    """Millimetres (float) -> micrometres (int)."""
    return int(round(v * U))


def fmt(v):
    s = "%.4f" % (v / U)
    s = s.rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def uid(*key):
    return str(uuid.uuid5(NS, "/".join(str(k) for k in key)))


# --------------------------------------------------------------- s-expressions

class Q(str):
    """A quoted string atom."""


def parse(text):
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
            stack[-1].append(Q(t[1:-1].replace('\\"', '"').replace("\\\\", "\\")))
        else:
            stack[-1].append(t)
    return stack[0]


def q(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def dump(node):
    if isinstance(node, list):
        return "(" + " ".join(dump(n) for n in node) + ")"
    if isinstance(node, Q):
        return q(node)
    return str(node)


def find(node, key):
    for n in node:
        if isinstance(n, list) and n and n[0] == key:
            return n
    return None


def find_all(node, key):
    return [n for n in node if isinstance(n, list) and n and n[0] == key]


# --------------------------------------------------------------- symbol library

class Pin:
    def __init__(self, number, name, etype, x, y, angle, unit, hidden):
        self.number, self.name, self.etype = number, name, etype
        self.x, self.y, self.angle = x, y, angle
        self.unit, self.hidden = unit, hidden


class SymbolDef:
    def __init__(self, lib_id, sexpr):
        self.lib_id = lib_id
        self.sexpr = sexpr
        self.power = find(sexpr, "power") is not None
        self.pins = []
        base = lib_id.split(":", 1)[1]
        for sub in find_all(sexpr, "symbol"):
            m = re.match(re.escape(base) + r"_(\d+)_(\d+)$", sub[1])
            if not m:
                raise ValueError("unexpected unit name %s in %s" % (sub[1], lib_id))
            unit = int(m.group(1))
            for p in find_all(sub, "pin"):
                at = find(p, "at")
                name = find(p, "name")[1]
                num = find(p, "number")[1]
                hidden = find(p, "hide") is not None and find(p, "hide")[1] == "yes"
                self.pins.append(Pin(str(num), str(name), p[1], mm(float(at[1])),
                                     mm(float(at[2])), int(float(at[3])), unit, hidden))
        self.units = max([p.unit for p in self.pins] + [1])
        props = {str(p[1]): str(p[2]) for p in find_all(sexpr, "property")}
        self.props = props

    def unit_pins(self, unit):
        return [p for p in self.pins if p.unit in (0, unit)]


class Library:
    def __init__(self, project_lib):
        self.project_lib = project_lib
        self.files = {}
        self.cache = {}

    def _lib(self, lib):
        if lib not in self.files:
            if lib == "rebirth":
                path = self.project_lib
            else:
                path = os.path.join(KICAD_SYMBOL_DIR, lib + ".kicad_sym")
            with open(path, encoding="utf-8") as f:
                tree = parse(f.read())[0]
            self.files[lib] = {str(s[1]): s for s in find_all(tree, "symbol")}
        return self.files[lib]

    def _flat(self, lib, name):
        sym = copy.deepcopy(self._lib(lib)[name])
        ext = find(sym, "extends")
        if ext is None:
            return sym
        parent = self._flat(lib, str(ext[1]))
        own_props = {str(p[1]): p for p in find_all(sym, "property")}
        out = [parent[0], Q(name)]
        for item in parent[2:]:
            if isinstance(item, list) and item and item[0] == "property" and str(item[1]) in own_props:
                out.append(own_props.pop(str(item[1])))
            elif isinstance(item, list) and item and item[0] == "symbol":
                sub = copy.deepcopy(item)
                sub[1] = Q(re.sub("^" + re.escape(str(ext[1])) + "_", name + "_", str(sub[1])))
                out.append(sub)
            else:
                out.append(item)
        # properties present only in the derived symbol go before the units
        extra = list(own_props.values())
        idx = next(i for i, it in enumerate(out) if isinstance(it, list) and it and it[0] == "symbol")
        out[idx:idx] = extra
        return out

    def get(self, lib_id):
        if lib_id not in self.cache:
            lib, name = lib_id.split(":", 1)
            sym = self._flat(lib, name)
            sym[1] = Q(lib_id)
            self.cache[lib_id] = SymbolDef(lib_id, sym)
        return self.cache[lib_id]


# --------------------------------------------------------------- geometry

def transform(px, py, rot, mirror):
    """Library point (y up) -> offset on the sheet (y down)."""
    a, b = px, -py
    r = math.radians(rot)
    c, s = int(round(math.cos(r))), int(round(math.sin(r)))
    a, b = a * c + b * s, -a * s + b * c
    if mirror == "x":
        b = -b
    elif mirror == "y":
        a = -a
    return a, b


# --------------------------------------------------------------- sheet

class Part:
    def __init__(self, sheet, sym, ref, value, at, rot, mirror, unit, conn, footprint,
                 fields, dnp, extra_props, in_bom):
        self.sheet, self.sym, self.ref, self.value = sheet, sym, ref, value
        self.at, self.rot, self.mirror, self.unit = at, rot, mirror, unit
        self.conn, self.footprint, self.fields = conn, footprint, fields
        self.dnp, self.extra_props, self.in_bom = dnp, extra_props, in_bom
        self.uuid = uid(sheet.file, ref, unit)

    def pin_xy(self, number):
        for p in self.sym.unit_pins(self.unit):
            if p.number == number or (p.name == number and number not in [q.number for q in self.sym.pins]):
                dx, dy = transform(p.x, p.y, self.rot, self.mirror)
                return (self.at[0] + dx, self.at[1] + dy)
        raise KeyError("%s has no pin %s in unit %d" % (self.ref, number, self.unit))


class Sheet:
    def __init__(self, design, name, file, title, paper, page):
        self.design, self.name, self.file = design, name, file
        self.title, self.paper, self.page = title, paper, page
        self.lib = design.lib
        self.parts = []
        self.power = []
        self.wires = []
        self.labels = []
        self.hlabels = []
        self.ncs = []
        self.texts = []
        self.sheets = []          # sheet symbols (root only)
        self.extra_junctions = []
        self.uuid = uid(file, "screen")
        self.sheet_uuid = None    # uuid of this sheet's symbol on the root
        self.ports = set()

    # ---- parts
    def part(self, lib_id, ref, value, at, rot=0, mirror=None, unit=1, conn=None,
             footprint="", fields=None, dnp=False, props=None, in_bom=True):
        sym = self.lib.get(lib_id)
        p = Part(self, sym, ref, value, (mm(at[0]), mm(at[1])), rot, mirror, unit,
                 dict(conn or {}), footprint, fields or {}, dnp, props or {}, in_bom)
        self.parts.append(p)
        return p

    def pin(self, ref, number, unit=None):
        for p in self.parts:
            if p.ref == ref and (unit is None or p.unit == unit):
                try:
                    x, y = p.pin_xy(number)
                    return (x / U, y / U)
                except KeyError:
                    if unit is not None:
                        raise
        raise KeyError("%s.%s" % (ref, number))

    def pwr(self, name, at, rot=0):
        lib_id = "rebirth:" + name if name in ("+5V_LED",) else "power:" + name
        sym = self.lib.get(lib_id)
        ref = self.design.next_pwr("#FLG" if name == "PWR_FLAG" else "#PWR")
        p = Part(self, sym, ref, name, (mm(at[0]), mm(at[1])), rot, None, 1, {}, "", {}, False, {}, True)
        self.power.append(p)
        return p

    # ---- wires and labels
    def wire(self, *pts):
        pts = [(mm(x), mm(y)) for x, y in pts]
        for a, b in zip(pts, pts[1:]):
            if a == b:
                continue
            if a[0] != b[0] and a[1] != b[1]:
                raise ValueError("diagonal wire %s -> %s on %s" % (a, b, self.file))
            self.wires.append((a, b))

    def label(self, name, at, angle=0, justify="left bottom"):
        self.labels.append((name, (mm(at[0]), mm(at[1])), angle, justify))

    def hlabel(self, name, at, shape, side):
        """side: where the text goes from the anchor: left/right/up/down."""
        angle = {"right": 0, "up": 90, "left": 180, "down": 270}[side]
        just = {"right": "left", "left": "right", "up": "left", "down": "right"}[side]
        self.hlabels.append((name, shape, (mm(at[0]), mm(at[1])), angle, just))
        self.ports.add(name)

    def nc(self, at):
        self.ncs.append((mm(at[0]), mm(at[1])))

    def text(self, s, at, size=1.27, bold=False, justify="left bottom"):
        self.texts.append((s, (mm(at[0]), mm(at[1])), size, bold, justify))

    def heading(self, s, at):
        self.text(s, at, size=2.54, bold=True)

    def junction(self, at):
        self.extra_junctions.append((mm(at[0]), mm(at[1])))

    # ---- connectivity post-processing
    def _pin_points(self):
        pts = []
        for p in self.parts + self.power:
            for pin in p.sym.unit_pins(p.unit):
                dx, dy = transform(pin.x, pin.y, p.rot, p.mirror)
                pts.append((p.at[0] + dx, p.at[1] + dy))
        for sh in self.sheets:
            for (name, etype, x, y, side) in sh["pins"]:
                pts.append((x, y))
        return pts

    def finish(self):
        pins = self._pin_points()
        label_pts = [l[1] for l in self.labels] + [h[2] for h in self.hlabels]
        ends = [e for w in self.wires for e in w]
        nodes = set(pins) | set(ends) | set(label_pts) | set(self.extra_junctions)

        def inside(pt, a, b):
            if a[0] == b[0] == pt[0]:
                return min(a[1], b[1]) < pt[1] < max(a[1], b[1])
            if a[1] == b[1] == pt[1]:
                return min(a[0], b[0]) < pt[0] < max(a[0], b[0])
            return False

        changed = True
        while changed:
            changed = False
            out = []
            for a, b in self.wires:
                cut = sorted((n for n in nodes if inside(n, a, b)),
                             key=lambda n: abs(n[0] - a[0]) + abs(n[1] - a[1]))
                if cut:
                    seq = [a] + cut + [b]
                    out.extend(zip(seq, seq[1:]))
                    changed = True
                else:
                    out.append((a, b))
            self.wires = out
        # remove duplicate segments
        seen, uniq = set(), []
        for a, b in self.wires:
            k = (min(a, b), max(a, b))
            if k in seen:
                raise ValueError("overlapping wire %s on %s" % (k, self.file))
            seen.add(k)
            uniq.append((a, b))
        self.wires = uniq
        deg = {}
        for a, b in self.wires:
            deg[a] = deg.get(a, 0) + 1
            deg[b] = deg.get(b, 0) + 1
        for pt in pins:
            if pt in deg:
                deg[pt] += 1
        self.junctions = sorted({pt for pt, d in deg.items() if d >= 3} | set(self.extra_junctions))

    # ---- output
    def lib_symbols(self):
        ids = []
        for p in self.parts + self.power:
            if p.sym.lib_id not in ids:
                ids.append(p.sym.lib_id)
        return [self.lib.get(i).sexpr for i in ids]

    def _field(self, name, value, at, angle=0, justify=None, hide=False, size=1.27):
        eff = "(effects (font (size %s %s))%s%s)" % (
            size, size, (" (justify %s)" % justify) if justify else "",
            " (hide yes)" if hide else "")
        return "(property %s %s (at %s %s %d) %s)" % (q(name), q(value), fmt(at[0]), fmt(at[1]), angle, eff)

    def _symbol_text(self, p, inst_path):
        fa = 90 if p.rot in (90, 270) else 0
        x, y = p.at
        f = p.fields
        lines = []
        mir = " (mirror %s)" % p.mirror if p.mirror else ""
        lines.append("(symbol (lib_id %s) (at %s %s %d)%s (unit %d) (exclude_from_sim no) (in_bom %s) (on_board yes) (dnp %s) (uuid %s)"
                     % (q(p.sym.lib_id), fmt(x), fmt(y), p.rot, mir, p.unit,
                        "yes" if p.in_bom and not p.ref.startswith("#") else "no",
                        "yes" if p.dnp else "no", q(p.uuid)))
        is_pwr = p.ref.startswith("#")
        if is_pwr:
            # value text above a positive rail, below a ground
            ground = p.value == "GND"
            if p.value == "PWR_FLAG":
                vy = y - mm(3.81)
            else:
                vy = y + mm(3.81) if ground else y - mm(3.556)
            lines.append(self._field("Reference", p.ref, (x, y), hide=True))
            lines.append(self._field("Value", p.value, (x, vy)))
        else:
            r = f.get("ref", (x + mm(2.54), y - mm(1.27), "left"))
            v = f.get("val", (x + mm(2.54), y + mm(1.27), "left"))

            def just(j):
                # KiCad mirrors a field's justification with a symbol mirrored about Y,
                # so give it the opposite one to keep the text where the generator put it.
                if j == "center":
                    return None
                # A symbol turned 180 degrees flips its fields' justification too.
                if (p.mirror == "y") != (p.rot == 180):
                    return {"left": "right", "right": "left"}.get(j, j)
                return j
            lines.append(self._field("Reference", p.ref, (r[0], r[1]), fa, just(r[2]),
                                     hide=f.get("hide_ref", False)))
            lines.append(self._field("Value", p.value, (v[0], v[1]), fa, just(v[2]),
                                     hide=f.get("hide_val", False)))
        lines.append(self._field("Footprint", p.footprint, (x, y), hide=True))
        lines.append(self._field("Datasheet", p.sym.props.get("Datasheet", ""), (x, y), hide=True))
        for k, v in p.extra_props.items():
            lines.append(self._field(k, v, (x, y), hide=True))
        for pin in p.sym.unit_pins(p.unit):
            lines.append("(pin %s (uuid %s))" % (q(pin.number), q(uid(p.uuid, "pin", pin.number))))
        lines.append("(instances (project %s (path %s (reference %s) (unit %d))))"
                     % (q(PROJECT), q(inst_path), q(p.ref), p.unit))
        return "  " + "\n    ".join(lines) + ")"

    def render(self, inst_path):
        self.finish()
        o = []
        o.append("(kicad_sch (version %s) (generator \"rebirth_build\") (generator_version \"10.0\")" % SCH_VERSION)
        o.append("  (uuid %s)" % q(self.uuid))
        o.append("  (paper %s)" % q(self.paper))
        tb = self.design.title_block(self)
        o.append("  " + tb)
        o.append("  (lib_symbols")
        for s in self.lib_symbols():
            o.append("    " + dump(s))
        o.append("  )")
        for (x, y) in self.junctions:
            o.append("  (junction (at %s %s) (diameter 0) (color 0 0 0 0) (uuid %s))"
                     % (fmt(x), fmt(y), q(uid(self.file, "j", x, y))))
        for (x, y) in self.ncs:
            o.append("  (no_connect (at %s %s) (uuid %s))" % (fmt(x), fmt(y), q(uid(self.file, "nc", x, y))))
        for i, (a, b) in enumerate(self.wires):
            o.append("  (wire (pts (xy %s %s) (xy %s %s)) (stroke (width 0) (type default)) (uuid %s))"
                     % (fmt(a[0]), fmt(a[1]), fmt(b[0]), fmt(b[1]), q(uid(self.file, "w", a, b))))
        for (s, (x, y), size, bold, just) in self.texts:
            o.append("  (text %s (exclude_from_sim no) (at %s %s 0) (effects (font (size %s %s)%s%s) (justify %s)) (uuid %s))"
                     % (q(s), fmt(x), fmt(y), size, size,
                        " (thickness %s)" % (0.508 if size >= 2.5 else 0.3) if bold else "",
                        " (bold yes)" if bold else "", just, q(uid(self.file, "t", s, x, y))))
        for (name, (x, y), angle, just) in self.labels:
            o.append("  (label %s (at %s %s %d) (effects (font (size 1.27 1.27)) (justify %s)) (uuid %s))"
                     % (q(name), fmt(x), fmt(y), angle, just, q(uid(self.file, "l", name, x, y))))
        for (name, shape, (x, y), angle, just) in self.hlabels:
            o.append("  (hierarchical_label %s (shape %s) (at %s %s %d) (effects (font (size 1.27 1.27)) (justify %s)) (uuid %s))"
                     % (q(name), shape, fmt(x), fmt(y), angle, just, q(uid(self.file, "h", name, x, y))))
        for sh in self.sheets:
            o.append(self.design.sheet_symbol_text(self, sh))
        for p in self.parts + self.power:
            o.append(self._symbol_text(p, inst_path))
        if self.design.root is self:
            o.append("  (sheet_instances (path \"/\" (page \"1\")))")
        o.append("  (embedded_fonts no)")
        o.append(")")
        return "\n".join(o) + "\n"


class Design:
    def __init__(self, outdir, project_lib, date, rev):
        self.outdir = outdir
        self.lib = Library(project_lib)
        self.date, self.rev = date, rev
        self.sheets = []
        self.root = None
        self._pwr = {}

    def next_pwr(self, prefix):
        n = self._pwr.get(prefix, 0) + 1
        self._pwr[prefix] = n
        return "%s%03d" % (prefix, n)

    def sheet(self, name, file, title, paper, page):
        s = Sheet(self, name, file, title, paper, page)
        if self.root is None:
            self.root = s
        else:
            self.sheets.append(s)
        return s

    def title_block(self, sh):
        total = len(self.sheets) + 1
        return ("(title_block (title %s) (date %s) (rev %s) (company %s) (comment 1 %s) (comment 2 %s))"
                % (q(sh.title), q(self.date), q(self.rev),
                   q("ReBirth RP2040 sequencer, session-style live panel"),
                   q("Sheet %s of %d: %s" % (sh.page, total, sh.name)),
                   q("GPL-3.0-or-later. Generated by gen/build.py: edit the generator.")))

    def add_sheet_symbol(self, child, at, size, pins):
        """pins: list of (name, etype, (x, y), side) with side 'left' or 'right'."""
        sh = {"child": child, "at": (mm(at[0]), mm(at[1])), "size": (mm(size[0]), mm(size[1])), "pins": []}
        for name, etype, (x, y), side in pins:
            sh["pins"].append((name, etype, mm(x), mm(y), side))
        child.sheet_uuid = uid("sheet-symbol", child.file)
        self.root.sheets.append(sh)
        return sh

    def sheet_symbol_text(self, root, sh):
        c = sh["child"]
        x, y = sh["at"]
        w, h = sh["size"]
        o = ["  (sheet (at %s %s) (size %s %s) (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)"
             % (fmt(x), fmt(y), fmt(w), fmt(h)),
             "    (stroke (width 0.1524) (type solid)) (fill (color 0 0 0 0.0000)) (uuid %s)" % q(c.sheet_uuid),
             "    (property \"Sheetname\" %s (at %s %s 0) (effects (font (size 1.524 1.524) (bold yes)) (justify left bottom)))"
             % (q(c.name), fmt(x), fmt(y - mm(0.762))),
             "    (property \"Sheetfile\" %s (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left top)))"
             % (q(c.file), fmt(x), fmt(y + h + mm(0.762)))]
        for (name, etype, px, py, side) in sh["pins"]:
            angle = 0 if side == "right" else 180
            just = "right" if side == "right" else "left"
            o.append("    (pin %s %s (at %s %s %d) (uuid %s) (effects (font (size 1.27 1.27)) (justify %s)))"
                     % (q(name), etype, fmt(px), fmt(py), angle, q(uid(c.file, "sheetpin", name)), just))
        o.append("    (instances (project %s (path %s (page %s))))" % (q(PROJECT), q("/" + root.uuid), q(str(c.page))))
        o.append("  )")
        return "\n".join(o)

    def write(self):
        paths = []
        root_path = "/" + self.root.uuid
        for s in [self.root] + self.sheets:
            inst = root_path if s is self.root else root_path + "/" + s.sheet_uuid
            path = os.path.join(self.outdir, s.file)
            with open(path, "w", encoding="utf-8") as f:
                f.write(s.render(inst))
            paths.append(path)
        return paths

    # ---- declared connectivity, for checking the exported netlist
    def declared_nets(self):
        nets = {}
        globals_ = set()
        for s in [self.root] + self.sheets:
            globals_ |= s.ports
        for s in [self.root] + self.sheets:
            for p in s.parts:
                for pin, net in p.conn.items():
                    if net is None:
                        continue
                    key = net if (net in globals_ or net in RAILS) else s.name + ":" + net
                    nets.setdefault(key, set()).add((p.ref, pin))
        return nets


RAILS = {"GND", "+3V3", "VBUS", "+5V_LED"}


def write_symbol_lib(path, symbols_text):
    with open(path, "w", encoding="utf-8") as f:
        f.write("(kicad_symbol_lib (version %s) (generator \"rebirth_build\") (generator_version \"10.0\")\n" % SYM_VERSION)
        for s in symbols_text:
            f.write("  " + s.strip() + "\n")
        f.write(")\n")
