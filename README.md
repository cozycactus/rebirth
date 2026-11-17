# ReBirth — RP2040 Pattern Sequencer

8-track, 16-step hardware sequencer inspired by [ReBirth RB-338](https://en.wikipedia.org/wiki/ReBirth_RB-338).  
Built for the Raspberry Pi Pico (RP2040) — make full tracks without a computer using external synths and drum machines.

## Features

- **8 tracks × 16 steps** — synth (melodic), drum (percussive) or kit per track
- **Kit track** — one track plays a whole TR-909 style drum machine over DIN MIDI:
  12 lanes per step (accent plus 11 voices), three velocity levels and flams
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
- **Note entry from a MIDI keyboard** — hold a step on the Pattern view and press
  a key on a keyboard at the DIN input: the step takes the key's note and velocity
- **Pads as a keyboard** — while a step of a SYNTH clip is held, the 64 pads are a
  four-octave piano and a pad press sets the step's note, with no MIDI keyboard
- **CV/Gate output** — 4 channels via MCP4822 DACs (1V/oct, 12-bit; default 0-2.048 V)
- **96 PPQN** internal timing, **24 PPQN** MIDI Clock, 20–300 BPM
- **External clock** — follows a MIDI Clock on the DIN input (CLOCK INT / EXT /
  AUTO) with Start, Stop, Continue and Song Position Pointer; the OLED shows the
  measured tempo as `EXT 145.0`
- **Swing value** per pattern (playback timing pending)
- **Step flags** — accent, slide, tie (synth and drum tracks)

## Architecture

Dual-core split for low-jitter timing:

| Core | Role |
|------|------|
| **Core 1** | Sequencer engine — timer IRQ at 96 PPQN, step triggers, MIDI TX, CV/Gate |
| **Core 0** | UI — OLED display, encoders, buttons, pattern editing |

Shared writes and playback snapshots are protected by spinlocks. The sequencer ISR uses a two-phase commit:
1. **Collect** — buffer MIDI messages, stage CV/gate changes
2. **Commit** — simultaneous GPIO writes for gates, then queue MIDI buffer

DIN MIDI bytes are queued into a UART TX ring buffer and drained by UART0's TX interrupt, one byte per interrupt, so the sequencer timer ISR does not wait for MIDI serial transmission. MIDI Clock and Start/Continue/Stop go out at the next message boundary, ahead of queued note bytes.

## Hardware

| Function | GPIOs | Notes |
|----------|-------|-------|
| MIDI TX/RX | 0, 1 | UART0 @ 31250 baud; RX (J402, opto) takes a keyboard, see Live Controls |
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

Sixteen CTest targets (fifteen test sources; `test_sync` is built twice, once per DAC
gain) cover pattern/song data (`test_pattern`, `test_song`), the kit lane
helpers and settings (`test_kit`), the MIDI input parser (`test_midi_in`), MIDI/gate output at
both DAC gains (`test_sync`, `test_sync_gain2`), the real timer/transport code
(`test_playback`), following an external clock (`test_clock`), flash storage (`test_storage`), the Session view
(`test_session`), the LED frame builder (`test_led_frame`), the MCP23017 key
matrix scan driver (`test_key_scan`), the panel input on that scanner
(`test_panel_input`), the MCP23017 button/LED expanders and encoder
buttons (`test_expanders`), encoder clicks and their acceleration
(`test_encoder`) and UI/OLED rendering (`test_ui`). Launch
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
- **Copy a clip:** hold a pad for half a second, then press another pad of the
  same track. The first pad's clip (steps, kit steps and length, not the
  track's routing or mute) is written over the second pad's, and the OLED
  shows `COPY A1>A3`. The second press does not launch or queue anything, so
  a clip that is playing or queued stays so and plays the copy. In the
  Session view the focus moves to the copy. The first pad stands for the
  clip it had when it was pressed, so changing the bank during the hold
  copies into another bank. Two pads pressed less than half a second apart are
  two launches, and a hold that the panel may have lost (after a panic, a
  save or load, or a panel error) is not trusted: press the source again.
  An existing clip in the target is overwritten and there is no undo.
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
- **Turning either encoder:** every screen counts whole clicks. A choice moves
  one per click however fast the turn: the focus, a list row, a kit lane, TYPE,
  OUTPUT, a kit step's value, the pattern slot and bank, the song cursor and a
  song entry's pattern. A number moves one per slow click and faster on a fast
  turn: clicks under 60 ms apart count 1 for the first four, then 4 each for
  three more, then 8 each. A pause of 60 ms or a turn back starts again at 1.
  Numbers are the tempo (0.1 BPM a step), a step's note and velocity, CHANNEL,
  GATE, a drum's NOTE, FLAM, the kit velocities and NOTE rows, and a song
  entry's repeats.
- Pads, SCENE, MUTE and PLAY work on every screen. Outside the Session view a
  pad launches without moving the focus.
- The step editor returns to the screen it was opened from. In the pattern
  view, encoder 1 long returns to the Session view; in the selector, encoder 2
  long does.
- The Pattern view of a SYNTH clip is a piano roll: time runs from top to
  bottom (step 1 at the top, four pixels a step) and pitch from left to right
  (four pixels a semitone, a window of 26 semitones that follows the clip's
  lowest and highest notes). A note is a 3x3 block, 4 wide with accent; a line
  under the block runs to the next note for a slide, and a single dot under it
  is a tie. A note outside the window is a bar at the left or right edge. Steps
  beyond the clip's length are a dotted line and show no notes, a row of dots
  after every fourth step marks the beats, and the step the track is playing is
  inverted. The four
  characters at the left show clip and track, length, the note at the window's
  left edge and the tempo; while a step is held they also show its number,
  note, velocity and flags (A accent, S slide, T tie), and a mark beside the
  step's rows. A DRUM clip keeps the step grid, and a kit its lanes.
- Pattern view step buttons on a SYNTH or DRUM track: a tap shorter than half
  a second toggles the step when the button is released. Hold one or more steps
  and turn encoder 1 for note or encoder 2 for velocity; encoder 1 short/long
  toggles accent/slide and encoder 2 long toggles tie on the held steps. The
  first edit of a held empty step creates it. A step held longer, or held while
  an encoder is turned or pressed, is not toggled on release. While a step is
  held the encoders do not change track, tempo or screen; pads, SCENE, MUTE and
  PLAY work as usual. On a kit track the step buttons edit the focused lane
  instead (see Editing a kit): a short press cycles it on release, a long press
  opens the kit step editor, and a held kit step takes no encoder, so TRACK
  still walks and VALUE still sets the tempo.
- A new SYNTH step starts at the note last entered on that track (C4 after
  power-up), not at C4: the last note is what a turn of encoder 1 on held steps
  or in the step editor left on the lowest step it changed. Steps that already
  have a note keep it, and DRUM and kit tracks do not use it. It is not saved.
  Holding empty steps and turning therefore moves them relative to it.
- **MIDI keyboard** on the DIN input (J402), any channel; notes only, no
  control change or pitch bend (a MIDI Clock there is the external clock, below). **Hold a step in the Pattern view and
  press a key:** the step is switched on if it was off and takes the key's
  note (SYNTH tracks) and velocity; two held steps take the same key, and a step
  that a key wrote to is not toggled on release. In the step editor the same
  works while the edited step's button is held (the step is not switched on or
  off). On a DRUM track the key sets the velocity only. **A key with no step
  held** sounds through the track being edited, on its MIDI channel (the
  focused track in the Session view, the configured one in track config), and
  becomes that track's last note, so the next steps switched on start there. A
  kit track passes the keys to its channel and nothing else. A key is released
  on the channel it started on, whatever the focus does meanwhile. At most half
  of the output queue is used by keys: a key pressed while it is that full is
  dropped, which cannot stop the sequencer. Keys are told apart by pitch, so
  two keys of one pitch on different keyboard channels count as one. A key lost
  at the UART (not counted) can give one wrong note; unplugging a keyboard
  with a key down leaves the note sounding until Panic. A keyboard's All Notes
  Off and All Sound Off release the notes it started, and so does a flash save
  or load.
- **Pads as a keyboard.** While a step of a SYNTH clip is held in the Pattern
  view (or the edited step's button in the step editor), the pads stop launching
  clips and are a keyboard: a pad press does what a key of the MIDI keyboard does
  (the held step is switched on if it was off and takes the note, the note is
  sounded on the track's channel), except that the velocity is always 100 and a
  step's own velocity is left alone. All eight columns are keys of the track being
  edited. Rows are paired from the bottom: in each pair the lower row is the white
  keys and the upper row the black keys, and the pairs are consecutive octaves.
  The window is fixed when the first step is held: the held step's octave (the
  track's last note for an empty step) is the second pair from the bottom, one
  octave below and two above, and it does not move until every step is let go.

  | Column | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
  |---|---|---|---|---|---|---|---|---|
  | lower row (white) | C | D | E | F | G | A | B | C |
  | upper row (black) | - | C# | D# | - | F# | G# | A# | - |

  Lights: white keys are the track's colour at half level, black keys dim, the pads
  with no note are off, and every pad of the held step's note is white at full. In
  the Session view, on DRUM and kit cells and with no step held, the pads launch
  and copy as before. A pad released after its step still ends its note.
- Hold both encoder buttons for Panic: it stops playback and returns to the
  Session view. Release both before issuing another command; keys held through
  the panic or pressed while it is held do nothing afterwards.
- In the selector, buttons 1-8 mute on **short release**; holding opens track
  configuration without muting. Save/load (9/10) require stopped playback (and
  no start waiting for an external Clock); buttons, pads and PLAY held through
  a save or load do nothing afterwards.
- **External clock.** In the selector, button 14 cycles the clock source:
  `CLOCK INT` (the device is the master, as at power-up), `CLOCK EXT` (it only
  follows a MIDI Clock on the DIN input) and `CLOCK AUTO` (it follows one when
  it is there, else runs on its own); the selector shows `CLK EXT` and button 14
  is lit when the source is not INT. It is not saved.
  - Each Clock received drives four of the 96 PPQN pulses: the first when it
    arrives, so every step starts on its Clock, the other three spaced by the
    measured tempo. Steps keep the master's timing, jitter included; a bunch of
    late Clocks is caught up quickly rather than played at once.
  - A Start from the master starts from the beginning on the first Clock after
    it; Stop stops; Continue goes on from where it stopped or from a Song
    Position Pointer received while stopped (in Pattern mode each track goes to
    the position modulo its length, in Song mode to the entry and repeat). An
    SPP during playback is ignored. With INT the wire's transport is ignored.
  - PLAY with EXT (or AUTO with a Clock coming in) waits for the next Clock to
    start; the tempo prefix blinks meanwhile, and PLAY again cancels it. This
    starts in time with a master that sends Clock without Start, but not on its
    bar.
  - The tempo line reads `EXT 145.0` (the measured tempo), `EXT  --.-` (EXT
    with no Clock) or `INT 120.0` (AUTO on its own timer). VALUE sets the tempo
    only on the internal timer and otherwise shows `TEMPO EXT`.
  - When the Clock stops for three Clock intervals (at least 50 ms), EXT stops
    and releases every note and gate; AUTO plays on at the measured tempo and
    follows the Clock again from where it is when it returns (the tempo locks,
    a phase lost in the gap stays until the next Start).
  - MIDI Clock out follows: one Clock out for each Clock in, while playing,
    with Start, Continue and Stop when the device's own transport changes. SPP
    is not passed on.
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

New saves use version 6 with CRC32; version 4 and 5 saves still load after
validation (they load with empty kits and default kit settings). Storage uses
one flash region, so losing power during a save can lose that save.

### Kit track

A kit track plays a TR-909 style drum machine over DIN MIDI from one track.
Each step has 12 lanes, in ReBirth's 909 column order:

| Lane | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Name | AC | BD | SD | LT | MT | HT | RS | CP | CH | OH | CC | RC |

AC is the total accent: it plays nothing itself and makes every hit on its
step one level louder. A voice lane holds off, note, accent or flam. A kit is
MIDI only: it never touches CV or the gates, and the track's OUTPUT, CV and
drum-note settings are ignored. At most two tracks can be kits; a third is
refused with `MAX 2 KITS` in the track config.

**Setting up a 909.** Set the track's TYPE to KIT in its track config
(hold its button 1-8 in the pattern selector), then set CHANNEL to the
machine's MIDI channel and the NOTE rows to the machine's note map. The
defaults follow the RD-9: BD 36, SD 38, LT 45, MT 47, HT 50, RS 37, CP 39,
CH 42, OH 46, CC 49, RC 51; other models are set on the bench. The kit
sends Note Off as Note On with velocity 0, so a machine that needs a real
Note Off is not supported. Three velocities give the three levels the 909
has, and the machine's accent threshold should lie between them:

| Step | Without AC | With AC |
|---|---|---|
| note | VEL NORM (80) | VEL ACC (110) |
| accent | VEL ACC (110) | VEL MAX (127) |
| flam | first hit as a note, second hit one level up | same |

A flam plays its second hit FLAM pulses later (96 pulses per quarter note,
default 3, 1-12). The config screen shows the time at the current tempo.

**The kick on its own track.** Keep the kick on an ordinary DRUM track on the
909's channel (note 36) and the BD lane of the kit empty. The kick then has its
own MUTE and its own clips, as a separate mixer channel did in a live rig.
Three things follow:
- The kit's AC lane does not reach the kick track. Accent the kick with its own
  step accent flag (velocity 127).
- The kick's plain steps use the step velocity (100 when a step is switched on),
  not the kit's VEL NORM. On a machine whose accent threshold lies below 100,
  lower that velocity in the step editor.
- The kick track and the kit's BD lane send the same note on the same channel,
  so use one or the other (see below).

**One note, one track.** Any track (DRUM, SYNTH or another kit) on the kit's
channel that plays one of the kit's notes has its note cut by the kit's
releases (a kit Note Off is Note On velocity 0 on that note), and both trigger
the same sound. Keep each kit note unique on its channel: leave the kit's BD
lane empty when the kick has its own track, move a voice's NOTE away from a
note another track plays there, and give two kits on one channel note maps that
do not share a note (or a channel each). The power-up demo follows this: track
1 is the kick, a DRUM track on CHANNEL 10 (note 36), and track 2 a kit on the
same channel with its default settings, its BD lane empty, SD on steps 5 and 13
and CH on every step.

**Drums on the leftmost columns.** Inside one step the notes go out in track
order, so the kick and the kit on the leftmost tracks get the earliest bytes.
On DIN MIDI the last note of a dense step still leaves several milliseconds
after the first (about 4 ms for a kit of six voices, 7 ms for all 11). Within a
kit the order is BD, SD, CP, CH, OH, RS, LT, MT, HT, CC, RC.

**Editing a kit.** TRACK walks the lanes: a kit is 12 stops, AC to RC, in the
Session view and the Pattern view, and turning right into a kit lands on AC,
left on RC. Each kit remembers its last lane (BD after power-up), and a pad on
the kit's column moves the focus onto it. The step buttons edit the focused
lane: a voice cycles off, note, accent, off (a flam step goes to off), AC
toggles. Setting CH on a step clears OH there and the other way round. The
step LEDs show the lane's non-zero steps; accent and flam are shown on the OLED
strip (`.` off, `o` note, `O` accent, `f` flam). A long press on a step, or a
TRACK press in the Pattern view, opens the kit step editor: TRACK moves the
lane cursor, VALUE sets the lane's value, step buttons choose another step and
a VALUE press returns. One click moves one lane, one row or one value step;
the kit settings are numbers and speed up on a fast turn (see Live Controls).

A kit track keeps its `step_t` data when its type changes, and its kit steps
are kept too, so changing the type back restores either.

## Project Structure

```
include/
  sequencer.h       — All data types and API declarations
src/
  main.c            — Core 0 entry, init, UI loop
  sequencer.c       — Core 1 engine, timer ISR, transport, launch quantization
  pattern.c         — Pattern data manipulation
  song.c            — Song editing, track mutes
  midi_out.c        — MIDI UART, buffered output, running status, input ring
  midi_in.c         — Parser for the bytes of the DIN input
  clock_in.c        — External clock: received Clock/Start/Stop/Continue/SPP for core 1,
                      tempo estimate
  kit.c             — Kit lane helpers, settings, glyphs
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
  test_kit.c        — Kit lane, settings and glyph helpers
  test_encoder.c    — Encoder clicks and detent-rate acceleration
  test_midi_in.c    — MIDI input parser
  test_clock.c      — Following an external clock: tempo, Start/Stop/Continue/SPP,
                      jitter, bursts, loss, AUTO
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
- [x] Kit track: 909 over DIN MIDI, 12 lanes, velocity levels, flams (bench items
      in [HARDWARE_STATUS.md](HARDWARE_STATUS.md)), save format v6
- [x] Note entry from a MIDI keyboard: step + key, pass-through otherwise (host
      tests only; the input circuit is a bench item in [HARDWARE_STATUS.md](HARDWARE_STATUS.md))
- [x] External MIDI clock: INT / EXT / AUTO, Start, Stop, Continue, SPP (host
      tests and the simulator; a real master is a bench item in
      [HARDWARE_STATUS.md](HARDWARE_STATUS.md))

## Known Issues

- The kit feature is not bench verified; see [HARDWARE_STATUS.md](HARDWARE_STATUS.md).
- The MIDI keyboard input is not bench verified (the H11L1 input circuit is not
  validated); see [HARDWARE_STATUS.md](HARDWARE_STATUS.md).
- Following an external clock is not bench verified. A byte lost in the UART
  receiver (only possible with core 0's interrupts off for more than one byte
  time) is not detected: a lost Clock leaves the device a Clock behind the
  master until its next Start.

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

Behavior of the original ReBirth RB-338 is taken from
[OpenRB-338](https://github.com/pingw33n/openrb-338) (also GPL-3.0).
