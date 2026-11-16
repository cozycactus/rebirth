# Hardware Status

Last updated: 2026-11-16

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
- UI tests cover Panic releases, selection/edit focus, mute/config presses and
  stopped-only storage. All six OLED views render with the production framebuffer.
- Storage tests cover CRC32, validated version-4 imports, and interrupted writes.
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

## Not Implemented Yet

- External MIDI clock sync input.
- Swing timing in playback. Pattern data stores the swing value, but the sequencer still runs straight timing.
- Preservation of the previous save across power loss (storage is a single region).
