# The Pads as a Keyboard while a Step Is Held — Implementation Plan

Spec: `docs/superpowers/specs/2026-11-17-pad-keyboard-design.md`.
Branch `claude/pad-keys`, on top of `claude/midi-in` (it reuses
`handle_midi_event()`, the pass-through table and `write_key_to_steps()`).

Rules for every task: tests first, seen failing, then the code; the whole
host suite passes before a task is finished; nothing outside the files a task
names; nothing new in the timer interrupt or the save format. One commit per
task. The plan names the tests; the implementer writes the code.

## Task 1: the layout and the lights

Files: `include/led_frame.h`, `src/led_frame.c`, `test/test_led_frame.c`.

- `uint8_t pad_key_note(uint8_t base, uint8_t column, uint8_t row)` as the
  spec's table; 0xFF for a pad without a note or arguments out of range.
- `led_state_t` gains `keys`, `keys_base`, `keys_track`, `keys_note`;
  `led_frame_build()` lights the pads from the layout when `keys` is set.

Tests (`test_led_frame`):
- `pad_key_note`: the bottom row with base 36 is 36, 38, 40, 41, 43, 45, 47,
  48; the row above it is none, 37, 39, none, 42, 44, 46, none; the top pair
  starts at base + 36; the last white pad of a pair equals the first of the
  pair above; base 72 gives 120 at the top right; out-of-range column or row
  gives 0xFF;
- with `keys` set: a white pad is the track's hue at half, a black pad the
  hue at dim, a pad without a note is off, and the pads of `keys_note` (two
  when it is a shared C) are white at full; `keys_note` 0xFF marks nothing;
- with `keys` set the clip grid's state (queued, playing, clips) does not
  show on the pads, and SCENE, MUTE, PLAY and BEAT are what they are without
  `keys`;
- with `keys` clear the frame is byte for byte what it was before this
  change (the existing tests);
- the budget still scales a full keyboard frame to the limit.

## Task 2: the pads as keys

Files: `src/ui.c`, `test/test_ui.c`, `README.md`, `ARCHITECTURE.md`.

- `ui`: whether the keyboard was on in the last pass, its `base`, and
  `pad_key[NUM_TRACKS][8]` (note + 1, 0 = not a key).
- One function decides whether the keyboard is on and, on the off-to-on
  edge, sets `base`. It is evaluated once per pass, before the panel events
  are handled.
- `handle_pad_press()`: with the keyboard on, a pad with a note goes to the
  key path (a `midi_in_event_t` Note On with velocity 100 through the same
  function the MIDI input uses, with the velocity write switched off) and is
  remembered; a pad without a note returns; neither reaches the launch or the
  copy code.
- The pad release: a remembered key sends its Note Off and is forgotten.
- Every pass: a remembered key whose pad the panel layer no longer reports
  down is released. `drop_pad_holds()` (panic, save, load) forgets the keys.
- The frame's `led_state_t` gets the four fields: `keys` is "on in this
  pass", `keys_note` as the spec says.
- README (Live Controls): the keyboard, its layout in a small table, what the
  lights mean. ARCHITECTURE: one paragraph (a pad in this mode is a key of
  the MIDI input path; how a lost release is handled).

Tests (`test_ui`; "sent" is the decoded output of the fake UART):
- Pattern view, SYNTH: hold an empty step, press the pad of a note: the step
  is on with that note and velocity 100, the note was sent as Note On
  velocity 100 on the track's channel, `last_note` follows, nothing is
  launched or queued, the focus is unchanged, and releasing the step does not
  toggle it off; releasing the pad sends the Note Off;
- a step with velocity 64 keeps 64 when a pad changes its note;
- the window: a held step with note 57 puts base at 36 (the bottom-left pad
  writes 36, the pad two rows up in column 5 writes 57); an empty held step
  uses `last_note`; the base does not move after a pad changed the step's
  note to another octave during the same hold; after the step is released
  and held again the base follows the new note; clamping at notes 0-11 and
  at 127;
- a pad without a note does nothing and launches nothing;
- two held steps both take the note;
- with no step held, and in the Session view with a step held, a pad
  launches as before; on a DRUM cell and on a kit cell with a step held a pad
  launches as before;
- a pad of another track's column is a key too (the keyboard uses all eight
  columns) and is passed through on the edited track's channel;
- the step is released while the pad is still down: the pad's release still
  sends the Note Off and does not launch; a pad that was a launcher press
  before the step was held is still a copy source afterwards and its release
  is a launcher's;
- hold-to-copy is not triggered by key pads: hold a key pad 500 ms, press
  another pad of that column: no copy, no notice;
- the panel layer stops reporting a key pad down without a release event:
  the next pass sends the Note Off, once;
- panic, save and load with a key pad down: no Note Off is sent twice and the
  pad's later release does nothing;
- Step Edit: with the edited step's button down a pad writes the note, the
  active flag is unchanged; with the button up a pad launches as before;
- the same C on two pads: second press Off then On, first release Off,
  second release nothing;
- Song mode: a key pad writes and shows no `SONG MODE`;
- the frame: with the keyboard on, `led_state_t` has `keys`, the base, the
  edited track and the held step's note; one pass after the step is released
  `keys` is clear;
- a pad key and a MIDI key in the same pass both work, in arrival order
  (MIDI input is handled before the panel events).

## Changes after the design review

The tests above were written to the revised spec: lights are checked through
the frame the fake receives, not the local `led_state_t`; "arrival order"
became "MIDI before panel events"; the budget test checks the keyboard frame
is within the budget; tests were added for a step and a pad in one pass, the
generation change, the Note Off on the original channel after the context
changed, Step Edit on DRUM and kit, and Step Edit's long press after a key.

## Order and review

1, then 2. The whole diff gets one independent review before a pull request.
