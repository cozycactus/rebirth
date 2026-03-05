# RP2040 Pattern Sequencer — Architecture

## Overview
ReBirth-style 8-track, 16-step pattern sequencer with song mode.
Outputs: MIDI (DIN + USB) and CV/Gate.
UI: OLED display + rotary encoders + buttons.

## Dual-Core Architecture

```
┌─────────────────────────────────────────────────────┐
│                    RP2040                            │
│                                                     │
│  ┌──────────────────┐   ┌────────────────────────┐  │
│  │     CORE 0       │   │       CORE 1           │  │
│  │                  │   │                        │  │
│  │  UI Engine       │   │  Sequencer Engine      │  │
│  │  ─ OLED display  │   │  ─ Timer IRQ @ 96 PPQN│  │
│  │  ─ Button scan   │   │  ─ Step advancement    │  │
│  │  ─ Encoder read  │   │  ─ MIDI TX (UART0)    │  │
│  │  ─ Menu system   │   │  ─ CV/Gate update     │  │
│  │  ─ Pattern edit  │   │  ─ Song chain logic   │  │
│  │                  │   │                        │  │
│  └────────┬─────────┘   └────────┬───────────────┘  │
│           │    Shared Memory     │                   │
│           └──────────┬───────────┘                   │
│              ┌───────┴────────┐                      │
│              │  Pattern Data  │                      │
│              │  Transport     │                      │
│              │  Song Data     │                      │
│              │  (mutex/spinlock)                     │
│              └────────────────┘                      │
└─────────────────────────────────────────────────────┘
```

## Why Dual-Core Split?

- **Core 1 (sequencer)**: Runs a hardware timer IRQ at 96 PPQN resolution.
  Jitter-free step triggering, MIDI byte output, DAC writes for CV.
  No display or I/O polling to stall timing.

- **Core 0 (UI)**: Runs the main loop — scans buttons, reads encoders,
  redraws OLED, handles menu navigation. Can take variable time
  without affecting sequencer timing.

## Hardware Design

### MIDI Output (UART0)
- GPIO 0 (TX) → 220Ω → DIN-5 pin 5
- 3.3V → 220Ω → DIN-5 pin 4
- Standard MIDI current-loop circuit
- 31250 baud, 8N1

### USB MIDI
- Native RP2040 USB via TinyUSB stack
- USB MIDI class device
- Shares same note events as DIN MIDI

### CV/Gate Outputs (4 channels)
- 2× MCP4822 (dual 12-bit SPI DAC) → 4 CV outputs
- SPI1: GPIO 10 (SCK), GPIO 11 (MOSI), GPIO 12/13 (CS0/CS1)
- Output range: 0–4.096V (1V/oct, ~3.4 octaves)
  - For wider range: add op-amp gain stage (2× → 0-8V, ~6.8 octaves)
- 4× Gate outputs: GPIO 14, 15, 16, 17 (direct 3.3V logic)
  - Buffer with transistor/MOSFET for 5V gate if needed
- Tracks 1–4: CV/Gate assignable
- Tracks 5–8: MIDI only (or reassignable)

### OLED Display
- SSD1309 128×64 (SPI for speed)
- SPI0: GPIO 2 (SCK), GPIO 3 (MOSI), GPIO 4 (CS), GPIO 5 (DC), GPIO 6 (RST)
- Full framebuffer in RAM (~1KB for 128×64 mono)

### User Input
- 4× Rotary encoders with push: GPIO 18–25 (2 pins per encoder + 1 switch)
  - Encoder A/B + switch = 3 pins × 4 = 12 pins
  - Use remaining GPIOs or I2C expander (MCP23017) for more buttons
- 16× Step buttons (optional LED matrix):
  - Via 74HC595 shift registers (directly drive LEDs)
  - Via 74HC165 shift registers (read buttons)
  - Or I2C: 2× MCP23017 = 32 GPIO

### Pin Budget (28 GPIO on Pico)

| Function       | GPIOs          | Count |
|----------------|----------------|-------|
| MIDI TX        | 0              | 1     |
| MIDI RX (sync) | 1              | 1     |
| OLED SPI0      | 2,3,4,5,6      | 5     |
| DAC SPI1       | 10,11,12,13    | 4     |
| Gate out       | 14,15,16,17    | 4     |
| Encoder 1      | 18,19,20       | 3     |
| Encoder 2      | 21,22,26       | 3     |
| I2C (expander) | 8,9            | 2     |
| Tempo LED      | 25             | 1     |
| **Total**      |                | **24**|
| **Remaining**  | 7,27,28        | **4** |

> Use MCP23017 on I2C for step buttons + LEDs (keeps GPIO count low).

## Software Modules

### 1. `sequencer.c` — Core Engine (Core 1)
- Timer-driven at 96 PPQN (pulses per quarter note)
- At each pulse: check if current pulse == step boundary (every 6 pulses for 16th notes)
- On step boundary: iterate 8 tracks, fire note-on/off, update CV/gate
- Handles swing (offset even steps by N pulses)
- Consumes pattern data from shared memory (read-only during playback)

### 2. `pattern.c` — Pattern Data
- 8 tracks × 16 steps × N patterns (32–64 patterns in flash)
- Each step: `{ uint8_t note, velocity, flags; }` (3 bytes)
- Per track: instrument type (synth/drum), MIDI channel, CV assign
- Total per pattern: 8 × 16 × 3 = 384 bytes → 64 patterns = 24KB

### 3. `song.c` — Song Mode
- Array of pattern indices with repeat counts
- Max 256 entries
- Song position pointer, advancing on pattern completion

### 4. `midi_out.c` — MIDI Output
- UART TX at 31250 baud
- Note On/Off, CC, clock (0xF8), start/stop/continue
- USB MIDI via TinyUSB (same events mirrored)

### 5. `cv_gate.c` — CV/Gate Output
- MCP4822 SPI writes (12-bit, ~1µs per update)
- Note-to-voltage lookup table (1V/octave, C0 = 0V)
- Gate high/low with configurable gate length (% of step)

### 6. `ui.c` — User Interface (Core 0)
- State machine: PATTERN_SELECT → STEP_EDIT → TRACK_CONFIG → SONG_EDIT → SETTINGS
- Encoder: navigate/select, push = confirm
- OLED pages:
  - Pattern view: 16 step indicators + current step highlight
  - Track config: MIDI ch, note range, CV assign
  - Song view: pattern chain list
  - Settings: BPM, swing, gate length

### 7. `display.c` — OLED Driver
- SSD1309 SPI driver with framebuffer
- Draw primitives: rectangles, text, step grid
- Double-buffer optional (960 bytes × 2)

## Timing Detail

At 120 BPM, 96 PPQN:
- Timer period = 60,000,000 µs / (120 × 96) = **5208 µs** (~5.2ms per pulse)
- 16th note = 6 pulses = **31.25ms**
- At 200 BPM: period = **3125 µs** — still very comfortable for RP2040

The timer IRQ on Core 1 takes ~50–100µs worst case (MIDI byte TX + DAC SPI write),
leaving >95% CPU headroom.

## BPM Range
- 20–300 BPM, adjustable in 0.1 BPM increments
- Timer period recalculated on BPM change

## Memory Budget
- Pattern storage: ~24KB (64 patterns)
- Song data: ~512 bytes
- OLED framebuffer: ~1KB
- Code + stack: ~100KB
- **Total: well within 264KB SRAM**
- Patterns can overflow to flash (2MB) for more storage

## Build System
Standard Pico SDK CMake project. Dependencies:
- pico-sdk
- tinyusb (included in pico-sdk)
- No external RTOS — bare-metal with timer IRQ
