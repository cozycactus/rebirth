---
name: midi-wire-budget
description: How to check MIDI output load, queue overflow and note admission in this sequencer with the byte-level model (tools/midi_wire_model) and the host density probe (tools/midi_density_probe). Use whenever work touches src/midi_out.c, the timing, step or admission parts of src/sequencer.c, src/clock_in.c, kit flams or more MIDI outputs, and for questions like "how many notes fit at 240 BPM", "will this overflow the MIDI queue", "can MIDI jitter be checked", "why did a note or flam drop". Also in Russian: "сколько нот влезет на выход", "переполнится ли очередь MIDI", "проверь нагрузку на MIDI", "пропуск нот", "флэм теряется", "джиттер MIDI клока", "четыре MIDI выхода".
---

# MIDI wire budget

A DIN MIDI wire sends one byte every 320 us. Whether a step's notes fit, and
whether the TX queue can overflow, is settled with a model and host
measurements, not by guessing.

**State of the code.** Per-output admission (budget, guard, reservation, the
flam cap) is a design, not yet in `src/`:
`docs/superpowers/specs/2026-11-20-four-midi-outputs-design.md`. Today a full
TX queue sets the overflow flag and the transport stops. Say which of the two you are talking about.

**Numbers.** Take the density table, the margin, the caps and the proof from
the spec ("Overload: admission per output", "The density promise", "No
overflow by construction"), and the model's coverage from
`tools/midi_wire_model/README.md`. Do not copy them into code comments, plans
or answers from memory: they have changed several times, and a copy drifts.

## Steps

1. Read the three spec sections above, and the README of the model.
2. Before changing the rule or its code, run the quick check and keep its
   output; run it again after:
   `python3 tools/midi_wire_model/run_checks.py` (under a minute, exits 1 on a
   failed assertion). Before changing the spec's numbers, run `--full`, and
   `--zero-drain LOG` when the rule's structure changes. If the rule itself
   changes, change `sim.py` to match and say so.
3. Measure today's firmware on the host: the fake UART in wire-time mode
   (`fake_uart_run_until()`, as `play_load()` in `test/test_playback.c`), or
   one cell of the probe:
   `python3 tools/midi_density_probe/run_probe.py cell "A N=25" 25 0 0 0 50 300`.
   It builds in a temporary copy and leaves the tree alone.
4. Timing of the wire: `test/test_playback.c` checks that every Clock starts
   on the wire within four byte times (1,280 us) of its pulse;
   `test/test_clock.c` covers following an external Clock (early, late and
   bunched Clocks, catch-up). MIDI jitter is checked there, in wire time.
5. The analog side of the MIDI ports (opto, driver, currents) is the SPICE
   bench: `python3 hardware/schematic/sim/run.py midi`.

## Limits

- The model is not the firmware: one port, no interrupt latency, no flash
  stalls, phase 1 in zero time. A green model run supports the rule; only host
  tests of the implementation show the code follows it.
- No board has been built. Report host and model results as such, and never
  report a bench measurement as done or pending.
