# RP2040 Pattern Sequencer Architecture

## Execution and Locking

| Context | Work |
| --- | --- |
| Core 1 timer IRQ | 96 PPQN timing, playheads, note durations, MIDI production, CV/gate output |
| Core 0 main loop | Buttons, encoders, OLED, edits, transport commands, stopped-only storage |
| Core 0 UART IRQ | Drain pending MIDI bytes into UART0 |

The playback lock serializes the complete timer handler with Start, Stop,
Continue, mode changes, selection and Panic. Stop/Panic return after any
in-progress step has finished and its outputs have been cleared. Bytes already
in the UART hardware still need time to reach the receiving instrument.

Lock order is **playback -> shared state or MIDI**. UI edits take the shared lock,
release it, then refresh playback data. Transport commands and storage operations
are Core 0 APIs. The timer uses internal stop helpers under the playback lock.

Control operations can delay the timer. Display SPI, pattern conversion and
flash writes do not run under the playback lock. A dual-core split alone does
not establish physical jitter.

## Prepared Patterns

All 32 locations have a prepared row for each of eight tracks. Core 0 builds
these at initialization and after edits. A revision and row are published
together under the shared lock. Playback copies a row only when its location or
revision changes, at a step boundary. Song mode uses the same cache.

The event cache occupies 28,928 bytes, plus revision counters and active rows.
Pattern and song data remain the editable source of truth. Code editing a
pattern must call `sequencer_refresh_track_playback()` after releasing the shared
lock, including for locations not currently playing.

## Playback Rules

- Each track has its own pattern, queue and 1-16 step playhead.
- Queued pattern changes apply on a launch quantization (default `1 BAR`):
  `BAR` and `BEAT` wait for a global step that is a multiple of 16 or 4, `STEP`
  applies on the next step, `TRACK LOOP` waits for that track's loop end (the
  old rule). With `BAR`, `BEAT` and `STEP` the new clip joins in phase: its
  playhead starts at `(global step % 16) % new length`, so it lines up with the
  bar. `TRACK LOOP` always starts the new clip at step 1. Queuing the location
  that is already playing just clears the queue and keeps the playhead.
  Shortening below the next step wraps to zero; extending preserves position.
  Under `BAR`, `BEAT` and `STEP` neither applies a pending queue; under
  `TRACK LOOP`, shortening below the playhead counts as the loop end and does
  apply it. The quantization is not saved.
- Routing (`track_config_t`: channel, drum note, output mode, CV channel, gate
  length) belongs to the track: `seq_set_track_config()` writes it to all 32
  locations and refreshes that track's prepared rows. Older saves whose
  locations differ play as before until that track's config is edited.
- Selection while stopped is immediate and survives Start.
- Stop cancels pending queues and retains the current playing locations.
- Manual queues are rejected in Song mode. Song entries apply on 16-step bar
  boundaries with repeat counts. Only changed tracks restart their loops.
- Empty/all-skipped songs stay stopped. Trailing skipped rows respect Stop mode.
- The editor can address a different location from playback. Entering it from
  Performance selects the currently controlled playback location.
- Mutes release active notes and gates on the next internal pulse. Unmute takes
  effect at the next active step.
- Gate length also determines MIDI note duration. Slide/tie are **outgoing** step
  flags: slide connects to the next pitch, tie holds the same pitch. They sustain
  until the next step instead of ending at the configured gate length.
  Rests, mutes and destination changes release the previous output.
- Slide provides MIDI legato and a held gate. CV changes by one DAC write;
  there is no programmed analog glide curve.
- Swing is stored but not implemented in playback.

## Timing and MIDI

The internal timer runs at 96 PPQN. MIDI Clock runs at **24 PPQN**, one message
every four internal pulses. At 120 BPM the integer timer period is 5,208 us; the
nominal MIDI Clock interval is approximately 20.833 ms.
[MIDI Timing Clock reference](https://docs.circuitpython.org/projects/midi/en/latest/api.html#adafruit_midi.timing_clock.TimingClock).

At a step boundary, the engine stages MIDI and CV/gate changes, commits gate
GPIO masks, then submits the MIDI batch. Gate edges are grouped; DAC writes and
DIN messages are sequential. This does not make all outputs simultaneous.

The 256-byte UART queue accepts complete messages or complete step batches.
FIFO checks, dequeues, writes and TX interrupt changes share one lock. Overflow
stops playback and sends All Sound Off / All Notes Off instead of continuing
with a truncated message. Panic discards software-queued output; hardware FIFO
bytes precede the cleanup messages.

After an overrun the timer re-arms a future deadline, preventing a past alarm
from stalling until the 32-bit counter wraps. It emits no catch-up burst.
Jitter snapshots are coherent; tempo changes do not count the new period as
timing noise. See [JITTER_MEASUREMENT.md](JITTER_MEASUREMENT.md).

## Storage

The last 128 KiB of the Pico's 2 MiB flash holds patterns and song data.
Save/load require stopped transport. Save drains MIDI before suspending XIP and
programs the validity header last. Version 5 adds CRC32. Load validates indices,
lengths and configuration before replacing RAM. Version 4 has the same payload
layout and is accepted after validation without a checksum.

This is one save region. Power loss during erase/program may lose the previous
save; an incomplete new save is rejected. Preserving the old save across power
loss needs a future two-slot/journal design.

## Hardware Interfaces

| Function | GPIO / Interface |
| --- | --- |
| DIN MIDI TX, reserved RX | 0, 1; UART0, 31,250 baud, 8N1 |
| SSD1309 OLED | 2 SCK, 3 MOSI, 4 CS, 5 DC, 6 reset; SPI0 |
| Jitter probe | 7 |
| MCP23017 buttons / LEDs | 8 SDA, 9 SCL; addresses 0x20 / 0x21 |
| Two MCP4822 DACs | 10 SCK, 11 MOSI, 12/13 CS; SPI1 |
| Gates | 14-17; 3.3 V logic |
| Encoder 1 | 18, 19, switch 20 |
| Encoder 2 | 21, 22, switch 26 |
| Tempo LED | 25 |

There are **two** encoders. Firmware uses MCP23017 expanders, not shift-register
button drivers. The Pico exposes 26 general-purpose GPIOs; this assignment uses
24, leaving GPIO 27 and 28.

### MIDI Circuit

Use a buffered MIDI OUT circuit conforming to the 3.3 V variant of
[MIDI CA-033](https://www.midi.org/wp-content/uploads/wpforo/default_attachments/1709416667-ca33-MIDI-10-Electrical-Specification-Update.pdf).
The earlier pair of 220-ohm resistors was a 5 V assumption, not the 3.3 V
reference circuit. CA-033 specifies different resistors and driver current
requirements for 3.3 V. The actual board circuit has not been validated here.

### CV Circuit

The MCP4822's internal reference is **2.048 V**. Default gain remains 1x:
nominal range 0-2.048 V, with code 4095 just below full scale. MIDI note 24 is
0 V, note 36 is 1 V, note 48 is 2 V; higher notes saturate. Absolute pitch is
rounded to the nearest DAC code, avoiding cumulative semitone rounding error.

`-DREBIRTH_CV_DAC_GAIN=2` selects nominal 0-4.096 V. It needs sufficient DAC
supply/headroom and compatible SPI levels, typically 5 V power with proper
3.3-to-5 V logic buffering. A 3.3 V supply cannot produce 4.096 V, and 3.3 V
GPIO must not be assumed to meet the input-high specification of a 5 V DAC.
[MCP4822 datasheet](https://ww1.microchip.com/downloads/en/devicedoc/20002249b.pdf).

SPI1 runs at 1 MHz: each 16-bit DAC word takes 16 us on the bus, followed by
analog settling. Tie LDAC appropriately for the intended update behavior.
Use one active source track per CV/gate pair. Analog calibration, output buffers
and gate-level compatibility require bench verification.

## Verification Boundaries

Host tests execute the production timer, transport, MIDI queue, storage, UI and
OLED renderer against fake peripherals. Unix tests exercise concurrent Stop/Panic
during a pending gate commit. DAC command tests cover both gains. These tests do
not measure RP2040 timing or analog voltages.

Inspect the linked ELF/map for memory usage. Measure GPIO 7 and physical outputs
under load before claiming an ISR budget or timing margin. See
[HARDWARE_STATUS.md](HARDWARE_STATUS.md) for remaining checks.
