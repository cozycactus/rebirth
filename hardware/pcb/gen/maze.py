"""A small two-layer grid router for what is left after the scripted and auto-routed copper.

Lee/A* on a square grid: tracks run in 8 directions on F.Cu and B.Cu, a via changes layer.
Every item of another net is grown by clearance + half the track width (or the via radius)
and blocks the cells it covers; holes and board edges block all nets. Each new route is
rasterized at once, so later routes keep clear of it. A connection that finds no way gets a
trial path that may cross the unlocked copper of other signal nets at a cost (`crossed_nets`);
build_pcb.py rips up just those nets and routes again.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import heapq
import math

import pcbnew

MM = pcbnew.FromMM
TO = pcbnew.ToMM
LAYERS = (pcbnew.F_Cu, pcbnew.B_Cu)


class Grid:
    def __init__(self, board, res=0.2, clearance=0.2, width=0.25, via=0.6, drill=0.3, edge=0.3, hole=0.3,
                 safety=0.06, skip=None):
        self.b = board
        self.skip = skip          # tracks and vias for which skip(item) is true are left out
        self.res, self.clr, self.w, self.via, self.drill = res, clearance, width, via, drill
        bb = board.GetBoardEdgesBoundingBox()
        self.x0, self.y0 = TO(bb.GetLeft()), TO(bb.GetTop())
        self.nx = int((TO(bb.GetRight()) - self.x0) / res) + 1
        self.ny = int((TO(bb.GetBottom()) - self.y0) / res) + 1
        n = self.nx * self.ny
        # owner per cell and layer for track centres, and one combined grid for via centres:
        # 0 free, net code if only that net is near, -1 blocked for all
        self.track = [bytearray(0) for _ in LAYERS]
        self.towner = [[0] * n for _ in LAYERS]
        self.vowner = [0] * n
        self.t_infl = clearance + width / 2 + safety
        self.v_infl = clearance + via / 2 + safety
        self.e_infl = (edge + width / 2 + safety, edge + via / 2 + safety)
        self.h_infl = (hole + width / 2 + safety, hole + via / 2 + safety)
        self._raster_board()

    # ---- grid helpers
    def idx(self, i, j):
        return j * self.nx + i

    def cell(self, x, y):
        return int(round((x - self.x0) / self.res)), int(round((y - self.y0) / self.res))

    def pos(self, i, j):
        return self.x0 + i * self.res, self.y0 + j * self.res

    def _mark(self, arr, k, net):
        cur = arr[k]
        if cur == 0:
            arr[k] = net
        elif cur != net:
            arr[k] = -1

    def _cells_near(self, x0, y0, x1, y1, r):
        i0, j0 = self.cell(x0 - r, y0 - r)
        i1, j1 = self.cell(x1 + r, y1 + r)
        for j in range(max(j0, 0), min(j1, self.ny - 1) + 1):
            for i in range(max(i0, 0), min(i1, self.nx - 1) + 1):
                yield i, j

    # ---- rasterization
    def add_segment(self, x1, y1, x2, y2, half, layers, net):
        """A track of half-width `half` (mm) on `layers`, as an obstacle for other nets."""
        for li, lay in enumerate(LAYERS):
            if lay not in layers:
                continue
            for k, (arr, infl) in enumerate(((self.towner[li], self.t_infl), (self.vowner, self.v_infl))):
                r = half + infl
                for i, j in self._cells_near(min(x1, x2), min(y1, y2), max(x1, x2), max(y1, y2), r):
                    px, py = self.pos(i, j)
                    if _seg_dist(px, py, x1, y1, x2, y2) <= r:
                        self._mark(arr, self.idx(i, j), net)

    def add_disc(self, x, y, rad, layers, net, hole=False):
        for li, lay in enumerate(LAYERS):
            if lay not in layers:
                continue
            for arr, infl in ((self.towner[li], self.h_infl[0] if hole else self.t_infl),
                              (self.vowner, self.h_infl[1] if hole else self.v_infl)):
                r = rad + infl
                for i, j in self._cells_near(x, y, x, y, r):
                    px, py = self.pos(i, j)
                    if (px - x) ** 2 + (py - y) ** 2 <= r * r:
                        self._mark(arr, self.idx(i, j), net)

    def add_item_hittest(self, item, layers, net, infl_t, infl_v):
        bb = item.GetBoundingBox()
        x0, y0, x1, y1 = TO(bb.GetLeft()), TO(bb.GetTop()), TO(bb.GetRight()), TO(bb.GetBottom())
        for li, lay in enumerate(LAYERS):
            if lay not in layers:
                continue
            for arr, infl in ((self.towner[li], infl_t), (self.vowner, infl_v)):
                acc = MM(infl)
                for i, j in self._cells_near(x0, y0, x1, y1, infl):
                    px, py = self.pos(i, j)
                    if item.HitTest(pcbnew.VECTOR2I(MM(px), MM(py)), acc):
                        self._mark(arr, self.idx(i, j), net)

    def _raster_board(self):
        b = self.b
        for t in b.GetTracks():
            if self.skip and self.skip(t):
                continue
            net = t.GetNetCode() or -1
            if t.Type() == pcbnew.PCB_VIA_T:
                p = t.GetPosition()
                self.add_disc(TO(p.x), TO(p.y), TO(t.GetWidth(pcbnew.F_Cu)) / 2, LAYERS, net)
                x, y = TO(p.x), TO(p.y)
                r = TO(t.GetDrillValue()) / 2 + self.drill / 2 + 0.5
                for i, j in self._cells_near(x, y, x, y, r):
                    px, py = self.pos(i, j)
                    if (px - x) ** 2 + (py - y) ** 2 <= r * r:
                        self.vowner[self.idx(i, j)] = -1
            else:
                s, e = t.GetStart(), t.GetEnd()
                self.add_segment(TO(s.x), TO(s.y), TO(e.x), TO(e.y), TO(t.GetWidth()) / 2, (t.GetLayer(),), net)
        for fp in b.GetFootprints():
            for p in fp.Pads():
                net = p.GetNetCode() or -1
                lays = [l for l in LAYERS if p.IsOnLayer(l)]
                if p.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                    q = p.GetPosition()
                    self.add_disc(TO(q.x), TO(q.y), TO(p.GetDrillSizeX()) / 2, LAYERS, -1, hole=True)
                    continue
                self.add_item_hittest(p, lays, net, self.t_infl, self.v_infl)
                if p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD:
                    # no via in or at the edge of an SMD pad, whatever its net (solder would wick)
                    bb = p.GetBoundingBox()
                    acc = MM(self.via / 2 + 0.1)
                    for i, j in self._cells_near(TO(bb.GetLeft()), TO(bb.GetTop()), TO(bb.GetRight()),
                                                 TO(bb.GetBottom()), self.via / 2 + 0.1):
                        px, py = self.pos(i, j)
                        if p.HitTest(pcbnew.VECTOR2I(MM(px), MM(py)), acc):
                            self.vowner[self.idx(i, j)] = -1
                if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH:
                    q = p.GetPosition()       # no via of any net on or next to a drilled hole
                    r = TO(p.GetDrillSizeX()) / 2 + self.h_infl[1] + 0.2
                    x, y = TO(q.x), TO(q.y)
                    for i, j in self._cells_near(x, y, x, y, r):
                        px, py = self.pos(i, j)
                        if (px - x) ** 2 + (py - y) ** 2 <= r * r:
                            self.vowner[self.idx(i, j)] = -1
            for g in fp.GraphicalItems():
                if g.GetLayer() == pcbnew.Edge_Cuts:
                    self.add_item_hittest(g, LAYERS, -1, *self.e_infl)
        for d in b.GetDrawings():
            if d.GetLayer() == pcbnew.Edge_Cuts:
                self.add_item_hittest(d, LAYERS, -1, *self.e_infl)
        zones = list(b.Zones())
        for fp in b.GetFootprints():
            zones += list(fp.Zones())
        for z in zones:
            if z.GetIsRuleArea():
                self._keepout(z)

    def _keepout(self, z):
        poly = z.Outline()
        bb = z.GetBoundingBox()
        x0, y0, x1, y1 = TO(bb.GetLeft()), TO(bb.GetTop()), TO(bb.GetRight()), TO(bb.GetBottom())
        for li, lay in enumerate(LAYERS):
            if not z.IsOnLayer(lay):
                continue
            targets = []
            if z.GetDoNotAllowTracks():
                targets.append((self.towner[li], self.w / 2 + 0.05))
            if z.GetDoNotAllowVias():
                targets.append((self.vowner, self.via / 2 + 0.05))
            for arr, infl in targets:
                for i, j in self._cells_near(x0, y0, x1, y1, infl):
                    px, py = self.pos(i, j)
                    if poly.Collide(pcbnew.VECTOR2I(MM(px), MM(py)), MM(infl)):
                        arr[self.idx(i, j)] = -1

    # ---- routing
    def free_track(self, li, k, net):
        o = self.towner[li][k]
        return o == 0 or o == net

    def free_via(self, k, net):
        o = self.vowner[k]
        return o == 0 or o == net

    def item_cells(self, item, net):
        """(layer index, cell) pairs inside the copper of a pad or track, usable as route ends."""
        out = set()
        bb = item.GetBoundingBox()
        x0, y0, x1, y1 = TO(bb.GetLeft()), TO(bb.GetTop()), TO(bb.GetRight()), TO(bb.GetBottom())
        for li, lay in enumerate(LAYERS):
            if not item.IsOnLayer(lay):
                continue
            for i, j in self._cells_near(x0, y0, x1, y1, 0.0):
                px, py = self.pos(i, j)
                if item.HitTest(pcbnew.VECTOR2I(MM(px), MM(py)), 0):
                    k = self.idx(i, j)
                    if self.free_track(li, k, net):
                        out.add((li, k))
        return out

    def route(self, net, starts, goals, via_cost=10.0, max_nodes=3000000, soft=None, penalty=0.0):
        """A* from any start to any goal cell. With `soft` (a full grid of the same board, while
        this one leaves some copper out), a cell that `soft` blocks costs `penalty` more."""
        if not starts or not goals:
            return None
        nx = self.nx
        gi = [(k % nx, k // nx) for _, k in goals]
        gx = sum(i for i, _ in gi) / len(gi)
        gy = sum(j for _, j in gi) / len(gi)
        spread = max(max(abs(i - gx), abs(j - gy)) for i, j in gi)

        def h(k):
            i, j = k % nx, k // nx
            dx, dy = abs(i - gx), abs(j - gy)
            d = max(dx, dy) + 0.414 * min(dx, dy) - spread
            return d if d > 0 else 0.0
        goal = set(goals)
        dist = {}
        prev = {}
        pq = []
        for s in starts:
            dist[s] = 0.0
            heapq.heappush(pq, (h(s[1]), 0.0, s))
        steps = ((1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0),
                 (1, 1, 1.414), (1, -1, 1.414), (-1, 1, 1.414), (-1, -1, 1.414))
        n = 0
        while pq:
            f, g, s = heapq.heappop(pq)
            if g > dist.get(s, 1e18):
                continue
            if s in goal:
                path = [s]
                while path[-1] in prev:
                    path.append(prev[path[-1]])
                return path[::-1]
            n += 1
            if n > max_nodes:
                return None
            li, k = s
            i, j = k % nx, k // nx
            for di, dj, c in steps:
                ii, jj = i + di, j + dj
                if not (0 <= ii < nx and 0 <= jj < self.ny):
                    continue
                kk = jj * nx + ii
                if not self.free_track(li, kk, net):
                    continue
                if di and dj:   # no corner cutting past an obstacle
                    if not (self.free_track(li, j * nx + ii, net) and self.free_track(li, jj * nx + i, net)):
                        continue
                ns = (li, kk)
                ng = g + c
                if soft is not None and not soft.free_track(li, kk, net):
                    ng += penalty
                if ng < dist.get(ns, 1e18):
                    dist[ns] = ng
                    prev[ns] = s
                    heapq.heappush(pq, (ng + h(kk), ng, ns))
            if self.free_via(k, net):
                ns = (1 - li, k)
                if self.free_track(1 - li, k, net):
                    ng = g + via_cost
                    if soft is not None and not soft.free_via(k, net):
                        ng += penalty
                    if ng < dist.get(ns, 1e18):
                        dist[ns] = ng
                        prev[ns] = s
                        heapq.heappush(pq, (ng + h(k), ng, ns))
        return None

    def commit(self, net_item, path, width=None):
        """Turn a path into tracks and vias on the board, and into obstacles on the grid."""
        width = width or self.w
        net = net_item.GetNetCode()
        pts = [(li, self.pos(k % self.nx, k // self.nx)) for li, k in path]
        # compress: keep corners and layer changes
        keep = [pts[0]]
        for a, b, c in zip(pts, pts[1:], pts[2:]):
            if a[0] != b[0] or b[0] != c[0]:
                keep.append(b)
                continue
            d1 = (round(b[1][0] - a[1][0], 4), round(b[1][1] - a[1][1], 4))
            d2 = (round(c[1][0] - b[1][0], 4), round(c[1][1] - b[1][1], 4))
            if d1 != d2:
                keep.append(b)
        keep.append(pts[-1])
        items = []
        for (la, pa), (lb, pb) in zip(keep, keep[1:]):
            if la != lb:
                v = pcbnew.PCB_VIA(self.b)
                v.SetPosition(pcbnew.VECTOR2I(MM(pa[0]), MM(pa[1])))
                v.SetWidth(MM(self.via))
                v.SetDrill(MM(self.drill))
                v.SetNet(net_item)
                self.b.Add(v)
                items.append(v)
                self.add_disc(pa[0], pa[1], self.via / 2, LAYERS, net)
                continue
            t = pcbnew.PCB_TRACK(self.b)
            t.SetStart(pcbnew.VECTOR2I(MM(pa[0]), MM(pa[1])))
            t.SetEnd(pcbnew.VECTOR2I(MM(pb[0]), MM(pb[1])))
            t.SetLayer(LAYERS[la])
            t.SetWidth(MM(width))
            t.SetNet(net_item)
            self.b.Add(t)
            items.append(t)
            self.add_segment(pa[0], pa[1], pb[0], pb[1], width / 2, (LAYERS[la],), net)
        return items


def _seg_dist(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    L = dx * dx + dy * dy
    if L == 0:
        return math.hypot(px - x1, py - y1)
    t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / L))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def unconnected_pairs(board, drc_json, skip=("GND",)):
    """Pairs of items still to join, from a kicad-cli DRC report of this board."""
    import json
    rep = json.load(open(drc_json))
    by_id = {}
    for t in board.GetTracks():
        by_id[t.m_Uuid.AsString()] = t
    for fp in board.GetFootprints():
        for p in fp.Pads():
            by_id[p.m_Uuid.AsString()] = p
    pairs = []
    for u in rep.get("unconnected_items", []):
        items = [by_id.get(i["uuid"]) for i in u["items"]]
        if len(items) != 2 or None in items:
            continue
        net = items[0].GetNet()
        if net.GetNetname() in skip:
            continue
        pairs.append((net.GetNetname(), net, items[0], items[1]))
    return pairs


FIRST = ("I2C_SDA", "I2C_SCL", "DAC_SCK_T", "DAC_MOSI_T", "DAC_CS1_N_T", "DAC_CS2_N_T", "GATE1", "GATE2",
         "GATE3", "GATE4", "VBUS")


def by_length(pairs):
    """The board-wide I2C bus first, then short connections before long ones."""
    def length(p):
        a, b = p[2].GetPosition(), p[3].GetPosition()
        first = 0 if p[0].rsplit("/", 1)[-1] in FIRST else 1
        return (first, math.hypot(TO(a.x - b.x), TO(a.y - b.y)))
    return sorted(pairs, key=length)


def route_all(board, pairs, order=by_length, log=print, first=(), **kw):
    """Route the given pairs one by one; returns the pairs that could not be routed."""
    g = Grid(board, **kw)
    failed = []
    pairs = order(pairs) if order else pairs
    pairs = [p for p in pairs if p[0] in first] + [p for p in pairs if p[0] not in first]
    for name, net, a, b in pairs:
        code = net.GetNetCode()
        width = 0.5 if name in ("+3V3", "VBUS") else (0.6 if name == "+5V_LED" else g.w)
        starts, goals = g.item_cells(a, code), g.item_cells(b, code)
        path = g.route(code, starts, goals)
        if path is None:
            failed.append((name, net, a, b))
            continue
        g.commit(net, path, width if width <= g.w else g.w)
    # second chance for what failed: a bigger search, cheaper vias
    still = []
    for name, net, a, b in failed:
        code = net.GetNetCode()
        path = g.route(code, g.item_cells(a, code), g.item_cells(b, code), via_cost=4.0, max_nodes=12000000)
        if path is None:
            still.append((name, net, a, b))
            log("  no route: %s" % name)
            continue
        g.commit(net, path)
    return still


def soft_net(name):
    """Signal nets may be ripped up and routed again; ground and the supply rails stay."""
    return name not in ("GND", "VBUS") and not name.startswith("+")


def crossed_nets(board, failed, penalty=40.0):
    """For each connection that found no way: the cheapest path when the unlocked copper of other
    signal nets may be crossed at `penalty` per cell, and the codes of the nets whose copper that
    path runs into."""
    soft_codes = {n.GetNetCode() for n in board.GetNetsByNetcode().values() if soft_net(n.GetNetname())}

    def skip(t):
        return not t.IsLocked() and t.GetNetCode() in soft_codes
    full = Grid(board)
    hard = Grid(board, skip=skip)
    soft_items = [t for t in board.GetTracks() if skip(t)]
    out = set()
    for name, net, a, b in failed:
        code = net.GetNetCode()
        path = hard.route(code, hard.item_cells(a, code), hard.item_cells(b, code), via_cost=4.0,
                          max_nodes=12000000, soft=full, penalty=penalty)
        if path is None:
            print("  %s: no way even through other signal nets" % name)
            continue
        hit = set()
        prev_li = None
        for li, k in path:
            x, y = full.pos(k % full.nx, k // full.nx)
            via = prev_li is not None and li != prev_li
            prev_li = li
            if full.free_track(li, k, code) and not (via and not full.free_via(k, code)):
                continue
            for t in soft_items:
                c = t.GetNetCode()
                if c == code or c in hit:
                    continue
                if t.Type() == pcbnew.PCB_VIA_T:
                    p = t.GetPosition()
                    d = math.hypot(TO(p.x) - x, TO(p.y) - y)
                    reach = TO(t.GetWidth(pcbnew.F_Cu)) / 2 + (full.v_infl if via else full.t_infl)
                else:
                    if t.GetLayer() != LAYERS[li] and not via:
                        continue
                    s_, e_ = t.GetStart(), t.GetEnd()
                    d = _seg_dist(x, y, TO(s_.x), TO(s_.y), TO(e_.x), TO(e_.y))
                    reach = TO(t.GetWidth()) / 2 + (full.v_infl if via else full.t_infl)
                if d <= reach:
                    hit.add(c)
        names = sorted(n.GetNetname()
                       for n in board.GetNetsByNetcode().values() if n.GetNetCode() in hit)
        print("  %s crosses %s" % (name, names))
        out |= hit
    return out


def net_items(board, codes):
    """Uuids of the unlocked tracks and vias of the given nets."""
    return {t.m_Uuid.AsString() for t in board.GetTracks() if t.GetNetCode() in codes and not t.IsLocked()}


def blockers(board, items, radius=1.5):
    """Every unlocked track and via of each other net that has copper within `radius` mm of the
    given items: whole nets are ripped up, so no half-routed pieces stay behind."""
    nets = set()
    for it in items:
        bb = it.GetBoundingBox()
        bb.Inflate(MM(radius))
        code = it.GetNetCode()
        for t in board.GetTracks():
            if t.GetNetCode() != code and not t.IsLocked() and bb.Intersects(t.GetBoundingBox()):
                nets.add(t.GetNetCode())
    gnd = board.FindNet("GND").GetNetCode()
    nets.discard(gnd)
    return {t.m_Uuid.AsString() for t in board.GetTracks() if t.GetNetCode() in nets and not t.IsLocked()}


def remove_items_text(pcb_path, uuids):
    """Delete tracks and vias by uuid from the board file (removal through the Python API
    leaves KiCad 10's board object unusable)."""
    import re
    text = open(pcb_path).read()
    n = 0
    for uid in uuids:
        pat = re.compile(r'\n\t\((?:segment|via|arc)\n(?:\t\t[^\n]*\n)*?\t\t\(uuid "%s"\)\n\t\)' % re.escape(uid))
        text, k = pat.subn("", text)
        n += k
    open(pcb_path, "w").write(text)
    return n
