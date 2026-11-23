#!/usr/bin/env python3
"""Checks of the MIDI output admission rule on a byte-level model (README.md).

    python3 tools/midi_wire_model/run_checks.py               # quick, under a minute
    python3 tools/midi_wire_model/run_checks.py --full        # the long sweeps, ~3 min
    python3 tools/midi_wire_model/run_checks.py --zero-drain LOG   # adds the zero-drain
                                                                   # sweep, ~2 min, resumable
    python3 tools/midi_wire_model/run_checks.py --margin 32 --flam-cap 319   # a variant

Exits 1 on any failed assertion: an overflow or fill + 3 x owed above 1,023
under the rule, a skip or a lost second hit at a load the density promise
covers, a calibration cell off the host measurements, or the old rule R0 no
longer overflowing where it should.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import argparse
import os
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import checks  # noqa: E402
from sim import Rule  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--full', action='store_true', help='the long sweeps')
    ap.add_argument('--zero-drain', metavar='LOG',
                    help='also run the zero-drain sweep, logging each tempo to LOG (resumes from it)')
    ap.add_argument('--margin', type=int, help='G = min(D + margin, 765); default the design\'s')
    ap.add_argument('--flam-cap', help="flam cap: a byte count or G; default the design's")
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    cap = args.flam_cap
    if cap is not None and cap != 'G':
        cap = int(cap)
    rule = Rule('R1', margin=args.margin, flam_cap=cap)
    r0 = Rule('R0')
    print('rule: %r' % rule)
    print('mode: %s%s' % ('full' if args.full else 'quick',
                          ', zero drain' if args.zero_drain else ''))
    t0 = time.time()
    fails = []
    fails += checks.calibrate(args.verbose)
    if args.full:
        fails += checks.density(rule, 1024, verbose=args.verbose)
        fails += checks.kit_edge(rule, 48, verbose=args.verbose)
        fails += checks.safety(rule, bpm_step=2, seeds=3)
        fails += checks.first_step(rule)
        fails += checks.thru_window(rule)
        checks.thru_window(r0)
        fails += checks.safety(r0, bpm_step=2, seeds=1, flam_both=False,
                               families=('prefill_G-258', 'catchup'), expect_safe=False)
    else:
        fails += checks.density(rule, 96, verbose=args.verbose)
        fails += checks.kit_edge(rule, 24, bpm_step=2.9, voices=(1, 11), fps=(1, 12),
                                 verbose=args.verbose)
        fails += checks.safety(rule, bpm_step=13, seeds=1)
        fails += checks.first_step(rule)
        fails += checks.thru_window(rule)
        fails += checks.safety(r0, bpm_step=6, seeds=1, flam_both=False,
                               families=('prefill_G-258',), expect_safe=False)
    if args.zero_drain:
        fails += checks.zero_drain(rule, args.zero_drain)
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
