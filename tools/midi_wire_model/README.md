# MIDI wire model

A byte-level, time-domain model of one MIDI output port, written to check the
admission rule of the four-output design before it is built:
`docs/superpowers/specs/2026-11-20-four-midi-outputs-design.md`, sections
"Overload: admission per output", "The density promise" and "No overflow by
construction". The rule is a design. Today's firmware has no admission: a full
TX queue sets the overflow flag and the transport stops.

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
  `kit_voice_order`, SYNTH steps as `prepare_trigger()` (Note Off before Note
  On, a slide's Note On before its Note Off, tie and slide continuations), all
  into one batch flushed in one send.
- **Pass-through** from core 0 (`thru_note_on()`, `thru_note_off()` in
  `src/ui.c`): retriggers (6 bytes) and Note Offs (3) pushed until refused,
  between pulses and at three points inside phase 1.
- **Rules** (`sim.Rule`): `R1` is the design's (margin, flam cap and first-step
  handling are its defaults, read from the spec; `--margin` and `--flam-cap`
  try variants); `R0` is the first version, kept to show the overflow that
  replaced it; `today` is today's firmware, used for calibration.
- **Owed**: the playback notes queued whose release is not; every check tracks
  fill + 3 x owed, the invariant of the spec's proof.

## Calibration

Rule `today` is checked against the host measurements of today's firmware
(the fake UART in wire-time mode; `tools/midi_density_probe`): 23 cells of
peak queue fill, wire bytes per step, the step where the transport stopped and
flam drops. All match exactly except two peaks of a stopping or saturated run,
which the check does not compare (the probe reports the peak before the fatal
send; the model's flammed 14-SYNTH load at 240 BPM peaks at 331 against 333).
An earlier version charged the budget to flams under rule `today` and gave 63
flam drops against the probe's 86 at 300 BPM; that is fixed, and the cell is
now one of the checked ones.

## Limits

- One port. Ports share nothing in the rule except the TX lock; that a release
  counts on its own port is argued in the spec, not modelled.
- No interrupt latency, no flash stalls, and phase 1 takes no time.
- Pass-through is not limited to the MIDI IN rate: a stronger adversary.
- Mutes, kind changes, Song advance and MIDI+CV tracks are not modelled; the
  spec's proof covers them as releases. A skipped Note On is assumed never to
  get a Note Off later (the spec's "MIDI sounding" bit).
- The model is not the firmware. A green run says the rule as modelled holds,
  not that an implementation does: that needs the host tests of the spec's
  "Tests" section.

## Running

From the repository root:

```
python3 tools/midi_wire_model/run_checks.py              # quick, under a minute
python3 tools/midi_wire_model/run_checks.py --full       # about 3 min
python3 tools/midi_wire_model/run_checks.py --zero-drain zd.jsonl   # + ~2 min
python3 tools/midi_wire_model/run_checks.py --margin 32 --flam-cap 319   # a variant
```

It prints the rule it ran, one line per check and exits 1 on a failed
assertion. The checks (`checks.py`):

| Check | Asserts |
|---|---|
| calibrate | rule `today` matches the host probe cells |
| density | every row of the spec's table (n notes, gate 10/50/100 %, tied, ties ending, sliding) and the kit corollaries: no skip, no lost second hit, no flam played plain |
| kit edge | 1-2 kits of 1/6/11 voices, flammed or not, plus SYNTH tracks filling the budget, at every D: no skip, no lost second hit |
| safety | prefill, hammered keys, slides, catch-up bursts, tempo jumps, random patterns, 40-300 BPM, kits flammed and not: no overflow, fill + 3 x owed <= 1,023 |
| first followed step | the first step following a Clock with no estimate, internal and master tempo apart: no skip within the table, no overflow |
| pass-through window | a key in phase 1 on an empty queue is accepted at every tempo |
| R0 | the old rule still overflows (prefill before a tie-ending step at slow tempo) |
| zero drain | one step from any state with fill + 3 x owed <= 1,023, the wire stalled, then Stop: no overflow; resumable from its log |

Run the quick mode before and after any change to the rule, `--full` before
the spec's numbers change. The zero-drain sweep logs one line per tempo and
resumes from the log, so a run cut short continues where it stopped.
