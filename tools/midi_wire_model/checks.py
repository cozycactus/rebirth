"""The model's checks. Each returns a list of failure strings (empty: passed)
and prints one summary line. run_checks.py chooses which run.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import json
import os
import random
import sys

from sim import (Rule, Seq, Synth, Thru, Overflow, run, pulse_us_for, D_for, PPS, QMAX,
                 CAP_SAFE, KIT_ORDER)
from cases import synth_tracks, kit, all_tracks

TABLE_BPMS = (60, 100, 120, 140, 160, 180, 187.5, 200, 240, 300)


def say(line):
    print(line)
    sys.stdout.flush()


def as_rule(rule):
    return rule if isinstance(rule, Rule) else Rule(rule)


def table_n(bpm):
    D = D_for(pulse_us_for(bpm))
    return min(64, (D - 6) // 6)


# ---------------------------------------------------------------------------
# Calibration: rule 'today' against the host measurements of the firmware
# (tools/midi_density_probe; 8 bars unless the bars are given)

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
    ('D14 flam g50', 180): (None, None, None), ('D14 flam g50', 200): (None, None, None),
    ('D14 flam g50', 240): (None, None, None), ('D0 flam g50', 300): (None, 319, None),
    ('B16 one-ch g50', 300): (None, 48, 72.0),
}
# flam sends refused at the cap (exact)
PROBE_FLAM = {('D14 flam g50', 180): 31, ('D14 flam g50', 200): 105,
              ('D14 flam g50', 240): 215, ('D14 flam g50', 300): 45,
              ('D0 flam g50', 300): 86}
# 64 bars (`run_probe.py long`): (the step where the transport stopped or None,
# flam drops or None)
PROBE_LONG = {('A26 g50', 300): (164, None), ('A32 g50', 240): (344, None),
              ('A48 g50', 160): (839, None), ('A25 g50', 300): (None, 0),
              ('C14 g50', 240): (None, 0), ('D14 flam g50', 240): (None, 1742),
              ('D22 flam g50', 200): (None, 1945), ('D30 flam g50', 160): (None, 1705),
              ('D6 flam g50', 300): (None, 1539)}

CAL_CASES = {
    'A16 g50': lambda: synth_tracks(16, 50), 'A16 g100': lambda: synth_tracks(16, 100),
    'A16 g10': lambda: synth_tracks(16, 10), 'A25 g50': lambda: synth_tracks(25, 50),
    'A26 g50': lambda: synth_tracks(26, 50),
    'A32 g50': lambda: synth_tracks(32, 50), 'A48 g50': lambda: synth_tracks(48, 50),
    'A64 g50': lambda: synth_tracks(62, 50) + synth_tracks(2, 50, first_index=0),
    'B16 one-ch g50': lambda: synth_tracks(16, 50, first_index=0, one_channel=True),
    'C14 g50': lambda: [kit(9, 50), kit(10, 50)] + synth_tracks(14, 50),
    'C22 g50': lambda: [kit(9, 50), kit(10, 50)] + synth_tracks(22, 50),
    'C14 g100': lambda: [kit(9, 100), kit(10, 100)] + synth_tracks(14, 100),
    'C14 g10': lambda: [kit(9, 10), kit(10, 10)] + synth_tracks(14, 10),
    'D0 flam g50': lambda: [kit(9, 50, True), kit(10, 50, True)],
    'D14 flam g50': lambda: [kit(9, 50, True), kit(10, 50, True)] + synth_tracks(14, 50),
    'D22 flam g50': lambda: [kit(9, 50, True), kit(10, 50, True)] + synth_tracks(22, 50),
    'D30 flam g50': lambda: [kit(9, 50, True), kit(10, 50, True)] + synth_tracks(30, 50),
    'D6 flam g50': lambda: [kit(9, 50, True), kit(10, 50, True)] + synth_tracks(6, 50),
}


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def firmware_tables():
    """kit_voice_order and default_notes as src/kit.c has them."""
    import re
    text = open(os.path.join(ROOT, 'src', 'kit.c')).read()
    out = {}
    for name in ('kit_voice_order', 'default_notes'):
        m = re.search(name + r'\[KIT_VOICES\]\s*=\s*\{([^}]*)\}', text)
        out[name] = [int(x) for x in m.group(1).split(',')] if m else None
    return out


def calibrate(long_cells=(('A26 g50', 300),), verbose=False):
    fails = []
    from cases import KIT_NOTES
    fw = firmware_tables()
    if fw['kit_voice_order'] != KIT_ORDER or fw['default_notes'] != KIT_NOTES:
        fails.append('calibrate: the kit order or notes differ from src/kit.c: %s' % fw)
    for case, bpm in sorted(PROBE):
        s = Seq('today', CAL_CASES[case](), bpm)
        ov = run(s, 128)
        stop = s.pulse // PPS if ov else None
        wps = s.port.wire / 128 if not ov else None
        want_stop, want_peak, want_wps = PROBE[(case, bpm)]
        if stop != want_stop:
            fails.append('calibrate %s %s BPM: stop %s, probe %s' % (case, bpm, stop, want_stop))
        if want_peak is not None and s.port.peak_fill != want_peak:
            fails.append('calibrate %s %s BPM: peak %d, probe %d' % (case, bpm, s.port.peak_fill, want_peak))
        if want_wps is not None and abs(wps - want_wps) > 0.01:
            fails.append('calibrate %s %s BPM: wire/step %.2f, probe %.2f' % (case, bpm, wps, want_wps))
        if (case, bpm) in PROBE_FLAM and s.flam_drops != PROBE_FLAM[(case, bpm)]:
            fails.append('calibrate %s %s BPM: flam drops %d, probe %d' % (case, bpm, s.flam_drops, PROBE_FLAM[(case, bpm)]))
        if verbose:
            say('  %-15s %5s BPM stop %-4s peak %4d wire/step %s flam drops %d' % (
                case, bpm, stop, s.port.peak_fill, '%.2f' % wps if wps else '-', s.flam_drops))
    for case, bpm in long_cells:
        s = Seq('today', CAL_CASES[case](), bpm)
        ov = run(s, 1024)
        stop = s.pulse // PPS if ov else None
        want_stop, want_fd = PROBE_LONG[(case, bpm)]
        if stop != want_stop or (want_fd is not None and s.flam_drops != want_fd):
            fails.append('calibrate 64 bars %s %s BPM: stop %s, flam drops %d; probe %s, %s' % (
                case, bpm, stop, s.flam_drops, want_stop, want_fd))
        if verbose:
            say('  %-15s %5s BPM, 64 bars: stop %s' % (case, bpm, stop))
    say('calibrate: %d probe cells of 8 bars, %d of 64 bars, %d mismatches' % (
        len(PROBE), len(long_cells), len(fails)))
    return fails


# ---------------------------------------------------------------------------
# Density: the table's rows and the kit corollaries skip nothing and lose no
# second hit; one note more than the table skips, the last track first

def _run(rule, tracks, bpm, steps):
    s = Seq(rule, tracks, bpm)
    ov = run(s, steps)
    return s, ov


def _bad(s, ov):
    return ov or s.skips or s.flam_voice_drops or s.flam_budget_drops or s.port.peak_Q > QMAX


def density(rule, steps, bpms=TABLE_BPMS, verbose=False):
    fails = []
    cells = 0
    tight = None
    for bpm in bpms:
        n = table_n(bpm)
        loads = [('n plain g%d' % g, lambda g=g: synth_tracks(n, g, first_index=0)) for g in (10, 50, 100)]
        loads += [('n tied', lambda: synth_tracks(n, 50, 'tied', first_index=0)),
                  ('n tie_end', lambda: synth_tracks(n, 50, 'tie_end', first_index=0)),
                  ('n slide', lambda: synth_tracks(n, 50, 'slide', first_index=0)),
                  ('n one channel', lambda: synth_tracks(n, 50, first_index=0, one_channel=True))]
        for label, mk in loads:
            s, ov = _run(rule, mk(), bpm, steps)
            cells += 1
            worst = max(f0 + S - G for f0, S, G in s.step_log)
            tight = worst if tight is None else max(tight, worst)
            if _bad(s, ov):
                fails.append('density %s BPM %s: skips %d%s' % (bpm, label, s.skips, ', OVERFLOW' if ov else ''))
            if verbose:
                say('  %6s BPM %-14s skips %d  max F0+S-G %d' % (bpm, label, s.skips, worst))
        # continuations on one pitch: after the first step nothing is sent or charged
        for mode in ('tie_same', 'slide_same'):
            s, ov = _run(rule, synth_tracks(n, 50, mode, first_index=0), bpm, 16)
            cells += 1
            if _bad(s, ov) or any(S for _, S, _ in s.step_log[1:]):
                fails.append('density %s BPM %s: a continuation was sent or skipped' % (bpm, mode))
        # one note more than the table: the budget skips it, and only it (Goal 4)
        if n < 64:
            for g in (50, 100):
                s, ov = _run(rule, synth_tracks(n + 1, g, first_index=0), bpm, 32)
                cells += 1
                last = s.tracks[-1]
                if ov or last.skips != 32 or any(t.skips for t in s.tracks[:-1]):
                    fails.append('density %s BPM n+1 gate %d: last track skipped %d of 32, others %d%s' % (
                        bpm, g, last.skips, sum(t.skips for t in s.tracks[:-1]), ', OVERFLOW' if ov else ''))
    # kit corollaries
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
        s, ov = _run(rule, mk(), bpm, steps)
        cells += 1
        if _bad(s, ov):
            fails.append('density %s BPM %s: skips %d, second hits lost %d, flams played plain %d%s' % (
                bpm, label, s.skips, s.flam_voice_drops, s.flam_budget_drops, ', OVERFLOW' if ov else ''))
        if verbose:
            say('  %6s BPM %-32s skips %d lost second hits %d' % (bpm, label, s.skips, s.flam_voice_drops))
    fails += skip_semantics(rule)
    say('density (%s, %d steps): %d loads, %d failing; tightest table step F0 + S - G = %d' % (
        rule, steps, cells, len(fails), tight))
    return fails


def skip_semantics(rule):
    """The spec's skipped Note On: a tie on the same pitch after a skip stays
    silent and charges 0; a kit short of budget skips its last voices in
    kit_voice_order."""
    fails = []
    bpm = 300
    n = table_n(bpm)
    tracks = synth_tracks(n, 100, first_index=0) + synth_tracks(1, 100, 'tie_same', first_index=n)
    s, ov = _run(rule, tracks, bpm, 32)
    tie = s.tracks[-1]
    if ov or tie.skips != 1 or tie.midi or s.skips != 1:
        fails.append('skip then tie: the tied track skipped %d times (want 1), sounds on MIDI %s' % (
            tie.skips, tie.midi))
    # a slide's Note Off counts against the reservation: with room for 9 slides
    # and 3 bytes, the 10th is refused
    tracks = _zd_build('slides', 50, False, 0, False)
    s = Seq(rule, tracks, 120)
    s.port.stall()
    G = min(D_for(s.P) + as_rule(rule).margin, CAP_SAFE)
    s.port.q.extend(([0x81, 61, 0] * 400)[:G - (6 * 10 - 3)])
    s.port.owed = 64
    try:
        s.run_pulse()
    except Overflow:
        pass
    if s.skips != 64 - 9:
        fails.append('slide reservation: %d slides admitted with room for 9 (+3 bytes)' % (64 - s.skips))
    # a kit the budget cuts: at 300 BPM (budget 150) six SYNTH tracks on T1-T6 (36),
    # a flammed kit on T7 (91) and a kit on T8 with 23 left: 5 of its 11 voices
    tracks = synth_tracks(6, 50, first_index=0) + [kit(9, 50, True), kit(10, 50)]
    s, ov = _run(rule, tracks, bpm, 4)
    k2 = s.tracks[7]
    want = KIT_ORDER[5:] * 4
    if ov or s.skips != len(want) or k2.skipped != want:
        fails.append('kit skip order: skipped %s, want the tail of kit_voice_order %s each step' % (
            k2.skipped[:6], KIT_ORDER[5:]))
    return fails


def _edge_pulses(bpm_lo, bpm_hi, every=1):
    """The smallest pulse interval for each D between the tempos: the least
    time for the bytes."""
    seen = {}
    for p in range(pulse_us_for(bpm_hi), pulse_us_for(bpm_lo) + 1):
        seen.setdefault(D_for(p), p)
    return sorted(seen.items())[::every]


def kit_edge(rule, steps, every=1, voices=(1, 6, 11), fps=(1, 12), verbose=False):
    """1-2 kits of `voices` voices, flammed or not, plus SYNTH tracks filling
    the budget (or the free tracks), at every D: no skip, no lost second hit,
    no flam played plain."""
    fails = []
    cells = 0
    rule = as_rule(rule)
    Ds = _edge_pulses(40, 300, every)
    for D, p in Ds:
        bpm = 60000000.0 / (p * 96)
        for nk in (1, 2):
            for v in voices:
                for flam, fp in [(False, 3)] + [(True, f) for f in fps]:
                    for gate in (10, 100):
                        probe = Seq(rule, [kit(9 + i, gate, flam, fp, v) for i in range(nk)], bpm)
                        probe.P = p
                        run(probe, 1)
                        if probe.skips or probe.flam_budget_drops:
                            continue                      # the kits alone exceed the budget
                        kc = D - 6 - probe.budget          # what admit() charged the kits
                        K = min((D - 6 - kc) // 6, 64 - nk)
                        if K < 0:
                            continue
                        tracks = [kit(9 + i, gate, flam, fp, v) for i in range(nk)] + synth_tracks(K, gate)
                        s = Seq(rule, tracks, bpm)
                        s.P = p
                        ov = run(s, steps)
                        cells += 1
                        if _bad(s, ov):
                            fails.append('kit edge D %d (%.1f BPM): %d kits x %d voices flam %s fp %d gate %d + %d SYNTH: '
                                         'skips %d, second hits lost %d, flams plain %d%s' % (
                                             D, bpm, nk, v, flam, fp, gate, K, s.skips, s.flam_voice_drops,
                                             s.flam_budget_drops, ', OVERFLOW' if ov else ''))
    say('kit edge (%s, %d steps): %d cells over %d values of D, %d failing' % (
        rule, steps, cells, len(Ds), len(fails)))
    if verbose:
        for f in fails[:10]:
            say('  ' + f)
    return fails


# ---------------------------------------------------------------------------
# Safety in time: adversarial families with the real drain

FAMILIES = ('prefill_G-258', 'prefill_511', 'prefill_765', 'hammer', 'slide_hammer',
            'catchup', 'jumps', 'random')


def family_run(rule, bpm, family, gate, seed=0, flam=False, steps=48):
    if family not in FAMILIES:
        raise ValueError(family)
    rng = random.Random(seed)
    style = {'slide_hammer': 'slide', 'jumps': 'slide', 'random': 'random'}.get(family, 'tie_end')
    tracks = all_tracks(gate, style, flam=flam, rng=rng if family == 'random' else None)
    thru = spacing = step_P = None
    G = min(D_for(pulse_us_for(bpm)) + as_rule(rule).margin, CAP_SAFE)
    if family.startswith('prefill'):
        # just before a step whose ties end; G - 258 is empty above about 250 BPM
        target = {'prefill_G-258': max(0, G - 258), 'prefill_511': 511, 'prefill_765': 765}[family]
        thru = Thru('prefill', at=7, target=target)
    elif family in ('hammer', 'slide_hammer', 'jumps'):
        thru = Thru('hammer')
    elif family == 'random':
        thru = Thru(rng.choice(['hammer', 'window', 'prefill']), at=rng.randint(1, 20),
                    offs=rng.random() < 0.5)
    if family in ('catchup', 'random'):
        burst_at = rng.randint(2, 30) * PPS + rng.randint(0, 23)
        P0 = pulse_us_for(bpm)

        def spacing(seq, p):
            # a burst: 48 pulses four times as fast, at least 250 us apart; the
            # Clock estimate falls with it; elsewhere a Clock's owed pulses may
            # run back to back
            if burst_at <= p < burst_at + 48:
                seq.P = max(P0 // 4, 250)
                return seq.P
            seq.P = P0
            if p % 4 == 0 and rng.random() < 0.1:
                return 0
            return P0
    if family in ('jumps', 'random'):
        def step_P(seq, s):
            return pulse_us_for(rng.uniform(40, 300))
    s = Seq(rule, tracks, bpm, thru=thru, seed=seed)
    ov = run(s, steps, spacing=spacing, step_P=step_P, between=thru.between if thru else None)
    return s, ov


def safety(rule, bpm_step, seeds, steps=48, flam_both=True, families=FAMILIES, bpm_lo=40,
           expect_safe=True):
    fails = []
    peak_fill = peak_Q = runs = overflows = 0
    for fam in families:
        fam_over = 0
        for flam in ((False, True) if flam_both else (False,)):
            for bpm in range(bpm_lo, 301, bpm_step):
                for gate in ((10, 50, 100) if fam != 'random' else (0,)):
                    for sd in range(seeds):
                        s, ov = family_run(rule, bpm, fam, gate, seed=sd * 1000 + bpm, flam=flam,
                                           steps=steps)
                        runs += 1
                        peak_fill = max(peak_fill, s.port.peak_fill)
                        peak_Q = max(peak_Q, s.port.peak_Q)
                        if ov or s.port.peak_Q > QMAX:
                            overflows += 1
                            fam_over += 1
                            if expect_safe and len(fails) < 20:
                                fails.append('safety %s %s BPM gate %d seed %d flam %s: %s, fill+3owed %d' % (
                                    fam, bpm, gate, sd, flam, ov or 'no overflow', s.port.peak_Q))
        if not expect_safe and fam_over == 0:
            fails.append('safety (%s) %s: expected an overflow, none found' % (rule, fam))
    say('safety (%s, %d steps): %d runs, peak fill %d, peak fill + 3 x owed %d, overflows %d' % (
        rule, steps, runs, peak_fill, peak_Q, overflows))
    return fails


def first_step(rule, steps=32):
    """The first step followed with no Clock estimate. Pulse 0 is on the first
    Clock; pulses 1-3 follow at the internal period, or run at once on the
    second Clock when that comes first; then the master's period. Nothing within
    the table skips and no second hit armed on the unbudgeted step is lost on an
    empty queue; nothing overflows, also with a queue that Start left holding
    releases and keys (then the cap of 765 may refuse a second hit)."""
    fails = []
    cases = 0
    for internal, master in ((300, 120), (120, 300), (40, 300), (300, 40), (120, 120)):
        Pi, Pm = pulse_us_for(internal), pulse_us_for(master)

        def at(p):
            if p >= 4:
                return 4 * Pm + (p - 4) * Pm
            return min(p * Pi, 4 * Pm)

        def spacing(seq, p):
            return at(p) - at(p - 1)
        n = table_n(master)
        for gate in (10, 100):
            for prefill in (0, 300):
                for label, mk in (('table n', lambda: synth_tracks(n, gate, first_index=0)),
                                  ('flammed kits + SYNTH in budget',
                                   lambda: [kit(9, gate, True), kit(10, gate, True)]
                                   + synth_tracks(min(62, (D_for(Pm) - 188) // 6), gate)),
                                  ('64 tracks + flammed kits', lambda: [kit(9, gate, True), kit(10, gate, True)]
                                   + synth_tracks(62, gate, 'slide'))):
                    if label.startswith('flammed') and D_for(Pm) - 6 < 182:
                        continue                          # two flammed kits exceed the budget
                    s = Seq(rule, mk(), master, following_unknown=True, D_first=D_for(Pi))
                    s.port.q.extend(([0x81, 61, 0] * 200)[:prefill])
                    ov = run(s, steps, spacing=spacing)
                    cases += 1
                    s1 = Seq(rule, mk(), master, following_unknown=True, D_first=D_for(Pi))
                    s1.port.q.extend(([0x81, 61, 0] * 200)[:prefill])
                    run(s1, 1, spacing=spacing)
                    if prefill == 0 and s1.flam_voice_drops:
                        fails.append('first step %d/%d %s gate %d prefill %d: %d second hits lost '
                                     'on the unbudgeted step' % (internal, master, label, gate, prefill,
                                                                 s1.flam_voice_drops))
                    if ov or s.port.peak_Q > QMAX:
                        fails.append('first step %d/%d %s gate %d prefill %d: overflow' % (
                            internal, master, label, gate, prefill))
                    if label != '64 tracks + flammed kits' and prefill == 0 and (s.skips or s.flam_voice_drops):
                        fails.append('first step internal %d master %d %s gate %d: skips %d, second hits lost %d' % (
                            internal, master, label, gate, s.skips, s.flam_voice_drops))
    say('first followed step (%s): %d cases, %d failing' % (rule, cases, len(fails)))
    return fails


class _OneKey:
    """One pass-through retrigger at the start of phase 1, recorded."""

    def __init__(self):
        self.done = False
        self.ok = None

    def __call__(self, seq, where):
        if where == 'start' and not self.done:
            self.done = True
            self.ok = seq.port.capped([0x80, 60, 0, 0x90, 60, 100], 511)


def thru_window(rule, step=0.25):
    """A key arriving in phase 1 of a real step on an empty queue is accepted
    at every tempo (40-300 BPM), with the table's load on the port."""
    rule = as_rule(rule)
    refused = []
    b = 40.0
    while b <= 300:
        key = _OneKey()
        s = Seq(rule, synth_tracks(table_n(b), 50, first_index=0), b, thru=key)
        ov = run(s, 1)
        if ov or not key.ok or s.port.peak_Q > QMAX:
            refused.append(b)
        b += step
    say('pass-through in phase 1 on an empty queue (%s): %s' % (
        rule.name, 'refused at %d tempos, up to %.2f BPM' % (len(refused), max(refused)) if refused
        else 'never refused'))
    return ['thru window (%s): refused up to %.2f BPM' % (rule.name, max(refused))] \
        if refused and rule.name == 'R1' else []


# ---------------------------------------------------------------------------
# Zero drain: one step from any state with fill + 3 x owed <= 1,023, the wire
# stalled, then Stop. The queued bytes before the step stand for keys or any
# earlier sends; the sounding notes are the owed ones. Resumable: one log line
# per finished tempo.

ZD_BPMS = [20, 40, 50, 60, 63.9, 64.1, 67.6, 67.7, 70, 78, 90, 98, 100, 105.4, 105.6, 120,
           160, 187.5, 240, 300]
ZD_STYLES = ('ties_end', 'slides', 'retrig', 'inactive', 'silent', 'mixed')


def _zd_build(style, gate, flam, nkits, leftovers):
    tracks = []
    for t in range(64):
        if t < nkits:
            k = kit(9 + t, gate, flam)
            k.sounding = (1 << 11) - 1 if leftovers else 0
            tracks.append(k)
            continue
        a = 52 + t % 40
        sounding = not (style == 'silent' or (style == 'mixed' and t < 33))
        base = 'retrig' if style in ('silent', 'mixed') else style
        pre = {'ties_end': (True, False), 'slides': (False, True), 'retrig': (False, False),
               'inactive': (False, False)}[base]
        ev = (False, a, False, False) if base == 'inactive' else (True, a + 1, False, base == 'slides')
        s = Synth(t % 16, [ev] * 16, gate)
        s.on = s.midi = sounding
        s.note, s.flags_tie_slide, s.countdown = a, pre, 0
        tracks.append(s)
    return tracks


def _zd_one(rule, bpm, style, gate, flam, F0, thru_mode, nkits, leftovers):
    tracks = _zd_build(style, gate, flam, nkits, leftovers)
    owed0 = sum(bin(t.sounding).count('1') if t.kit else int(t.midi) for t in tracks)
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
            s.t += s.P
            if thru:
                thru.between(s)
        msg, n = [], 0                 # Stop: every release, no drain
        for tr in tracks:
            if tr.kit:
                m, c = s.kit_release_msg(tr)
                msg += m
                n += c
            elif tr.on and tr.midi:
                msg += s.synth_off_msg(tr)
                n += 1
                tr.on = tr.midi = False
        p.mandatory(msg, -n, 'stop')
    except Overflow as e:
        ov = e.args[0]
    return p.peak_Q, ov, F0 + 3 * owed0


def zero_drain(rule, log_path=None, f0_step=3, bpms=ZD_BPMS, expect_safe=True):
    rule = as_rule(rule)
    done = {}
    if log_path and os.path.exists(log_path):
        with open(log_path) as f:
            for line in f:
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue                  # a line cut short by a stop
                if rec.get('rule') == repr(rule) and rec.get('f0_step') == f0_step:
                    done[rec['bpm']] = rec
    log = open(log_path, 'a') if log_path else None
    try:
        for bpm in bpms:
            if bpm in done:
                continue
            runs = overflows = 0
            peak = 0
            raised = 0      # the highest fill + 3 x owed among runs where the step raised it
            first = None
            for style in ZD_STYLES:
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
            if log:
                log.write(json.dumps(rec) + '\n')
                log.flush()
            done[bpm] = rec
            if log:
                say('  zero drain %s BPM: %d runs, peak fill + 3 x owed %d (%d where the step raised it), '
                    'overflows %d' % (bpm, runs, peak, raised, overflows))
    finally:
        if log:
            log.close()
    recs = [done[b] for b in bpms]
    runs = sum(r['runs'] for r in recs)
    over = sum(r['overflows'] for r in recs)
    peak = max(r['peak'] for r in recs)
    raised = max(r.get('raised', 0) for r in recs)
    say('zero drain (%s, %d tempos, F0 step %d): %d runs, peak fill + 3 x owed %d '
        '(%d where the step raised it), overflows %d' % (rule, len(bpms), f0_step, runs, peak, raised, over))
    fails = []
    if expect_safe and over:
        fails.append('zero drain: %d overflows, e.g. %s' % (
            over, [(r['bpm'], r['first']) for r in recs if r['first']][:3]))
    if not expect_safe and not over:
        fails.append('zero drain (%s): expected overflows, none found' % rule)
    return fails
