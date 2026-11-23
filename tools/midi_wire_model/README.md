# MIDI wire model

A byte-level, time-domain model of one MIDI output port, written to check the
admission rule of the four-output design before it is built:
`docs/superpowers/specs/2026-11-20-four-midi-outputs-design.md`, sections
"Overload: admission per output", "The density promise" and "No overflow by
construction". The rule is a design. Today's firmware has no admission: a full
TX queue sets the overflow flag and the transport stops.

It checks the rule, not code in `src/`: it runs no line of C. An
implementation is checked by its host tests.

Python 3, standard library only.

## What it models

- **The queue** of `src/midi_out.c`: 1,024 bytes (1,023 usable), whole 3-byte
  messages with their status, running status applied in the drain (a status
  the drain skips takes no wire time), Clock as a flag taken at a message
  boundary, a UART with shifter and holding register, FIFO off, 320 us a byte.
- **The pulse** of `src/sequencer.c`, in its order: Clock every 4 pulses;
  note-off and kit countdowns, sent directly (at gate 100 % they land at the
  step boundary, before the fill is read); flam second hits
  (`kit_play_flam()`, one capped send per kit); then phase 1 in track order:
  kit leftovers sent directly (`play_kit_step()`), kit Note Ons in
  `kit_voice_order` with the notes of `default_notes` (both checked against
  `src/kit.c`), SYNTH steps as `prepare_trigger()` (Note Off before Note On, a
  slide's Note On before its Note Off, tie and slide continuations), all into
  one batch flushed in one send.
- **A skipped Note On** as the spec has it: the track's state (note, flags,
  countdown) advances as if admitted, a separate "MIDI sounding" bit stays
  clear, and no Note Off is ever sent for it; a tie on the same pitch after it
  stays silent and charges 0.
- **Pass-through** from core 0 (`thru_note_on()`, `thru_note_off()` in
  `src/ui.c`): retriggers (6 bytes) and Note Offs (3) on rotating channels,
  pushed until refused, just before each pulse and at three points inside
  phase 1.
- **Rules** (`sim.Rule`): `R1` is the design's (margin, flam cap and first-step
  handling are its defaults, read from the spec; `--margin` and `--flam-cap`
  try variants); `R0` is the first version, kept to show the overflow that
  replaced it; `today` is today's firmware, used for calibration.
- **Owed**: the playback notes queued whose release is not; every check tracks
  fill + 3 x owed, the invariant of the spec's proof.

## Calibration

Rule `today` against the host probe of today's firmware
(`tools/midi_density_probe`, the fake UART in wire-time mode): 24 cells of 8
bars (peak queue fill, wire bytes per step, the step where the transport
stopped, flam drops, one cell on one channel) and 9 cells of 64 bars (the
stopping step at bar 11, 22 and 53, flam drops of the flammed loads). All match
exactly. Peaks of runs that stop are not compared (the probe reports the peak
up to the last pulse that survived), and the flammed 14-SYNTH load at 240 BPM
peaks at 331 in the model against 333 in the probe; that peak is not compared,
its flam drops are. Calibration exercises the queue, the drain and the event
order, not the admission rule, which today's firmware does not have.

## Limits

- One port. That a release counts on its own port is argued in the spec, not
  modelled.
- No interrupt latency, no flash stalls, and phase 1 takes no time.
- Pass-through is not limited to the MIDI IN rate: a stronger adversary.
- External clock: only the first step without an estimate, catch-up bursts
  (pulses four times as fast, the estimate falling with them) and owed pulses
  run back to back; not the estimator of `src/clock_in.c` itself.
- Not modelled: mutes, kind changes, Song advance, MIDI+CV tracks, per-step
  channel changes, tracks shorter than 16 steps, the 450-byte step buffer and
  the 4-entry real-time queue. The spec's proof covers the first four as
  releases.
- The model is not the firmware. A green run says the rule as modelled holds,
  not that an implementation does.

## Running

From the repository root:

```
python3 tools/midi_wire_model/run_checks.py               # quick, under a minute
python3 tools/midi_wire_model/run_checks.py --full        # every sweep, about 25 min
python3 tools/midi_wire_model/run_checks.py --full --log /tmp/zd.jsonl   # resumable
python3 tools/midi_wire_model/run_checks.py --margin 32 --flam-cap 319   # a variant
```

It prints the rule it ran, one line per check, and exits 1 on a failed
assertion. The checks (`checks.py`; quick mode runs each on a smaller grid):

| Check | Asserts |
|---|---|
| calibrate | rule `today` matches the probe cells; the kit tables match `src/kit.c` |
| density | every row of the table (n notes at gate 10/50/100 %, tied, ties ending, sliding, on one channel) and the kit corollaries: no skip, no lost second hit, no flam played plain; continuations on one pitch send and charge nothing; n + 1 notes skip the last track and only it |
| skip semantics | a tie after a skipped note stays silent; a slide's Note Off counts against the reservation; a kit short of budget skips the tail of `kit_voice_order` |
| kit edge | 1-2 kits of 1/6/11 voices, flammed or not, plus SYNTH tracks filling the budget, at every D (the shortest step for each): no skip, no lost second hit, no flam played plain |
| safety | prefill before a step whose ties end, hammered keys, slides, catch-up bursts, tempo jumps, random patterns, kits flammed and not: no overflow, fill + 3 x owed <= 1,023 (full: 20-300 BPM, 128 steps) |
| first followed step | pulses 1-3 at the internal period or bunched on the second Clock: no skip within the table, no second hit lost on an empty queue, no overflow with a queue left full |
| pass-through window | a key in phase 1 of a real step on an empty queue is accepted at every tempo |
| R0 | the old rule still overflows in the prefill family |
| zero drain | one step from any state with fill + 3 x owed <= 1,023 (sounding, silent and mixed tracks), the wire stalled, then Stop: no overflow; quick mode samples it, full mode runs it whole and resumes from its log |

Run the quick mode before and after any change to the rule, `--full` before
the spec's numbers change.
