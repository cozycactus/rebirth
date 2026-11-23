---
name: midi-wire-budget
description: How to check MIDI output load, TX queue overflow and note admission in this sequencer with the byte-level model (tools/midi_wire_model) and the host density probe (tools/midi_density_probe). Use whenever work touches the TX queue of src/midi_out.c, the pass-through caps in src/ui.c (thru_note_on, thru_note_off, THRU_*_QUEUE_MAX), the step, timing, admission or output-port parts of src/sequencer.c and src/song.c, the Clock estimate in src/clock_in.c, kit flams or more MIDI outputs, and for questions like "how many notes fit at 240 BPM", "will this overflow the MIDI queue", "why did a note or flam drop", "can MIDI timing be checked". Also in Russian: "сколько нот влезет на выход", "переполнится ли очередь MIDI", "проверь нагрузку на MIDI", "пропуск нот", "флэм теряется", "джиттер MIDI клока", "четыре MIDI выхода".
---

# MIDI wire budget

A DIN MIDI wire sends one byte every 320 us. Whether a step's notes fit, and
whether the TX queue can overflow, is settled with a model and host
measurements, not by guessing.

**State of the code.** Per-output admission (budget, guard, reservation, the
flam cap) is a design, not yet in `src/`:
`docs/superpowers/specs/2026-11-20-four-midi-outputs-design.md`. Today a full
TX queue sets the overflow flag and the transport stops. Say which of the two
you are talking about.

**Numbers.** Take the density table, the margin, the caps and the proof from
the spec ("Overload: admission per output", "The density promise", "No
overflow by construction"), and the model's coverage from
`tools/midi_wire_model/README.md`. Do not copy them into code comments, plans
or answers from memory: they have changed several times, and a copy drifts.

## Steps

1. Read the three spec sections above, and the README of the model.
2. Changing the rule (the spec): change `tools/midi_wire_model/sim.py` to
   match, then run `python3 tools/midi_wire_model/run_checks.py` (quick, under
   a minute) and `--full` (about 25 minutes; its zero-drain sweep resumes from
   its log after an interruption). Both must pass before the spec's numbers
   change. The model runs no C: it checks the rule, never an implementation.
3. Changing the code in `src/`: the host tests check it (`test/test_playback.c`,
   `test/test_clock.c`; the spec's "Tests" section lists what the
   implementation adds). Measure load on the host with the fake UART in
   wire-time mode (`fake_uart_run_until()`, as `play_load()` does), or one cell
   of the probe:
   `python3 tools/midi_density_probe/run_probe.py cell "A N=25" 25 0 0 0 50 300`
   (8 bars; `long` for the 64-bar cells). It builds in a temporary copy. Its
   `midi_out.patch` fits today's `src/midi_out.c`; once the queue changes, the
   patch must be updated before the probe runs.
4. Timing on the wire, as far as the host can see it: `test/test_playback.c`
   checks in wire time that every Clock starts within four byte times
   (1,280 us) of its pulse; `test/test_clock.c` covers following an external
   Clock (early, late and bunched Clocks, catch-up), mostly without wire time.
   Neither sees interrupt latency or the hardware: real jitter is for a board.
5. The analog side of the MIDI ports (opto, driver, currents) is the SPICE
   bench, if `hardware/schematic/sim/run.py` is in the tree:
   `python3 hardware/schematic/sim/run.py midi`.

## Limits

- The model is not the firmware: one port, no interrupt latency, no flash
  stalls, phase 1 in zero time; its README lists what it leaves out.
- No board has been built. Report host and model results as such, and never
  report a bench measurement as done or pending.
