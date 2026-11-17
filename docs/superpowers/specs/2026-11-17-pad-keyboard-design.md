# The Pads as a Keyboard while a Step Is Held — Design

Date: 2026-11-17. Status: behaviour and layout approved by the project owner
in conversation (piano layout; no scale setting). Revised the same day after
an independent review (see "Review changes"). Builds on
`2026-11-17-midi-keyboard-entry-design.md`: a pad pressed in this mode is a
key, handled by the same code as a key of a MIDI keyboard.

## Intent

With a MIDI keyboard a held step takes its pitch from one key press. Without
one the pitch is still dialled with an encoder. The panel already has 64 pads
that do nothing useful while a step is being edited: they launch clips, which
is not what a hand holding a step is doing. While a step is held they become
a four-octave keyboard.

## Goals

1. Hold a step of a SYNTH clip in the Pattern view (or the edited step in
   Step Edit) and press a pad: the step takes that pad's note.
2. The pad lights show the keyboard and the step's current note.
3. No setting, no menu, no mode switch: the held step is the mode.

## Non-goals

- A scale or root setting; an isomorphic (fourths) layout.
- Velocity from the pads (they are switches).
- The keyboard in the Session view, on DRUM and kit cells, or with no step
  held (there the pads launch clips as today).
- Chords, recording while playing.

## Anti-goals

- A pad launching, queueing or copying a clip while it is shown as a key.
- A note left sounding because a pad's release was lost or the step was let
  go first.
- The keyboard shifting under the fingers during one hold.

## When the pads are a keyboard

Once per pass, after the MIDI input is handled and before the panel events,
the UI takes a **snapshot**: whether the keyboard is on, for which track and
location, with which steps, and its window. The pad presses of that pass and
the LED frame both use the snapshot; nothing decides again later in the pass.
A panic and a screen change clear it.

The keyboard is **on** when either holds:

- `ui.mode == UI_PATTERN_VIEW`, the cell is a SYNTH cell (not DRUM, not kit),
  and a step is held: `ui.grid_hold & steps->held` is not empty (the rule the
  MIDI keys use: a step pressed or released in this pass does not count);
- `ui.mode == UI_STEP_EDIT` on a SYNTH track and the edited step's button is
  down and was not pressed in this pass.

So a step and a pad pressed in the same pass launch (the keyboard is not on
yet), and the frame of that pass shows the clip grid. The lights follow the
routing within one frame (33 ms).

**The reference step** is the lowest held step in the Pattern view and the
edited step in Step Edit.

**The window.** The keyboard shows four octaves starting at the C `base`.
`base` is chosen when the keyboard turns on (it was off in the previous
pass) and stays fixed until it turns off:

- the reference note `n` is the reference step's stored note if that step is
  on or its note is not 0, else `last_note[track]`;
- `base = 12 * (n / 12 - 1)`, clamped to `0..72`: the reference note's octave
  is the second pair of rows from the bottom, with one octave below it and
  two above. At the clamps (notes 0-11 and above 95) it is another pair, and
  notes above 120 have no pad.

**The layout.** Rows are numbered 0 (top, printed 1) to 7 (bottom, printed 8).
Octave `k = (7 - row) / 2` (0 for the bottom pair). In each pair the lower row
(odd index) is the white keys and the upper row (even index) the black keys:

| Column | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| white row, semitone | 0 (C) | 2 | 4 | 5 | 7 | 9 | 11 | 12 (C) |
| black row, semitone | none | 1 | 3 | none | 6 | 8 | 10 | none |

The pad's note is `base + 12 * k + semitone`. The last white pad of a pair is
the same C as the first white pad of the pair above. Highest note: 120, on
the last white pad of the top pair (row index 1, column 7).

## What a pad does

Pad events are handled where they are today (`handle_panel_events()`, every
screen).

**Press, keyboard on.**
- A pad with a note is a key: exactly what a Note On from the MIDI input does
  (`handle_midi_event()`), with two differences: the pass-through velocity is
  100, and the write never changes a step's velocity (an explicit flag on the
  write; a step switched on in the Pattern view keeps its stored velocity, and
  gets 100 only when that was 0, as `pattern_toggle_step()` does). In Step
  Edit the step is not switched on or off. So: the note is
  passed through to the track's channel (capped sends, the note table), each
  held step is switched on if it was off and takes the note,
  `last_note[track]` follows, and the held steps are not toggled on release.
- The pad is remembered as a key with its note
  (`pad_key[track][row] = note + 1`), but only when its Note On was sent: a
  press the capped send refused owns no note (the step is written all the
  same), so its release cannot end a note another key holds on that pitch.
- It does not launch, queue, cancel, copy or move the focus. The keyboard is
  checked before the copy source is looked for and before any of the copy
  bookkeeping is written; a key press also clears its own `pad_seen` bit, so
  a pad that was a key is never a copy source later.
- A pad without a note (the three dark pads of a black row) does nothing.

**Press, keyboard off.** Today's behaviour: launch, or copy.

**Release.** If the pad is remembered as a key: the note's Note Off by the
pass-through rule, and the pad is forgotten, whether or not the keyboard is
still on. Otherwise today's behaviour (the copy hold ends).

**A release that never comes.** The panel layer can forget a key without an
event (overflow, chip reset, resync), and after a failed read it can go on
reporting a pad down whose release it lost. Each key remembers the panel
layer's sync generation at its press. On every pass, after the panel events,
a pad remembered as a key is released as above when
`panel_input_pad_down()` no longer reports it down or the generation has
changed, and every pad key is released when the generation changed during
this pass's poll (the panel layer can lose its key state in a poll and still
hand out a press it had accepted before; that key would carry the new
generation). A panic and a save or load forget all pad keys (the pass-through
table is handled there already), and so does anything that ends every
passed-through note: All Notes Off from the MIDI keyboard, or a gap in its
input.

**The same note twice.** Two pads can carry the same C. The pass-through
table is by pitch: the second press retriggers, the first release ends the
note, the second release does nothing.

**Step Edit's long press.** In Step Edit a long press of a step button
switches that step on or off. A button that is being used to hold the step
for a key must not do that: once a key (a pad or a MIDI key) has written to
the edited step, that button's long press is ignored until it is released.
(This also corrects the MIDI keyboard entry it builds on.)

MIDI input is handled before the panel events of a pass, so a MIDI key and a
pad key in one pass leave the step with the pad's note.

SCENE, MUTE and PLAY are not affected. In Song mode the keys work (nothing
is launched).

## The lights

`led_state_t` gains the keyboard:

```c
bool    keys;           // the pads are shown as a keyboard
uint8_t keys_base;      // the C of the bottom-left pad
uint8_t keys_track;     // whose hue
uint8_t keys_note;      // the note to mark, 0xFF = none
```

When `keys` is set, `led_frame_build()` lights the 64 pads from the layout
instead of the clip grid:

- a white key: the track's hue at `LED_LEVEL_HALF`;
- a black key: the track's hue at `LED_LEVEL_DIM`;
- a pad without a note: off;
- every pad whose note is `keys_note`: white at `LED_LEVEL_FULL`.

SCENE, MUTE, PLAY and BEAT are built as today. The current budget applies to
the frame as today.

`keys` and `keys_base` come from the snapshot, `keys_track` is the snapshot's
track. `keys_note` is read at frame time from the snapshot's reference step,
so it follows the pad just pressed: its stored note when the step is on (in
Step Edit: on, or its note is not 0), else 0xFF.

The layout lives in one place, a pure function both the UI and the frame
builder use:

```c
// led_frame.h (pure): the note of pad (track column, row) in a keyboard whose
// bottom-left pad is the C `base`, or 0xFF for a pad without a note.
uint8_t pad_key_note(uint8_t base, uint8_t column, uint8_t row);
```

## Constraints

- Core 0 only; no new lock; nothing new in the timer interrupt or the save
  format.
- The clip grid, the copy gesture and the launch path are unchanged when the
  keyboard is off.
- `test_ui.c`'s panel fake already has `panel_input_pad_down` and queued
  presses without releases.

## Approaches considered

**Layout.** Piano (chosen by the owner): needs no setting and reads at a
glance. By scale (seven degrees and the octave per row): fastest inside one
key, but needs a root and a scale setting and cannot reach notes outside the
scale. Fourths: no setting and more reach, but unfamiliar to anyone who has
not played a grid instrument.

**How a pad writes.** As a key of the MIDI input (chosen): one code path for
the write, the pass-through, the capped sends and the last note. A separate
"set note" path would repeat all of it and could drift from the keyboard's
behaviour.

**When the window is chosen.** Once, when the keyboard turns on (chosen).
Every pass: the keyboard would jump an octave under the finger as soon as a
pad changed the step's note.

**Knowing a pad was a key.** Remember it at the press (chosen). Deciding at
the release by whether the keyboard is on now: a step let go before the pad
would turn the pad's release into a launcher's, and the note would hang.

## Left to the builder

- Names of the static helpers and of the `ui` fields.
- Whether the layout table is two arrays or a formula.

## Review changes

An independent review of the first draft (three reviewers) led to these
changes: the single snapshot per pass used by both the routing and the
lights; the reference step and note defined for Step Edit and for a step
that is on with note 0; a key pad kept out of the copy bookkeeping by
construction; the sync generation as a second sign of a lost release; the
velocity rule stated as it really is; Step Edit's long press not toggling a
step that a key wrote to; the position of note 120 and the behaviour at the
clamps.

The review of the code (three reviewers, two of them ran it) added: a key
pressed in a poll that lost the panel's key state is released in that pass
(found by reproducing a hung note with the real panel layer); a pad whose
Note On was refused is not remembered; the pads own nothing after All Notes
Off; pad events outside the 8 by 8 tables are ignored. Not changed, by
choice: a step and a pad pressed in one pass launch while the next frame
already shows the keyboard, and a pad pressed in the pass that leaves the
step editor is still a key (the lights the finger saw are what counts); a
dark pad of the keyboard ends a stale copy hold of that same pad.

## Verification

- Host tests listed in the plan; the whole suite passes.
- Simulator: the artist takes pitches from the pads (no MIDI keyboard) and
  the gesture counts for the same set are compared; the keyboard is seen on
  the simulated panel.
- Not verifiable here: how the lit keyboard reads on the real panel and
  whether 64 lit pads stay inside the current budget visibly (the frame is
  scaled by the existing budget rule).
