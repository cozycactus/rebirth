# RP2040 Pattern Sequencer Architecture

## Execution and Locking

| Context | Work |
| --- | --- |
| Core 1 timer IRQ | 96 PPQN timing, playheads, note durations, MIDI production, CV/gate output |
| Core 0 main loop | Panel I2C polling (TCA8418 keys, MCP23017 step buttons and LEDs), PLAY and encoders, edits, transport commands and launches; per ~33 ms frame: one transport snapshot, the RGB frame build and its LED DMA start, the OLED; stopped-only storage |
| Core 0 UART IRQ | Drain pending MIDI bytes into UART0 |

The playback lock serializes the complete timer handler with Start, Stop,
Continue, mode changes, selection and Panic. Stop/Panic return after any
in-progress step has finished and its outputs have been cleared. Bytes already
in the UART hardware still need time to reach the receiving instrument.

Lock order is **playback -> shared state or MIDI**. UI edits take the shared lock,
release it, then refresh playback data. Transport commands and storage operations
are Core 0 APIs. The timer uses internal stop helpers under the playback lock.

What can delay the timer: Core 0 sections that hold the playback lock, i.e.
transport commands, pad/scene launches, Panic and the per-frame transport
snapshot (one short copy per frame, never one per UI pass); shared-lock
sections the timer waits on; and bus or flash contention from Core 0, such as
the LED DMA and Core 0 code running from flash through the XIP cache. The
snapshot, the only one of these that runs about 30 times a second, runs from
RAM together with the lock helpers it calls (`seq_lock`, `seq_unlock`, the
playback lock helpers), so its hold is a fixed short copy that never waits on
an XIP cache miss; the user-triggered sections run from flash. Panel I2C
polling, the LED frame build, display SPI, pattern conversion and flash
writes do not run under the playback lock. A dual-core split alone does not
establish physical jitter.

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
  Under `BAR`, `BEAT` and `STEP`, neither shortening nor extending a clip
  applies a pending queue; under `TRACK LOOP`, shortening below the playhead
  counts as the loop end and does apply it. The quantization is not saved.
- A pad or scene press is decided and written in one critical section
  (`transport_launch()`, playback lock then shared state), so the timer never
  applies a queue between the decision and the write.
- Routing (`track_config_t`: type, channel, drum note, output mode, CV channel,
  gate length) belongs to the track: `seq_set_track_config()` writes it to all
  32 locations and refreshes that track's prepared rows. Older saves whose
  locations differ play as before until that track's config is edited.
- `muted` in `track_config_t` is a stored mute per location (from older saves);
  routing edits leave it as it is in each location. A track is audible when
  neither the live mute nor the stored mute of the location it plays is set;
  MUTE, the RGB lights, the selector's step LEDs 1-8 and the OLED mute lines
  all show a track as muted by this rule (`seq_get_track_mutes()`).
  MUTE on a muted track clears the live bit and the stored mute of every
  location, so it is undone with one press.
- Selection while stopped is immediate and survives Start.
- Stop cancels pending queues and retains the current playing locations.
- Manual queues are rejected in Song mode. Song entries apply on 16-step bar
  boundaries with repeat counts. Only changed tracks restart their loops.
- Empty/all-skipped songs stay stopped. Trailing skipped rows respect Stop mode.
- The editor can address a different location from playback. Entering the
  pattern view or the step editor from the Session view targets the Session
  focus (track, location); those screens keep that target until they are left.
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
| I2C0 bus | 8 SDA, 9 SCL; 400 kHz; external pull-ups (2.2-4.7 kOhm), internal ones disabled |
| MCP23017 buttons / LEDs | I2C 0x20 / 0x21 |
| TCA8418 panel key scanner | I2C 0x34; 8 x 10 matrix, 80 keys; 3.3 V only; no INT line, polled |
| Two MCP4822 DACs | 10 SCK, 11 MOSI, 12/13 CS; SPI1 |
| Gates | 14-17; 3.3 V logic |
| Encoder 1 | 18, 19, switch 20 |
| Encoder 2 | 21, 22, switch 26 |
| Tempo LED | 25 |
| RGB LED chain | 27; 82 x SK6812MINI-E via PIO, level-shifted to 5 V |
| PLAY button | 28 to GND, internal pull-up, 20 ms debounce |

There are **two** encoders. Firmware uses MCP23017 expanders, not shift-register
button drivers. The Pico exposes 26 general-purpose GPIOs; this assignment uses
all of them (GPIO 27 LED data, GPIO 28 PLAY).

Every I2C transaction, initialisation included, uses the SDK timeout API (2000 us)
and checks the exact byte count, so a missing or stuck chip never hangs the UI,
but each failed transaction can delay one UI pass by up to its 2 ms timeout. Any
I2C error marks that chip unconfigured: its configuration is written again
before the next read, and an attempt that fails is retried after 100 ms, not on
every pass, so a dead bus costs about one timeout per chip per 100 ms. PLAY is a
plain GPIO and is sampled first on every pass, whatever the bus does. About once
a second each chip's configuration is read back (TCA8418 CFG, MCP23017 IPOL,
step-LED IODIR), so a chip that reset (brown-out, ESD) is reconfigured; the
button expander's ports then become the new baseline without events. A reset
button expander's idle lines read as pressed, so its IPOL is also read back
before a sample with three or more keys going down at once is accepted. A failed
button read keeps the last accepted state; the step LED mask is cached only
after a successful write. Core 1 never touches I2C or the LEDs.

A TCA8418 FIFO overflow, or an event read that fails after the event count was
read, drops the whole batch: the key state is forgotten and the FIFO drained,
with nothing reported. A flash save stalls both cores (only a save uses the
multicore lockout) and a load stalls only Core 0; afterwards the inputs are
resynced: `panel_input_resync()` drops every queued panel event (PLAY
included) and drains the FIFO, re-initialising the chip only if that fails;
`mcp23017_resync()` and `encoder_resync()` take the buttons as they are now,
with no press, release or long press from the stall; and the UI drops the
step gestures in progress. When a panic is released, the panel and the step
buttons are resynced the same way. After a resync or an overflow on a chip
that kept its configuration (the drain worked), every key reads as up: such a
chip reports a key held across the drain only by its release, which then
changes nothing, and a key whose release was drained presses normally again.
A re-initialised chip (failed drain, incomplete read, chip reset) may report a
held key again, so a key that was down then stays suppressed until a release
has held for 8 ms. PLAY that was down stays suppressed until a release has
held for 20 ms. A bounce or a re-reported press never becomes a command.

### Power

The LEDs take 5 V from VBUS, so the device is powered through its USB socket
only and must stay within 500 mA at 5 V (RGB chain budget 200 mA). The firmware
has no USB stack and never enumerates, so a host may hold it to 100 mA; this is
a bench check on real laptops. The frame builder scales the RGB brightness to
keep the estimated current within the budget.

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
