# Hardware Status

Last updated: 2026-05-24

## Verified In This Repo

- Host unit tests build and pass.
- RP2040 firmware target builds with Pico SDK 2.2.0.
- DIN MIDI UART output code is compiled into the firmware target.
- CV/Gate, OLED, encoder, MCP23017, step LED, flash storage, and UI modules compile into the firmware target.
- GPIO 7 jitter probe and ISR jitter stats compile into the firmware target.
- USB MIDI scaffolding has been removed by design.
- DIN MIDI output uses a UART TX interrupt queue instead of blocking serial writes in the sequencer timer ISR.
- Pattern-mode playback uses prepared step events instead of copying full pattern data in the normal timer step path.

## Needs Bench Verification

- Timer ISR jitter under dense MIDI/CV output using GPIO 7 and on-device stats.
- Compare jitter stats before/after UART TX queue and prepared playback events on physical hardware.
- Gate alignment across all four CV/Gate channels.
- DIN MIDI electrical output and byte timing.
- OLED init sequence and contrast on the actual SSD1309 panel.
- MCP23017 step button and LED wiring.
- Flash save/load behavior while playback is stopped and while playback is running.

## Not Implemented Yet

- External MIDI clock sync input.
- Swing timing in playback. Pattern data stores the swing value, but the sequencer still runs straight timing.
