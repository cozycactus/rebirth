# Hold-Step Editing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In Pattern View, holding a step button and using the encoders edits that step's note, velocity and flags without leaving the grid.

**Architecture:** All input logic stays in `src/ui.c` (Core 0). New UI state tracks which steps are held, which still count as a tap, when each was pressed, and which steps an encoder button press belongs to. Step taps move from press to release. The Step Edit encoder code is extracted into one helper shared by Step Edit and the hold gesture. `display_draw_pattern_view()` gains one parameter naming the held step.

**Tech Stack:** C11, Raspberry Pi Pico SDK 2.2.0 (firmware), CMake + CTest host tests with fake peripherals (`test/`).

**Spec:** `docs/superpowers/specs/2026-11-16-hold-step-edit-design.md`

## Global Constraints

These were written for Tasks 1-3 on `48bac06`. Task 4 moves the work onto the
Session-panel `master`, where three of them change: the Performance view is
now the Session view, encoder 2 short no longer calls `toggle_transport()`,
and the host tests report `out of 11`.

- The gesture exists only in `UI_PATTERN_VIEW`. Performance, Step Edit, Pattern Select, Track Config and Song Edit keep their current behaviour; the sequencer engine, storage format and step LEDs are not touched.
- Every pattern write, taps included, happens under `seq_lock()`, and the pattern index is read under it. `sequencer_refresh_track_playback(track, pattern_index)` is called once after `seq_unlock()` whenever anything changed; activating a step counts as a change even if its values did not move. `toggle_transport()` and mode changes never run while `seq_lock()` is held.
- Nothing is added to the Core 1 timer path or under the playback lock.
- A tap is a release less than `MCP_LONG_PRESS_MS` (500 ms) after the press, measured with `to_ms_since_boot(get_absolute_time())` in the UI. `steps->long_pressed` is not used by the gesture.
- Note and velocity range is 0-127, clamped, as in Step Edit.
- The held-step screen line is at most `FONT_COLS` (21) characters.
- If a test in this plan cannot pass against an implementation that follows the spec, stop and report it. Do not change the expectation and do not bend the implementation to fit.
- Host tests: `cmake -S test -B test/build && cmake --build test/build -j && ctest --test-dir test/build --output-on-failure` must report `100% tests passed out of 7`.
- Firmware: `cmake --build build -j` must end with `Built target rp2040_sequencer`.
- Commit messages follow the repository style: one imperative sentence, e.g. `Add hold-step editing to Pattern View`.

## Review Focus

1. A step is released before the encoder button that was pressed during its hold (short and long press, both encoders): the step is not toggled, the encoder event edits that step, the UI mode does not change.
2. Press and release of one step arrive in the same `ui_update()`: the step toggles exactly once.
3. A turn arrives in the same `ui_update()` as a step's release (it is an edit) or as its first press (it is not).
4. Turning past the end of the range: the value clamps without wrapping, and a turn that leaves the value unchanged still cancels the tap.
5. Holding an empty step on a drum track and turning encoder 2: the hit is created with the track's `drum_note` and the adjusted velocity.

Each of these has a test in Task 1.

---

### Task 1: Hold gesture input in Pattern View

**Files:**
- Modify: `src/ui.c` — `ui_state_t` (lines 29-61), `handle_panic_combo` (94-111), `handle_pattern_view` (216-272), `handle_step_edit` (276-349), `ui_update` (909-962)
- Modify: `test/CMakeLists.txt` — `test_ui` target (lines 78-89)
- Test: `test/test_ui.c`

**Interfaces:**
- Consumes: `step_buttons_t` from `include/mcp23017.h` (`pressed` and `released` are edge events cleared at the end of `ui_update()`; `held` is the sampled state), `encoder_button_held()`, `pattern_toggle_step()`, `sequencer_refresh_track_playback()`.
- Produces: `ui.grid_hold` (`uint16_t`, bit n = step n is held since a press seen in Pattern View). Task 2 reads it in `ui_render()`.

- [ ] **Step 1: Let the UI tests reach the timer handler, add helpers, fix the bare-press test**

In `test/test_ui.c` add `#include "../src/sequencer.c"` after `#include "../src/ui.c"`, and remove `${REBIRTH_SRC}/sequencer.c` from the `test_ui` sources in `test/CMakeLists.txt` (`test_playback.c` already includes it this way).

Add after `press_encoder()`:

```c
#define STEP(track, step) g_seq.patterns[0].steps[track][step]

static void step_down(uint16_t mask) {
    step_buttons.pressed |= mask;
    step_buttons.held |= mask;
    ui_update();
}

static void step_up(uint16_t mask) {
    step_buttons.held &= (uint16_t)~mask;
    step_buttons.released |= mask;
    ui_update();
}

static void turn(uint8_t encoder, int8_t delta) {
    deltas[encoder] = delta;
    ui_update();
}

// The real driver reports a short press when the button is released and a
// long press while it is still down.
static void encoder_down(uint8_t encoder) {
    held[encoder] = true;
    ui_update();
}

static void encoder_up(uint8_t encoder, button_event_t event) {
    held[encoder] = false;
    press_encoder(encoder, event);
}

static void encoder_short(uint8_t encoder) {
    encoder_down(encoder);
    encoder_up(encoder, BTN_PRESS);
}

static void encoder_long(uint8_t encoder) {
    encoder_down(encoder);
    press_encoder(encoder, BTN_LONG_PRESS);
    encoder_up(encoder, BTN_RELEASE);
}

static void enter_pattern_view(void) {
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
}

static void tick(void) {
    timer_hw->timerawl = timer_hw->alarm[0];
    sequencer_timer_isr();
}

static unsigned count_note_on(uint8_t note, uint8_t velocity) {
    unsigned found = 0;
    for (unsigned i = 0; i + 2 < fake_midi_length; i++) {
        if (fake_midi[i] == MIDI_NOTE_ON && fake_midi[i + 1] == note &&
            fake_midi[i + 2] == velocity) found++;
    }
    return found;
}
```

In `test_editor_follows_performance_selection`, replace the Pattern View lines `step_buttons.pressed = 1; ui_update();` with `step_down(1); step_up(1);`. The earlier `step_buttons.pressed = 1u << 13;` in that test is a Performance-view press and stays.

Run: `cmake -S test -B test/build && cmake --build test/build -j && ctest --test-dir test/build --output-on-failure`
Expected: `100% tests passed out of 7` (the helpers are unused so far; a warning about that is fine).

- [ ] **Step 2: Write the failing tests**

Add these tests and a `RUN(...)` line for each in `main()`:

```c
static void test_grid_tap_toggles_on_release(void) {
    enter_pattern_view();
    step_down(1u << 4);
    CHECK(STEP(0, 4).flags == 0);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
    step_down(1u << 4);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == 0);
}

static void test_grid_hold_turn_edits_note_without_toggling(void) {
    enter_pattern_view();
    step_down(1u << 4);
    step_up(1u << 4);                       // active, note 60, velocity 100
    step_down(1u << 4);
    for (int i = 0; i < 50; i++) ui_update();   // idle updates keep the hold
    turn(0, 3);
    CHECK(STEP(0, 4).note == 63);
    CHECK(ui.selected_track == 0);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
    turn(0, 1);                             // navigation is back
    CHECK(ui.selected_track == 1);
    CHECK(STEP(0, 4).note == 63);
}

static void test_grid_hold_turn_edits_velocity_not_tempo(void) {
    enter_pattern_view();
    step_down(1u << 4);
    step_up(1u << 4);
    float bpm = g_seq.transport.bpm;
    step_down(1u << 4);
    turn(1, -10);
    CHECK(STEP(0, 4).velocity == 90);
    CHECK(g_seq.transport.bpm == bpm);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
}

static void test_grid_hold_turn_creates_empty_step(void) {
    enter_pattern_view();
    step_down(1u << 7);
    turn(0, 2);
    CHECK(STEP(0, 7).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(0, 7).note == 62);
    CHECK(STEP(0, 7).velocity == 100);
    step_up(1u << 7);
    CHECK(STEP(0, 7).flags == STEP_FLAG_ACTIVE);
}

static void test_grid_hold_time_decides_tap_or_peek(void) {
    enter_pattern_view();
    step_down(1u << 4);
    sleep_ms(MCP_LONG_PRESS_MS - 1);
    step_up(1u << 4);                       // 499 ms: still a tap
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
    step_down(1u << 4);
    sleep_ms(MCP_LONG_PRESS_MS);
    step_up(1u << 4);                       // 500 ms: peek, the step stays on
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
    step_down(1u << 4);
    sleep_ms(MCP_LONG_PRESS_MS);
    turn(0, 3);                             // a long hold still edits
    CHECK(STEP(0, 4).note == 63);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
}

static void test_grid_tap_beside_a_held_step(void) {
    enter_pattern_view();
    step_down(1u << 2);
    sleep_ms(MCP_LONG_PRESS_MS);
    step_down(1u << 9);
    step_up(1u << 9);
    CHECK(STEP(0, 9).flags == STEP_FLAG_ACTIVE);
    step_up(1u << 2);
    CHECK(STEP(0, 2).flags == 0);
}

static void test_grid_two_held_steps_edit_independently(void) {
    enter_pattern_view();
    pattern_set_step(&g_seq.patterns[0], 0, 2, 60, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(&g_seq.patterns[0], 0, 9, 72, 40,
                     STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT);
    step_down((1u << 2) | (1u << 9));
    turn(0, 1);
    CHECK(STEP(0, 2).note == 61);
    CHECK(STEP(0, 9).note == 73);
    turn(1, -10);
    CHECK(STEP(0, 2).velocity == 90);
    CHECK(STEP(0, 9).velocity == 30);
    encoder_short(0);
    CHECK(STEP(0, 2).flags == (STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT));
    CHECK(STEP(0, 9).flags == STEP_FLAG_ACTIVE);
    step_up((1u << 2) | (1u << 9));
    CHECK(STEP(0, 2).flags == (STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT));
    CHECK(STEP(0, 9).flags == STEP_FLAG_ACTIVE);
}

static void test_grid_hold_leaves_unheld_steps_alone(void) {
    enter_pattern_view();
    pattern_set_step(&g_seq.patterns[0], 0, 3, 40, 60, STEP_FLAG_ACTIVE);
    pattern_set_step(&g_seq.patterns[0], 1, 4, 50, 70, STEP_FLAG_ACTIVE);
    step_down(1u << 4);
    turn(0, 5);
    encoder_short(0);
    CHECK(STEP(0, 3).note == 40);
    CHECK(STEP(0, 3).velocity == 60);
    CHECK(STEP(0, 3).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(1, 4).note == 50);
    CHECK(STEP(1, 4).velocity == 70);
    CHECK(STEP(1, 4).flags == STEP_FLAG_ACTIVE);
    step_up(1u << 4);
}

static void test_grid_hold_edits_the_edited_pattern_not_the_playing_one(void) {
    enter_pattern_view();
    g_seq.active_pattern[0] = 3;            // editor on A4 while A1 plays
    pattern_set_step(&g_seq.patterns[3], 0, 4, 60, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(&g_seq.patterns[0], 0, 4, 70, 100, STEP_FLAG_ACTIVE);
    step_down(1u << 4);
    turn(0, 3);
    CHECK(g_seq.patterns[3].steps[0][4].note == 63);
    CHECK(STEP(0, 4).note == 70);
    step_up(1u << 4);
}

static void test_grid_hold_encoder_presses_set_flags(void) {
    enter_pattern_view();
    step_down(1u << 4);
    encoder_short(0);
    CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT));
    CHECK(STEP(0, 4).note == 60);
    CHECK(STEP(0, 4).velocity == 100);
    encoder_long(1);
    CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT | STEP_FLAG_TIE));
    encoder_long(0);                        // slide clears tie
    CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT | STEP_FLAG_SLIDE));
    CHECK(ui.mode == UI_PATTERN_VIEW);
    encoder_short(1);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT | STEP_FLAG_SLIDE));
    CHECK(ui.mode == UI_PATTERN_VIEW);
}

// Review Focus 1
static void test_grid_encoder_press_outlives_step_release(void) {
    enter_pattern_view();
    step_down(1u << 4);
    step_up(1u << 4);                       // active
    step_down(1u << 4);
    encoder_down(0);
    step_up(1u << 4);                       // the step is released first
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
    encoder_up(0, BTN_PRESS);
    CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT));
    CHECK(ui.mode == UI_PATTERN_VIEW);
    encoder_short(0);                       // an ordinary press navigates again
    CHECK(ui.mode == UI_STEP_EDIT);
}

// Review Focus 1
static void test_grid_encoder_long_press_outlives_step_release(void) {
    enter_pattern_view();
    step_down(1u << 4);
    step_up(1u << 4);
    step_down(1u << 4);
    encoder_down(1);
    step_up(1u << 4);
    press_encoder(1, BTN_LONG_PRESS);       // fires while the encoder is still down
    CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | STEP_FLAG_TIE));
    CHECK(ui.mode == UI_PATTERN_VIEW);
    encoder_up(1, BTN_RELEASE);
    CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | STEP_FLAG_TIE));
    CHECK(ui.mode == UI_PATTERN_VIEW);
}

// Review Focus 1
static void test_grid_play_during_hold_cancels_the_tap(void) {
    enter_pattern_view();
    step_down(1u << 4);
    encoder_down(1);
    step_up(1u << 4);
    encoder_up(1, BTN_PRESS);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    CHECK(STEP(0, 4).flags == 0);           // neither created nor toggled
    CHECK(ui.mode == UI_PATTERN_VIEW);
}

// Review Focus 1
static void test_grid_track_is_frozen_while_an_encoder_owns_the_gesture(void) {
    enter_pattern_view();
    step_down(1u << 4);
    encoder_down(0);
    step_up(1u << 4);
    turn(0, 1);                             // encoder 1 is still down
    CHECK(ui.selected_track == 0);
    encoder_up(0, BTN_PRESS);
    CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT));
    CHECK(STEP(1, 4).flags == 0);
    turn(0, 1);
    CHECK(ui.selected_track == 1);
}

static void test_grid_ignores_button_held_before_entering(void) {
    step_down(1u << 0);                     // Performance: mutes track 1
    CHECK(g_seq.track_mutes == 1);
    enter_pattern_view();                   // button 1 is still down
    turn(0, 1);
    CHECK(ui.selected_track == 1);          // not a hold gesture
    step_up(1u << 0);
    CHECK(STEP(0, 0).flags == 0);
    CHECK(STEP(1, 0).flags == 0);
}

static void test_grid_panic_forgets_held_steps(void) {
    enter_pattern_view();
    step_down(1u << 4);
    held[0] = held[1] = true;
    press_encoder(0, BTN_LONG_PRESS);       // Panic returns to Performance
    CHECK(ui.mode == UI_PERFORMANCE);
    held[0] = held[1] = false;
    ui_update();                            // unlatch
    CHECK(!ui.panic_latched);
    enter_pattern_view();                   // step 5 is still physically down
    turn(0, 1);
    CHECK(ui.selected_track == 1);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == 0);
    CHECK(STEP(1, 4).flags == 0);
}

static void test_grid_hold_edit_reaches_playback(void) {
    enter_pattern_view();
    step_down(1u << 0);
    turn(0, 100);                           // step 1 created at note 127
    step_up(1u << 0);
    step_down(1u << 0);
    step_up(1u << 0);                       // tap: off, the note stays 127
    step_down(1u << 0);
    turn(0, 5);                             // on again; the value did not change
    step_up(1u << 0);
    CHECK(STEP(0, 0).flags == STEP_FLAG_ACTIVE);
    transport_start();
    fake_midi_length = 0;
    timer_hw->alarm[0] = pulse_interval_us;
    tick();
    CHECK(count_note_on(127, 100) == 1);
}

static void test_grid_hold_edit_while_playing_is_heard(void) {
    enter_pattern_view();
    transport_start();
    timer_hw->alarm[0] = pulse_interval_us;
    tick();                                 // step 1 plays; nothing is on it
    step_down(1u << 1);
    turn(0, 2);                             // create step 2 while playing
    step_up(1u << 1);
    fake_midi_length = 0;
    for (unsigned i = 0; i < PULSES_PER_STEP; i++) tick();
    CHECK(count_note_on(62, 100) == 1);
}

// Review Focus 2
static void test_grid_tap_within_one_update_toggles_once(void) {
    enter_pattern_view();
    step_buttons.pressed = step_buttons.released = 1u << 4;
    ui_update();
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
}

// Review Focus 3
static void test_grid_turn_and_release_in_one_update_is_an_edit(void) {
    enter_pattern_view();
    step_down(1u << 4);
    deltas[0] = 2;
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(0, 4).note == 62);
    CHECK(ui.selected_track == 0);
}

// Review Focus 3
static void test_grid_turn_in_the_same_update_as_the_press_is_not_an_edit(void) {
    enter_pattern_view();
    deltas[0] = 2;
    step_down(1u << 4);
    CHECK(STEP(0, 4).flags == 0);
    CHECK(ui.selected_track == 0);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);    // still a tap
    CHECK(STEP(0, 4).note == 60);
}

// Review Focus 4
static void test_grid_hold_turn_clamps_and_still_cancels_tap(void) {
    enter_pattern_view();
    step_down(1u << 4);
    turn(0, 100);
    CHECK(STEP(0, 4).note == 127);
    step_up(1u << 4);
    step_down(1u << 4);
    turn(0, 5);                             // already at the top: value unchanged
    CHECK(STEP(0, 4).note == 127);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
    step_down(1u << 4);
    turn(1, -127);
    CHECK(STEP(0, 4).velocity == 0);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);
}

// Review Focus 5
static void test_grid_hold_creates_drum_hit_with_kit_note(void) {
    enter_pattern_view();
    turn(0, 4);                             // track 5: drum, drum_note 40
    CHECK(ui.selected_track == 4);
    step_down(1u << 3);
    turn(1, -10);
    CHECK(STEP(4, 3).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(4, 3).note == 40);
    CHECK(STEP(4, 3).velocity == 90);
    step_up(1u << 3);
    CHECK(STEP(4, 3).flags == STEP_FLAG_ACTIVE);
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build test/build -j && ctest --test-dir test/build --output-on-failure -R ui_tests`
Expected: `ui_tests` FAILS; the first failing line is the `CHECK(STEP(0, 4).flags == 0)` after `step_down` in `test_grid_tap_toggles_on_release` (the step toggles on press today).

- [ ] **Step 4: Add the hold state to `ui_state_t`**

```c
    // PATTERN_VIEW hold gesture
    uint16_t grid_hold;                         // Steps pressed in Pattern View and still down
    uint16_t grid_tap_pending;                  // Subset that still toggles on release
    uint32_t grid_press_ms[NUM_STEP_BUTTONS];   // When each held step was pressed
    uint16_t grid_enc_target[NUM_ENCODERS];     // Steps held while that encoder's button was down
```

- [ ] **Step 5: Extract the shared edit helpers**

Add above `handle_pattern_view`:

```c
// Applies Step Edit encoder input to one step. Returns true if the step changed.
static bool apply_step_edit(step_t *st, int8_t e1d, int8_t e2d,
                            button_event_t e1b, button_event_t e2b);

// For every step in mask: activate it if inactive, then apply_step_edit().
// Returns true if any step changed; an activation counts. Caller holds seq_lock().
static bool edit_held_steps(pattern_t *pat, uint8_t track, uint16_t mask,
                            int8_t e1d, int8_t e2d,
                            button_event_t e1b, button_event_t e2b);
```

The body of `apply_step_edit` is the note, velocity, accent, slide and tie code now in `handle_step_edit` (lines 286-318), moved without behaviour change. `handle_step_edit` calls it for `ui.edit_step` inside its existing `seq_lock()` section as `if (apply_step_edit(st, e1d, e2d, e1b, e2b)) changed = true;`; Step Edit still does not activate steps, and everything else in it stays.

- [ ] **Step 6: Rewrite the step-button and encoder handling in `handle_pattern_view`**

The order of operations is what the tests cannot fully determine, so it is fixed here:

```c
uint32_t now = to_ms_since_boot(get_absolute_time());

// 1. Steps already held when this update began (still down, or released just
//    now). Rotation edits only these.
uint16_t turn_targets = ui.grid_hold & (uint16_t)(steps->held | steps->released);

// 2. Holds that start in this view; set ui.grid_press_ms[s] = now for each
//    bit of steps->pressed.
ui.grid_hold |= steps->pressed;
ui.grid_tap_pending |= steps->pressed;

// 3. Keep only buttons that are down now or were released in this update.
ui.grid_hold &= (uint16_t)(steps->held | steps->released);
ui.grid_tap_pending &= ui.grid_hold;

// 4. An encoder button that is down while steps are held joins the gesture.
for (uint8_t enc = 0; enc < NUM_ENCODERS; enc++) {
    if (encoder_button_held(enc) && ui.grid_hold) {
        ui.grid_enc_target[enc] |= ui.grid_hold;
        ui.grid_tap_pending &= (uint16_t)~ui.grid_hold;
    }
}
uint16_t e1_targets = ui.grid_enc_target[0] | ui.grid_hold;
uint16_t e2_targets = ui.grid_enc_target[1] | ui.grid_hold;
bool gesture = ui.grid_hold || ui.grid_enc_target[0] || ui.grid_enc_target[1];
```

5. Dispatch each input by this table. "Edit" means `edit_held_steps()` on the named targets and clearing those bits in `ui.grid_tap_pending`.

| Input | Its targets are non-empty | No targets and `gesture` is false | No targets and `gesture` is true |
| --- | --- | --- | --- |
| `e1d`, `e2d` | edit `turn_targets` (note, velocity) | track select, tempo, as today | ignored |
| `e1b == BTN_PRESS` | edit `e1_targets` (accent) | enter Step Edit, as today | ignored |
| `e1b == BTN_LONG_PRESS` | edit `e1_targets` (slide) | back to Performance, as today | ignored |
| `e2b == BTN_LONG_PRESS` | edit `e2_targets` (tie) | open Pattern Select, as today | ignored |
| `e2b == BTN_PRESS` | `toggle_transport()` in every case | | |
| `BTN_RELEASE` | nothing in every case | | |

6. Taps: `pattern_toggle_step()` for each step in `steps->released & ui.grid_tap_pending` whose `(uint32_t)(now - ui.grid_press_ms[s])` is less than `MCP_LONG_PRESS_MS`.
7. `ui.grid_hold` and `ui.grid_tap_pending` drop `steps->released`.
8. An encoder whose button is up gives up its targets: `if (!encoder_button_held(enc)) ui.grid_enc_target[enc] = 0;`. This runs after the dispatch, because a short press arrives in the update where the button is already up.

- [ ] **Step 7: Forget the gesture outside Pattern View**

`ui.grid_hold`, `ui.grid_tap_pending` and both `ui.grid_enc_target` entries are zero whenever `ui.mode != UI_PATTERN_VIEW` at the end of `ui_update()`, including the early return taken by Panic. Put the clearing next to the existing `ui.select_short_pending` resets (`handle_panic_combo` and step 6 of `ui_update`).

- [ ] **Step 8: Run all host tests**

Run: `cmake --build test/build -j && ctest --test-dir test/build --output-on-failure`
Expected: `100% tests passed out of 7`; the `ui_tests` output lists a `PASS` line for each new test.

- [ ] **Step 9: Commit**

```bash
git add src/ui.c test/test_ui.c test/CMakeLists.txt
git commit -m "Add hold-step editing to Pattern View"
```

---

### Task 2: Held-step line on the screen and documentation

**Files:**
- Modify: `include/sequencer.h:268`, `src/display.c` (`display_draw_pattern_view`, lines 235-295), `src/ui.c` (`ui_render`, lines 849-855), `README.md` (Live Controls)
- Test: `test/test_ui.c` (new tests; existing call sites in `test_nonplaying_editor_hides_playhead`)

**Interfaces:**
- Consumes: `ui.grid_hold` from Task 1, and the helpers `step_down`, `step_up`, `turn`, `enter_pattern_view` added to `test/test_ui.c` in Task 1.
- Produces: `void display_draw_pattern_view(const pattern_t *p, uint8_t track, uint8_t step, int8_t held_step);` — `held_step` is a step index 0-15 whose values replace the track info line and which is marked in the grid; any other value (`-1` by convention) means none and renders exactly what the function renders today.

- [ ] **Step 1: Write the failing tests**

Add to `test/test_ui.c` with a `RUN(...)` line each, and pass `-1` as the new fourth argument in the two existing `display_draw_pattern_view` calls of `test_nonplaying_editor_hides_playhead`. The info line occupies rows 10-17; the grid bracket of step index n is drawn at x = 8n, y = 33.

```c
static void render(void) {
    display_clear();
    ui_render();
}

// Compares rows 10-17 of the current frame with the given text drawn alone.
// Clears the display.
static void check_info_line(const char *expected) {
    bool actual[FONT_CHAR_H][OLED_WIDTH];
    for (unsigned y = 0; y < FONT_CHAR_H; y++) {
        for (unsigned x = 0; x < OLED_WIDTH; x++) actual[y][x] = ssd1309_get_pixel(x, 10 + y);
    }
    display_clear();
    ssd1309_draw_string(0, 10, expected, false);
    for (unsigned y = 0; y < FONT_CHAR_H; y++) {
        for (unsigned x = 0; x < OLED_WIDTH; x++) CHECK(actual[y][x] == ssd1309_get_pixel(x, 10 + y));
    }
}

static void test_held_step_line_is_rendered(void) {
    uint8_t idle[OLED_BUF_SIZE];
    enter_pattern_view();
    render();
    memcpy(idle, ssd1309_get_framebuffer(), sizeof(idle));
    step_down(1u << 4);                     // shown from the first moment of the hold
    render();
    CHECK(ssd1309_get_pixel(32, 33));       // bracket under step 5
    check_info_line("S05 Note:C-1  Vel:  0");
    sleep_ms(MCP_LONG_PRESS_MS);
    step_up(1u << 4);                       // peek: nothing changed
    render();
    CHECK(memcmp(idle, ssd1309_get_framebuffer(), sizeof(idle)) == 0);
}

static void test_held_step_line_shows_lowest_held_step(void) {
    enter_pattern_view();
    step_down(1u << 9);
    turn(0, -59);                           // step 10: note 1
    turn(1, 27);                            // velocity 127
    render();
    check_info_line("S10 Note:C#-1 Vel:127");
    step_down(1u << 2);                     // a lower step joins
    render();
    CHECK(ssd1309_get_pixel(16, 33));       // bracket under step 3 only
    CHECK(!ssd1309_get_pixel(72, 33));
    check_info_line("S03 Note:C-1  Vel:  0");
    step_up(1u << 2);
    render();
    CHECK(ssd1309_get_pixel(72, 33));
    check_info_line("S10 Note:C#-1 Vel:127");
    step_up(1u << 9);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build test/build -j`
Expected: compile error in `test_ui.c` — too many arguments to `display_draw_pattern_view`.

- [ ] **Step 3: Add the `held_step` parameter**

Change the declaration in `include/sequencer.h` and the definition in `src/display.c` to the signature in Interfaces. When `held_step` is 0-15, the line at y = 10 is

```c
snprintf(info, sizeof(info), "S%02d Note:%-4s Vel:%3d", held_step + 1, note_str, st->velocity);
```

with `note_str` from `note_to_str()` and `st` the held step, and the grid is drawn with `draw_step_grid(p, track, step, held_step)`. For any other value the function behaves as it does today.

- [ ] **Step 4: Pass the held step from `ui_render`**

In the `UI_PATTERN_VIEW` case pass the index (0-15) of the lowest set bit of `ui.grid_hold` as `held_step`, or `-1` when `ui.grid_hold` is zero. The index, not the bit's value.

- [ ] **Step 5: Document the gesture**

In `README.md` under "Live Controls", add after the Performance bullets:

```markdown
- Pattern view: a tap shorter than half a second toggles a step when the
  button is released. Hold one or more steps and turn encoder 1 for note or
  encoder 2 for velocity; encoder 1 short/long toggles accent/slide and
  encoder 2 long toggles tie on the held steps. The first edit of a held empty
  step creates it. A step held longer, or held while an encoder is turned or
  pressed, is not toggled on release. Encoder 2 short is still start/stop;
  track, tempo and mode changes are suspended while a step is held.
```

- [ ] **Step 6: Run host tests and build the firmware**

Run: `cmake --build test/build -j && ctest --test-dir test/build --output-on-failure`
Expected: `100% tests passed out of 7`.

Run: `cmake --build build -j`
Expected: ends with `Built target rp2040_sequencer`.

- [ ] **Step 7: Commit**

```bash
git add include/sequencer.h src/display.c src/ui.c test/test_ui.c README.md
git commit -m "Show held step values in Pattern View"
```

---

### Task 3: Final review fixes

Three reviewers read the branch after Tasks 1 and 2. Each found one place where `handle_pattern_view` departs from the spec, and several tests that a wrong implementation would pass. The spec was amended in Behaviour 2 and 3; read those two items again before starting.

**Files:**
- Modify: `src/ui.c` — `ui_state_t`, `clear_grid_gesture`, `handle_pattern_view`
- Test: `test/test_ui.c`

**Interfaces:**
- Consumes: the state and helpers Task 1 added (`ui.grid_hold`, `ui.grid_tap_pending`, `ui.grid_enc_target[]`, `edit_held_steps()`; test helpers `step_down`, `step_up`, `turn`, `encoder_down`, `encoder_up`, `enter_pattern_view`, `tick`, `STEP`).
- Produces: nothing new for other code.

**Findings to fix:**

1. *A long press does not reach steps that join later.* `BTN_LONG_PRESS` arrives once, while the encoder button is still down. A step pressed after that, with the button still down, enters `ui.grid_enc_target[enc]` and loses its tap but receives no edit. Required: once `BTN_LONG_PRESS` has edited an encoder's targets, every step that enters `ui.grid_enc_target[enc]` afterwards, while that button stays down, receives the same long-press edit exactly once, in the update in which it joins. A step that already received it is not toggled again if it is released and pressed again. The record of who received it is dropped wherever `ui.grid_enc_target[enc]` is dropped.
2. *An encoder event edits a step it does not belong to.* The targets of an encoder button event are `ui.grid_enc_target[enc] | ui.grid_hold`. When a short press completes (button already up) in the same update in which a new step is first seen down, that step is edited and its tap is cancelled. Required: the targets of a button event are `ui.grid_enc_target[enc]` alone. The mask is already filled while the button is down and survives until after the dispatch.
3. *A turn in the same update as a first press leaves the tap pending.* Required: when `e1d` or `e2d` is non-zero, the steps in `steps->pressed` lose their `ui.grid_tap_pending` bit. They are still not edited, and the turn still does not navigate.

- [ ] **Step 1: Write the failing tests**

In `test/test_ui.c`, replace `test_grid_turn_in_the_same_update_as_the_press_is_not_an_edit` (and its `RUN` line) with:

```c
static void test_grid_turn_in_the_same_update_as_the_press_cancels_the_tap(void) {
    enter_pattern_view();
    deltas[0] = 2;
    step_down(1u << 4);
    CHECK(STEP(0, 4).flags == 0);
    CHECK(ui.selected_track == 0);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == 0);           // neither edited nor toggled
    CHECK(STEP(0, 4).note == 0);
}
```

Add these tests with a `RUN(...)` line each:

```c
static void test_grid_long_press_applies_to_later_steps(void) {
    for (uint8_t enc = 0; enc < NUM_ENCODERS; enc++) {
        reset();
        enter_pattern_view();
        uint8_t flag = enc == 0 ? STEP_FLAG_SLIDE : STEP_FLAG_TIE;
        step_down(1u << 4);
        encoder_down(enc);
        press_encoder(enc, BTN_LONG_PRESS);
        CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | flag));
        step_up(1u << 4);
        step_down(1u << 9);                 // the same encoder button is still down
        CHECK(STEP(0, 9).flags == (STEP_FLAG_ACTIVE | flag));
        for (unsigned i = 0; i < 3; i++) ui_update();
        CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | flag));
        CHECK(STEP(0, 9).flags == (STEP_FLAG_ACTIVE | flag));
        step_down(1u << 4);                 // an earlier target is not toggled twice
        step_up((1u << 4) | (1u << 9));
        encoder_up(enc, BTN_RELEASE);
        CHECK(STEP(0, 4).flags == (STEP_FLAG_ACTIVE | flag));
        CHECK(STEP(0, 9).flags == (STEP_FLAG_ACTIVE | flag));
        CHECK(ui.mode == UI_PATTERN_VIEW);
        turn(0, 1);
        CHECK(ui.selected_track == 1);
    }
}

static void test_grid_encoder_press_ending_as_a_step_goes_down_is_not_part_of_it(void) {
    enter_pattern_view();
    encoder_down(0);                        // no step is held yet
    held[0] = false;
    buttons[0] = BTN_PRESS;                 // the short press completes ...
    step_down(1u << 9);                     // ... in the update that first sees step 10 down
    CHECK(STEP(0, 9).flags == 0);           // not created, not accented
    CHECK(ui.mode == UI_PATTERN_VIEW);
    step_up(1u << 9);
    CHECK(STEP(0, 9).flags == STEP_FLAG_ACTIVE);    // an ordinary tap
    CHECK(STEP(0, 9).note == 60);
}

static void test_grid_tap_edits_selected_track_and_pattern(void) {
    enter_pattern_view();
    turn(0, 4);                             // track 5
    CHECK(ui.selected_track == 4);
    g_seq.active_pattern[4] = 3;            // editor on A4 while A1 plays
    step_down(1u << 3);
    step_up(1u << 3);
    CHECK(g_seq.patterns[3].steps[4][3].flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(4, 3).flags == 0);
    CHECK(STEP(0, 3).flags == 0);
}

static void test_grid_driver_long_press_event_does_not_toggle(void) {
    enter_pattern_view();
    step_down(1u << 4);
    sleep_ms(MCP_LONG_PRESS_MS);
    step_buttons.long_pressed = 1u << 4;    // what mcp23017 emits at 500 ms
    ui_update();
    CHECK(STEP(0, 4).flags == 0);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == 0);           // peek, not a tap
}

static void test_grid_hold_edit_refreshes_the_edited_track_and_pattern(void) {
    turn(0, 4);                             // Performance: track 5
    step_down(1u << 11);                    // slot 4: A4 is selected at once while stopped
    step_up(1u << 11);
    CHECK(g_seq.transport.current_pattern[4] == 3);
    enter_pattern_view();
    step_down(1u << 0);
    turn(1, -10);                           // drum hit at velocity 90
    step_up(1u << 0);
    CHECK(g_seq.patterns[3].steps[4][0].flags == STEP_FLAG_ACTIVE);
    transport_start();
    fake_midi_length = 0;
    timer_hw->alarm[0] = pulse_interval_us;
    tick();
    unsigned found = 0;
    for (unsigned i = 0; i + 2 < fake_midi_length; i++) {
        if (fake_midi[i] == (MIDI_NOTE_ON | 4) && fake_midi[i + 1] == 40 &&
            fake_midi[i + 2] == 90) found++;
    }
    CHECK(found == 1);
}

static void test_step_edit_note_and_velocity_still_work(void) {
    press_encoder(0, BTN_PRESS);            // Pattern View
    press_encoder(0, BTN_PRESS);            // Step Edit, step 1
    CHECK(ui.mode == UI_STEP_EDIT);
    turn(0, 3);
    CHECK(STEP(0, 0).note == 3);
    turn(1, 10);
    CHECK(STEP(0, 0).velocity == 10);
    CHECK(STEP(0, 0).flags == 0);           // Step Edit does not activate a step
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build test/build -j && ctest --test-dir test/build --output-on-failure -R ui_tests`
Expected: `ui_tests` FAILS. Run `test/build/test_ui` and note which of the new tests fail before any code change: the three that cover the findings must fail (`..._cancels_the_tap`, `..._applies_to_later_steps`, `..._is_not_part_of_it`); the other four pin behaviour that is already correct and are expected to pass. To see each of the three fail, comment out the earlier failing `RUN` line temporarily, and restore it.

- [ ] **Step 3: Fix the three findings in `handle_pattern_view`**

Implement the three "Required" statements above. The state that records which targets already received a long press is yours to name; it lives in `ui_state_t` next to `grid_enc_target` and is cleared in `clear_grid_gesture()` and wherever `ui.grid_enc_target[enc]` is reset. Keep every pattern write under the existing `seq_lock()` section and the single refresh after it.

- [ ] **Step 4: Run all host tests and build the firmware**

Run: `cmake --build test/build -j && ctest --test-dir test/build --output-on-failure`
Expected: `100% tests passed out of 7`.

Run: `cmake --build build -j`
Expected: ends with `Built target rp2040_sequencer`.

- [ ] **Step 5: Commit**

```bash
git add src/ui.c test/test_ui.c
git commit -m "Fix hold-step issues found in the multi-model review"
```


### Task 4: Port to the Session-panel master

Tasks 1-3 were built on `48bac06`. `origin/master` has since gained the
Session view, the panel keys and a reworked step-button driver, and conflicts
with this branch in `README.md`, `include/sequencer.h`, `src/display.c`,
`src/ui.c` and `test/test_ui.c`. This task merges `origin/master` into the
branch. The spec's "Port to master" section is the authority on behaviour.

**Files:**
- Merge: `origin/master` into `claude/hold-step-edit` (one merge commit)
- Resolve: `README.md`, `include/sequencer.h`, `src/display.c`, `src/ui.c`, `test/test_ui.c`
- Test: `test/test_ui.c`

**Port rules:**

1. `master` is the baseline for everything except the gesture. In every
   conflict take `master`'s side, then add the gesture to it. After the merge
   `git diff origin/master HEAD` must show the gesture, its tests and its docs
   and nothing else: no line of `master` reverted, reworded or reordered.
2. `handle_pattern_view`: keep `master`'s five navigation actions exactly as
   `master` has them (encoder 1 turn = track, encoder 2 turn = tempo, encoder 1
   short = Step Edit with `ui.step_edit_return = UI_PATTERN_VIEW`, encoder 1
   long = `UI_SESSION`, encoder 2 long = Pattern Select) and run them only when
   no gesture is active. Remove `master`'s toggle-on-press call. The gesture
   logic is the branch's, unchanged.
3. Encoder 2 short does not call `toggle_transport()` in Pattern View, with or
   without a hold. During a hold it only cancels the tap.
4. Gesture state is cleared in `drop_step_gestures()`. There is no separate
   `clear_grid_gesture()` and no extra clearing in `ui_update()`: `master`
   already calls `drop_step_gestures()` on a screen change, on Panic and after
   a flash save or load.
5. Step Edit uses the shared `apply_step_edit()`; `master`'s
   `ui.mode = ui.step_edit_return` stays.
6. `display_draw_pattern_view()` keeps the fourth parameter `int8_t held_step`
   from Task 2.
7. The gesture does not use `toggle_steps()`: edits and taps stay in one
   `seq_lock()` section followed by one refresh, as on the branch.

**Tests:**

- Harness: keep `master`'s (`reset`, `tick`, `pulses`, panel fakes,
  `press_step`, `long_step`, `release_step`, `panel`, `patterns_before`,
  `render_views`). Add the branch helpers that `master` lacks (`STEP`,
  `step_down`, `step_up`, `turn`, `encoder_down`, `encoder_up`,
  `encoder_short`, `encoder_long`, `enter_pattern_view`, `count_note_on`,
  `render`, `check_info_line`); drop the branch's duplicate `tick`.
- Every test that exists on `master` stays exactly as on `master`, with its
  `RUN` line. Tests of the old base that `master` removed stay removed.
- All 31 gesture tests of Tasks 1-3 stay, with their assertions. Change one
  only where `master` forces it, and say in the report which and why. Known
  cases:
  - `enter_pattern_view()` now runs from the Session view, where encoder 1
    short calls `target_focus()` and so overwrites `ui.selected_track` and
    `g_seq.active_pattern[]`. A test that needs another track or pattern must
    choose it after entering, or move the focus first, and must assert after
    entering that the target is the one it means. Tests about "the edited
    track and pattern" must use a track other than 0 and a pattern other than
    0 and check that track 0 and pattern 0 are untouched.
  - `test_grid_play_during_hold_cancels_the_tap` becomes
    `test_grid_encoder2_short_during_hold_only_cancels_the_tap`: the transport
    stays `TRANSPORT_STOPPED` and the step is not toggled.
  - Any other test that started the transport with encoder 2 short uses
    `panel(PANEL_PLAY, 0, 0)`; any assertion that encoder 2 short started it
    is inverted.
  - `test_grid_panic_forgets_held_steps`: Panic lands in `UI_SESSION`. The
    step is still down; releasing it must not toggle, in the Session view and
    after entering Pattern View again.
  - `test_grid_ignores_button_held_before_entering`: the button goes down in
    the Session view.
- Add these tests, each with a `RUN` line:

```c
static void test_pattern_view_navigation_without_a_hold(void) {
    enter_pattern_view();
    turn(0, 1);
    CHECK(ui.selected_track == 1);
    float bpm = g_seq.transport.bpm;
    turn(1, 10);
    CHECK(g_seq.transport.bpm > bpm);
    encoder_long(1);
    CHECK(ui.mode == UI_PATTERN_SELECT);
    encoder_short(0);                       // confirm: back to Pattern View
    CHECK(ui.mode == UI_PATTERN_VIEW);
    encoder_short(0);
    CHECK(ui.mode == UI_STEP_EDIT);
    encoder_short(1);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    encoder_short(1);                       // no action in Pattern View
    CHECK(ui.mode == UI_PATTERN_VIEW);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    encoder_long(0);
    CHECK(ui.mode == UI_SESSION);
}

static void test_grid_hold_suspends_every_screen_change(void) {
    enter_pattern_view();
    step_down(1u << 2);
    encoder_short(0);                       // would open Step Edit
    CHECK(ui.mode == UI_PATTERN_VIEW);
    encoder_long(0);                        // would return to the Session view
    CHECK(ui.mode == UI_PATTERN_VIEW);
    encoder_long(1);                        // would open Pattern Select
    CHECK(ui.mode == UI_PATTERN_VIEW);
    encoder_short(1);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    step_up(1u << 2);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    CHECK(STEP(0, 2).flags & STEP_FLAG_ACTIVE);     // created by the first edit
    CHECK(STEP(0, 2).flags & STEP_FLAG_ACCENT);
    CHECK(STEP(0, 2).flags & STEP_FLAG_TIE);
    CHECK(!(STEP(0, 2).flags & STEP_FLAG_SLIDE));   // cleared by the tie
}

static void test_grid_panel_keys_during_a_hold_leave_the_tap(void) {
    enter_pattern_view();
    step_down(1u << 3);
    panel(PANEL_PLAY, 0, 0);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    panel(PANEL_MUTE, 1, 0);
    CHECK(g_seq.track_mutes == 2);
    step_up(1u << 3);
    CHECK(STEP(0, 3).flags & STEP_FLAG_ACTIVE);
}

static void test_grid_step_that_vanishes_without_a_release_ends_the_hold(void) {
    enter_pattern_view();
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    step_down(1u << 4);
    step_buttons.held = 0;                  // driver took a new baseline: no release event
    ui_update();
    turn(0, 3);
    CHECK(ui.selected_track == 3);          // navigation is back
    CHECK(memcmp(patterns_before, g_seq.patterns, sizeof(patterns_before)) == 0);
}

static void test_grid_step_held_without_a_press_event_never_joins(void) {
    enter_pattern_view();
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    step_buttons.held = 1u << 4;            // appeared at a driver baseline: no press event
    ui_update();
    turn(0, 3);
    CHECK(ui.selected_track == 3);
    step_up(1u << 4);
    CHECK(memcmp(patterns_before, g_seq.patterns, sizeof(patterns_before)) == 0);
}
```

**README:** take `master`'s "Live Controls" unchanged and add this bullet
after the one that begins "The step editor returns to the screen it was opened
from"; the branch's old bullet goes away with the old section:

```markdown
- Pattern view step buttons: a tap shorter than half a second toggles the step
  when the button is released. Hold one or more steps and turn encoder 1 for
  note or encoder 2 for velocity; encoder 1 short/long toggles accent/slide and
  encoder 2 long toggles tie on the held steps. The first edit of a held empty
  step creates it. A step held longer, or held while an encoder is turned or
  pressed, is not toggled on release. While a step is held the encoders do not
  change track, tempo or screen; pads, SCENE, MUTE and PLAY work as usual.
```

- [ ] **Step 1: Merge and resolve**

```bash
git merge --no-commit --no-ff origin/master
```

Resolve the five files by the port rules. Do not touch any file the merge did
not mark as conflicted, except to make the result compile.

- [ ] **Step 2: Adapt and add the tests**

As listed under "Tests". Before fixing a test that fails after the merge,
decide whether the test or the code is wrong: the spec decides.

- [ ] **Step 3: Run all host tests and build the firmware**

Run: `cmake -S test -B test/build && cmake --build test/build -j && ctest --test-dir test/build --output-on-failure`
Expected: `100% tests passed out of 11`.

Run: `cmake --build build -j`
Expected: ends with `Built target rp2040_sequencer`.

- [ ] **Step 4: Check the diff against master**

Run: `git diff origin/master -- src include README.md test/CMakeLists.txt`
Expected: only gesture lines. Anything else is a reverted line of `master`: restore it.

- [ ] **Step 5: Commit**

Commit the merge with the message `Merge origin/master: port hold-step editing to the Session panel`.
