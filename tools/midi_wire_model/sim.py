"""Byte-level, time-domain model of one MIDI output port under Pattern playback.

Not firmware code. It mirrors the event order of src/sequencer.c (run_pulse,
prepare_trigger, prepare_note_off, play_kit_step, kit_play_flam) and the queue
of src/midi_out.c (whole 3-byte messages in the queue, running status applied
at the drain, Clock as a flag taken at message boundaries, a UART with shifter
and holding register, FIFO off, 320 us a byte), plus pass-through from core 0
(src/ui.c thru_note_on / thru_note_off).

Rules (Rule.name):
  today  today's firmware: no admission, flam cap 319, pass-through caps 511/767;
         a full queue is an overflow (the transport stops).
  R0     the first version of the design's rule: guard F0 + admitted Note On
         bytes <= G, reservation G - fill, capped sends fill + reserved + length
         <= own cap; G = min(D + 32, 765). Kept to show why it was replaced.
  R1     the design's rule (docs/superpowers/specs/2026-11-20-four-midi-outputs-
         design.md, "Overload: admission per output"): every byte the step puts
         on the port since F0 counts, reservation min(G - F0, 516), capped sends
         check their own cap and fill + reserved + length <= 765, G = min(D + 72,
         765), flam cap G, no budget on the first step followed without a Clock
         estimate.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

from collections import deque
import random

BYTE_US = 320
QMAX = 1023            # usable TX queue bytes
CAP_SAFE = 765         # 1,023 - 258 (MIDI_MANDATORY_MAX)
STEP_MAX = 516         # the most one step can put on one port
THRU_ON_CAP = 511
PPS = 24               # pulses per step
KIT_ORDER = [0, 1, 6, 7, 8, 5, 2, 3, 4, 9, 10]   # src/kit.c kit_voice_order (voice indices)


class Rule:
    def __init__(self, name, margin=None, flam_cap=None, first_free=None):
        if name not in ('today', 'R0', 'R1'):
            raise ValueError(name)
        self.name = name
        self.margin = margin if margin is not None else {'today': 32, 'R0': 32, 'R1': 72}[name]
        # int, or 'G' = min(D + margin, 765) at the flam's time
        self.flam_cap = flam_cap if flam_cap is not None else ('G' if name == 'R1' else 319)
        self.first_free = first_free if first_free is not None else (name == 'R1')
        self.thru_off_cap = 767 if name == 'today' else 765

    def __repr__(self):
        return 'Rule(%s, margin %d, flam cap %s, first followed step %s)' % (
            self.name, self.margin, self.flam_cap,
            'unbudgeted' if self.first_free else 'from the internal period')


def as_rule(rule):
    return rule if isinstance(rule, Rule) else Rule(rule)


def pulse_us_for(bpm):
    return int(60000000.0 / (bpm * 96.0))


def D_for(pulse_us):
    return (pulse_us * PPS) // BYTE_US


class Overflow(Exception):
    pass


class Port:
    def __init__(self, rule):
        self.rule = rule
        self.q = deque()          # queued bytes
        self.msg_left = 0
        self.last_status = 0
        self.ran_empty = True
        self.clock_pending = False
        self.hold = False         # UART holding register occupied
        self.hold_byte = None
        self.shift_end = 0        # when the shifter frees
        self.now = 0
        self.reserved = 0
        self.wire = 0
        self.wire_clock = 0
        self.overflow = None
        self.peak_fill = 0
        self.peak_Q = 0           # fill + 3 x owed
        self.owed = 0             # playback notes queued whose release is not queued

    @property
    def fill(self):
        return len(self.q)

    def _take_byte(self):
        """midi_tx_drain_locked: one byte into the holding register, or None."""
        if self.msg_left == 0:
            if self.clock_pending:
                self.clock_pending = False
                return 'clk'
            if not self.q:
                return None
            status = self.q[0]
            self.msg_left = 3
            if status == self.last_status and not self.ran_empty:
                self.q.popleft()          # running status: no wire time
                self.msg_left -= 1
            else:
                self.last_status = status
                self.ran_empty = False
        b = self.q.popleft()
        self.msg_left -= 1
        if not self.q:
            self.ran_empty = True
        return b

    def advance(self, t):
        """Run the UART and its TX interrupt up to time t."""
        while True:
            if not self.hold:
                b = self._take_byte()
                if b is None:
                    break
                self.hold = True
                self.hold_byte = b
            start = max(self.shift_end, self.now)
            if start > t:
                break
            self.now = start
            self.shift_end = start + BYTE_US
            self.wire += 1
            if self.hold_byte == 'clk':
                self.wire_clock += 1
            self.hold = False
        self.now = max(self.now, t)

    def stall(self):
        """Stop the wire for good (zero-drain checks)."""
        self.hold, self.hold_byte, self.shift_end = True, 0, 10 ** 15

    def _note(self):
        f = self.fill
        if f > self.peak_fill:
            self.peak_fill = f
        Q = f + 3 * self.owed
        if Q > self.peak_Q:
            self.peak_Q = Q

    def mandatory(self, data, d_owed, what=''):
        """midi_send_bytes: a release or the step batch; a full queue overflows."""
        self.advance(self.now)
        if self.fill + len(data) > QMAX:
            if self.overflow is None:
                self.overflow = (what, self.now, self.fill, len(data))
            raise Overflow(self.overflow)
        self.q.extend(data)
        self.owed += d_owed
        self._note()
        self.advance(self.now)

    def capped(self, data, own_cap, d_owed=0):
        """midi_try_send_bytes_capped with the rule's reservation."""
        self.advance(self.now)
        n = len(data)
        f = self.fill
        name = self.rule.name
        if name == 'today':
            ok = f + n <= own_cap
        elif name == 'R0':
            ok = f + self.reserved + n <= own_cap
        else:
            ok = f + n <= own_cap and f + self.reserved + n <= CAP_SAFE
        if not ok:
            return False
        self.q.extend(data)
        self.owed += d_owed
        self._note()
        self.advance(self.now)
        return True

    def clock(self):
        self.advance(self.now)
        self.clock_pending = True
        self.advance(self.now)


# ---------------------------------------------------------------------------
# Tracks

class Synth:
    def __init__(self, ch, steps, gate_pct):
        # steps: 16 x (active, note, tie, slide)
        self.ch = ch
        self.steps = steps
        self.gate = max(1, gate_pct * PPS // 100)
        self.on = False           # note_is_on: advances on a skipped Note On too
        self.midi = False         # the spec's "MIDI sounding" bit: a Note Off is owed
        self.note = 0
        self.flags_tie_slide = (False, False)
        self.countdown = 0
        self.skips = 0
        self.kit = False


class Kit:
    def __init__(self, ch, notes, lanes, gate_pct, flam_pulses=3):
        # lanes: 16 x 11 values: 0 off, 1 note, 3 flam
        self.ch = ch
        self.notes = notes
        self.lanes = lanes
        self.gate = max(1, gate_pct * PPS // 100)
        self.flam_pulses = flam_pulses
        self.sounding = 0
        self.flam = 0
        self.countdown = 0
        self.flam_countdown = 0
        self.flam_G = None        # G of the step that armed the flam
        self.skipped = []         # voices skipped, in the order they were skipped
        self.skips = 0
        self.kit = True


class Seq:
    """One port, its tracks and the pulse loop. `following_unknown`: the first
    step is played following a Clock with no estimate yet; `D_first` is then the
    D the internal period gives (used unless the rule leaves that step
    unbudgeted)."""

    def __init__(self, rule, tracks, bpm, thru=None, seed=0, following_unknown=False,
                 D_first=None):
        self.rule = as_rule(rule)
        self.port = Port(self.rule)
        self.tracks = tracks
        self.P = pulse_us_for(bpm)
        self.thru = thru
        self.following_unknown = following_unknown
        self.D_first = D_first
        self.skips = 0
        self.budget_skips = 0
        self.guard_skips = 0
        self.flam_drops = 0            # flam sends refused at the flam cap
        self.flam_voice_drops = 0      # second hits lost at the flam cap
        self.flam_budget_drops = 0     # flams played plain for want of budget
        self.flam_ahead_max = 0        # queued bytes ahead of an accepted second hit
        self.pulse = 0
        self.t = 0
        self.rng = random.Random(seed)
        self.in_window = False
        self.step_log = []             # (F0, S, G) per step

    def synth_off_msg(self, tr):
        return [0x80 | tr.ch, tr.note & 0x7F, 0]

    def synth_release_batch(self, tr, batch):
        """prepare_note_off(): the Note Off only when the MIDI bit is set; the
        state ends either way. Returns the change in owed."""
        d = 0
        if tr.on and tr.midi:
            batch += self.synth_off_msg(tr)
            self.S += 3
            d = -1
        tr.on = tr.midi = False
        tr.countdown = 0
        return d

    def kit_release_msg(self, k):
        out = []
        n = 0
        for v in KIT_ORDER:
            if k.sounding & (1 << v):
                out += [0x90 | k.ch, k.notes[v], 0]
                n += 1
        k.sounding = 0
        k.countdown = 0
        k.flam = 0
        k.flam_countdown = 0
        return out, n

    def direct(self, data, d_owed, what):
        self.port.mandatory(data, d_owed, what)
        if self.in_window:
            self.S += len(data)

    def admit(self, charge, on_bytes, add_bytes):
        """on_bytes: the Note On (3); add_bytes: what it adds with a Note Off sent
        with it (3, or 6 for a slide)."""
        name = self.rule.name
        if name == 'today':
            return True
        if charge > self.budget:
            self.budget_skips += 1
            return False
        if name == 'R0':
            ok = self.F0 + self.adm_on + on_bytes <= self.G
        else:
            ok = self.S + add_bytes <= self.port.reserved
        if not ok:
            self.guard_skips += 1
            return False
        self.budget -= charge
        self.adm_on += on_bytes
        return True

    def phase1(self, step):
        port = self.port
        port.advance(self.t)
        self.D = D_for(self.P)
        self.G = min(self.D + self.rule.margin, CAP_SAFE)
        if self.following_unknown and self.pulse == 0:
            if self.rule.first_free:
                self.D, self.G = 10 ** 6, CAP_SAFE
            elif self.D_first is not None:
                self.D = self.D_first
                self.G = min(self.D + self.rule.margin, CAP_SAFE)
        self.F0 = port.fill
        if self.rule.name == 'R0':
            port.reserved = max(0, self.G - self.F0)
        elif self.rule.name == 'R1':
            port.reserved = min(max(0, self.G - self.F0), STEP_MAX)
        else:
            port.reserved = 0
        self.S = 0
        self.adm_on = 0
        self.budget = self.D - 6
        self.in_window = True
        batch = []
        bat_owed = 0
        if self.thru:
            self.thru(self, 'start')
        cut = self.rng.randrange(len(self.tracks) + 1)
        for i, tr in enumerate(self.tracks):
            if self.thru and i == cut:
                self.thru(self, 'mid')
            if tr.kit:
                if tr.sounding:
                    msg, n = self.kit_release_msg(tr)       # leftovers, sent directly
                    self.direct(msg, -n, 'kit leftovers')
                else:
                    tr.countdown = 0
                    tr.flam = 0
                    tr.flam_countdown = 0
                word = tr.lanes[step % 16]
                if not any(word):
                    continue
                first = True
                first_flam = True
                flam = 0
                for v in KIT_ORDER:
                    val = word[v]
                    if val == 0:
                        continue
                    c = 4 + (2 if first else 0)
                    want_flam = val == 3
                    cf = c + ((4 + (1 if first_flam else 0)) if want_flam else 0)
                    plain_for_budget = False
                    if want_flam and (self.rule.name == 'today' or cf <= self.budget):
                        charge = cf
                    else:
                        plain_for_budget = want_flam
                        want_flam = False
                        charge = c
                    if not self.admit(charge, 3, 3):
                        self.skips += 1
                        tr.skips += 1
                        tr.skipped.append(v)
                        continue
                    if plain_for_budget:
                        self.flam_budget_drops += 1
                    first = False
                    if want_flam:
                        first_flam = False
                        flam |= 1 << v
                    batch += [0x90 | tr.ch, tr.notes[v], 100]
                    bat_owed += 1
                    self.S += 3
                    tr.sounding |= 1 << v
                tr.countdown = tr.gate
                if flam:
                    off = tr.gate + tr.flam_pulses
                    tr.countdown = off if off < PPS else PPS
                    tr.flam = flam
                    tr.flam_countdown = tr.flam_pulses
                    tr.flam_G = self.G
            else:
                active, note, tie, slide = tr.steps[step % 16]
                if not active:
                    bat_owed += self.synth_release_batch(tr, batch)
                    continue
                ptie, pslide = tr.flags_tie_slide
                if tr.on and (ptie or pslide) and tr.note == note:
                    # a tie or slide continuation: silent, charge 0, also after a skip
                    tr.flags_tie_slide = (tie, slide)
                    tr.countdown = 0 if (tie or slide) else tr.gate
                    continue
                if tr.on and pslide:
                    # legato: the new Note On first, then the old note's Note Off,
                    # sent only if the old note sounds on MIDI
                    old_midi = tr.midi
                    if self.admit(6, 3, 6 if old_midi else 3):
                        batch += [0x90 | tr.ch, note, 100]
                        self.S += 3
                        if old_midi:
                            batch += self.synth_off_msg(tr)
                            self.S += 3
                        else:
                            bat_owed += 1
                        tr.midi = True
                    else:
                        self.skips += 1
                        tr.skips += 1
                        if old_midi:
                            batch += self.synth_off_msg(tr)
                            self.S += 3
                            bat_owed -= 1
                        tr.midi = False
                else:
                    bat_owed += self.synth_release_batch(tr, batch)
                    if self.admit(6, 3, 3):
                        batch += [0x90 | tr.ch, note, 100]
                        bat_owed += 1
                        self.S += 3
                        tr.midi = True
                    else:
                        self.skips += 1
                        tr.skips += 1
                        tr.midi = False
                # a skipped Note On advances the state as an admitted one would
                tr.on = True
                tr.note = note
                tr.flags_tie_slide = (tie, slide)
                tr.countdown = 0 if (tie or slide) else tr.gate
        if self.thru and cut == len(self.tracks):
            self.thru(self, 'mid')
        if self.thru:
            self.thru(self, 'end')
        if batch:                     # the flush: one send, then no reservation
            port.mandatory(batch, bat_owed, 'flush')
        port.reserved = 0
        self.in_window = False
        self.step_log.append((self.F0, self.S, self.G))

    def run_pulse(self):
        port = self.port
        port.advance(self.t)
        p = self.pulse
        if p % 4 == 0:
            port.clock()
        for tr in self.tracks:
            if tr.kit:
                if tr.countdown > 0:
                    tr.countdown -= 1
                    if tr.countdown == 0:
                        msg, n = self.kit_release_msg(tr)
                        if msg:
                            self.direct(msg, -n, 'kit release')
            elif tr.countdown > 0:
                tr.countdown -= 1
                if tr.countdown == 0 and tr.on:
                    if tr.midi:
                        self.direct(self.synth_off_msg(tr), -1, 'note off')
                    tr.on = tr.midi = False
        for tr in self.tracks:
            if tr.kit and tr.flam_countdown > 0:
                tr.flam_countdown -= 1
                if tr.flam_countdown == 0:
                    msg = []
                    for v in KIT_ORDER:
                        if tr.flam & (1 << v):
                            msg += [0x90 | tr.ch, tr.notes[v], 0,
                                    0x90 | tr.ch, tr.notes[v], 110]
                    tr.flam = 0
                    cap = self.rule.flam_cap
                    if cap == 'G':
                        cap = tr.flam_G       # the G of the step that armed the flam
                    port.advance(port.now)
                    ahead = port.fill
                    if msg and not port.capped(msg, cap):
                        self.flam_drops += 1
                        self.flam_voice_drops += len(msg) // 6
                    elif msg:
                        self.flam_ahead_max = max(self.flam_ahead_max, ahead)
        if p % PPS == 0:
            self.phase1(p // PPS)
        self.pulse += 1


def run(seq, steps, spacing=None, step_P=None, between=None):
    """Plays `steps` steps. spacing(seq, pulse) -> us to the next pulse;
    step_P(seq, step) -> the nominal pulse interval from that step (tempo
    change); between(seq) runs core 0 between pulses. Returns None, or the
    overflow as (what, time, fill, length)."""
    try:
        for s in range(steps):
            if step_P:
                seq.P = step_P(seq, s)
            for _ in range(PPS):
                seq.run_pulse()
                seq.t += spacing(seq, seq.pulse) if spacing else seq.P
                if between:
                    # core 0 acts just before the next pulse (the worst moment)
                    seq.port.advance(seq.t)
                    between(seq)
        seq.port.advance(seq.t + 10_000_000)
        return None
    except Overflow as e:
        return e.args[0]


# ---------------------------------------------------------------------------
# Pass-through (core 0)

class Thru:
    """A greedy keyboard: retrigger Note Ons (Off + On, 6 bytes) and Note Offs
    (3) until refused; not limited to the MIDI IN rate. mode: 'hammer' (between
    pulses and at three points inside phase 1), 'window' (only inside phase 1),
    'prefill' (once, before step `at`, up to `target` bytes or to the caps)."""

    def __init__(self, mode='hammer', offs=True, at=None, ons=True, target=None):
        self.mode = mode
        self.target = target
        self.offs = offs
        self.ons = ons
        self.at = at
        self.ch = 0               # keys on rotating channels: no running status

    def _msgs(self):
        self.ch = (self.ch + 1) % 16
        c = self.ch
        return [0x80 | c, 60, 0, 0x90 | c, 60, 100], [0x80 | c, 61, 0]

    def push(self, seq, limit=None):
        port = seq.port
        off_cap = seq.rule.thru_off_cap
        accepted = 0
        while self.ons and (limit is None or port.fill + 6 <= limit) and \
                port.capped(self._msgs()[0], THRU_ON_CAP):
            accepted += 1
        while self.offs and (limit is None or port.fill + 3 <= limit) and \
                port.capped(self._msgs()[1], off_cap):
            accepted += 1
        return accepted

    def __call__(self, seq, where):
        if self.mode in ('hammer', 'window'):
            self.push(seq)

    def between(self, seq):
        if self.mode == 'hammer':
            self.push(seq)
        elif self.mode == 'prefill' and seq.pulse == self.at * PPS:
            self.push(seq, self.target)
