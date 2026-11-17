# Note Entry from a MIDI Keyboard — Implementation Plan

Spec: `docs/superpowers/specs/2026-11-17-midi-keyboard-entry-design.md`.
Branch `claude/midi-in`, on top of `claude/pattern-roll` (it uses
`ui.last_note` and `toggle_step()` from there).

Rules for every task: tests first, seen failing, then the code; the whole
host suite passes before a task is finished; nothing outside the files a task
names; nothing new in the timer interrupt, the save format or the LED frame.
One commit per task. The plan names the tests; the implementer writes the
code.

## Task 1: the parser

Files: `include/midi_in.h`, `src/midi_in.c`, `test/test_midi_in.c`,
`test/CMakeLists.txt` (new target `test_midi_in`, CTest name
`midi_in_tests`), `CMakeLists.txt` (firmware source list).

Tests (`test_midi_in`):
- Note On and Note Off, each channel nibble, give the right event;
- Note On velocity 0 is a Note Off;
- running status: one status, three note pairs, three events;
- a real-time byte `F8` and Active Sensing `FE` between status and data, and
  between the two data bytes, change nothing;
- `FF` in the middle of a message resets the parser: `90 3C FF 40 64` gives
  nothing;
- control change (other than 120/123), pitch bend and aftertouch produce
  nothing and the note after them, with its own status, parses;
- program change keeps its running status: `C0 05 3C 64` gives nothing, and
  `90 3C 64` after it is a note;
- CC 120 and CC 123, on any channel, give `MIDI_IN_ALL_OFF`;
- a SysEx `F0 .. F7` with data bytes that look like notes produces nothing,
  and the running status from before it is not used afterwards;
- a system common byte in the middle of a note drops it: `90 3C F2 00 00 40
  64` gives nothing;
- data bytes before any status produce nothing;
- a new status in the middle of a message drops the partial one;
- after `midi_in_parser_reset()` data bytes are ignored until a status.

## Task 2: receiving bytes

Files: `src/midi_out.c`, `include/sequencer.h` (declarations beside the other
`midi_*`), `test/fakes/hardware/uart.h`, `test/fakes/fake_hardware.[ch]`,
`test/test_playback.c` (it already tests the UART queue).

- Fakes: `uart_is_readable`, `uart_getc`, `fake_uart_rx_push(const uint8_t *,
  unsigned)`, `fake_uart_rx_irq` recorded by `uart_set_irq_enables`, all reset
  by `fake_hardware_reset()`.
- `midi_out.c`: the ring with its push in RAM; the handler reads every
  readable byte (also with the ring full) before it takes the TX lock;
  `rx = true` at the three `uart_set_irq_enables` calls with the values the
  spec gives; `midi_in_read`, `midi_in_dropped`, `midi_in_flush`. Under
  `MOCK_HARDWARE` the three exist and do nothing.

Tests (`test_playback`, beside the existing queue tests):
- after `midi_init()` the receive interrupt is enabled and the transmit one
  is not; it still is after a drain that empties the queue, after
  `midi_clear_output_queue()` and after a panic;
- bytes pushed to the fake and the handler run: `midi_in_read` returns them
  in order, then false;
- more bytes than the ring holds: the fake's receive queue is empty after the
  handler (every byte was read), the first ones are kept, the rest counted in
  `midi_in_dropped`, and reading makes room again; the ring wraps correctly
  over several fill-and-read rounds;
- `midi_in_flush()` empties the ring and leaves the dropped count;
- received bytes do not change what is transmitted (a queued message leaves
  byte for byte as before while bytes arrive).

## Task 3: the keys in the UI

Files: `src/ui.c`, `test/test_ui.c`, `test/CMakeLists.txt` (`test_ui` links
`midi_in.c`).

- `ui`: the parser, `thru_channel[128]` (0xFF = not sounding), a per-note
  "release pending" mark, the dropped count last seen.
- One function called at the start of `ui_update()`, before the panic check:
  a changed dropped count is a gap; then at most 48 bytes are read and
  parsed; each event does the pass-through and the write of the spec's
  table. While `ui.panic_latched`, the bytes are discarded instead.
- Pass-through sends go through `midi_try_send_bytes_capped()` only, with
  the caps the spec gives; pending releases are retried every pass.
- Gap, release-all, the clear at a panic, release-all before
  `flash_storage_save()`/`flash_storage_load()` and a gap after them.
- A test helper that pushes MIDI bytes to the fake UART, runs the UART
  interrupt handler, then one `ui_update()`.

Tests (`test_ui`; "sent" means the decoded output of the fake UART):
- Session view, no step held: a key is sent as Note On on the focused
  track's channel with its velocity; its release as Note Off; the clip is
  unchanged; `last_note` of that SYNTH track is the note; a Session short
  press afterwards switches a step on with that note;
- the focus moves to a track on another channel while the key is down: the
  Note Off goes to the first channel;
- the same key twice without a release: Off, then On again;
- the same pitch from two input channels: one sounding note, released by the
  first Note Off, the second sends nothing;
- a Note Off for a key that was never on sends nothing;
- Track Config on track 5 while `selected_track` is 0: the key goes to track
  5's channel;
- Pattern view, SYNTH: hold an empty step, press a key: the step is on with
  the key's note and velocity, it is not toggled off on release, `last_note`
  follows; two steps held take the same note; a step that already had a note
  is overwritten; Note On with velocity 0 writes nothing;
- a key and a step press in the same update: the step is not written and
  toggles on release as a tap; a key in the update in which the step is
  released: not written;
- two keys in one update: the step holds the second;
- a key, then an encoder turn of +2 in the same update: the step holds the
  key's note plus 2;
- Pattern view, DRUM: the held step is on with the key's velocity; its
  stored note is what `pattern_toggle_step()` gave it;
- Step Edit: with the edited step's button down the step takes note and
  velocity, its active flag is unchanged and `last_note` follows; with the
  button up nothing in the pattern changes and the key is passed through; on
  a DRUM track only the velocity;
- kit track: the key is passed to the kit's channel and nothing in the
  pattern or `last_note` changes, with and without a step held;
- a write while the clip plays is heard (as
  `test_grid_hold_edit_while_playing_is_heard`);
- **flood**: the same Note On repeated until the output queue is half full:
  `midi_output_overflowed()` stays false, the transport keeps playing, the
  keys beyond the cap send nothing and are not in the table; after the queue
  drains a key works again;
- a Note Off that finds the queue completely full stays pending and is sent
  on a later pass;
- `MIDI_IN_ALL_OFF` (CC 123 from the keyboard) releases every sounding key;
- more bytes than the ring holds between two updates (a gap): every sounding
  key is released, and a note split by the loss is not written or sent;
- save and load with a key down: its Note Off is sent before the stall, the
  parser is reset afterwards (data bytes with no new status do nothing);
- panic while a key is down: the table is cleared, a later Note Off for that
  key sends nothing; keys pressed while the panic is latched send and write
  nothing and are not acted on after it is released;
- 100 bytes waiting: one update handles 48, the next the rest;
- control change and clock bytes between the notes change nothing;
- on a DRUM or kit track `last_note` is not written.

## Task 4: documentation

Files: `README.md` (Features, Hardware table, Live Controls, Status list),
`ARCHITECTURE.md` (the context table: the UART interrupt also receives; a
"MIDI input" paragraph with the pass-through and release rule),
`HARDWARE_STATUS.md` (the input circuit is unvalidated; what to check on the
bench: the opto's output at the Pico pin with a keyboard connected; that no
byte is lost while the display is redrawn and while a set plays; that the
receive interrupt does not fire continuously; the receive path's place in
RAM in the map file). README also says: a key lost at the UART can give one
wrong note; unplugging a keyboard with a key down needs Panic.

## Order and review

1, 2, 3, 4 in order. Each task is checked against the spec before the next;
the whole diff gets one independent review before a pull request.
