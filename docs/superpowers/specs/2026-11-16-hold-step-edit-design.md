# Hold-Step Editing in Pattern View — Design

Date: 2026-11-16. Status: approved by the project owner; revised the same day
after an independent review (see "Review changes").

## Intent

Editing a step's note, velocity or flags today means leaving the step grid for
the Step Edit screen. On Ableton Push the same job is one gesture: hold a step
and turn an encoder. This change brings that gesture to Pattern View so a step
can be shaped without leaving the grid, including while the sequencer plays.

## Behaviour

Applies to `UI_PATTERN_VIEW` only.

1. **Tap.** A step button released less than `MCP_LONG_PRESS_MS` (500 ms) after
   the UI saw it pressed, with no encoder input during the hold, toggles the
   step **on release**. Today it toggles on press. The UI measures the hold
   with its own clock; it does not rely on the button driver's long-press
   event, which is not emitted when the release is sampled first.
2. **Hold and turn.** While one or more steps are held, encoder 1 changes the
   note and encoder 2 changes the velocity of every held step. Range and
   clamping are the same as in Step Edit (0-127). A turn that arrives in the
   same update as a step's first press does not edit that step.
3. **Hold and press an encoder.** An encoder button that is down at any moment
   while steps are held belongs to the gesture until that button is released.
   Its event applies to every step that was held while it was down, even if
   those steps were released before the encoder button: encoder 1 short
   toggles accent, encoder 1 long toggles slide (clearing tie), encoder 2 long
   toggles tie (clearing slide). Each step is toggled independently. Encoder 2
   short stays play/stop.
4. **Empty steps.** The first edit of an inactive step activates it with the
   same defaults `pattern_toggle_step()` applies, then applies the edit.
5. **No toggle after encoder input.** A held step is not toggled on release
   once an encoder was turned or an encoder button was down during its hold.
   This includes encoder 2 pressed for play/stop, and a turn that clamping
   left without effect.
6. **Peek.** A step held for `MCP_LONG_PRESS_MS` or longer with no encoder
   input is not toggled on release. Nothing changes; the screen just shows its
   values.
7. **Navigation is suspended while a gesture is active**, that is while a step
   is held or an encoder button that joined a hold is still down. Encoder 1
   does not change the track, encoder 2 does not change tempo, and encoder
   presses do not change the UI mode.
8. **Only presses seen in Pattern View count.** A button that was already down
   when Pattern View was entered is not editable and its release does not
   toggle. Leaving Pattern View or triggering Panic forgets the whole gesture.

## Screen

While a step is held, the track info line (`Ch:1  MIDI L:16`, y = 10) is
replaced by the values of the lowest-numbered held step, from the first moment
of the hold, and that step is marked in the grid the way Step Edit marks its
selected step:

```
S05 Note:C4   Vel:100
```

The line must fit `FONT_COLS` (21 characters). Step LEDs are unchanged.

## Goals, non-goals, anti-goals

- Goal: note, velocity, accent, slide and tie editable from the grid.
- Non-goal: changes to Performance view, Step Edit, Pattern Select, Song Edit,
  the sequencer engine, storage format or step LEDs.
- Non-goal: making the step note meaningful on drum tracks. Playback uses the
  track's `drum_note` there, as before; the gesture edits the stored note the
  same way Step Edit does.
- Non-goal: changing how accent and velocity interact. An accented step plays
  at velocity 127 whatever its stored velocity, as before.
- Anti-goal: an accidental step toggle during a hold gesture. A step that is
  playing must never drop out or appear because the user held it to edit it,
  whichever finger lifts first.
- Anti-goal: any new work under the playback lock or in the Core 1 timer path.

## Constraints

- Edits take the shared lock, release it, then call
  `sequencer_refresh_track_playback()` (see ARCHITECTURE.md, Prepared Patterns).
  Activating a step counts as an edit even when the value did not change.
- `step_buttons_t.held` is the sampled, debounced state (20 ms cadence);
  `pressed`, `released` and `long_pressed` are edge events cleared at the end
  of each `ui_update()`. The 500 ms boundary therefore carries that sampling
  uncertainty.
- An encoder's short press is reported when its button is released, a long
  press while it is still down; `encoder_button_held()` is the only signal
  that a press has begun.
- Host tests (`test/test_ui.c`) include `src/ui.c` directly and drive it
  through fake encoder and button state and a fake clock.

## Approaches

1. **Toggle on release, with per-step "tap still pending" tracking** — chosen.
   Pattern Select already works this way for mutes (`select_short_pending`),
   and it is how Push behaves. Cost: a tap now takes effect when the finger
   lifts instead of when it lands.
2. Toggle on press, then undo the toggle if an encoder moves. Rejected: while
   playing, the step would audibly drop out or sound before being restored.
3. Offer the gesture only inside Step Edit. Rejected: it keeps the screen
   switch the change is meant to remove.

## Decided how

- New UI state: a mask of steps held since a press seen in Pattern View, a
  mask of those still eligible for a tap toggle, the time each was pressed,
  and per encoder the steps its button press belongs to.
- The note/velocity/flag edit code is shared between Step Edit and the hold
  gesture so both stay identical.
- `display_draw_pattern_view()` learns which step is held (none = unchanged
  output).
- README "Live Controls" documents the gesture.

## Left to the builder

- Names and exact shape of the new state and helpers beyond those the plan
  fixes.
- How the held step index is computed from the hold mask.

## Review changes

Three independent reviewers read the first version. Changed as a result:

- Behaviour 3, 5 and 7: an encoder button pressed during a hold now belongs to
  the gesture until it is released. Before, releasing the step a moment
  before the encoder button toggled the step and sent the encoder press to
  navigation.
- Behaviour 5: any encoder button pressed during a hold cancels the tap,
  encoder 2 for play/stop included. A design that lets the tap happen after
  the encoder event resolves as "short" was considered and rejected as more
  state for a rare overlap.
- Behaviour 1 and 6: hold time is measured by the UI clock instead of the
  driver's long-press event.
- Behaviour 2: a turn in the same update as the first press is not an edit.
- Screen example corrected to the octave naming the code uses (note 60 = C4).
