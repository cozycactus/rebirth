# Hardware Status

Last updated: 2026-09-15

## Verified In This Repo

- Host unit tests build and pass.
- RP2040 firmware target builds with Pico SDK 2.2.0.
- DIN MIDI UART output code is compiled into the firmware target.
- CV/Gate, OLED, encoder, MCP23017, step LED, flash storage, and UI modules compile into the firmware target.
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

## Not Implemented Yet

- External MIDI clock sync input.
- Swing timing in playback. Pattern data stores the swing value, but the sequencer still runs straight timing.
- Preservation of the previous save across power loss (storage is a single region).
