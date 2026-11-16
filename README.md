# ReBirth — RP2040 Pattern Sequencer

8-track, 16-step hardware sequencer inspired by [ReBirth RB-338](https://en.wikipedia.org/wiki/ReBirth_RB-338).  
Built for the Raspberry Pi Pico (RP2040) — make full tracks without a computer using external synths and drum machines.

## Features

- **8 tracks × 16 steps** — synth (melodic) or drum (percussive) per track
- **32 pattern locations** arranged like ReBirth: banks A-D, slots 1-8
- **Independent pattern selection per track** — each track can play or queue a different bank/slot
- **Per-track pattern length** — each track can loop at 1-16 sixteenth-note steps
- **Live pattern editing** — step, length, and track-setting edits update playback while running
- **Session view** — 8×8 pad grid (tracks × slots of the shown bank), SCENE column,
  MUTE row and a PLAY button; the 16 step buttons edit the focused clip
- **Launch quantization** — pad and scene launches land on the next bar (default),
  beat, step, or at the end of the track's loop
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

The Session view is home. The grid shows one bank (A-D) of 8 slots per track:
column = track, row = slot. The **focus** is one cell (track, slot); the step
buttons edit it.

- **Pad:** launches that slot on the track at the next launch point and moves
  the focus there. While stopped the selection is immediate. Pressing a queued
  pad cancels the queue; pressing the playing pad changes nothing. An empty
  slot launches too, which silences the track.
- **SCENE:** launches the row on every track together; tracks already playing
  it keep playing. Pressed while any track is queued to that row, it cancels
  all queues.
- **MUTE:** toggles the track's mute at once.
- **PLAY:** starts and stops the transport; it is the only start/stop. Stop
  cancels pending queues.
- **Step buttons 1-16:** a short press toggles the focused clip's step on
  release; a long press opens the step editor on that step without toggling it.
  Steps beyond the clip's length do nothing.
- **Encoder 1 (TRACK):** turn to focus the previous/next track (on the slot it
  plays, or its queued one); press opens the pattern view on the focus; long
  press shows the next bank A → B → C → D → A.
- **Encoder 2 (VALUE):** turn for tempo; press cycles the launch quantization
  (1 BAR, 1 BEAT, NEXT STEP, TRACK LOOP; 1 BAR after power-up); long press opens
  the pattern selector on the focus.
- Pads, SCENE, MUTE and PLAY work on every screen. Outside the Session view a
  pad launches without moving the focus.
- The step editor returns to the screen it was opened from. In the pattern
  view, encoder 1 long returns to the Session view; in the selector, encoder 2
  long does.
- Hold both encoder buttons for Panic: it stops playback and returns to the
  Session view. Release both before issuing another command; keys held through
  the panic or pressed while it is held do nothing afterwards.
- In the selector, buttons 1-8 mute on **short release**; holding opens track
  configuration without muting. Save/load (9/10) require stopped playback.
- Pad and scene launches are unavailable in Song mode (`SONG MODE`); a pad
  still moves the focus. In the song editor, button 2 selects Song mode and
  button 3 Pattern playback; PLAY starts it.
- Song entries can be inserted or removed while Song mode plays: the playing
  entry finishes, then the entry after it in the edited list plays.
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
- [x] Session view: pad grid, scenes, mutes, PLAY and launch quantization
- [x] Panic / all-notes-off recovery
- [x] Host regression tests for playback, UI, storage and output
- [x] OLED display driver (SSD1309, SPI, full framebuffer)
- [x] UI state machine (pattern edit, song edit, track config, pattern select)
- [x] Flash storage — save/load all patterns + song (step 9/10 in pattern select)
- [x] Jitter measurement — GPIO probe + on-device ISR timing stats
- [x] USB MIDI removed by design
- [ ] MIDI clock sync (external)

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

Behavior of the original ReBirth RB-338 is taken from
[OpenRB-338](https://github.com/pingw33n/openrb-338) (also GPL-3.0).
