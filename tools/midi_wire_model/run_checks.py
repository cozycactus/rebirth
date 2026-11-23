#!/usr/bin/env python3
"""Checks of the MIDI output admission rule on a byte-level model (README.md).

    python3 tools/midi_wire_model/run_checks.py            # quick, under a minute
    python3 tools/midi_wire_model/run_checks.py --full     # every sweep, about 25 min
    python3 tools/midi_wire_model/run_checks.py --full --log /tmp/zd.jsonl   # resumable
    python3 tools/midi_wire_model/run_checks.py --margin 32 --flam-cap 319   # a variant

Exits 1 on any failed assertion: an overflow or fill + 3 x owed above 1,023
under the rule (time-domain families and the zero-drain sweep from any state),
a skip, a lost second hit or a flam played plain at a load the density promise
covers, one note above the table not skipped (or the wrong one), the skipped
Note On semantics broken, a calibration cell off the host measurements, or the
old rule R0 no longer overflowing where it should.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import argparse
import os
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import checks  # noqa: E402
from sim import Rule, CAP_SAFE  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--full', action='store_true', help='every sweep at full size')
    ap.add_argument('--log', metavar='FILE',
                    help='the zero-drain sweep logs each tempo here and resumes from it '
                         '(default: a file in the system temporary folder)')
    ap.add_argument('--margin', type=int, help="G = min(D + margin, 765); default the design's")
    ap.add_argument('--flam-cap', help="flam cap: a byte count up to 765, or G; default the design's")
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    cap = args.flam_cap
    if cap is not None and cap != 'G':
        cap = int(cap)
        if cap > CAP_SAFE:
            ap.error('a flam cap above 765 breaks the proof; the model would clip it at 765')
    rule = Rule('R1', margin=args.margin, flam_cap=cap)
    r0 = Rule('R0')
    print('rule: %r' % rule)
    print('mode: %s' % ('full' if args.full else 'quick'))
    t0 = time.time()
    fails = []
    if args.full:
        fails += checks.calibrate(long_cells=tuple(checks.PROBE_LONG), verbose=args.verbose)
        fails += checks.density(rule, 1024, verbose=args.verbose)
        fails += checks.kit_edge(rule, 48, fps=(1, 3, 12), verbose=args.verbose)
        fails += checks.safety(rule, bpm_step=2, seeds=3, steps=128, bpm_lo=20)
        fails += checks.first_step(rule)
        fails += checks.thru_window(rule, step=0.01)
        checks.thru_window(r0, step=0.01)
        fails += checks.safety(r0, bpm_step=2, seeds=1, flam_both=False,
                               families=('prefill_G-258',), expect_safe=False)
        log = args.log or os.path.join(tempfile.gettempdir(), 'midi_wire_model_zero_drain.jsonl')
        print('zero drain log: %s' % log)
        fails += checks.zero_drain(rule, log)
    else:
        fails += checks.calibrate(verbose=args.verbose)
        fails += checks.density(rule, 96, verbose=args.verbose)
        fails += checks.kit_edge(rule, 24, every=8, voices=(1, 11), fps=(1, 12),
                                 verbose=args.verbose)
        fails += checks.safety(rule, bpm_step=13, seeds=1, steps=96)
        fails += checks.first_step(rule)
        fails += checks.thru_window(rule)
        fails += checks.safety(r0, bpm_step=6, seeds=1, flam_both=False,
                               families=('prefill_G-258',), expect_safe=False)
        fails += checks.zero_drain(rule, f0_step=31, bpms=(40, 67.6, 105.6, 300))
    print('time: %.0f s' % (time.time() - t0))
    if fails:
        print('FAILED: %d' % len(fails))
        for f in fails:
            print('  ' + f)
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
