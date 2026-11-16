# RP2040 Pattern Sequencer Architecture

## Execution and Locking

| Context | Work |
| --- | --- |
| Core 1 timer IRQ | 96 PPQN timing, playheads, note durations, MIDI production, CV/gate output |
| Core 0 main loop | Panel I2C polling (TCA8418 keys, MCP23017 step buttons and LEDs), PLAY and encoders, edits, transport commands and launches; per ~33 ms frame: one transport snapshot, the RGB frame build and its LED DMA start, the OLED; stopped-only storage |
| Core 0 UART IRQ | Drain pending MIDI bytes into UART0, one byte per interrupt (hardware FIFO off) |

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
the LED DMA and Core 0 code running from flash through the XIP cache. Panel
I2C polling, the LED frame build, display SPI, pattern conversion and flash
writes do not run under the playback lock. A dual-core split alone does not
establish physical jitter.

## Prepared Patterns

All 32 locations have a prepared row for each of eight tracks. Core 0 builds
these at initialization and after edits. A revision and row are published
together under the shared lock. Playback copies a row only when its location or
revision changes, at a step boundary. Song mode uses the same cache.

A row is tagged with its kind: the 16 events of a SYNTH or DRUM cell, or a
kit row (below), in one union. The event cache occupies 29,696 bytes (256 rows
of 116 bytes), plus revision counters and active rows.
Pattern and song data remain the editable source of truth. Code editing a
pattern must call `sequencer_refresh_track_playback()` after releasing the shared
lock, including for locations not currently playing.

## Kit Tracks

A kit track plays 12 lanes per step (AC, BD, SD, LT, MT, HT, RS, CP, CH, OH,
CC, RC) over MIDI only. Voice `v` is lane `v + 1`; every per-voice array is in
lane order.

- **Data.** `pattern_t.kit_steps[track][step]` is one `uint32_t` per step, lane
  `i` in bits `2i..2i+1` (0 off, 1 note, 2 accent, 3 flam; AC is 0 or 1; bits
  24-31 zero). It is used only where that location's `tracks[c].type` is
  `TRACK_TYPE_KIT`; the cell's `step_t` data is untouched, so a type change
  erases nothing. Kit settings (`kit_config_t`: 11 notes, `vel_normal`,
  `vel_accent`, `vel_max`, `flam_pulses`) belong to the track, not the location,
  in `g_seq.kit_config[]`; channel, gate length and stored mute come from
  `track_config_t`. `seq_set_kit_config()` stores the settings (clamped) and
  refreshes that track's 32 prepared rows, like `seq_set_track_config()`.
- **Prepared kit row.** `lanes[16]` (the cell's step words, zeroed at and beyond
  the length and when the stored mute is set), the 11 notes, the three
  velocities, channel, gate pulses and flam pulses. It is self-contained, so the
  revision and refresh path is the one the event rows use. A kit cell has a clip
  when any voice lane (not AC) is non-zero within the length; a stored mute does
  not hide it.
- **Two-kit limit.** A track counts as a kit when its type is KIT in any
  location; at most two tracks may be (`MAX_KIT_TRACKS`), so at most two kits
  play at once, also in Song mode and with mixed launches. `seq_set_track_config()`
  refuses a third kit before it changes anything and the save validator enforces
  the same rule.
- **Pulse order.** Each pulse: 1 MIDI Clock; 2 live mutes (a muted kit releases
  its sounding voices and cancels a pending flam); 3 note-off countdowns;
  4 flam countdowns (the second hits); 5 on a step boundary, the step. Kit
  messages of 2-4 go out through their own send call, never through `midi_buf`,
  which step 5 clears. With the gate at 100 % the countdown ends in step 3 of the
  pulse that begins the next step, before its Note Ons.
- **A kit step.** The voices go in the fixed order BD, SD, CP, CH, OH, RS, LT,
  MT, HT, CC, RC. A note is `AC ? vel_accent : vel_normal`, an accent
  `AC ? vel_max : vel_accent`, a flam plays its first hit at the note level and
  its second hit one level up `flam_pulses` later as Note On velocity 0 then Note
  On. The note-off countdown is `gate_pulses`, or
  `min(PULSES_PER_STEP, gate_pulses + flam_pulses)` on a step with a flam. Note
  Off of a kit voice is Note On with velocity 0, so a kit's messages share one
  status byte; SYNTH and DRUM tracks still send 0x80. One release channel per
  kit track: every voice is released at the latest at the next step boundary,
  before that step's hits, so all sounding voices come from one step and share
  the channel stored at that step; each keeps the note it was sent on. A
  settings edit therefore cannot strand a note (host builds check the invariant
  before a channel is stored).
- **Kind change.** When a row of another kind is installed (type change, load),
  the ISR first releases what the old kind left sounding. Stop, Panic, Start
  while playing and a play-mode change queue a release of every sounding kit
  voice before the MIDI Stop byte. The Stop, a real-time byte, may overtake
  them on the wire, and so may the Start and Clock of a restart, also real-time
  bytes; every voice is still released before the next run's notes.
- **Flam reserve.** A flam's second hits are the only kit message that may be
  dropped. Flam second hits must leave 288 TX queue bytes free
  (`KIT_FLAM_RESERVE`); the headroom covers the releases and new first hits of
  an immediate restart as well as scheduled playback (two kits' note-offs and
  the worst step burst, 168 bytes as queued in one pulse). Otherwise the voices
  sound until their note-off and `flam_drops` goes up, one per kit per flam
  pulse. It shows on the pattern-select screen as `FLAM DROP <n>` when
  non-zero.

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
  neither the live mute nor the stored mute of the location it plays is set.
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

The 512-byte UART queue accepts complete messages or complete step batches
(`MIDI_BUF_SIZE` is 160 bytes; the worst step burst is 102 bytes as stored, 82
on the wire). It keeps whole messages with their status bytes, and the drain
applies running status as it writes: a status equal to the last one written is
skipped, except on the first message after the queue ran empty (its last byte
written), which always carries its status. Real-time bytes do not change the
running status. The UART hardware FIFO is off and the drain writes one byte per
writable check, so the TX interrupt runs once per byte on Core 0.

Clock and transport bytes go ahead of note bytes. `midi_send_clock()` sets one
pending-Clock flag (never a counter, so Clocks are not released in a bunch).
Start, Continue and Stop wait in their own small queue. At each message
boundary the drain sends a queued Start/Continue/Stop, then a pending Clock,
then the next note message. A Clock byte may wait for the byte being shifted out
and a complete channel message whose status is already in the holding register:
up to four byte times, 1.28 ms, before interrupt latency, against up to about
10 ms behind a kit burst without this rule. Stop,
Panic and `midi_clear_output_queue()` clear the Clock flag, so no Clock follows
a Stop; `midi_wait_output_idle()` waits for it too.

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
programs the validity header last. Load validates indices, lengths and
configuration before replacing RAM.

Format version 6 lays out a header page, the patterns (`MAX_PATTERNS` x the
current `pattern_t`, rounded to a page), the song (rounded to a page) and the
kit settings (`kit_config[NUM_TRACKS]`, rounded to a page). Its CRC32 covers the
three payloads at their unpadded lengths. Versions 4 and 5 are still loaded:
their patterns are the legacy record, which is the start of today's
`pattern_t` (a byte prefix, 459 bytes on the RP2040; `kit_steps` follows at
offset 460, after one padding byte), so the song sits at the offset after the
page-rounded legacy patterns and the checksum covers the legacy lengths. Each
legacy record is copied into a zeroed `pattern_t`, so the kit steps are empty
and every kit setting gets its default. Version 4 has no checksum and
is accepted after validation. Every offset and length is taken from the version
and the compiler's type sizes, never hard-coded. Validation also requires: type
KIT in at most two tracks across all locations, kit step bits 24-31 zero, AC 0 or
1, and kit settings in range. The patterns payload is 31,104 bytes and the save
builds each payload in a static RAM copy buffer padded to whole pages, so
`.data` + `.bss` is about 110.5 KiB (see [HARDWARE_STATUS.md](HARDWARE_STATUS.md)).
The save and the load first wait for MIDI output to go idle, then copy the
patterns one location per `seq_lock()` section (then the song and the kit
settings in one more), so Core 0's interrupts are never off for the whole
31 KB copy.

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
button expander's ports then become the new baseline without events. A failed
button read keeps the last accepted state; the step LED mask is cached only
after a successful write. Core 1 never touches I2C or the LEDs.

A TCA8418 FIFO overflow, or an event read that fails after the event count was
read, drops the whole batch: the key state is forgotten and the FIFO drained,
with nothing reported. A flash save or load stalls both cores; afterwards the
inputs are resynced: `panel_input_resync()` drops every queued panel event
(PLAY included) and drains the FIFO, re-initialising the chip only if that
fails; `mcp23017_resync()` and `encoder_resync()` take the buttons as they are
now, with no press, release or long press from the stall; and the UI drops the
step gestures in progress. When a panic is released, the panel and the step
buttons are resynced the same way. After a resync or an overflow, a key or PLAY
that was down stays suppressed until a release has held for its debounce time
(8 ms, 20 ms for PLAY), so a bounce or a press the chip reports again never
becomes a command.

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
under load before claiming an ISR budget or timing margin; the kit track's ISR
budget, wire-level Clock interval and Core 0 interrupts-off window are bench items. See
[HARDWARE_STATUS.md](HARDWARE_STATUS.md) for remaining checks.
