"""The model's checks. Each returns a list of failure strings (empty: passed)
and prints one summary line. run_checks.py chooses which run.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import json
import os
import random
import sys

from sim import (Rule, Seq, Thru, Overflow, run, pulse_us_for, D_for, PPS, QMAX,
                 CAP_SAFE)
from cases import synth_tracks, kit, all_tracks, KIT_NOTES
from sim import Kit, Synth

TABLE_BPMS = (60, 100, 120, 140, 160, 180, 187.5, 200, 240, 300)


def say(line):
    print(line)
    sys.stdout.flush()


def table_n(bpm):
    D = D_for(pulse_us_for(bpm))
    return min(64, (D - 6) // 6)


# ---------------------------------------------------------------------------
# Calibration: rule 'today' against the host measurements of the firmware

# (case, bpm): (stop step or None, peak queued bytes or None, wire bytes per step or None)
PROBE = {
    ('A16 g50', 300): (None, 48, 102.0), ('A16 g100', 300): (None, 95, 101.62),
    ('A16 g10', 100): (None, 56, 102.0), ('A16 g10', 300): (None, 82, 102.0),
    ('A26 g50', 300): (None, 813, None), ('A32 g50', 240): (None, 439, None),
    ('A32 g50', 300): (22, None, None), ('A48 g50', 180): (26, None, None),
    ('A48 g50', 300): (6, None, None), ('A64 g50', 140): (15, None, None),
    ('A64 g50', 300): (4, None, None), ('C14 g50', 240): (None, 108, 182.0),
    ('C14 g50', 300): (29, None, None), ('C22 g50', 240): (22, None, None),
    ('C14 g100', 240): (None, 215, None), ('C14 g10', 240): (None, 192, None),
    ('D0 flam g50', 100): (None, 130, 188.0), ('D0 flam g50', 240): (None, 163, None),
    ('D14 flam g50', 300): (24, None, None),
}
# flam sends refused at the cap: (case, bpm): probe value (checked within 2)
PROBE_FLAM = {('D14 flam g50', 180): 31, ('D14 flam g50', 200): 105,
              ('D14 flam g50', 240): 215, ('D14 flam g50', 300): 45,
              ('D0 flam g50', 300): 86}

CAL_CASES = {
    'A16 g50': lambda: synth_tracks(16, 50), 'A16 g100': lambda: synth_tracks(16, 100),
    'A16 g10': lambda: synth_tracks(16, 10), 'A26 g50': lambda: synth_tracks(26, 50),
    'A32 g50': lambda: synth_tracks(32, 50), 'A48 g50': lambda: synth_tracks(48, 50),
    'A64 g50': lambda: synth_tracks(62, 50) + synth_tracks(2, 50, first_index=0),
    'C14 g50': lambda: [kit(9, 50), kit(10, 50)] + synth_tracks(14, 50),
    'C22 g50': lambda: [kit(9, 50), kit(10, 50)] + synth_tracks(22, 50),
    'C14 g100': lambda: [kit(9, 100), kit(10, 100)] + synth_tracks(14, 100),
    'C14 g10': lambda: [kit(9, 10), kit(10, 10)] + synth_tracks(14, 10),
    'D0 flam g50': lambda: [kit(9, 50, True), kit(10, 50, True)],
    'D14 flam g50': lambda: [kit(9, 50, True), kit(10, 50, True)] + synth_tracks(14, 50),
}


def calibrate(verbose=False):
    fails = []
    cells = sorted(set(PROBE) | set(PROBE_FLAM))
    for case, bpm in cells:
        s = Seq('today', CAL_CASES[case](), bpm)
        ov = run(s, 128)
        stop = s.pulse // PPS if ov else None
        wps = s.port.wire / 128 if not ov else None
        if (case, bpm) in PROBE:
            want_stop, want_peak, want_wps = PROBE[(case, bpm)]
            if stop != want_stop:
                fails.append('calibrate %s %s BPM: stop %s, probe %s' % (case, bpm, stop, want_stop))
            if want_peak is not None and s.port.peak_fill != want_peak:
                fails.append('calibrate %s %s BPM: peak %d, probe %d' % (case, bpm, s.port.peak_fill, want_peak))
            if want_wps is not None and abs(wps - want_wps) > 0.01:
                fails.append('calibrate %s %s BPM: wire/step %.2f, probe %.2f' % (case, bpm, wps, want_wps))
        if (case, bpm) in PROBE_FLAM and abs(s.flam_drops - PROBE_FLAM[(case, bpm)]) > 2:
            fails.append('calibrate %s %s BPM: flam drops %d, probe %d' % (case, bpm, s.flam_drops, PROBE_FLAM[(case, bpm)]))
        if verbose:
            say('  %-13s %5s BPM stop %-4s peak %4d wire/step %s flam drops %d' % (
                case, bpm, stop, s.port.peak_fill, '%.2f' % wps if wps else '-', s.flam_drops))
    say('calibrate: %d probe cells, %d mismatches' % (len(cells), len(fails)))
    return fails


# ---------------------------------------------------------------------------
# Density: the table's rows and the kit corollaries must skip nothing and lose
# no second hit

def _density_one(rule, tracks, bpm, steps):
    s = Seq(rule, tracks, bpm)
    ov = run(s, steps)
    return s, ov


def density(rule, steps, bpms=TABLE_BPMS, verbose=False):
    fails = []
    cells = 0
    tight = None
    for bpm in bpms:
        n = table_n(bpm)
        loads = [('n plain g%d' % g, lambda g=g: synth_tracks(n, g, first_index=0)) for g in (10, 50, 100)]
        loads += [('n tied', lambda: synth_tracks(n, 50, 'tied', first_index=0)),
                  ('n tie_end', lambda: synth_tracks(n, 50, 'tie_end', first_index=0)),
                  ('n slide', lambda: synth_tracks(n, 50, 'slide', first_index=0))]
        for label, mk in loads:
            s, ov = _density_one(rule, mk(), bpm, steps)
            cells += 1
            worst = max(f0 + S - G for f0, S, G in s.step_log)
            tight = worst if tight is None else max(tight, worst)
            if ov or s.skips or s.flam_voice_drops:
                fails.append('density %s BPM %s: skips %d, second hits lost %d%s' % (
                    bpm, label, s.skips, s.flam_voice_drops, ', OVERFLOW' if ov else ''))
            if verbose:
                say('  %6s BPM %-12s skips %d  max F0+S-G %d' % (bpm, label, s.skips, worst))
    # kit corollaries of the density promise
    kit_loads = []
    for g in (10, 50, 100):
        kit_loads += [
            (240, '2 kits + 14 SYNTH g%d' % g, lambda g=g: [kit(9, g), kit(10, g)] + synth_tracks(14, g)),
            (240, '2 kits + 14 SYNTH sliding g%d' % g, lambda g=g: [kit(9, g), kit(10, g)] + synth_tracks(14, g, 'slide')),
            (240, '2 kits flammed g%d' % g, lambda g=g: [kit(9, g, True), kit(10, g, True)]),
            (240, '2 kits flammed fp12 g%d' % g, lambda g=g: [kit(9, g, True, 12), kit(10, g, True, 12)]),
            (300, 'default 16 SYNTH g%d' % g, lambda g=g: synth_tracks(16, g)),
            (300, 'default 16 SYNTH sliding g%d' % g, lambda g=g: synth_tracks(16, g, 'slide')),
        ]
    kit_loads.append((120, '2 kits flammed + 33 SYNTH g100',
                      lambda: [kit(9, 100, True), kit(10, 100, True)] + synth_tracks(33, 100)))
    for bpm, label, mk in kit_loads:
        s, ov = _density_one(rule, mk(), bpm, steps)
        cells += 1
        if ov or s.skips or s.flam_voice_drops or s.flam_budget_drops:
            fails.append('density %s BPM %s: skips %d, second hits lost %d, flams played plain %d%s' % (
                bpm, label, s.skips, s.flam_voice_drops, s.flam_budget_drops, ', OVERFLOW' if ov else ''))
        if verbose:
            say('  %6s BPM %-32s skips %d lost second hits %d' % (bpm, label, s.skips, s.flam_voice_drops))
    say('density (%s, %d steps): %d loads, %d failing; tightest step F0 + S - G = %d' % (
        rule, steps, cells, len(fails), tight))
    return fails


def kit_edge(rule, steps, bpm_step=0.3, voices=(1, 6, 11), fps=(1, 12), verbose=False):
    """1-2 kits of `voices` voices, flammed or not, plus SYNTH tracks filling
    the budget exactly, at every D: no skip, no lost second hit."""
    fails = []
    cells = 0
    seen = set()
    b10 = 400
    while b10 <= 3000:
        bpm = b10 / 10
        b10 += int(bpm_step * 10)
        D = D_for(pulse_us_for(bpm))
        if D in seen:
            continue
        seen.add(D)
        for nk in (1, 2):
            for v in voices:
                for flam, fp in [(False, 3)] + [(True, f) for f in fps]:
                    for gate in (10, 100):
                        kc = (4 * v + 2 + ((4 * v + 1) if flam else 0)) * nk
                        K = (D - 6 - kc) // 6
                        if K < 0 or K > 62:
                            continue
                        tracks = [kit(9 + i, gate, flam, fp, v) for i in range(nk)] + synth_tracks(K, gate)
                        s = Seq(rule, tracks, bpm)
                        ov = run(s, steps)
                        cells += 1
                        if ov or s.skips or s.flam_voice_drops:
                            fails.append('kit edge %.1f BPM D %d: %d kits x %d voices flam %s fp %d gate %d + %d SYNTH: '
                                         'skips %d, second hits lost %d%s' % (
                                             bpm, D, nk, v, flam, fp, gate, K, s.skips, s.flam_voice_drops,
                                             ', OVERFLOW' if ov else ''))
    say('kit edge (%s, %d steps): %d cells over %d values of D, %d failing' % (
        rule, steps, cells, len(seen), len(fails)))
    if verbose:
        for f in fails[:10]:
            say('  ' + f)
    return fails


# ---------------------------------------------------------------------------
# Safety in time: adversarial families with the real drain

def family_run(rule, bpm, family, gate, seed=0, flam=False, steps=48):
    rng = random.Random(seed)
    style = {'hammer': 'tie_end', 'slide_hammer': 'slide', 'catchup': 'tie_end',
             'jumps': 'slide', 'random': 'random'}.get(family, 'tie_end')
    tracks = all_tracks(gate, style, flam=flam, rng=rng if family == 'random' else None)
    thru = spacing = step_P = None
    rr = rule if isinstance(rule, Rule) else Rule(rule)
    G = min(D_for(pulse_us_for(bpm)) + rr.margin, CAP_SAFE)
    if family.startswith('prefill'):
        target = {'prefill_G-258': G - 258, 'prefill_511': 511, 'prefill_765': 765}[family]
        thru = Thru('prefill', at=7, target=target)
    elif family in ('hammer', 'slide_hammer', 'jumps'):
        thru = Thru('hammer')
    elif family == 'random':
        thru = Thru(rng.choice(['hammer', 'window', 'prefill']), at=rng.randint(1, 20),
                    offs=rng.random() < 0.5)
    if family in ('catchup', 'random'):
        burst_at = rng.randint(2, 30) * PPS + rng.randint(0, 23)

        def spacing(seq, p):
            # a burst: 48 pulses four times as fast, at least 250 us apart
            if burst_at <= p < burst_at + 48:
                return max(seq.P // 4, 250)
            return seq.P
    if family in ('jumps', 'random'):
        def step_P(seq, s):
            return pulse_us_for(rng.uniform(40, 300))
    s = Seq(rule, tracks, bpm, thru=thru, seed=seed)
    ov = run(s, steps, spacing=spacing, step_P=step_P, between=thru.between if thru else None)
    return s, ov


FAMILIES = ('prefill_G-258', 'prefill_511', 'prefill_765', 'hammer', 'slide_hammer',
            'catchup', 'jumps', 'random')


def safety(rule, bpm_step, seeds, flam_both=True, families=FAMILIES, expect_safe=True):
    fails = []
    peak_fill = peak_Q = runs = overflows = 0
    for fam in families:
        for flam in ((False, True) if flam_both else (False,)):
            for bpm in range(40, 301, bpm_step):
                for gate in ((10, 50, 100) if fam != 'random' else (0,)):
                    for sd in (range(seeds) if fam in ('catchup', 'jumps', 'random') else (0,)):
                        s, ov = family_run(rule, bpm, fam, gate, seed=sd * 1000 + bpm, flam=flam)
                        runs += 1
                        peak_fill = max(peak_fill, s.port.peak_fill)
                        peak_Q = max(peak_Q, s.port.peak_Q)
                        if ov or s.port.peak_Q > QMAX:
                            overflows += 1
                            if expect_safe and len(fails) < 20:
                                fails.append('safety %s %s BPM gate %d seed %d flam %s: %s, fill+3owed %d' % (
                                    fam, bpm, gate, sd, flam, ov or 'no overflow', s.port.peak_Q))
    say('safety (%s): %d runs, peak fill %d, peak fill + 3 x owed %d, overflows %d' % (
        rule, runs, peak_fill, peak_Q, overflows))
    if not expect_safe and overflows == 0:
        fails.append('safety (%s): expected an overflow, none found' % rule)
    return fails


def first_step(rule):
    """The first step followed with no Clock estimate, internal and master
    tempo apart: nothing within the table skips, nothing overflows."""
    fails = []
    for internal, master in ((300, 120), (120, 300), (40, 300), (300, 40), (120, 120)):
        n = table_n(master)
        for label, mk in (('table n', lambda: synth_tracks(n, 50, first_index=0)),
                          ('64 tracks + flammed kits', lambda: [kit(9, 10, True), kit(10, 10, True)]
                           + synth_tracks(62, 10, 'slide'))):
            s = Seq(rule, mk(), master, following_unknown=True,
                    D_first=D_for(pulse_us_for(internal)))
            ov = run(s, 32)
            if ov or s.port.peak_Q > QMAX:
                fails.append('first step internal %d master %d %s: overflow' % (internal, master, label))
            if label == 'table n' and s.skips:
                fails.append('first step internal %d master %d: %d skips within the table' % (
                    internal, master, s.skips))
    say('first followed step (%s): 10 cases, %d failing' % (rule, len(fails)))
    return fails


# ---------------------------------------------------------------------------
# Zero drain: one step from any state with fill + 3 x owed <= 1,023, the wire
# stalled, then Stop. Resumable: one log line per finished tempo.

ZD_BPMS = [40, 50, 60, 63.9, 64, 64.1, 70, 78, 90, 98, 100, 120, 160, 187.5, 240, 300]


def _zd_build(style, gate, flam, nkits, leftovers):
    tracks = []
    for t in range(64):
        if t < nkits:
            k = kit(9 + t, gate, flam)
            k.sounding = (1 << 11) - 1 if leftovers else 0
            tracks.append(k)
            continue
        a = 52 + t % 40
        pre = {'ties_end': (True, False), 'slides': (False, True), 'retrig': (False, False),
               'inactive': (False, False)}[style]
        ev = (False, a, False, False) if style == 'inactive' else (True, a + 1, False, style == 'slides')
        s = Synth(t % 16, [ev] * 16, gate)
        s.on, s.note, s.flags_tie_slide, s.countdown = True, a, pre, 0
        tracks.append(s)
    return tracks


def _zd_one(rule, bpm, style, gate, flam, F0, thru_mode, nkits, leftovers):
    tracks = _zd_build(style, gate, flam, nkits, leftovers)
    owed0 = sum(bin(t.sounding).count('1') if t.kit else int(t.on) for t in tracks)
    if F0 + 3 * owed0 > QMAX:
        return None
    thru = Thru(thru_mode) if thru_mode != 'none' else None
    s = Seq(rule, tracks, bpm, thru=thru, seed=F0)
    p = s.port
    p.stall()
    p.q.extend(([0x91, 1, 1] * 400)[:F0])
    p.owed = owed0
    ov = None
    try:
        for _ in range(PPS + 1):       # the step and the next boundary
            s.run_pulse()
            if thru:
                thru.between(s)
            s.t += s.P
        msg, n = [], 0                 # Stop: every release, no drain
        for tr in tracks:
            if tr.kit:
                m, c = s.kit_release_msg(tr)
                msg += m
                n += c
            elif tr.on:
                msg += s.synth_off_msg(tr)
                n += 1
                tr.on = False
        p.mandatory(msg, -n, 'stop')
    except Overflow as e:
        ov = e.args[0]
    return p.peak_Q, ov, F0 + 3 * owed0


def zero_drain(rule, log_path, f0_step=3, expect_safe=True):
    rule = rule if isinstance(rule, Rule) else Rule(rule)
    done = {}
    if os.path.exists(log_path):
        with open(log_path) as f:
            for line in f:
                rec = json.loads(line)
                if rec.get('rule') == repr(rule) and rec.get('f0_step') == f0_step:
                    done[rec['bpm']] = rec
    with open(log_path, 'a') as log:
        for bpm in ZD_BPMS:
            if bpm in done:
                continue
            runs = overflows = 0
            peak = 0
            raised = 0      # the highest fill + 3 x owed among runs where the step raised it
            first = None
            for style in ('ties_end', 'slides', 'retrig', 'inactive'):
                for gate in (10, 100):
                    for flam in (False, True):
                        for nk, lo in ((2, True), (2, False), (0, False)):
                            for thru_mode in ('none', 'window', 'hammer'):
                                for F0 in range(0, QMAX + 1, f0_step):
                                    r = _zd_one(rule, bpm, style, gate, flam, F0, thru_mode, nk, lo)
                                    if r is None:
                                        break
                                    runs += 1
                                    peak = max(peak, r[0])
                                    if r[0] > r[2]:
                                        raised = max(raised, r[0])
                                    if r[1] or r[0] > QMAX:
                                        overflows += 1
                                        if first is None:
                                            first = [style, gate, flam, nk, lo, thru_mode, F0]
            rec = dict(rule=repr(rule), f0_step=f0_step, bpm=bpm, runs=runs, peak=peak,
                       raised=raised, overflows=overflows, first=first)
            log.write(json.dumps(rec) + '\n')
            log.flush()
            done[bpm] = rec
            say('  zero drain %s BPM: %d runs, peak fill + 3 x owed %d (%d where the step raised it), '
                'overflows %d' % (bpm, runs, peak, raised, overflows))
    runs = sum(r['runs'] for r in done.values())
    over = sum(r['overflows'] for r in done.values())
    peak = max(r['peak'] for r in done.values())
    raised = max(r.get('raised', 0) for r in done.values())
    say('zero drain (%s): %d runs, peak fill + 3 x owed %d (%d where the step raised it), '
        'overflows %d' % (rule, runs, peak, raised, over))
    fails = []
    if expect_safe and over:
        fails.append('zero drain: %d overflows, e.g. %s' % (
            over, [(b, r['first']) for b, r in done.items() if r['first']][:3]))
    if not expect_safe and not over:
        fails.append('zero drain (%s): expected overflows, none found' % rule)
    return fails


def thru_window(rule):
    """A pass-through key inside phase 1 on an empty queue is accepted at every
    tempo from 40 to 300 BPM (0.01 BPM steps): retrigger (6 bytes), Note On (3),
    Note Off (3)."""
    from sim import Port, THRU_ON_CAP, STEP_MAX
    rule = rule if isinstance(rule, Rule) else Rule(rule)
    refused = {}
    for b in range(4000, 30001):
        bpm = b / 100
        G = min(D_for(pulse_us_for(bpm)) + rule.margin, CAP_SAFE)
        p = Port(rule)
        p.reserved = max(0, G) if rule.name == 'R0' else min(G, STEP_MAX)
        for label, data, cap in (('retrigger', [0x80, 60, 0, 0x90, 60, 100], THRU_ON_CAP),
                                 ('Note On', [0x90, 60, 100], THRU_ON_CAP),
                                 ('Note Off', [0x80, 60, 0], rule.thru_off_cap)):
            p.q.clear()
            if not p.capped(data, cap):
                refused[label] = max(refused.get(label, 0), bpm)
    say('pass-through in phase 1 on an empty queue (%s): %s' % (
        rule.name, ', '.join('%s refused up to %.2f BPM' % kv for kv in refused.items()) or 'never refused'))
    return ['thru window: %s refused up to %.2f BPM' % kv for kv in refused.items()] if rule.name == 'R1' else []
