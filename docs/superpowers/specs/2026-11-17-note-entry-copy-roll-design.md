# Faster Note Entry, Clip Copy and a Piano Roll — Design

Date: 2026-11-17. Status: approved by the project owner in conversation
(behaviour of all three parts; the piano roll's exact pixel layout is the
builder's, shown to the owner in the simulator afterwards). Revised the same
day after an independent review (see "Review changes").

## Intent

A scripted Goa trance set was played on the simulated device
(`sim/`, not yet in the repository). Programming its 23 clips took 977
encoder clicks and every clip had to be typed again in every row, because:

- a new step always starts at C4, so each note is dialled from there
  (a bass line on A1 is 27 semitones away, every time);
- a clip cannot be copied;
- the Pattern view shows which steps are on, not their pitch, so a melodic
  line cannot be seen while it is entered.

This change removes those three costs. It adds no screen and no menu level.

## Goals

1. A new step on a SYNTH track starts at the pitch last entered on that track.
2. One gesture copies a clip to another slot of the same track.
3. The Pattern view of a SYNTH clip draws the clip as a piano roll with time
   running top to bottom.

## Non-goals

- Copying a whole row (scene), copying between tracks, undo, clearing a clip.
- Any change to DRUM or kit editing, to the Session screen, to saves.
- A larger display. The roll is drawn for the present 128x64 OLED.

## Anti-goals

- A copy that happens by accident while clips are launched quickly.
- A press that both copies and launches.
- New state in the flash image.

## Part 1: the last note

- `ui.c` keeps `last_note[NUM_TRACKS]`, UI RAM only, 60 (C4) after power-up.
  It is not saved and not cleared by Stop, Panic or load.
- **Reading it.** Whenever the UI switches a step on (Session short press,
  Pattern view tap, the activation that comes with the first edit of a held
  empty step, the Step Edit long press), on a track whose type is
  `TRACK_TYPE_SYNTH`, and the step's stored note was 0 before it was switched
  on, the step's note is set to `last_note[track]` **after**
  `pattern_toggle_step()` has run (that function writes C4 over a note of 0,
  so seeding before it would lose a last note of 0). A step that already
  stores a note keeps it, as today. `pattern_toggle_step()` is unchanged.
- **Writing it.** After an encoder turn changed the note of at least one step
  on a SYNTH track (hold-step editing or Step Edit), `last_note[track]`
  becomes the resulting note of the lowest-numbered step the turn was applied
  to (the turn's targets, whether or not clamping left that one unchanged).
  A turn that changed no note leaves it alone.
- Consequence: holding empty steps and turning TRACK moves them relative to
  the last note, not relative to C4. Entering twelve bass notes on A1 is one
  27-semitone turn for the first handful and taps for the rest.
- DRUM and kit tracks never read or write it.
- Note 0 keeps its present meaning of "never set" (`pattern_toggle_step`
  already treats it so).

## Part 2: clip copy

**Gesture.** Hold a pad for at least `PAD_COPY_HOLD_MS` (500 ms, the device's
long-press time), then press another pad of the same track (same column). The
source must still be down when the target's press is handled; the time counts
from the moment the UI handled the source's press.

**Effect.**
- The source clip's `steps[track]`, `kit_steps[track]` and its effective
  length (`pattern_get_track_length`, written with `pattern_set_track_length`,
  so a stored 0 that falls back to the pattern length copies as that length)
  are written over the target location's, under the
  shared lock, then the target's prepared row is refreshed
  (`sequencer_refresh_track_playback`). Track routing and the stored `muted`
  flag are not touched (they belong to the track / the location).
- The target press does **not** launch, queue or cancel anything: the
  transport's current and queued locations are the same after it as before.
- The source is the location its pad stood for when it was pressed. If the
  shown bank is changed during the hold, the source stays that location and
  the target is the pad's location in the bank shown now, so a clip can be
  copied to another bank (`COPY A1>B3`).
- A step button already held when the copy happens stays bound to the cell it
  was pressed on (the existing rule); presses after the copy edit the target.
- In the Session view the focus moves to the target cell, so the step buttons
  edit the copy at once. On other screens the focus stays.
- The OLED shows `COPY A1>A3` for about a second (the existing notice).
- A target that already holds a clip is overwritten. The 500 ms hold is the
  guard against accidents.
- Works while playing and while stopped, in Pattern and Song mode, on every
  screen (pads are global).

**What stays as it is.**
- The source pad's own press already did what a pad press does (launch, queue,
  select, cancel). Holding it adds nothing by itself.
- Two pads of one track pressed less than 500 ms apart are two ordinary
  presses.
- A pad of another track pressed during a hold is an ordinary press.
- Several pads of the track held: the source is the one held longest.
  Hold A, press B, press C copies A to both.

**Held state.** The UI must never act on a hold that is not real. Three things
can make it unreal: the panel layer forgets its keys without sending releases
(FIFO overflow, chip reset, panic, flash stall); a release is lost while the
bus fails; and a pad can already be reported down by the panel layer while its
press is still waiting in the event ring behind another one. So a pad is a
source only when all of these hold:

1. the UI has handled a press of it and no release since
   (`pad_seen[track]` bit, set on the press event, cleared on the release
   event);
2. the panel layer reports it down now;
3. the panel layer has not recovered since that press: its sync generation is
   the one recorded at the press;
4. `now - pad_down_ms >= PAD_COPY_HOLD_MS`, with `pad_down_ms` recorded when
   the UI handled the press.

```c
// panel_input.h
// True while the pad is reported down (debounced, accepted).
bool panel_input_pad_down(uint8_t track, uint8_t row);
// Changes whenever key state may have been lost: a chip (re)initialisation,
// a lost sync, a resync, any I2C error. A hold recorded under an older value
// must not be trusted.
uint32_t panel_input_generation(void);
```

After a recovery, a panic or a save/load the source has to be pressed again.
Known limit: if the source is released in the same poll in which the target
goes down, the press is an ordinary launch.

**Copy primitive.** `pattern.c` gains the pure

```c
// Copies one track's steps, kit steps and effective length from src to dst.
// The track configuration is not copied. No-op for an invalid track or
// dst == src.
void pattern_copy_track(pattern_t *dst, const pattern_t *src, uint8_t track);
```

`session.c` gains `void session_set_focus(uint8_t track, uint8_t location)`
for the focus move (bounds-checked; the lane rule of `session_get_focus`
applies unchanged).

## Part 3: the piano roll

`display_draw_pattern_view()` keeps its signature. When the cell's track type
is `TRACK_TYPE_SYNTH` it draws the roll below; a DRUM cell keeps today's step
grid; a kit cell never reaches this function.

Layout, 128x64:

```
x 0..23   info column, four characters wide, eight text lines
x 24..127 roll: 26 semitones x 4 px; 16 steps x 4 px, step 1 at the top
```

**Roll.** Coordinates are computed signed and clipped to the roll
(`24..127`, `0..63`) before anything is drawn.
- Step `s` occupies rows `4s .. 4s+3`. Semitone `n` of the window occupies
  columns `24 + 4n .. 24 + 4n + 3`; `x` below is the first of them, `y = 4s`.
- A step that is on: a 3x3 block (`x..x+2`, `y..y+2`). With accent the block
  is 4 wide (`x..x+3`). Row `y+3` never belongs to a block.
- Slide: a line in row `y+3` from column `min(x, xn)` to `max(x, xn) + 2`,
  where `xn` is the block column of the next step, when the next step is on
  and inside the length; otherwise nothing. Tie: the single pixel
  `(x+1, y+3)`.
- A note below the window is a bar at column 24, rows `y..y+2`; above it, at
  column 127. Slides to or from such a note end at that column.
- Steps at or beyond the clip's length: a dotted line (every second pixel) in
  row `y+1` across the roll, and no notes.
- Beat lines: in row `y+3` of steps 4, 8 and 12, the pixel of every
  semitone's fourth column (`24 + 4n + 3`), drawn before the notes.
- Playing step: the roll's four rows of that step are inverted, under the
  same condition as the old view's playhead: the pattern is the one the track
  is playing, the transport is playing, and the step is inside the length.
- Held step (`held_step >= 0`): column 23, the gap at the right of the info
  column, is set for its four rows.

**Window.** 26 semitones starting at `base`:
- `lo`/`hi` are the lowest and highest note of the steps that are on and
  inside the length. With none on, `lo = hi = 60`.
- `b = lo - (25 - (hi - lo)) / 2` if `hi - lo <= 25`, else `b = lo`.
- If a step is held, on and inside the length, with note `h`:
  `b = clamp(b, h - 25, h)`, so the held note is always inside the window.
- `base = clamp(b, 0, 102)`.
- One far-off note can therefore push the rest of a line to the edge bars;
  that is accepted.
- The window is recomputed every frame; it is a pure function of the clip and
  the held step.

**Info column** (font 6x8, lines at y = 0, 8, .. 56):

| Line | Not holding | Holding a step |
|---|---|---|
| 0 | clip and track, `A2T4` | same |
| 1 | length, `L16` | same |
| 2 | blank | step, `S07` |
| 3 | blank | note name, `A#3` |
| 4 | blank | velocity, `v100` |
| 5 | blank | flags: `A` accent, `S` slide, `T` tie, in that order |
| 6 | note at the window's left edge, `A2`, `C#-1` | same |
| 7 | tempo, whole BPM, `145` | same |

The transport symbol, the mute line and the queue line of the old view are
not drawn in the roll (the pads show them). `render_pattern_view()` does not
call `display_draw_bpm()` for a SYNTH cell: the tempo is line 7.

## Constraints

- Core 0 only. No new lock. The copy takes the shared lock once, as
  `toggle_steps()` does, and refreshes playback after releasing it.
- The roll draws from the pattern without a lock, as the present view does
  (patterns are written by Core 0 only).
- No change to the save format, to `sequencer.c` playback, to the LED frame.
- `test_ui.c` fakes the panel layer; it must provide `panel_input_pad_down`.
- Another branch (`session-panel-mcp-scan`) is rewriting the panel scanner.
  The only contact is the one new accessor in `panel_input.[ch]`.

## Approaches considered

**Where the last note lives.** (a) Per track in UI RAM (chosen): a bass track
stays low and a lead stays high. (b) One global value: every switch between
tracks would start from the wrong octave. (c) In the pattern or the save:
changes the flash image for a convenience value.

**Copy gesture.** (a) Hold source, press target, with a hold time (chosen):
no new control, and the hold separates it from fast clip switching. (b) Any
overlap of two pads copies: a quick alternation between two clips of one track
would destroy one of them. (c) A modifier (an encoder button) plus pads: safe,
but the encoder buttons already mean accent, quantization and screen changes,
and a third hand is needed to hold a step as well. (d) Copy only into an empty
slot: an accidental copy could then not be repaired, because the device has no
clip clear.

**Held state.** (a) Ask the panel layer (chosen). (b) Track press and release
events in the UI: wrong after every silent release (overflow, chip reset,
resync), and a stale hold would turn a later launch into a copy.

**Roll orientation.** Time top to bottom (chosen by the owner): pitch gets the
128-px axis and a step is exactly 4 px. Left to right would align steps with
the step buttons but leave 64 px for pitch.

## Left to the builder

- Exact glyph choices in the info column if a line does not fit four
  characters.
- Whether the roll's drawing lives in one function or several.
- Test names and where in `test_ui.c` they sit.

## Review changes

An independent review of the first draft (two reviewers, one of them running
the host tests) led to these changes:

- the last note is applied after `pattern_toggle_step()`, so a last note of 0
  survives; the step that sets it is defined for joined holds and clamping;
- a copy source must have had its press handled by the UI, and the panel
  layer's sync generation must be unchanged: two pads accepted in one poll,
  or a hold left over from a bus failure, can no longer copy;
- the source is a location, fixed at its press, so a bank change during the
  hold cannot redirect it;
- the copied length is the effective one;
- the roll's playhead keeps the old view's condition; the held-step marker
  moved off the columns the edge bars use; accent no longer covers the row
  that slide and tie use; the window rule is one formula that always keeps
  the held note; the left-edge note is printed without a prefix so it fits
  four characters.

## Verification

- Host tests (`test/`): the cases listed in the plan, all existing tests
  still pass, `render_views` writes the new view to the PBM.
- Simulator: the artist score is changed to use both gestures; the count of
  encoder clicks and presses for the same 23 clips is compared with the
  present 977 clicks; the roll is shown to the owner on the simulated OLED.
