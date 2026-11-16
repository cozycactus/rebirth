# Session-style live panel: design

Date: 2026-11-16. Status: for approval; revised after a review by Codex, DeepSeek and Grok.
Mockup: `hardware/panel/build_panel.py` (Blender model, renders in `hardware/panel/renders/`).

## Intent

Turn the sequencer into an instrument that is played live with the hands, the way
clips are launched in Ableton Live's Session view, instead of through a menu and two
encoders. The target music is layered, loop-based electronic music (goa / acid trance
and similar): parts are added and removed every few bars, 16th-note lines with accent
and slide drive external synths over DIN MIDI and CV/Gate, and the computer is not
needed on stage.

The pattern model stays ReBirth-like (banks A-D x slots 1-8 per track, 16 steps,
per-track length); OpenRB-338 (https://github.com/pingw33n/openrb-338) remains the
reference for ReBirth behavior. The look of the device is its own and must not imitate
any commercial controller.

## Goals

- Every live action is one press on a dedicated control, with no menu: launch a pattern
  on a track, launch a row on all tracks (scene), mute, toggle steps of the focused
  pattern, start/stop, tempo, bank change.
- The state is readable at a glance from the lights: what plays, what is queued, what is
  empty, what is muted, where the playhead is.
- Launches land in time (quantized), so a press anywhere in the bar is safe.
- Sequencer timing stays as it is: no new work on Core 1 beyond the launch-quantization
  check, no added jitter from LEDs or the new I2C traffic.

## Non-goals (separate projects, later)

- Patterns longer than 16 steps (needed for 2-4 bar lead lines).
- MIDI CC per step / recorded knob movements.
- Recording live launches into the song (ReBirth-style song recording).
- Clip copy/delete/duplicate gestures on the grid.
- Velocity-sensitive pads.
- Quantized mutes.
- MIDI input (the rear MIDI IN socket stays reserved), external MIDI clock sync, swing in
  playback.
- PCB layout and enclosure CAD. This document fixes the panel layout and the electrical
  interface; the board and the case are the next step.

## Anti-goals (a failure even if it works)

- Any timing regression on Core 1 (GPIO 7 jitter stats, MIDI Clock) caused by this work.
- Transport stopping or a track going silent from an accidental press of something that
  is not PLAY, a pad, a scene or a mute.
- A live action that needs the OLED or a menu.
- A panel that looks like a copy of an existing product.

## Approaches considered

**Panel layout.** Decided by the user: **B**.
- A. 8x8 grid only; the grid switches into a 2x8 step-edit mode. Compact, but clips are
  hidden while steps are edited.
- **B. 8x8 grid plus a separate row of 16 step buttons.** Launch with one hand, edit steps
  with the other; the existing step buttons stay. Wider device.
- C. 8x4 grid plus 16 steps. Smaller and cheaper, but 32 locations need 8 pages.

**Grid electronics.** Author's call: **TCA8418 + addressable RGB LEDs on PIO.**
*Changed: the board (PR #10) uses the fallback named under "Input latency", two MCP23017 (0x22, 0x23) scanning
the diode matrix from Core 0, because the TCA8418 reports a press 25-40 ms late, past the
15 ms requirement. The TCA8418 paragraphs below are kept as the original decision; see
`2026-11-16-mcp23017-key-scan-design.md`.*
- **TCA8418 keypad scanner (I2C) + SK6812 chain on one GPIO via PIO.** One chip scans all
  80 keys with an event FIFO; one free GPIO drives every LED; the bus already exists.
  Needs a 3.3 V -> 5 V data buffer, a defined power-up state and a current budget.
- More MCP23017s + IS31FL3741 I2C LED matrix driver. Everything on I2C and constant-current,
  but ~240 PWM bytes per frame compete with the button polling on the 400 kHz bus, and the
  driver is a fine-pitch QFN.
- An off-the-shelf USB grid controller read through USB host. Rejected: USB was removed by
  design (host scheduling jitter), and the device would stop being one instrument.

## The panel (layout B)

As in the Blender model (sizes are the model's; exact parts are the builder's):

```
 [ OLED ]   (TRACK)  (VALUE)        [PLAY]   o BEAT
                                              SCENE
 1  [c1][c2][c3][c4][c5][c6][c7][c8]          [>]
 2  [  ][  ][  ][  ][  ][  ][  ][  ]          [>]
 ...                                 8 rows
 8  [  ][  ][  ][  ][  ][  ][  ][  ]          [>]
 MUTE [m1][m2][m3][m4][m5][m6][m7][m8]
  o  o  o  o   o  o  o  o   o  o  o  o   o  o  o  o      step LEDs
 [1][2][3][4] [5][6][7][8] [9][10][11][12] [13][14][15][16]
```

- Grid: 8 columns = tracks 1-8, 8 rows = slots 1-8 of the shown bank. Pads 17 mm at
  19.5 mm pitch, RGB-lit.
- SCENE column: 8 buttons, one per row, RGB-lit.
- MUTE row: 8 buttons, one per track, RGB-lit.
- 16 step buttons with their LEDs: MCP23017 0x20 / 0x21. *Changed: steps 8 and 16 are on a
  third MCP23017 at 0x24, because GPA7/GPB7 are output-only; see `2026-11-16-mcp23017-key-scan-design.md`.*
- Top strip: OLED (SSD1309 128x64, the 2.42" class), encoder TRACK (enc 1), encoder VALUE
  (enc 2), PLAY (RGB-lit), BEAT (RGB, in the chain).
- Rear: CV 1-4, GATE 1-4, MIDI IN (reserved), MIDI OUT, USB.
- Enclosure in the model: 280 x 270 mm, top plate tilted from 20 mm (front) to 45 mm (rear).

## Hardware interface

| Part | Connection |
| --- | --- |
| TCA8418 key scanner | *Changed: not on the board.* The keys are scanned by MCP23017 U801 (0x22) and U802 (0x23) on I2C0, from Core 0; steps 8 and 16 are on MCP23017 U703 (0x24). See `2026-11-16-mcp23017-key-scan-design.md`. Original: I2C0 (GPIO 8/9), address 0x34, shared with MCP23017 0x20/0x21. 3.3 V supply (the part is not 5 V tolerant). No INT line (no GPIO left); polled. |
| Key matrix | 8 rows x 10 columns = 80 keys, the chip's maximum. Logical map: pads on rows 1-8 x columns 1-8 (row = slot, column = track); SCENE on column 9; MUTE on column 10, one mute per row. One diode per key, oriented for the TCA8418's scan direction, so several held keys never produce ghost presses. |
| I2C bus | External pull-ups (2.2-4.7 kOhm, sized on the measured bus capacitance) instead of the RP2040's internal ones; 400 kHz. |
| RGB LEDs | 82 x SK6812MINI-E (RGB, 24-bit variant; 64 pads, 8 scene, 8 mute, PLAY, BEAT) in one chain. Data from GPIO 27 via a PIO state machine, through a 74AHCT1G125 powered from 5 V with OE tied low, a pull-down on its input so the chain sees a steady low until the PIO runs, and a 330-470 Ohm series resistor at the first LED. 100 nF per LED, a bulk capacitor at the chain input, 5 V and ground run as a bus rather than through the LEDs. |
| Power | The LEDs take 5 V from VBUS, so the device is powered through its USB socket only. See "Power". |
| PLAY button | GPIO 28 to GND, internal pull-up, 20 ms firmware debounce. |
| Pico on-board LED (GPIO 25) | No longer the panel's beat light (GPIO 25 is not on the Pico's header); it may keep blinking on the board. |
| Everything else | Unchanged (ARCHITECTURE.md, "Hardware Interfaces"). |

After this, all 26 header GPIOs are taken; further controls go on I2C.

Key and LED positions are mapped to grid coordinates by two tables in firmware, so the
board wiring order is free.

**Input latency.** *Changed: the bench question below is answered by the datasheet: the
TCA8418 scans every 25 ms and reports a press 25-40 ms late with its debounce on or off, so
the board uses the fallback. Debounce is 5 ms in firmware; see `2026-11-16-mcp23017-key-scan-design.md`.* Requirement: press to event in firmware within 15 ms, for pads, SCENE
and MUTE alike. The TCA8418's built-in debounce is long (tens of milliseconds by the
reviewers' reading of the datasheet; to verify), so it is disabled and the firmware
debounces for 5-10 ms. If the bench shows the chip cannot meet 15 ms, the fallback is two
more MCP23017s (0x22, 0x23) scanning the same diode matrix from Core 0.

**Power.** The device must run from a laptop's USB port (decided by the user), so the whole
device stays within 500 mA at 5 V. The firmware has no USB stack, so the device never
enumerates and never asks the host for 500 mA; most laptop ports supply it anyway, but USB
allows a host to hold an unconfigured device to 100 mA. The bench check runs on real
laptops; if one limits the current, the fallback is a minimal USB device on Core 0 that only
enumerates and declares 500 mA (no MIDI over USB, so the reason USB was removed still holds)
(A). Budget: about 200 mA for the RGB chain, the rest for the Pico, OLED,
DACs, expanders and the 16 step LEDs. The light levels below are chosen so that the
busiest designed frame stays within the RGB budget without scaling. On top of that the
frame builder enforces the budget: it estimates the dynamic current from the channel
values (per-channel current of the chosen part, measured on the bench; 12 mA nominal for
the SK6812MINI-E, 20 mA as the conservative figure until measured) and scales only that
part: `scale = min(1, (budget - idle_total) / dynamic)`, where `idle_total` is the
chain's measured idle current (~1 mA per LED nominal).

## Firmware

### Mapping and routing

Cell (track `c`, row `r`) in shown bank `b` is pattern location `L = b * 8 + r` for track
`c`: its steps are `patterns[L].steps[c]` and its length `patterns[L].track_length[c]`.
A cell **has a clip** when one of its steps within that length has `STEP_FLAG_ACTIVE`.
The pattern and song format do not change.

**Routing belongs to the track, not to the clip** (A). Today every location carries its own
`tracks[c]` (MIDI channel, drum note, output, CV channel, gate length, muted), so a launch
would silently re-route a track. From now on a column is one instrument: the track-config
screen writes the edited config to all 32 locations of that track, so launches never change
routing and the save format stays. Older saves whose locations differ play as before until
that track's config is edited. `tracks[c].muted` is the stored track mute; the MUTE buttons
keep using the live mute mask; a track is audible when neither is set.

### Session view

A new home view replaces `UI_PERFORMANCE`. Author's calls are marked (A).

**Focus** is a pair (track, location) owned by the Session view, separate from
`active_pattern`. The step row shows and edits the focused cell. Entering the pattern view
or the step editor from the Session view sets their target to the focus; those screens keep
that target until they are left, whatever happens on the grid.

- **Pad (c, r):** queue track `c` to location `L` and move the focus there. Pressing the pad
  of a location that is already queued for that track cancels the queue. Pressing the pad
  the track already plays, with no queue pending, changes nothing in playback (no restart)
  (A). An empty cell is launched like any other, so the track goes silent from the launch:
  this is how a single track is stopped (A).
- **SCENE r:** if any track has a queue to row `r` of the shown bank, cancel all queues (A).
  Otherwise launch the row in one critical section (as `transport_queue_pattern` does today):
  every track whose current location is not `L` is queued to it, empty cells included, and a
  track already playing `L` keeps playing and loses any queue it had (A). A fully empty row
  therefore stops all tracks at the next launch point.
- **MUTE c:** toggle the track's live mute at once (existing `seq_toggle_track_mute`).
- **Steps 1-16:** a press toggles `STEP_FLAG_ACTIVE` of the focused cell's step **on
  release**, unless it became a long press; a long press opens the step editor (note,
  velocity, accent, slide, tie) for that step without toggling it. Edits go through the
  existing refresh path (`sequencer_refresh_track_playback`), so they are heard at once even
  on a location that is only queued. The step editor returns to the screen it was opened
  from. Steps beyond the cell's length are shown dark and their buttons do nothing (A).
- **PLAY:** start when stopped, stop when playing (existing semantics: Stop cancels pending
  queues and keeps the playing locations).
- **TRACK encoder:** rotate moves the focus to the previous/next track, onto the location
  that track plays (its queued one if a queue is pending) (A); press opens the pattern view
  on the focus; long press steps the shown bank A -> B -> C -> D -> A (the focus stays where
  it is).
- **VALUE encoder:** rotate changes tempo (as now); press cycles the launch quantization;
  long press opens the existing pattern-select screen, the hub to length, track config,
  song edit and save/load.
- **Both encoder buttons held:** Panic (existing).

**Global controls.** PLAY, pads, SCENE and MUTE are handled in `ui_update` before the
per-screen dispatch, outside any held lock, in every screen (A). Outside the Session view a
pad only launches (no focus change). The encoder short press no longer starts/stops the
transport in any screen; PLAY is the only start/stop (A). In Song mode pad and scene
launches are rejected as manual queues are today, a pad press still moves the focus, and the
grid shows the song's playback.

**Flash save/load** stalls Core 0 and Core 1. Afterwards the firmware drains the TCA8418
FIFO, clears its overflow flag and treats every key as released, so nothing pressed during
the stall becomes a command (A). *Changed: the scanner has no FIFO. A resync does no I2C and
takes every key as held until it has been seen up for 5 ms, so nothing pressed during the
stall becomes a command; see `2026-11-16-mcp23017-key-scan-design.md`.*

### Launch quantization

A global setting, changed with VALUE press and shown on the OLED. Stored in `g_seq`, written
by Core 0 under the playback lock, read by the ISR. Changing it re-targets queues already
pending. Options:

- `TRACK LOOP` — at the end of that track's loop (today's behavior);
- `1 BAR` — at a global step that is a multiple of 16;
- `1 BEAT` — at a global step that is a multiple of 4;
- `NEXT STEP` — at the next step.

The global step is `pulse_count / PULSES_PER_STEP`, which in Pattern mode counts from Start
and is never reset by launches (`current_step` is that value mod 16). Today queues are
applied only inside the track's loop-end branch of the timer ISR; that is not enough. The
check moves to every step boundary in Pattern mode: for each track with a queue, decide
"due" by the mode (`TRACK LOOP` keeps the loop-end test), and when due switch the location,
reload the prepared row and play the new step in the same step. O(tracks), no new locks.
Song mode keeps its own bar logic.

**Where a launched pattern starts** (A): it plays as if it had started at the last bar
line. At a bar line that is step 1; launched on beat 3 with `1 BEAT` it starts at step 9 of a
16-step pattern, so the groove never shifts. Formally the new track step is
`(global_step mod 16) mod length`. Tracks not launched keep their own phase. This rule is for
`1 BAR`, `1 BEAT` and `NEXT STEP`; `TRACK LOOP` keeps today's behavior and starts the new
pattern at step 1.

Default: `1 BAR`; `TRACK LOOP` stays as an option (decided by the user). Existing tests
that assume today's rule set `TRACK LOOP` explicitly.

### Lights

Track colors are 8 fixed hues (red, orange, yellow, green, cyan, blue, violet, pink).
Brightness levels `dim`, `half` and `full` are fixed PWM codes well below 255, set on the
bench so that the busiest designed frame fits the power budget. Blinks run on 8th notes and
the slow blink on half notes, both taken from the transport position (not a free-running
timer); while stopped they use a 120 BPM clock.

**Pad**, first matching rule wins:

| State | Light |
| --- | --- |
| queued, has a clip | track hue, blinking full / off |
| queued, empty | dim white, blinking |
| current, playing, has a clip | track hue, full |
| current, playing, empty (track stopped) | dim white |
| current, transport stopped, has a clip | track hue, half |
| has a clip | track hue, dim |
| otherwise | off |

The focused cell has no light of its own: it is shown by the step row and the OLED (A).

**SCENE r** (shown bank), first matching rule wins: blinking green while any track is
queued to this row; full green when all 8 tracks play this row; dim green when some do;
dim white when the row has a clip; otherwise off.

**MUTE c:** base light is track hue `half` when audible, off when muted (live or stored).
The focused track is `full` when audible and dim white when muted. When the track plays a
location outside the shown bank, the light blinks slowly; when the track's queue points
outside the shown bank, it blinks on 8th notes instead, so a pending launch is never hidden.

**PLAY:** green while playing, off when stopped. **BEAT:** white flash on each quarter
note, brighter on the bar line, while playing.

**Step LEDs** (single color, unchanged hardware): in the Session view, the active steps of
the focused cell within its length; when the focused cell is playing, its playhead
(`current_track_step` of that track) blinks. Other screens keep their own masks.

Core 0 keeps a 64-bit "has a clip" mask per bank, updated when steps or lengths are edited
and on load, so frames never scan the pattern data. For each frame (~30 fps) it copies the
transport fields it needs (current and queued locations, mutes, transport position) in one
short critical section under the shared-state lock, builds the 82-LED frame outside any
lock into an SRAM buffer, and starts a DMA transfer to the PIO; it never waits for the
transfer with interrupts off. Core 1 never touches the LEDs or I2C.

### Input servicing

`ui_update` services the TCA8418 on every pass of the UI loop (not once per frame): read the
event count, then all pending events, then check the overflow flag (enable both overflow
bits, as the datasheet's errata requires). On overflow: drain, clear, and treat all keys as
released. Every I2C transaction has a timeout and a checked result; a failed read is ignored,
never blocks the UI. Reading the MCP23017 ports as one two-byte transfer halves today's bus
time. *Changed: `ui_update` runs one step of the key scan on every pass (a configuration
check, then columns 0-9), with no event FIFO and no overflow; see `2026-11-16-mcp23017-key-scan-design.md`.*

### OLED

Session screen, three lines and a bar indicator: bank and the row all tracks play (or
`MIXED`); focused track, its location and length; tempo and launch quantization. The bar
indicator shows the global step within the bar.

### Storage

No change to the save format. Launch quantization and the shown bank are not saved (A).

## Testing

Host tests (fakes as in `test/fakes`):
- cell <-> location mapping and "has a clip" (including steps beyond the length);
- track config edits apply to all 32 locations; launches never change routing;
- pad launch, cancel, playing-pad no-op, empty-cell launch;
- scene launch (atomic), cancel, a row that partly plays already, a fully empty row;
- every quantization mode in the real timer/transport code with lengths 5, 12 and 16:
  due point, start step of the launched pattern, Stop with pending queues, Song mode
  rejection, mode change while a queue is pending;
- the LED frame builder (a pure function of the state) for every rule of the light tables,
  their priorities and the current budget;
- TCA8418 event decoding over a fake I2C, a 10-key burst, overflow and the post-save drain;
  PLAY debounce; step press-versus-long-press. *Changed: the tests run the MCP23017 scanner
  over a fake key matrix instead; see `2026-11-16-mcp23017-key-scan-design.md`, "Tests".*

Bench (HARDWARE_STATUS.md): press-to-event latency; several pads held at once without ghost
presses; LED current with the busiest frame and with a full grid; total USB current and
inrush, powered from at least two different laptops; power-up with no flash of the LED chain; I2C rise times with the chosen pull-ups;
CV/Gate noise with LEDs animating; GPIO 7 jitter and wire-level MIDI Clock unchanged while
scenes launch, the LEDs animate, the key matrix is scanned and the OLED refreshes. *Changed:
the bench list is in `2026-11-16-mcp23017-key-scan-design.md`, "Bench".*

## Documentation

README features and "Live Controls", ARCHITECTURE.md (hardware table: GPIO 27, GPIO 28,
I2C 0x34 (changed: 0x22, 0x23 and 0x24), pull-ups, power; launch quantization and routing in "Playback Rules"),
HARDWARE_STATUS.md, JITTER_MEASUREMENT.md (new load cases).

## Left to the builder

Exact pad, key and LED parts; the matrix wiring order and diode orientation per the
TCA8418 datasheet (changed: the board's own wiring, PR #10); PIO program and DMA details; the OLED screen layout within the three
lines above; the PWM codes for `dim` / `half` / `full`; module and function names; how the
existing performance-view code is retired or reused.
