# ReBirth — RP2040 Pattern Sequencer

8-track, 16-step hardware sequencer inspired by [ReBirth RB-338](https://en.wikipedia.org/wiki/ReBirth_RB-338).  
Built for the Raspberry Pi Pico (RP2040) — make full tracks without a computer using external synths and drum machines.

## Features

- **8 tracks × 16 steps** — synth (melodic) or drum (percussive) per track
- **32 pattern locations** arranged like ReBirth: banks A-D, slots 1-8
- **Independent pattern selection per track** — each track can play or queue a different bank/slot
- **Per-track pattern length** — each track can loop at 1-16 sixteenth-note steps
- **Live pattern editing** — step, length, and track-setting edits update playback while running
- **Performance view** — safe live mutes, per-track pattern queueing, transport, BPM, and jitter status
- **Song mode** — chain patterns with repeat counts, loop or stop at end
- **Pattern queue** — schedule the next pattern during playback
- **Track mutes** — instant bitmask-based mute/unmute
- **Panic combo** — hold both encoder buttons to stop playback, drop gates, and send MIDI all-notes-off
- **Grouped gate edges** — two-phase output; DAC and DIN MIDI transfers remain sequential
- **MIDI output** — DIN-5 UART only, avoiding USB MIDI host scheduling jitter
- **CV/Gate output** — 4 channels via MCP4822 DACs (1V/oct, 12-bit; default 0-2.048 V)
- **96 PPQN** internal timing, **24 PPQN** MIDI Clock, 20–300 BPM
- **Swing value** per pattern (playback timing pending)
- **Step flags** — accent, slide, tie

## Architecture

Dual-core split for low-jitter timing:

| Core | Role |
|------|------|
| **Core 1** | Sequencer engine — timer IRQ at 96 PPQN, step triggers, MIDI TX, CV/Gate |
| **Core 0** | UI — OLED display, encoders, buttons, pattern editing |

Shared writes and playback snapshots are protected by spinlocks. The sequencer ISR uses a two-phase commit:
1. **Collect** — buffer MIDI messages, stage CV/gate changes
2. **Commit** — simultaneous GPIO writes for gates, then queue MIDI buffer

DIN MIDI bytes are queued into a UART TX ring buffer and drained by UART0's TX interrupt, so the sequencer timer ISR does not wait for MIDI serial transmission.

## Hardware

| Function | GPIOs | Notes |
|----------|-------|-------|
| MIDI TX/RX | 0, 1 | UART0 @ 31250 baud |
| OLED (SSD1309) | 2–6 | SPI0, 128×64 |
| DAC (2× MCP4822) | 10–13 | SPI1, 4 CV channels |
| Gate outputs | 14–17 | 3.3V logic (4 channels) |
| Jitter probe | 7 | High while sequencer timer ISR runs |
| Encoders | 18–22, 26 | 2× rotary + push |
| I²C expander | 8, 9 | MCP23017 for step buttons |
| Tempo LED | 25 | |

## Build

Requires [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk).

```bash
export PICO_SDK_PATH=/path/to/pico-sdk

mkdir build && cd build
cmake ..
make -j$(nproc)
```

Flash the resulting `.uf2` file by holding BOOTSEL and dragging to the Pico's USB drive.

The default DAC gain is 1x. For a board designed for 0-4.096 V CV, configure with
`-DREBIRTH_CV_DAC_GAIN=2`; the DAC supply and logic interface must support it.
See [ARCHITECTURE.md](ARCHITECTURE.md#cv-circuit) before choosing the hardware.

## Tests

Host-side unit tests (no hardware needed):

```bash
cd test
mkdir build && cd build
cmake ..
make -j$(nproc)

ctest --output-on-failure
```

Seven test executables cover pattern/song data, MIDI/gate output at both DAC
gains, the real timer/transport code, flash storage, and UI/OLED rendering.
Unix builds also exercise concurrent Stop/Panic during a pending gate commit.

## Live Controls

- Performance: encoder 1 selects the track; encoder 2 changes tempo.
- Buttons 1-8 mute on press. Buttons 9-16 select bank slots 1-8: while stopped
  selection is immediate; while playing it queues at that track's loop end.
- Encoder 1 short opens the selected playing pattern for editing; long cycles bank.
- Encoder 2 short starts/stops; long opens the pattern selector.
- Hold both encoder buttons for Panic. Release both before issuing another command.
- In the selector, buttons 1-8 mute on **short release**; holding opens track
  configuration without muting. Save/load (9/10) require stopped playback.
- Manual queues are unavailable in Song mode. In the song editor, button 2
  starts Song mode and button 3 returns to Pattern playback.
- Slide/tie connect a step to the following step. Gate length controls MIDI
  note duration as well as CV gates.
- Step editor: encoder 1 short toggles accent, long toggles slide; encoder 2
  long toggles tie. Accent can combine with either connection type.

New saves use version 5 with CRC32; version 4 saves still load after validation.
Storage uses one flash region, so losing power during a save can lose that save.

## Project Structure

```
include/
  sequencer.h       — All data types and API declarations
src/
  main.c            — Core 0 entry, init, UI loop
  sequencer.c       — Core 1 engine, timer ISR, transport
  pattern.c         — Pattern data manipulation
  song.c            — Song editing, track mutes
  midi_out.c        — MIDI UART + buffered output
  cv_gate.c         — MCP4822 DAC + GPIO gate control
  display.c         — OLED drawing layer
  ui.c              — UI state machine
HARDWARE_STATUS.md  — Build/bench verification status
JITTER_MEASUREMENT.md — Timing measurement procedure
test/
  test_pattern.c    — Pattern logic tests
  test_sync.c       — Synchronization layer tests
  test_song.c       — Song mode tests
  test_playback.c   — Timer, transport, note and MIDI queue regression tests
  test_storage.c    — Save validation and interrupted-write tests
  test_ui.c         — UI events and production OLED rendering
```

## Status

- [x] Sequencer engine with 96 PPQN timer
- [x] Two-phase commit for track synchronization
- [x] Song mode with pattern chaining
- [x] ReBirth-style pattern banks A-D × 1-8
- [x] Independent per-track pattern selection and queueing
- [x] Per-track pattern lengths
- [x] Pattern queue
- [x] Track mutes
- [x] Performance-safe live control view
- [x] Panic / all-notes-off recovery
- [x] Host regression tests for playback, UI, storage and output
- [x] OLED display driver (SSD1309, SPI, full framebuffer)
- [x] UI state machine (pattern edit, song edit, track config, pattern select)
- [x] Flash storage — save/load all patterns + song (step 9/10 in pattern select)
- [x] Jitter measurement — GPIO probe + on-device ISR timing stats
- [x] USB MIDI removed by design
- [ ] MIDI clock sync (external)

## License

TBD
