# Jitter Measurement

The firmware is DIN MIDI only. USB MIDI support was removed so timing is not exposed to USB host scheduling.

The current timing path also removes two major avoidable ISR costs:

- MIDI bytes are queued into a UART TX ring and drained by UART0's TX interrupt,
  one byte per interrupt with the UART FIFO off. The drain applies running status;
  Clock and Start/Continue/Stop go out at the next message boundary, ahead of
  queued note bytes.
- Pattern and Song playback use prepared events. Core 0 builds the cache on initialization and after edits.

The playback lock serializes timer processing and transport controls. This prevents
Stop/Panic races, but control operations and shared-lock contention can still add
latency. No physical before/after jitter reduction has been established.

## Firmware Metrics

The sequencer timer ISR records these values in microseconds:

- `J`: max IRQ entry latency, measured as actual ISR entry time minus the scheduled timer alarm time.
- `P`: max absolute pulse-period error, measured between consecutive ISR entries.
- `I`: max ISR duration.
- `samples`: number of timer ISR samples.
- `overruns`: count of interrupts at least one pulse late or handlers that miss their next deadline.

A missed alarm is re-armed in the future without a catch-up burst. This prevents
a timer stall; it cannot recover timing already missed. Tempo changes are
accounted for when calculating pulse-period error.

In pattern select, the OLED footer shows:

```text
J:<latency> P:<period> I:<isr>
```

Press step button 13 in pattern select to reset the counters.

## GPIO Probe

GPIO 7 is the timing probe. It is driven high at sequencer timer ISR entry and low at ISR exit.

Use a logic analyzer or oscilloscope:

1. Connect probe channel 0 to GPIO 7 and ground to Pico GND.
2. Optional: connect channels to MIDI TX GPIO 0 and gate outputs GPIO 14-17.
3. Start playback at 120 BPM, then repeat at 200 BPM and 300 BPM.
4. Use a dense stress pattern: all 8 tracks active on all 16 steps, with MIDI and CV/Gate enabled where available.
5. Capture at least 10,000 timer pulses.

## What To Measure

- GPIO 7 rising-edge interval should match the pulse period:
  - 120 BPM: 5208 us
  - 200 BPM: 3125 us
  - 300 BPM: 2083 us
- GPIO 7 pulse width is the ISR duration.
- Gate edge alignment can be checked across GPIO 14-17.
- MIDI TX bursts on GPIO 0 can be compared against gate edges to quantify output skew.
- Decode MIDI Clock separately: 24 messages per quarter, approximately 20.833 ms
  apart at 120 BPM. GPIO 7 measures the 96 PPQN timer, not wire-level MIDI jitter.
- DIN messages share a serial link at 320 us per byte. Behind a note burst a
  Clock byte waits for the byte being shifted out and at most one complete
  channel message (four byte times, 1.28 ms, before interrupt latency), not for
  the whole burst; gate alignment does not prove MIDI alignment.
- Session panel load case: in pattern select, press step button 13 to reset the
  counters, then return to the Session view (encoder 2 long press) and run the
  load there: scenes launching every bar, the 82 LEDs animating, the key matrix
  scanned and the OLED refreshing. Open pattern select again (encoder 2 long
  press in the Session view) and read `J`, `P` and `I`. GPIO 7 jitter, the
  ISR duration (`I`, and the GPIO 7 pulse width) and wire-level MIDI Clock
  must match the idle capture. Core 1 never touches I2C or the LEDs, so a
  change points to lock contention (launches, the per-frame transport
  snapshot), bus contention (the LED DMA), or XIP-cache contention. The timer
  handler and the whole kit and MIDI path it runs are in RAM: the kit step,
  flam and release code (inlined into the handler, plus `kit_take_release()`
  and the `kit_voice_order` table), the MIDI sends it makes, the step buffer,
  the TX drain and the UART TX interrupt handler; `seq_lock()`/`seq_unlock()`
  (launches, Song advances) run from RAM as well, with the per-frame transport
  snapshot. Its other callees stay in flash as on the base commit and share the
  flash cache with Core 0 code: `load_prepared_track()` (every step),
  `send_note_off_if_active()` and `prepare_note_off()` (event tracks),
  `gate_commit()` and `cv_set_note()` (CV/gate), the SDK's
  divider wrapper (`pulse_count % 24` and `/ 24` on every pulse: the Cortex-M0+
  has no long multiply), and on Stop and overflow `stop_playback_outputs()`,
  `midi_clear_output_queue()` and `midi_send_all_notes_off()`.
- Kit track load case (two kits): make two tracks kits with every voice on every
  step, the flam lane on as many steps as possible with FLAM at 3 and at 12
  pulses, the other six tracks busy, MIDI and CV/Gate on, and capture at 140-150
  BPM, then at 200 and 300 BPM. First capture the same pattern load on a build of
  the base commit (3cbd4f9) as the baseline: it has no kits, so
  use its densest synth/drum pattern. The GPIO 7 maximum pulse width (`I`) may
  rise by at most 20 us over that baseline; `J` and `P` and the wire-level MIDI
  Clock interval (GPIO 0 decoded) must be no worse than the baseline. A Clock
  byte may wait for the byte being shifted out and a complete channel message
  whose status is already in the holding register (the PL011 keeps a one-byte
  holding register with the FIFO off): up to four byte times, 1.28 ms, before
  interrupt latency, against up to about 10 ms behind a kit burst on the earlier
  path. Up to 200 BPM `FLAM DROP` must stay 0 (on the host no second hit is
  dropped there). At 300 BPM with every voice of both kits flammed the queue is
  overloaded by design: second hits are dropped once the 1,024-byte TX queue
  would hold more than 319 bytes with them, and counted as `FLAM DROP <n>` on
  the pattern-select screen; nothing else may be dropped and playback must not
  stop, also when PLAY restarts the run.
- Exercise mutes, live edits, different loop lengths, Song transitions,
  tempo changes, Stop and Panic. Check that gates stay low after Stop/Panic.
- Save/load are rejected while running. After stopping, test save/load and restart
  to check alarm recovery after flash lockout.

## Core 0 Interrupts-Off Sections

`seq_lock()`, the playback lock and the MIDI TX lock are spin locks: the core
that holds one has its interrupts off. On Core 0 that holds back the UART TX
interrupt, which runs once per MIDI byte. A window longer than 100 us (a third
of a byte time) leaves a gap on the wire and can hold a Clock back. A section
that finds its lock held by Core 1 also waits with interrupts off: for the
playback lock up to the rest of a running timer ISR (`I`), for the other two
the length of one Core 1 section (one prepared row, one song advance, one burst).

The sections Core 0 can enter while playing, the flash save aside. MIDI goes
out in batches, never message by message, and no section copies more than one
location's data (`sequencer_snapshot()` reads one byte from each playing
location):

| Section | Locks | Work inside |
|---|---|---|
| `transport_start()` while playing | playback, seq, TX | kit releases as one send, note-offs of up to 8 tracks as a second send, Stop, position reset, the Song entry; lock released; each track's prepared row loaded in its own `seq_lock` section; then state and Start |
| `transport_stop()` | playback, seq, TX | state, kit releases as one send, note-offs as a second send, Stop |
| `sequencer_panic()` | playback, TX | queue cleared, kit releases as one send, note-offs as a second send, Stop, the 32 All Sound Off / All Notes Off messages as one 96-byte send |
| `transport_set_mode()` with a change | playback, TX | as `transport_stop()`, plus 8 queue slots |
| `transport_set_bpm()`, `transport_set_launch_quant()` | playback, seq | one or two values |
| `transport_queue_pattern()`, `transport_queue_track_pattern()`, `transport_launch()` | playback, seq | 8 tracks' queue and current-location slots (transport fields only) |
| `transport_clear_queue()`, `transport_clear_track_queue()` | playback | 8 or 1 queue slots |
| `sequencer_snapshot()`, once per UI frame | playback, seq | 8 tracks' transport fields and the stored-mute byte of each playing location, in one section so a frame sees one transport state |
| `sequencer_get_jitter_stats()`, `sequencer_reset_jitter_stats()` | playback | 12 values, or one flag |
| `transport_continue()` | playback | returns at once while playing (its 8-track load runs only when stopped) |
| `sequencer_refresh_track_playback()`, after every edit | seq, twice | one location's steps (or kit steps) and config copied out; the row built with interrupts on; one prepared row (116 bytes) and its clip bit published |
| `seq_kit_tracks()` | seq | one short section per location (32): the kit mask of one location's track types |
| `seq_set_track_config()` | seq | a KIT config runs `seq_kit_tracks()` first; then one location's config per section, 32 sections, then one refresh per location |
| `seq_set_kit_config()` | seq | the track's kit settings (about 15 bytes) in one section; then one refresh per location |
| `seq_toggle_track_mute()` | seq | the live mute bit; then one location's stored mute per section |
| `seq_set_track_mute()`, `seq_get_track_mutes()`, `sequencer_clip_mask()`, `song_clear()`, `song_set_end_mode()`, `song_append*()` | seq | a few bytes |
| `song_insert*()`, `song_remove()`, `song_move()` | seq | `memmove` of the entries after the edit point: up to 255 x 9 = 2,295 bytes (see below) |
| UI (`ui.c`): step toggles, step editor, length -/+, track config read, focus, pattern select, Song editor reads | seq | one location's steps or config, one song entry, or a few bytes |
| UART0 TX interrupt | TX | a byte or two (the FIFO is off) |
| `midi_send_*()`, `midi_output_overflowed()`, `midi_clear_output_queue()` | TX | one message or one batch copied in, then the drain as above |

`flash_storage_load()` refuses while playing; the flash save and
`midi_wait_output_idle()` run only when stopped. Stopped, the save and the load
wait for MIDI output to go idle and then copy one location per `seq_lock()`
section (972 bytes; the song and the kit settings, about 2.4 KB, in one more),
never the 31 KB of patterns in one section.

Song list edits are the exception: `memmove` copies byte by byte (7-9 cycles a
byte in the firmware's newlib), so an insert, remove or move near the start of a
long song keeps Core 0's interrupts off for about 130-165 us at 125 MHz with 256
entries, and under 100 us below about 150 entries. This is an estimate from the
instruction count, not a measurement. The same section can also hold back
Core 1's timer ISR, which takes `seq_lock` every step.

## Current Bench Status

No physical before/after capture is recorded for these changes, the kit track
included: its GPIO 7, wire Clock and interrupts-off figures are listed as
`NOT VERIFIED` in [HARDWARE_STATUS.md](HARDWARE_STATUS.md). Host tests verify
timing logic and output ordering, not physical jitter. Use the GPIO probe and
on-device counters above for repeatable measurements on the assembled hardware.
