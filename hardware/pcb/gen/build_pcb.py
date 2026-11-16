#!/usr/bin/env python3
"""Generate the two boards from their schematics.

Run with KiCad's own Python (it has pcbnew):

    PY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
    $PY hardware/pcb/gen/build_pcb.py panel place      # footprints, outline, holes, regular routing
    $PY hardware/pcb/gen/build_pcb.py panel route      # Freerouting for the rest (FREEROUTING_JAR)
    $PY hardware/pcb/gen/build_pcb.py panel maze       # grid router for whatever is left
    $PY hardware/pcb/gen/build_pcb.py panel finish     # zones, fill, DRC with schematic parity

The same for `main`. `place` overwrites the board; `route` and `finish` work on the saved file.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pcbnew  # noqa: E402

import common  # noqa: E402

BOARDS = {"panel": os.path.join(common.PCB, "panel"), "main": os.path.join(common.PCB, "main")}
FREEROUTING_JAR = os.environ.get("FREEROUTING_JAR", "")


def paths(name):
    d = BOARDS[name]
    return d, os.path.join(d, name + ".kicad_sch"), os.path.join(d, name + ".kicad_pcb"), os.path.join(d, name + ".kicad_pro")


def place(name):
    d, sch, pcb, pro = paths(name)
    os.makedirs(os.path.join(d, ".check"), exist_ok=True)
    comps, nets, pin_net = common.export_netlist(sch, os.path.join(d, ".check", "netlist.net"))
    bd = common.Board(comps, nets, pin_net)
    if name == "panel":
        import panel as layout
    else:
        import mainboard as layout
    layout.place(bd)
    layout.holes(bd)
    if hasattr(layout, "prerouted"):
        layout.prerouted(bd)
    common.write_project_rules(pro, getattr(layout, "EXTRA_CLASSES", ()))
    common.write_dru(pro)
    bd.save(pcb)
    print("%s: %d footprints, %d nets -> %s" % (name, len(bd.fp), len(nets), pcb))


def route(name, passes=None):
    passes = passes or os.environ.get("ROUTE_PASSES", "20")
    d, sch, pcb, pro = paths(name)
    if not FREEROUTING_JAR or not os.path.exists(FREEROUTING_JAR):
        sys.exit("set FREEROUTING_JAR to a Freerouting 2.x jar")
    board = pcbnew.LoadBoard(pcb)
    dsn = os.path.join(d, ".check", name + ".dsn")
    ses = os.path.join(d, ".check", name + ".ses")
    if not pcbnew.ExportSpecctraDSN(board, dsn):
        sys.exit("DSN export failed")
    strip_nets(dsn, ("GND",))
    common.run(["java", "-jar", FREEROUTING_JAR, "-de", dsn, "-do", ses, "-mp", passes, "--gui.enabled=false"],
               check=True)
    if not pcbnew.ImportSpecctraSES(board, ses):
        sys.exit("SES import failed")
    normalize_vias(board)
    pcbnew.SaveBoard(pcb, board)


def normalize_vias(board):
    """Router vias take their net class size (power nets 0.8/0.4, the rest 0.6/0.3)."""
    big = {n for _, _, _, _, _, nets in common.NETCLASSES for n in nets}
    for v in board.GetTracks():
        if v.Type() != pcbnew.PCB_VIA_T:
            continue
        dia, drill = (0.8, 0.4) if v.GetNetname() in big else (0.6, 0.3)
        v.SetWidth(common.MM(dia))
        v.SetDrill(common.MM(drill))


def maze_route(name, rounds=None):
    """Join what is still unconnected (GND aside) with the grid router in gen/maze.py. A connection
    that finds no way through gets a trial path that may cross other signal nets; the nets it
    crosses are ripped up, the failed connections go first in the next round and the ripped nets
    after them. The last round rips nothing, and the best round is kept."""
    import shutil
    import maze
    rounds = rounds or int(os.environ.get("MAZE_ROUNDS", "8"))
    d, sch, pcb, pro = paths(name)
    rep = os.path.join(d, ".check", "drc_maze.json")
    best_path = os.path.join(d, ".check", name + "_maze_best.kicad_pcb")
    best = None
    first = []
    for r in range(rounds):
        common.run([common.KICAD_CLI, "pcb", "drc", "--format", "json", "-o", rep, pcb], capture_output=True)
        board = pcbnew.LoadBoard(pcb)
        pairs = maze.unconnected_pairs(board, rep)
        print("%s: round %d, %d connections to route" % (name, r + 1, len(pairs)))
        failed = maze.route_all(board, pairs, first=tuple(first))
        pcbnew.SaveBoard(pcb, board)
        if best is None or len(failed) < best:
            best = len(failed)
            shutil.copyfile(pcb, best_path)
        if not failed:
            print("%s: everything routed" % name)
            return
        for f in failed:          # nets that failed once keep their priority
            if f[0] in first:
                first.remove(f[0])
            first.insert(0, f[0])
        if r == rounds - 1:
            break
        rip = maze.net_items(board, maze.crossed_nets(board, failed))
        n = maze.remove_items_text(pcb, rip)
        print("%s: %d not routed %s; ripped up %d items of the nets in the way" % (
            name, len(failed), sorted({f[0] for f in failed}), n))
    if best < len(failed):
        shutil.copyfile(best_path, pcb)
    print("%s: still not routed after %d rounds: %d connections (best round kept)" % (name, rounds, best))


def strip_nets(dsn, names):
    """GND is poured on both layers and stitched afterwards: the router leaves it alone."""
    import re
    text = open(dsn).read()
    for n in names:
        text = re.sub(r'\(net %s\s*\(pins[^)]*\)\s*\)' % re.escape(n), "", text)
        text = re.sub(r'(\(class [^\n]*?)\s%s(?=[\s)])' % re.escape(n), r"\1", text)
    open(dsn, "w").write(text)


def finish(name):
    d, sch, pcb, pro = paths(name)
    print("dangling vias removed: %d" % common.remove_dangling_vias_text(pcb))
    board = pcbnew.LoadBoard(pcb)
    comps, _, _ = common.export_netlist(sch, os.path.join(d, ".check", "netlist.net"))
    common.sync_fields(board, comps)
    if name == "panel":
        import panel as layout
    else:
        import mainboard as layout
    if hasattr(layout, "finish"):
        layout.finish(board)
    pcbnew.SaveBoard(pcb, board)
    stuck = getattr(board, "_stuck_pads", [])
    if stuck:
        # GND pads walled in by signal copper: rip it up, give the pads their via, route again
        import maze
        refpads = sorted({(p.GetParentFootprint().GetReference(), p.GetNumber()) for p in stuck})
        rip = maze.blockers(board, stuck, 1.2)
        print("%d GND pads walled in %s, ripping up %d items" % (len(stuck), refpads,
                                                                 maze.remove_items_text(pcb, rip)))
        board = pcbnew.LoadBoard(pcb)
        print("walled-in pads joined: %d" % common.stitch_these(board, refpads))
        pcbnew.SaveBoard(pcb, board)
        maze_route(name)
    rep = os.path.join(d, ".check", "drc.json")
    common.run([common.KICAD_CLI, "pcb", "drc", "--schematic-parity", "--severity-all", "--refill-zones",
                "--save-board", "--format", "json", "-o", rep, pcb])
    drc = json.load(open(rep))
    summary = {}
    for v in drc.get("violations", []) + drc.get("unconnected_items", []) + drc.get("schematic_parity", []):
        key = (v.get("severity"), v.get("type"))
        summary[key] = summary.get(key, 0) + 1
    print("%s DRC: %d violations, %d unconnected, %d parity" % (
        name, len(drc.get("violations", [])), len(drc.get("unconnected_items", [])),
        len(drc.get("schematic_parity", []))))
    for (sev, typ), n in sorted(summary.items()):
        print("  %-8s %-32s %d" % (sev, typ, n))


if __name__ == "__main__":
    board_name, step = sys.argv[1], sys.argv[2]
    {"place": place, "route": route, "maze": maze_route, "finish": finish}[step](board_name)
