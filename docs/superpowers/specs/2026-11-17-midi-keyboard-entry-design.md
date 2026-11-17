# Note Entry from a MIDI Keyboard — Design

Date: 2026-11-17. Status: behaviour approved by the project owner in
conversation ("step + key"; no record mode in this version). Revised the same
day after an independent review (see "Review changes"). Builds on
`2026-11-17-note-entry-copy-roll-design.md` (the last note).

## Intent

Pitch is the slow part of writing a melodic clip: each note is dialled with
an encoder. A MIDI keyboard gives a pitch and a velocity in one key press.
The board already has the input (J402, an H11L1 opto, UART0 RX on GPIO 1,
marked "reserved"); the firmware configures the pin and never reads it.

## Goals

1. Hold a step and press a key: the step takes the key's note and velocity.
2. A key pressed with no step held is heard through the track being edited
   and becomes that track's last note, so the next steps switched on take it.
3. No new screen and no mode.

## Non-goals

- Recording while the sequencer plays, step record with an advancing cursor.
- MIDI Clock, Start, Stop or Continue from the input (no external sync).
- Control change, pitch bend, aftertouch, program change, SysEx: ignored,
  not passed through.
- A channel filter: every channel is accepted.
- Audition on CV/gate: the timer on core 1 owns those outputs.
- A setting to switch the pass-through off.
- Step entry from the Session view (its step buttons toggle on release and
  open the editor on a long press; a third meaning does not fit there).

## Anti-goals

- A key changing a clip while no step button is held. (A key does set the
  track's last note, which a step switched on later takes: that is goal 2.)
- A note left sounding on a synth because the focus moved between its Note On
  and its Note Off, or because its Note Off arrived while the device could
  not listen.
- Keys stopping the sequencer: a full MIDI output queue today stops the
  transport, and a keyboard must not be able to cause that.
- Received bytes delaying the 96 PPQN timer.

## Receiving

- The receive interrupt is enabled wherever `midi_out.c` sets the UART's
  interrupt enables, which today pass `rx = false`: `midi_init()` becomes
  `(true, false)`, `midi_tx_drain_locked()` `(true, pending)`,
  `midi_clear_output_queue()` `(true, false)`. (The last one also runs on
  core 1 from the timer's panic, as it does today.)
- The UART interrupt handler (core 0) first reads every readable byte, then
  drains the transmit queue as today. Reading does not take the TX lock. A
  byte is always read, also when the ring is full: with the FIFO off the
  receive interrupt stays asserted until the data register is read. A byte
  that finds the 64-byte ring full is counted and dropped. The ring's push
  is placed in RAM like the handler.
- `bool midi_in_read(uint8_t *byte)` takes one byte (core 0, main loop);
  `uint32_t midi_in_dropped(void)` is the count of dropped bytes;
  `void midi_in_flush(void)` empties the ring. They live in `midi_out.c`,
  which owns the UART.
- Not detected: a byte lost in the UART itself (overrun, framing). The SDK
  call used to read (`uart_getc`) hides the error flags. Such a loss can
  produce one wrong note until the next status byte. Bench item.

**Gaps.** When bytes may have been lost the stream cannot be trusted: with
running status the next bytes can be read as a wrong note, and a Note Off may
be among the lost ones. A gap is declared when `midi_in_dropped()` has
changed, around a flash save or load, and at a panic. At a gap the UI empties
the ring, resets the parser (data bytes are ignored until the next channel
status) and releases every passed-through note (below).

## Parsing

`src/midi_in.c`, pure (no hardware), core 0 only:

```c
typedef enum { MIDI_IN_NOTE_ON, MIDI_IN_NOTE_OFF, MIDI_IN_ALL_OFF } midi_in_kind_t;
typedef struct { midi_in_kind_t kind; uint8_t channel, note, velocity; } midi_in_event_t;
typedef struct { uint8_t status, data[2], have; } midi_in_parser_t;
void midi_in_parser_reset(midi_in_parser_t *p);
// Feeds one byte; true when it completed an event, written to *out.
bool midi_in_parse(midi_in_parser_t *p, uint8_t byte, midi_in_event_t *out);
```

- Running status is honoured. Note On with velocity 0 is a Note Off.
- Any status byte drops a message in progress.
- Real-time bytes `0xF8..0xFE` (Active Sensing included) are ignored and do
  not disturb a message in progress. `0xFF` (System Reset) resets the parser.
- System common bytes (`0xF0..0xF7`) clear the running status; data bytes are
  then ignored until the next channel status (this also skips a SysEx body).
- Control change 120 (All Sound Off) and 123 (All Notes Off) give
  `MIDI_IN_ALL_OFF`: a keyboard's own panic releases the passed-through notes.
- Other channel messages are consumed with their correct length (two data
  bytes, one for `0xC0..0xDF`) and produce nothing. Running status applies to
  them too: after `C0 05`, the bytes `3C 64` are two more program changes.
- A data byte with no running status is ignored.

## What a key does

`ui_update()` reads the input at its start, before the panic check, at most
48 bytes a pass (the rest waits in the ring). While a panic is latched the
bytes are discarded. Otherwise each event is handled at once, in order, so
before the panel events and the screen handler of that pass.

**The track and its cell.** Session view: the focused track at the focused
location. Track Config: `ui.config_track` at its active location. Every other
screen: `ui.selected_track` at its active location. The type and the MIDI
channel are read from that cell's track configuration.

**Pass-through.** Keys are told apart by pitch alone, whatever channel the
keyboard sends on.
- Note On: sent as Note On on the track's channel with the key's velocity,
  and `thru_channel[note]` remembers the channel. If the note is already in
  the table its Note Off is sent first.
- Note Off: if `thru_channel[note]` is set, Note Off goes to that channel and
  the entry is cleared; otherwise nothing. A note is always released where it
  was started.
- **The output queue.** A full queue stops the transport
  (`midi_output_overflowed()`), so the pass-through never uses the sends that
  set that flag. It uses `midi_try_send_bytes_capped()`: a Note On (with the
  Note Off of a retrigger, as one batch) is sent only if the queue then holds
  at most half its capacity; if not, the key is dropped and not entered in
  the table. A Note Off may fill the queue to three quarters, so the
  sequencer's own sends still fit; if that fails the note stays in the table,
  marked for release, and the release is tried again on every pass.
- **Release all** (a gap, `MIDI_IN_ALL_OFF`): every note in the table gets
  its Note Off by the rule above.
- Before a flash save or load starts, release all, wait for the output queue
  to empty (the transport is stopped there) and retry any refused release, so
  no Note Off is left for after the stall; after it, a gap. `ui_init()` also
  empties the ring: bytes from before the UI started may have lost a Note Off. At a
  panic the table is simply cleared: All Notes Off has gone out. The timer's
  own panic (queue overflow) does not tell the UI; the table then holds stale
  entries, which cost one redundant Note Off each when the key is released.
- A keyboard unplugged with a key down leaves that note sounding (Active
  Sensing is not monitored). Panic silences it.
- A played note shares the track's channel with the sequencer's notes. A mono
  synth is retriggered and a Note Off of the same pitch may cut a sequenced
  note, as a keyboard on that channel would.
- On a kit track the keys go to the kit's channel unchanged (they play the
  drum machine).

**Writing (Note On only).** A step counts as held when it is in
`ui.grid_hold` and its button is down now (`steps->held`); a step pressed or
released in this pass does not count.

| Where | Condition | Effect |
|---|---|---|
| Pattern view, SYNTH cell | steps held | each is switched on if it was off (`toggle_step()`), then takes the key's note and velocity; `last_note[track]` = the note |
| Pattern view, DRUM cell | steps held | each is switched on if it was off and takes the key's velocity; the stored note is left alone (playback uses the track's drum note) |
| Step Edit | the edited step's button is down | the step takes the velocity and, on a SYNTH track, the note (`last_note[track]` follows); it is not switched on or off, as with the encoders |
| any screen, SYNTH track | none of the above | `last_note[track]` = the note |
| kit cell | | nothing |

- A held step that a key wrote to is not toggled on release, as after an
  encoder edit (`ui.grid_tap_pending` loses those steps).
- The write takes the shared lock once per key and refreshes the track's
  playback afterwards, as the encoder edits do.
- Several keys in one pass are applied in order; the last one wins. An
  encoder turn in the same pass is applied after the keys. Accent, slide and
  tie are not touched. An accented step still plays at velocity 127.
- Velocity is the key's, 1..127.

## Constraints

- Core 0 only for everything new: the ring's producer is the UART interrupt
  on core 0, its consumer the main loop on core 0. No new lock. The one
  exception is the interrupt-enable write in `midi_clear_output_queue()`,
  which core 1 already makes today.
- Nothing new runs in the timer interrupt. Nothing new in the save format.
- `test/fakes` gains `uart_is_readable`, `uart_getc` and a way to queue
  received bytes; the receive interrupt enable is recorded as the transmit
  one is. The fake models a queue, not the PL011's one-byte holding register:
  the 320 us deadline, a receive interrupt that never clears and the real
  interleaving with the transmit drain are bench items, not host tests.
- `midi_out.c` under `MOCK_HARDWARE` (the `test_sync` builds) has no queue
  and no interrupt: `midi_in_read` returns false there.

## Approaches considered

**Where bytes are read.** (a) The UART interrupt into a ring (chosen): the
main loop can stall for a display flush longer than one byte time. (b)
Polling the UART in `ui_update()`: with the FIFO off every stall longer than
320 us loses bytes. (c) Turning the FIFO on: the transmit side depends on it
being off (a Clock waits at most one message).

**What a key means.** (a) Step + key, pass-through otherwise (chosen by the
owner): no mode, fits hold-step editing. (b) Step record with a cursor: needs
a mode and a way to enter rests, accents and slides. (c) Real-time record:
touches the timer and quantization.

**Releasing a played note.** (a) Remember the channel per note (chosen). (b)
Send the Note Off on the current track's channel: a note hangs when the focus
moves while a key is down.

**A full output queue.** (a) Capped sends that drop the key (chosen). (b) The
ordinary sends: a stuck key or a fast player fills the queue (three bytes in
can mean six out) and the existing overflow rule stops the whole sequencer.

## Left to the builder

- The ring's size constant and the names of static helpers.
- Whether the pass-through and the write are one function or two.

## Review changes

An independent review of the first draft led to these changes:

- the pass-through uses capped sends and drops a key when the queue is half
  full, so keys cannot trigger the overflow that stops the transport;
- lost bytes are a "gap": the ring is emptied, the parser reset and every
  passed-through note released; a save or load releases the notes before it
  starts; the handler reads a byte even when the ring is full;
- the input is read before the panic check and discarded while a panic is
  latched;
- Step Edit writes only while the edited step's button is down; a held step
  must be down now, not only remembered;
- the track on the Track Config screen is the configured one; the cell's own
  track configuration gives type and channel;
- the parser drops a partial message at any status byte, resets on `0xFF`,
  and turns CC 120/123 into "release all";
- a DRUM step's stored note is left alone;
- the three places that set the interrupt enables are named with their new
  values.

## Verification

- Host tests listed in the plan; the whole suite passes.
- Simulator (outside the tracked files for now): the virtual UART gets a
  receive side, the scripted artist enters pitches from a keyboard, and the
  count of gestures for the same set is compared with the present figures.
- Not verifiable here: the H11L1 input circuit on a real board. It is a
  bench item (`HARDWARE_STATUS.md`).
