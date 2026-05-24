# ReBirth — RP2040 Pattern Sequencer

8-track, 16-step hardware sequencer inspired by [ReBirth RB-338](https://en.wikipedia.org/wiki/ReBirth_RB-338).  
Built for the Raspberry Pi Pico (RP2040) — make full tracks without a computer using external synths and drum machines.

## Features

- **8 tracks × 16 steps** — synth (melodic) or drum (percussive) per track
- **64 patterns** with **A/B variations** each
- **Song mode** — chain patterns with repeat counts, loop or stop at end
- **Pattern queue** — schedule the next pattern during playback
- **Track mutes** — instant bitmask-based mute/unmute
- **Simultaneous output** — two-phase commit ensures all tracks fire at the same time
- **MIDI output** — DIN-5 UART only, avoiding USB MIDI host scheduling jitter
- **CV/Gate output** — 4 channels via MCP4822 DACs (1V/oct, 12-bit)
- **96 PPQN** timing resolution, 20–300 BPM
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
| Encoders | 18–22, 26 | 4× rotary + push |
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

## Tests

Host-side unit tests (no hardware needed):

```bash
cd test
mkdir build && cd build
cmake ..
make -j$(nproc)

./test_pattern   # 18 tests — pattern data manipulation
./test_sync      # 15 tests — MIDI buffer, gate commit, sync ordering
./test_song      # 29 tests — song editing, pattern queue, track mutes
```

## Project Structure

```
include/
  sequencer.h       — All data types and API declarations
src/
  main.c            — Core 0 entry, init, UI loop
  sequencer.c       — Core 1 engine, timer ISR, transport
  pattern.c         — Pattern data manipulation
  song.c            — Song editing, pattern queue, track mutes
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
```

## Status

- [x] Sequencer engine with 96 PPQN timer
- [x] Two-phase commit for track synchronization
- [x] Song mode with pattern chaining
- [x] A/B pattern variations
- [x] Pattern queue
- [x] Track mutes
- [x] 62 unit tests passing
- [x] OLED display driver (SSD1309, SPI, full framebuffer)
- [x] UI state machine (pattern edit, song edit, track config, pattern select)
- [x] Flash storage — save/load all patterns + song (step 9/10 in pattern select)
- [x] Jitter measurement — GPIO probe + on-device ISR timing stats
- [x] USB MIDI removed by design
- [ ] MIDI clock sync (external)

## License

TBD
