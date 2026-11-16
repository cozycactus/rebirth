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
| I²C0 bus | 8, 9 | 400 kHz; external 2.2–4.7 kΩ pull-ups to 3.3 V (internal pulls are off) |
| I²C devices | on 8, 9 | MCP23017 0x20 step buttons 1-7 and 9-15, MCP23017 0x24 step buttons 8 and 16, MCP23017 0x21 step LEDs, MCP23017 0x22 + 0x23 panel key matrix scanner (pads, SCENE, MUTE) |
| Tempo LED | 25 | Pico on-board LED |
| RGB LED data | 27 | 82× SK6812MINI-E via PIO, buffered to 5 V |
| PLAY button | 28 | Momentary switch to GND, internal pull-up |

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

Twelve CTest targets (eleven test sources; `test_sync` is built twice, once per DAC
gain) cover pattern/song data (`test_pattern`, `test_song`), MIDI/gate output at
both DAC gains (`test_sync`, `test_sync_gain2`), the real timer/transport code
(`test_playback`), flash storage (`test_storage`), the Session view
(`test_session`), the LED frame builder (`test_led_frame`), the MCP23017 key
matrix scan driver (`test_key_scan`), the panel input on that scanner
(`test_panel_input`), the MCP23017 button/LED expanders and encoder buttons
(`test_expanders`) and UI/OLED rendering (`test_ui`). Launch
quantization runs in `sequencer.c` and is tested in `test_playback`.
Unix builds also exercise concurrent Stop/Panic during a pending gate commit.

## Live Controls

The Session view is home. The grid shows one bank (A-D) of 8 slots per track:
column = track, row = slot. The **focus** is one cell (track, slot); the step
buttons edit it.

- **Pad:** launches that slot on the track at the next launch point and moves
  the focus there. While stopped the selection is immediate. Pressing a queued
  pad cancels the queue; pressing the playing pad changes nothing except that
  it cancels a pending queue to another slot of that track. An empty slot
  launches too, which silences the track.
- **SCENE:** launches the row on every track together; tracks already playing
  it keep playing. Pressed while any track is queued to that row, it cancels
  all queues.
- **MUTE:** toggles the track's mute at once. On a track silenced by a mute
  stored in an older save, it makes the track audible and clears that stored
  mute in every slot.
- **PLAY:** starts and stops the transport; it is the only start/stop. Stop
  cancels pending queues.
- **Step buttons 1-16:** a short press toggles the step on release; a long
  press opens the step editor on that step without toggling it. Both act on
  the clip that was focused when the button went down, even if a pad or
  encoder 1 moves the focus before release; leaving the Session view drops
  the press. Steps beyond the clip's length do nothing.
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
  configuration without muting. Save/load (9/10) require stopped playback;
  buttons, pads and PLAY held through a save or load do nothing afterwards.
- A long press only counts on the screen where the button went down.
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
  sequencer.c       — Core 1 engine, timer ISR, transport, launch quantization
  pattern.c         — Pattern data manipulation
  song.c            — Song editing, track mutes
  midi_out.c        — MIDI UART + buffered output
  cv_gate.c         — MCP4822 DAC + GPIO gate control
  display.c         — OLED drawing layer
  ssd1309.c         — OLED controller driver
  ui.c              — UI state machine
  session.c         — Session view: bank, focus, pad/scene/mute presses
  led_frame.c       — Pure LED frame builder (light tables, current budget)
  rgb_leds.c        — 82-LED SK6812 chain on GPIO 27 (PIO)
  key_scan.c        — Key matrix scanner on two MCP23017 (I2C 0x22, 0x23)
  panel_input.c     — Panel key and PLAY debounce, events, recovery, resync
  mcp23017.c        — Step buttons (MCP23017 0x20 and 0x24)
  step_leds.c       — Step LEDs (MCP23017 0x21)
  encoder.c         — Two rotary encoders with push buttons
  flash_storage.c   — Save/load of patterns and song
HARDWARE_STATUS.md  — Build/bench verification status
JITTER_MEASUREMENT.md — Timing measurement procedure
test/
  test_pattern.c    — Pattern logic tests
  test_sync.c       — Synchronization layer tests
  test_song.c       — Song mode tests
  test_playback.c   — Timer, transport, launch quantization, note and MIDI
                      queue regression tests
  test_storage.c    — Save validation, interrupted writes, input resync
  test_ui.c         — UI events and production OLED rendering
  test_session.c    — Pad/scene launch and cancel, focus, bank
  test_led_frame.c  — LED light tables and current budget
  test_key_scan.c   — Scan driver: configuration, one column low, faults, chip resets
  test_panel_input.c — Matrix scan decoding, debounce, recovery, resync, latency
  test_expanders.c  — MCP23017 (0x20 + 0x24), step LED and encoder error handling, resync
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
