# Hardware Status

Last updated: 2026-11-17 (MIDI keyboard input added)

## Migrating An Existing Build

The Session panel changes two things an existing board must provide even
without the new grid, and the key scanner changes two more:

- **PLAY on GPIO 28.** PLAY is now the only start/stop; the encoder presses no
  longer start or stop the transport. Wire a momentary switch from GPIO 28 to
  GND (the internal pull-up is enabled, 20 ms firmware debounce).
- **External I2C pull-ups.** The RP2040's internal pull-ups on SDA (GPIO 8) and
  SCL (GPIO 9) are now disabled. Fit 2.2-4.7 kOhm pull-ups from each line to
  3.3 V, or the step buttons and step LEDs stop working.
- **Key scanner.** The TCA8418 (0x34) is replaced by two MCP23017, U801 at 0x22
  and U802 at 0x23, scanning the diode matrix (pin map in
  `hardware/schematic/README.md`); the TCA8418 is no longer read.
- **Steps 8 and 16 on U703 (0x24), GPA0 and GPA1.** They were on GPA7 and GPB7
  of 0x20, which are output-only on current MCP23017 datasheets. An existing
  board with those two buttons on 0x20 loses steps 8 and 16: the firmware now
  drives those pins low (a button to GND there is harmless). Add U703 at 0x24,
  or live with 14 steps; a missing U703 costs only those two.

Without U801 and U802 the panel keys simply report nothing (the configuration is
retried every 100 ms); without the LED chain GPIO 27 drives nothing.

## Verified In This Repo

- Host unit tests build and pass.
- RP2040 firmware target builds with Pico SDK 2.2.0.
- DIN MIDI UART output code is compiled into the firmware target.
- CV/Gate, OLED, encoder, MCP23017, step LED, flash storage, and UI modules compile into the firmware target,
  as do the Session view (`session.c`), the LED frame builder (`led_frame.c`), the RGB LED chain
  (`rgb_leds.c`, PIO and DMA), the key scan driver (`key_scan.c`) and the panel input (`panel_input.c`).
- GPIO 7 jitter probe and ISR jitter stats compile into the firmware target.
- USB MIDI scaffolding has been removed by design.
- DIN MIDI output uses a UART TX interrupt queue instead of blocking serial writes in the sequencer timer ISR.
- Pattern and Song playback use a versioned prepared-row cache built on Core 0.
- Host tests execute the timer/transport code, including MIDI Clock rate,
  independent loops, live edits, note durations and overflow recovery.
- Unix two-thread tests cover Stop/Panic during a pending gate commit.
- UI tests cover Panic releases, selection/edit focus, mute/config presses,
  stopped-only storage, and hold-step editing in the pattern view on SYNTH and
  DRUM cells beside the kit lane rules on kit cells. The six screens (Session
  view, pattern view, step editor, pattern select, track config, Song editor)
  and the four kit screens (Session lane strip, kit pattern view, kit step
  editor, kit config) render with the production framebuffer.
- Storage tests cover CRC32, validated version-4, version-5 and version-6 loads,
  and interrupted writes.
- Expander tests cover failed MCP23017 reads (state kept, chip reconfigured),
  failed step LED writes (direction and mask rewritten), chip resets found by
  config readback, retries limited to one per 100 ms on a dead bus, step
  button and encoder resync after a flash stall, and internal pulls off on
  GPIO 8/9, and a reset button expander (0x20 or 0x24) whose idle lines read
  as pressed, reads only when a sample is due, steps 8 and 16 from 0x24, a
  missing 0x24, and a long press only at a fresh good sample (a 499 ms hold
  never long-presses). Key scan tests run the driver over a fake key matrix:
  register values, one column low at every row read, a fault at every
  transfer, a reset of either chip before any transfer or inside a read. Panel
  input tests cover the same for the scanner, plus a chord of all 80 keys
  without ghosts, keys or PLAY held through a resync, keys released during a
  stall, a tap lost during an outage, a chip reset after the scan's check that
  would settle a tap, a chatter or the end of a suppression, and press-to-event
  in a host loop model that counts wire bytes only (no START/STOP or SDK call
  overhead; not a device measurement): within 15 ms for one key (11.69 ms
  worst), a ten-key chord (12.86 ms) and 50 presses a second held 60 ms
  (11.77 ms); under bursts of presses far beyond playing (a press every
  0.37-2 ms) up to 15.59 ms, bounded at 25 ms with no press lost. Storage
  tests cover the input resync after save and load.
- DAC command tests cover the 2.048 V reference, both gains and octave scaling.
- Kit track (host tests and both firmware builds, DAC gain 1 and 2, with no
  warnings): lane helpers, prepared kit rows, the pulse order, velocity levels,
  flams at exactly `flam_pulses`, note-offs as Note On velocity 0, releases on
  mute, Stop, Panic and type change, the two-kit limit, running status and Clock
  ahead of note bytes over a fake UART (shift and holding register, no FIFO),
  the flam cap (a flam's second hits are queued only while the 1,024-byte TX
  queue then holds at most 319 bytes; mandatory messages may use the rest), a
  restart under two dense kits and a seeded sweep of 2,000 random restarts
  (140-300 BPM, gates 10-100 %, FLAM 1-12; peak 517 queued bytes, no overflow),
  two kits flamming every voice at gate 100 % with no second hit dropped at 140,
  170 and 200 BPM, save format v6 and loading real v4/v5 images, encoder clicks
  with detent-rate acceleration for numbers only, and the kit OLED screens. The
  sanitizer build passes too, also with `-fshort-enums`.
- RAM from `arm-none-eabi-size -A build/fw1/rp2040_sequencer.elf` after a fresh
  build: `.data` 10,780 + `.bss` 105,264 = 116,044 bytes (about 113.3 KiB) of
  the RP2040's 264 KiB; the `.heap` and the two stack sections are separate
  (2,048 bytes each). The branch base has `.data` 4,396 + `.bss` 69,876 = 74,272
  bytes (about 72.5 KiB). Before `master` was merged in, the kit branch had
  `.data` 10,188 + `.bss` 105,184 = 115,372 bytes (about 112.7 KiB); the merge
  added 592 bytes of `.data` (the per-frame `sequencer_snapshot()`, 512 bytes,
  and `seq_lock()` and `seq_unlock()`, 60 and 52, now in RAM, less the two
  16-byte veneers through which the timer ISR reached the lock helpers in
  flash) and 80 bytes of `.bss` (the hold-step state in the UI). The design
  expected about 108,000 bytes (about 105.5 KiB) with the grown flash copy
  buffer; the build is about 7.9 KiB above that and still fits. On the kit
  branch `.bss` grew by 35,308 bytes: as estimated, the kit steps and
  settings in `g_seq` (16,536 bytes), the save's copy buffer `pattern_buf`
  (14,848 to 31,232 bytes), the prepared rows and the kit runtime state, and 512
  more for the TX queue, doubled to 1,024 bytes after review. The estimate left
  out `.data`, which also holds the code that runs from RAM; it grew by 5,792
  bytes. The kit and MIDI path the timer ISR runs is in RAM (`arm-none-eabi-nm`,
  addresses 0x2000xxxx): the ISR itself grew from 2,736 to 6,092 bytes with
  `kit_release()`, `play_kit_step()` and `kit_play_flam()` inlined into it;
  `kit_take_release()` (708 bytes), `kit_voice_order` (11), the MIDI TX drain
  `midi_tx_drain_locked()` (624), the UART interrupt handler (92),
  `midi_send_bytes()` (204, plus a 208-byte constant-length clone),
  `midi_try_send_bytes_capped()` (244), `midi_send_clock()` (108),
  `midi_send_note_off()` (38), `midi_output_overflowed()` (104) and the step
  buffer's `midi_buf_clear()`, `midi_buf_flush()`, `midi_buf_note_on()` and
  `midi_buf_note_off()` (12, 28, 68, 68). Since the merge `seq_lock()` and
  `seq_unlock()` are in RAM too. The ISR's other callees stay in flash as on the
  base commit (see JITTER_MEASUREMENT.md, Session panel load case, and the bench
  item below).

- MIDI keyboard input (host tests and the firmware build): the parser, the receive ring
  with the handler reading before the TX lock and also with the ring full, the
  interrupt enables, and the keys in the UI over a fake UART (pass-through with
  capped sends, step and Step Edit writes, gaps, flash save/load, panic). The
  fake UART models a queue, not the PL011's one-byte holding register.

## Needs Bench Verification

- Timer ISR jitter under dense MIDI/CV output using GPIO 7 and on-device stats.
- Compare jitter stats before/after UART TX queue and prepared playback events on physical hardware.
- Gate alignment across all four CV/Gate channels.
- DIN MIDI electrical output and byte timing.
- OLED init sequence and contrast on the actual SSD1309 panel.
- MCP23017 step button and LED wiring.
- Flash save/load while stopped, rejection while running, restart after flash lockout.
- CV calibration and supply/logic compatibility. Default gain is 1x
  (nominal 0-2.048 V); the 2x build option requires suitable hardware.
- Wire-level MIDI Clock jitter under dense note traffic, independently of GPIO 7.

### Kit track

None of these has been run on hardware; every item is `NOT VERIFIED`. Baseline
means a build of the commit this branch is based on (3cbd4f9),
measured first with the same densest pattern load.

- `NOT VERIFIED` Note map: the 909 plays BD, SD, LT, MT, HT, RS, CP, CH, OH, CC
  and RC from the default notes (or the notes set in the config) on the right
  voice, on the configured channel.
- `NOT VERIFIED` Three velocity levels audible: a plain note, an accented note
  (or a note with AC) and an accented note with AC sound as three different
  dynamic levels, with the accent threshold of the machine between them; if not,
  VEL NORM, VEL ACC and VEL MAX are adjusted and recorded.
- `NOT VERIFIED` Running status and Note On velocity 0 are understood by the 909:
  every note stops at its gate, none sticks, none is missed.
- `NOT VERIFIED` Flams at 140-150 BPM sound as flams (FLAM 3 and 12 pulses).
- `NOT VERIFIED` GPIO 7 maximum ISR duration with two dense kits (every voice,
  every step, flams on) within +20 us of the baseline.
- `NOT VERIFIED` ISR base-commit callees still in flash: the SDK divider wrapper
  (`pulse_count % 24` and `/ 24` on every pulse), `load_prepared_track()` (every
  step), `gate_commit()`/`cv_set_note()`,
  `send_note_off_if_active()` and `prepare_note_off()` run from flash through
  the XIP cache, as on the base commit, while the kit and MIDI path runs from
  RAM. With two dense kits and Core 0 busy (OLED, LEDs, panel scans), the GPIO 7
  maximum ISR duration must stay within the +20 us budget; if XIP-cache misses
  push it over, `PICO_DIVIDER_IN_RAM=1` and `__not_in_flash_func` on those
  functions move them to RAM.
- `NOT VERIFIED` Wire-level MIDI Clock interval (GPIO 0 decoded) with two dense
  kits no worse than the baseline. By design a Clock byte may wait for the byte
  being shifted out and a complete channel message whose status is already in
  the holding register: up to four byte times, 1.28 ms, before interrupt
  latency.
- `NOT VERIFIED` Longest Core 0 interrupts-off window while playing (editing,
  launching, track-config edits, Song edits) under 100 us, Song list edits near
  the front of a long song excepted (Known Follow-Ups).
- `NOT VERIFIED` UI loop time with the per-byte UART interrupt (3,125 interrupts
  per second at full rate) shows no new input lag or missed encoder edges.
- `NOT VERIFIED` OLED kit screens (Session lane strip, kit pattern view, kit step
  editor, kit config) and the tight `FLAM DROP <n>` line of the pattern-select
  screen are legible and not clipped on the real SSD1309.
- `NOT VERIFIED` Edges per detent of the fitted encoders (`ENC_EDGES_PER_DETENT`
  is 4, EC11 type): one click moves one lane, one value or one config row.
- `NOT VERIFIED` Encoder acceleration feels right: clicks under 60 ms apart
  (`ENC_ACCEL_FAST_MS`) count x4 from the fifth and x8 from the eighth, for
  numbers only. At full speed the tempo moves 0.8 BPM per click.

### MIDI keyboard input

None of these has been run on hardware; every item is `NOT VERIFIED`. The input
circuit (J402, an H11L1 opto, GPIO 1) has not been validated.

- `NOT VERIFIED` The opto's output at the Pico pin with a keyboard connected: a
  clean 3.3 V logic level, idle high, 31,250 baud, correct polarity.
- `NOT VERIFIED` No byte is lost while the display is redrawn and while a set
  plays (`midi_in_dropped()` stays 0 with a keyboard playing dense chords; the
  ring is 64 bytes and the redraw can stall the main loop).
- `NOT VERIFIED` The receive interrupt does not fire continuously (the UI loop
  time and the GPIO 7 jitter with the keyboard idle and playing).
- `NOT VERIFIED` The receive path is in RAM in the map file: the UART interrupt
  handler and the ring's push (inlined into it).
- `NOT VERIFIED` A byte lost in the UART itself (overrun, framing) is not
  detected, as `uart_getc()` hides the error flags; it can give one wrong note.
- `NOT VERIFIED` A keyboard unplugged with a key down leaves the note sounding
  (Active Sensing is not monitored); Panic silences it.
- `NOT VERIFIED` The 320 us deadline of the receive interrupt against the
  transmit drain and the display's interrupts-off sections.

### Session panel

- Press-to-event latency, for single keys and for chords (target 15 ms;
  estimated 7-10 ms typical, 14.2 ms worst for a key alone, about 15.3 ms for a
  ten-key chord; bursts of presses far beyond playing can pass 15 ms) for pads,
  SCENE and MUTE, and the frame pass and the scan period on the device.
- Several keys held at once: no ghost presses. Each SCENE and MUTE key held:
  no other key reports.
- Row rise time after a column is released (scope on a row with a key held in
  the previous column; the estimate is about 10 us, the firmware leaves about
  67 us).
- I2C load and SDA integrity with the scan running.
- CV/Gate noise with the scan running.
- Steps 8 and 16 on U703 (0x24), and 14 working steps without it.
- LED current with the busiest frame and with a full grid.
- Total USB current and inrush, powered from at least two different laptops.
- Power-up with no flash of the LED chain.
- I2C rise times with the chosen external pull-ups.
- CV/Gate noise with the LEDs animating.
- GPIO 7 jitter and wire-level MIDI Clock unchanged while scenes launch, the
  LEDs animate, the key matrix is scanned and the OLED refreshes.
- SK6812MINI-E colour order (GRB as sent) and the chain order table in
  `rgb_leds.c` against the board wiring.
- A pad, a step button and PLAY held through a flash save/load, and through a
  panic, do nothing once released.
- PLAY debounce (20 ms) on the real switch: one press, one start or stop.
- `NOT VERIFIED` Quantised MUTE on the real panel: how the white flash of a
  waiting change (white on odd sixteenths) and the steady white of a collected
  group read against the track colours and the bank blinks, and holding one
  MUTE key while pressing another on the real matrix (both reported, the held
  key's release after the other's press).

## Known Follow-Ups

- Song list edits (`song_insert*()`, `song_remove()`, `song_move()`) near the
  front of a 256-entry song keep Core 0's interrupts off for an estimated
  130-165 us (a `memmove` under `seq_lock`; JITTER_MEASUREMENT.md).

## Not Implemented Yet

- External MIDI clock sync input. The DIN input takes notes only; Clock, Start, Stop and Continue are ignored.
- Swing timing in playback. Pattern data stores the swing value, but the sequencer still runs straight timing.
- Preservation of the previous save across power loss (storage is a single region).
