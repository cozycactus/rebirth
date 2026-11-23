# Host density probe

Measures how much playback one DIN MIDI wire carries with today's firmware:
the production code with the host-test fakes, the fake UART in wire-time mode
(320 us a byte, no interrupt latency, no flash stalls). It produced the
"Measured on the host" table of
`docs/superpowers/specs/2026-11-20-four-midi-outputs-design.md` and calibrates
`tools/midi_wire_model`.

```
python3 tools/midi_density_probe/run_probe.py cell "A N=25" 25 0 0 0 50 300
python3 tools/midi_density_probe/run_probe.py            # the full table, CSV
python3 tools/midi_density_probe/run_probe.py long       # the 64-bar cells
```

A cell is: name, SYNTH tracks, kits on T1-T2 (0/1), every kit voice flammed
(0/1), every SYNTH track on MIDI channel 1 (0/1), gate %, BPM; 8 bars.
SYNTH tracks follow the kits, channel = track % 16, note 52 + track, a note on
every step; kits on channels 10 and 11 with every voice on every step.

`run_probe.py` copies `src/`, `include/` and `test/` to a temporary folder,
applies `midi_out.patch` (counters for the peak fill and the overflow cause),
enlarges the fake's byte capture to 1 MiB, adds `density_probe.c` (which
includes `test/test_playback.c` for its helpers) as a CMake target beside the
host tests, builds and runs it. The tree is not touched. Needs CMake, a C
compiler and `patch` on a Unix-like host (the target links the host tests'
thread library). If `src/midi_out.c` changes so that the patch
no longer applies, update the patch.

A cell runs 8 bars. The spec's "Holds to" column needs 64 bars near the edge:
`run_probe.py long` runs those cells. The spec's family D figures are charges;
the CSV has wire and queued bytes, not charges.

CSV columns: result (`ok` or `STOP`), the 0-based step that stopped, the peak
queued bytes (statuses included, before running status), the overflow cause
(`tx`, `rt`, `buf`), flam drops, wire bytes and wire bytes per step (Clock
included), queued bytes per step, Clocks expected and on the wire, the latest
Clock's lateness in us, overruns, the fill at the end of the run. Wire bytes
per step leave out the one Start byte; for a run that stopped, the queued bytes
include the releases and All Notes Off of the panic. At gate 100 % the last
step's Note Offs are never sent, hence 101.62 instead of 102.
