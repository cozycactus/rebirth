# Faster Note Entry, Clip Copy and a Piano Roll — Implementation Plan

Spec: `docs/superpowers/specs/2026-11-17-note-entry-copy-roll-design.md`.
Branch `claude/pattern-roll`, from `origin/master` at 20125c2.

Rules for every task: write the tests first and see them fail, then the code;
run the whole host suite before finishing (`cmake -S test -B <build dir>`,
build, `ctest --output-on-failure`); change nothing outside the files a task
names; no change to `sequencer.c`, the save format or the LED frame. Commit
each task on its own. The plan names the tests; the implementer writes the
code.

## Task 1: `pattern_copy_track` and `session_set_focus`

Files: `src/pattern.c`, `include/sequencer.h`, `src/session.c`,
`include/session.h`, `test/test_pattern.c`, `test/test_session.c`.

- `void pattern_copy_track(pattern_t *dst, const pattern_t *src, uint8_t track)`:
  copies `steps[track]`, `kit_steps[track]` and the effective length
  (`pattern_set_track_length(dst, track, pattern_get_track_length(src, track))`);
  nothing else. No-op for `track >= NUM_TRACKS` or `dst == src`.
- `void session_set_focus(uint8_t track, uint8_t location)`: ignored when
  `track >= NUM_TRACKS` or `location >= MAX_PATTERNS`.

Tests:
- `test_pattern`: a source with `track_length[track] == 0` and `length == 8`
  copies as length 8 onto a 16-step target; after a copy the step arrays and
  effective lengths of that track are equal and
  every other track, `tracks[]`, `length`, `swing` and `name` of the target
  are unchanged; an invalid track changes nothing.
- `test_session`: the focus is the cell set; invalid arguments leave it; on a
  kit track `session_get_focus().lane` is the remembered lane.

## Task 2: the last note

Files: `src/ui.c`, `test/test_ui.c`, `README.md` (Live Controls).

- `ui.last_note[NUM_TRACKS]`, set to 60 for every track in `ui_init()` after
  the `memset`.
- One helper that switches a step on and, on a SYNTH track whose step stored
  note 0 before, sets its note to the last note afterwards (0 included).
  Called from `toggle_steps()`, from the
  activation in `edit_held_steps()`, from the tap loop in
  `handle_step_grid()` and from the Step Edit long press.
- `last_note[track]` is written after an encoder turn changed at least one
  note: in `edit_held_steps()` from the lowest step of the turn's targets, in
  `handle_step_edit()` from the edited step. SYNTH tracks only.

Tests (`test_ui`):
- power-up: a Session short press on a SYNTH track gives note 60;
- hold a step, turn TRACK by -27: the step is 33; a Session short press on
  another empty step of that track then gives 33; the same on a different
  SYNTH track still gives 60;
- a Pattern view tap and the Step Edit long press seed the same way;
- hold two empty steps and turn by +2: both are last + 2 and the last note
  follows;
- a step that was on with note 45, switched off and on again, is still 45
  and does not change the last note;
- several held steps with different notes: the last note is the lowest
  step's result;
- turn a step down to note 0: the next new step is 0, not 60, and a new
  step held and turned by +2 is 2;
- S10 held, S03 joins in the same update as the turn: only S10 is edited and
  the last note is S10's;
- a DRUM track's new step takes `drum_note`; a velocity turn does not change
  the last note; a turn that clamping left without effect does not either.

## Task 3: clip copy

Files: `include/panel_input.h`, `src/panel_input.c`, `src/ui.c`,
`test/test_panel_input.c`, `test/test_ui.c`, `README.md`, `ARCHITECTURE.md`
(one paragraph under Playback Rules).

- `bool panel_input_pad_down(uint8_t track, uint8_t row)`: the `accepted`
  state of that pad's key through `key_map`; false for arguments out of range.
- `uint32_t panel_input_generation(void)`: incremented in `panel_input_init`,
  `panel_input_resync`, `lose_sync`, and wherever `chip_configured` is cleared
  after an I2C error.
- `#define PAD_COPY_HOLD_MS 500` in `ui.c`; per pad: the time, the location
  and the generation of its last handled press; per track a bit mask of pads
  whose press was handled and whose release was not.
- In `handle_panel_events()`: a pad release clears its bit (releases are
  ignored today). On a pad press: look for a source by the spec's four
  conditions among the other pads of the track (longest held wins), then
  record this press. With a source: copy under `seq_lock`, refresh the
  target's playback, move the Session focus when `ui.mode == UI_SESSION`, show
  `COPY <src>><dst>`, and do not call `session_pad()`. Without one, today's
  path.
- `test_ui.c`'s panel fake gains `panel_input_pad_down` backed by a table
  (set by a queued press, cleared by a queued release, by `reset()` and by
  the fake resync), `panel_input_generation` (bumped by the fake resync and
  settable by a test), and a way to queue a press without its release.

Tests:
- `test_panel_input`: `pad_down` is false before a press, true once the press
  is accepted, false after the release, after `panel_input_resync()` and
  after an overflow drain; the generation changes on resync, on an overflow,
  on a failed count read and on a chip found reset, and not on an ordinary
  poll.
- `test_ui`:
  - hold a pad 500 ms, press another row of the track: steps, kit steps and
    effective length are copied, the transport's current and queued locations
    are what they were before the target press, the focus is on the target,
    the notice text is `COPY A1>A3`;
  - the same onto a target that is playing, and onto one that is queued:
    still no change to current or queued;
  - at 499 ms the second press is an ordinary launch and nothing is copied;
  - two pads of one track whose presses arrive in the same update, long
    after power-up: two launches, no copy;
  - a pad of another track during the hold launches as usual;
  - the panel no longer reports the source down: no copy;
  - the generation changed since the source's press: no copy, also when the
    panel reports the pad down;
  - the source's release was handled, then it is reported down again without
    a new press event: no copy;
  - two sources held: the longer one is copied;
  - hold A1, change the bank (TRACK long), press row 3: `COPY A1>B3`, B3 holds
    A1's steps, B1 is untouched;
  - in Song mode the source press shows `SONG MODE` as today and the target
    press shows `COPY ...` and copies;
  - outside the Session view the copy happens and the focus stays;
  - a step button held since before the copy toggles its original cell on
    release; a step pressed after the copy toggles the target;
  - copying over the clip that is playing is heard (as
    `test_grid_hold_edit_while_playing_is_heard` checks a live edit);
  - a kit track's clip copies its kit steps.

## Task 4: the piano roll

Files: `src/display.c`, `src/ui.c` (`render_pattern_view`), `test/test_ui.c`
(including `render_views`), `README.md`.

- In `display_draw_pattern_view()`: a SYNTH cell draws the roll and the info
  column exactly as the spec's Part 3; a DRUM cell keeps the present drawing.
- The window base is one static function of `(pattern, track, held_step)`.
  All roll coordinates are computed as `int` and clipped before drawing.
- `render_pattern_view()` calls `display_draw_bpm()` only for a non-SYNTH
  cell.
- Existing tests that look at the old SYNTH picture
  (`test_held_step_line_is_rendered`, `test_held_step_line_shows_lowest_held_step`,
  `test_nonplaying_editor_hides_playhead`, and any other that renders the
  Pattern view of track 0-3) are rewritten for the roll; the old drawing's
  checks move to a DRUM track, where they must pass unchanged.

Tests (`test_ui`, by pixel):
- one note: its 3x3 block is at the column and rows the spec gives and the
  window is centred on it; accent makes it 4 wide and leaves row `y+3` clear;
- two notes 25 semitones apart both sit inside the window; 26 apart, the
  higher one is the bar at column 127;
- notes 0 and 127 alone: block in window column 0 and column 25;
- notes 40 and 90 with the step on 60 held: the held note is inside the
  window and both others are edge bars; a held step beyond the length does
  not move the window;
- slide draws its row from the lower to the higher column, also to an edge
  bar, and nothing when the next step is off or beyond the length; no pixel
  is written outside `24..127` for a slide from note 40 to note 100;
- tie sets its single pixel, with and without accent;
- steps beyond the length show the dotted row and no block;
- the playing step's rows are inverted only when the pattern is the playing
  one, the transport plays and the step is inside the length; stopped, or
  viewing another location, nothing is inverted;
- a held step sets column 23 on its rows and fills info lines 2-5; not
  holding leaves them blank;
- an empty clip centres the window on C4;
- info lines 0, 1, 6, 7 show the clip and track, the length, the left-edge
  note (four characters at most, `C#-1` included) and the whole tempo; no
  info text reaches column 24;
- a DRUM cell's picture: the old checks, moved to a DRUM track;
- `render_views` adds the roll (not holding, and holding a step) to the PBM.

## Task 5: documentation and the simulator

- README: Live Controls gains the copy gesture and the last-note rule; the
  Pattern view paragraph describes the roll. ARCHITECTURE: where the held
  state comes from and why.
- Outside the repository's tracked files (the simulator lives untracked in
  the main checkout): rebuild the simulator against this branch, change the
  artist to use the last note and the copy, run its checks, compare the
  gesture counts with the present ones, and show the roll on the simulated
  OLED.

## Order and review

Tasks 1, 2, 3, 4 in order (3 needs 1; 2 and 4 touch different functions of
`ui.c` but are done one after the other to avoid conflicts). Each task is
reviewed against the spec before the next starts; the whole diff gets one
independent review before a pull request is proposed.
