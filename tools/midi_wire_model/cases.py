"""Track sets for the model's checks.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

from sim import Synth, Kit

KIT_NOTES = [36, 38, 39, 42, 46, 37, 41, 45, 48, 49, 51]


def synth_tracks(n, gate, mode='plain', first_index=2):
    """n SYNTH tracks from track `first_index`, channel = track % 16.
    mode: 'plain' the same note every step (as the host probe); 'tied' tie
    flag and a new pitch every step (the tie ends every step); 'slide' slide
    flag and a new pitch every step; 'tie_end' tied on even steps, a new pitch
    on odd ones."""
    out = []
    for k in range(n):
        t = first_index + k
        base = (52 + t) % 100
        steps = []
        for s in range(16):
            if mode == 'plain':
                steps.append((True, base, False, False))
            elif mode == 'tied':
                steps.append((True, base + (s % 2) * 20, True, False))
            elif mode == 'slide':
                steps.append((True, base + (s % 2) * 20, False, True))
            elif mode == 'tie_end':
                steps.append((True, base + ((s + 1) // 2 % 2) * 20, s % 2 == 0, False))
            else:
                raise ValueError(mode)
        out.append(Synth(t % 16, steps, gate))
    return out


def kit(ch, gate, flam=False, flam_pulses=3, voices=11):
    """A kit hitting its first `voices` voices on every step, flammed or not."""
    val = 3 if flam else 1
    lanes = [[val if v < voices else 0 for v in range(11)] for _ in range(16)]
    return Kit(ch, KIT_NOTES, lanes, gate, flam_pulses)


def all_tracks(gate, style, flam=False, rng=None):
    """Every track on one port: two kits (T1, T2) and 62 SYNTH tracks.
    style: 'tie_end', 'slide' or 'random' (with rng: random lanes, gates,
    flags and flam pulses)."""
    tracks = []
    for t in range(64):
        if t < 2:
            if rng:
                lanes = [[rng.choice([0, 1, 1, 3]) for _ in range(11)] for _ in range(16)]
                tracks.append(Kit(9 + t, KIT_NOTES, lanes, rng.choice([10, 50, 100]),
                                  rng.randint(1, 12)))
            else:
                tracks.append(kit(9 + t, gate, flam))
            continue
        a = 52 + t % 40
        if style == 'tie_end':
            steps = [(True, a + ((s + 1) // 2 % 2) * 20, s % 2 == 0, False) for s in range(16)]
        elif style == 'slide':
            steps = [(True, a + (s % 2) * 20, False, True) for s in range(16)]
        elif style == 'random':
            steps = [(rng.random() < 0.85, a + rng.choice([0, 0, 20]), rng.random() < 0.4,
                      rng.random() < 0.4) for _ in range(16)]
        else:
            raise ValueError(style)
        tracks.append(Synth(t % 16, steps, rng.choice([10, 50, 100]) if rng else gate))
    return tracks
