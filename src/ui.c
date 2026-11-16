#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>
#include "sequencer.h"
#include "kit.h"
#include "ssd1309.h"
#include "encoder.h"
#include "mcp23017.h"
#include "flash_storage.h"
#include "step_leds.h"
#include "session.h"
#include "panel_input.h"
#include "led_frame.h"
#include "rgb_leds.h"

// ─── Tempo LED ─────────────────────────────────────────────────
#define TEMPO_LED_PIN   25
#define TEMPO_LED_MS    30      // LED on-time per beat

// ─── UI Modes ──────────────────────────────────────────────────

typedef enum {
    UI_SESSION,             // Home: pad grid, focus cell on the step row
    UI_PATTERN_VIEW,        // Main step grid
    UI_STEP_EDIT,           // Edit note/velocity/flags
    UI_PATTERN_SELECT,      // Choose bank A-D and pattern 1-8
    UI_TRACK_CONFIG,        // Track settings
    UI_SONG_EDIT,           // Song chain editor
    UI_KIT_STEP_EDIT,       // One kit step: all 12 lanes
} ui_mode_t;

// ─── UI State ──────────────────────────────────────────────────

typedef struct {
    ui_mode_t mode;
    bool panic_latched;
    uint16_t step_gestures;         // steps pressed on this screen, still held
    uint16_t select_short_pending;
    uint16_t step_short_pending;    // SESSION, kit in PATTERN_VIEW: acted on at release
    session_focus_t step_cell[STEPS_PER_PATTERN]; // cell and lane focused at each press

    // PATTERN_VIEW
    uint8_t selected_track;         // 0–7

    // PATTERN_VIEW hold gesture, SYNTH and DRUM cells only (a kit's presses
    // are bound in step_cell instead)
    uint16_t grid_hold;                         // Steps pressed in Pattern View and still down
    uint16_t grid_tap_pending;                  // Subset that still toggles on release
    uint32_t grid_press_ms[NUM_STEP_BUTTONS];   // When each held step was pressed
    uint16_t grid_enc_target[NUM_ENCODERS];     // Steps held while that encoder's button was down
    uint16_t grid_enc_long_done[NUM_ENCODERS];  // Targets that already got that button's long press

    // STEP_EDIT
    uint8_t edit_step;              // 0–15
    ui_mode_t step_edit_return;     // Screen the step editor was opened from
    uint8_t kit_cursor;             // KIT_STEP_EDIT: lane under the cursor

    // PATTERN_SELECT
    uint8_t select_pattern;         // 0–31 (bank A-D × slot 1-8)

    // TRACK_CONFIG
    uint8_t config_track;           // 0–7
    uint8_t config_param;           // row: 0–4, on a kit 0..KIT_PARAM_COUNT-1

    // SONG_EDIT
    uint16_t song_cursor;           // Position in song
    uint8_t  song_field;            // 0=pattern, 1=repeats

    // Tempo LED
    uint32_t last_blink_ms;
    bool     led_on;
    uint32_t led_anim_ms;
    bool     led_anim_phase;

    // Flash notification: shown for ~1 second, then cleared
    char     notify_msg[17];    // up to 16 chars
    uint8_t  notify_ticks;      // counts down at ~30 fps
} ui_state_t;

static ui_state_t ui;

// ─── Helpers ───────────────────────────────────────────────────

static inline int8_t clamp_i8(int16_t val, int16_t lo, int16_t hi) {
    if (val < lo) return (int8_t)lo;
    if (val > hi) return (int8_t)hi;
    return (int8_t)val;
}

static uint8_t clamp_pattern_length(uint8_t len) {
    if (len < 1) return 1;
    if (len > STEPS_PER_PATTERN) return STEPS_PER_PATTERN;
    return len;
}

// Short-press-on-release detector. Keys pressed within `mask` wait in *pending
// until they are released (returned as short presses) or long-pressed (dropped).
// If long_presses is given it receives the long presses of pending keys only,
// so a key pressed on another screen or before a panic never counts.
static uint16_t take_short_releases(uint16_t *pending, const step_buttons_t *steps,
                                    uint16_t mask, uint16_t *long_presses) {
    *pending |= steps->pressed & mask;
    if (long_presses) *long_presses = steps->long_pressed & *pending;
    *pending &= (uint16_t)~steps->long_pressed;
    uint16_t short_releases = steps->released & *pending;
    *pending &= (uint16_t)~steps->released;
    return short_releases;
}

// Forgets every step gesture in progress: on a screen change, a panic and a
// flash save/load. Keys still held then never act on release or long press.
static void drop_step_gestures(void) {
    ui.step_gestures = 0;
    ui.select_short_pending = 0;
    ui.step_short_pending = 0;
    ui.grid_hold = 0;
    ui.grid_tap_pending = 0;
    for (uint8_t enc = 0; enc < NUM_ENCODERS; enc++) {
        ui.grid_enc_target[enc] = 0;
        ui.grid_enc_long_done[enc] = 0;
    }
}

// Toggles the steps in `mask` of one track at one location and refreshes its
// playback. Takes seq_lock: call with no lock held.
static void toggle_steps(uint8_t track, uint8_t location, uint16_t mask) {
    if (!mask) return;
    uint32_t lock_state = seq_lock();
    pattern_t *pat = &g_seq.patterns[location];
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (mask & (1u << s)) pattern_toggle_step(pat, track, s);
    }
    seq_unlock(lock_state);
    sequencer_refresh_track_playback(track, location);
}

// Patterns are written by Core 0 only: the panel reads them without a lock.
static bool is_kit(session_focus_t cell) {
    return kit_is_cell(&g_seq.patterns[cell.location], cell.track);
}

static uint16_t steps_within_length(uint8_t track, uint8_t location) {
    uint8_t len = clamp_pattern_length(
        pattern_get_track_length(&g_seq.patterns[location], track));
    return (uint16_t)((1u << len) - 1u);
}

// Presses step `step` of a kit lane (kit_press) and refreshes its playback.
// Takes seq_lock: call with no lock held.
static void press_kit_step(session_focus_t cell, uint8_t step) {
    uint32_t lock_state = seq_lock();
    uint32_t *word = &g_seq.patterns[cell.location].kit_steps[cell.track][step];
    *word = kit_press(*word, cell.lane);
    seq_unlock(lock_state);
    sequencer_refresh_track_playback(cell.track, cell.location);
}

// A step press belongs to the cell and lane focused when it was made, whatever
// the focus does before release. Steps pressed now within `bind` are bound to
// `cell`; a short press acts on release on its own cell (a kit lane cycles,
// other cells toggle). Returns the long presses of bound keys, which no longer
// act on release.
static uint16_t handle_bound_steps(session_focus_t cell, uint16_t bind,
                                   const step_buttons_t *steps) {
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (steps->pressed & bind & (1u << s)) ui.step_cell[s] = cell;
    }
    uint16_t long_presses;
    uint16_t short_releases = take_short_releases(&ui.step_short_pending, steps,
                                                  bind, &long_presses);
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (!(short_releases & (1u << s))) continue;
        session_focus_t bound = ui.step_cell[s];
        if (is_kit(bound)) {
            press_kit_step(bound, s);
        } else {
            toggle_steps(bound.track, bound.location, (uint16_t)(1u << s));
        }
    }
    return long_presses;
}

static void show_notify(const char *msg) {
    snprintf(ui.notify_msg, sizeof(ui.notify_msg), "%s", msg);
    ui.notify_ticks = 30;
}

static void toggle_transport(void) {
    if (g_seq.transport.state == TRANSPORT_PLAYING) {
        transport_stop();
    } else if (g_seq.transport.state == TRANSPORT_PAUSED) {
        transport_continue();
    } else {
        transport_start();
    }
}

static bool handle_panic_combo(button_event_t e1b, button_event_t e2b) {
    if (ui.panic_latched) {
        if (!encoder_button_held(0) && !encoder_button_held(1)) {
            ui.panic_latched = false;
            // A pad, PLAY or step pressed during the panic and still in its
            // debounce time must not fire once commands are accepted again.
            panel_input_resync();
            mcp23017_resync();
        }
        return true; // Consume both releases, including a late short press.
    }
    if ((e1b == BTN_LONG_PRESS && encoder_button_held(1)) ||
        (e2b == BTN_LONG_PRESS && encoder_button_held(0))) {
        sequencer_panic();
        // Panel keys held through the panic must not fire afterwards.
        panel_input_resync();
        ui.panic_latched = true;
        drop_step_gestures();
        ui.mode = UI_SESSION;
        show_notify("PANIC: ALL OFF");
        return true;
    }
    return false;
}

// ─── Global panel controls (every screen) ──────────────────────

// Pads, SCENE, MUTE and PLAY. Called with no lock held: every session_* and
// transport call below takes the locks itself.
static void handle_panel_events(void) {
    panel_event_t ev;
    while (panel_input_next(&ev)) {
        if (!ev.down) continue;
        switch (ev.kind) {
            case PANEL_PAD:
                // Outside the Session view a pad only launches.
                if (!session_pad(ev.track, ev.row, ui.mode == UI_SESSION)) {
                    show_notify("SONG MODE");
                }
                break;
            case PANEL_SCENE:
                if (!session_scene(ev.row)) show_notify("SONG MODE");
                break;
            case PANEL_MUTE:
                session_mute(ev.track);
                break;
            case PANEL_PLAY:
                toggle_transport();
                break;
        }
    }
}

// Drops panel events, e.g. while the panic combo is still held.
static void discard_panel_events(void) {
    panel_event_t ev;
    while (panel_input_next(&ev)) {
    }
}

// ─── Tempo LED Handler ────────────────────────────────────────

static void handle_tempo_led(void) {
    if (g_seq.transport.state == TRANSPORT_PLAYING &&
        g_seq.transport.step_triggered) {
        // New step just triggered — light LED
        g_seq.transport.step_triggered = false;
        gpio_put(TEMPO_LED_PIN, 1);
        ui.led_on = true;
        ui.last_blink_ms = to_ms_since_boot(get_absolute_time());
    }

    if (ui.led_on) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - ui.last_blink_ms >= TEMPO_LED_MS) {
            gpio_put(TEMPO_LED_PIN, 0);
            ui.led_on = false;
        }
    }
}

// ─── Session Handler ───────────────────────────────────────────

// Points the editing screens (selected_track, active_pattern) at a cell. A kit
// track's lane needs no copy: every screen reads session_kit_lane().
static void target_focus(session_focus_t focus) {
    ui.selected_track = focus.track;
    uint32_t lock_state = seq_lock();
    g_seq.active_pattern[focus.track] = focus.location;
    seq_unlock(lock_state);
}

// The kit step editor on `step` of the selected track's active cell, the lane
// cursor on `lane`; VALUE press returns to `from`.
static void open_kit_editor(uint8_t step, uint8_t lane, ui_mode_t from) {
    ui.edit_step = step;
    ui.kit_cursor = lane;
    ui.step_edit_return = from;
    ui.mode = UI_KIT_STEP_EDIT;
}

// The first long-pressed step opens an editor on the cell and lane it was bound
// to at its press: the kit step editor on a kit cell, else the note editor.
static void open_editor_on_long_press(uint16_t long_presses, ui_mode_t from) {
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (!(long_presses & (1u << s))) continue;
        session_focus_t cell = ui.step_cell[s];
        target_focus(cell);
        if (is_kit(cell)) {
            open_kit_editor(s, cell.lane, from);
        } else {
            ui.edit_step = s;
            ui.step_edit_return = from;
            ui.mode = UI_STEP_EDIT;
        }
        return;
    }
}

static void handle_session_view(encoder_turn_t e1, encoder_turn_t e2,
                                button_event_t e1b, button_event_t e2b,
                                step_buttons_t *steps) {
    // TRACK rotate: walk the stops, a kit's lanes included.
    if (e1.detents != 0) {
        session_move_focus(e1.detents);
    }

    // VALUE rotate: tempo.
    if (e2.amount != 0) {
        float new_bpm = g_seq.transport.bpm + (float)e2.amount * 0.1f;
        transport_set_bpm(new_bpm);
    }

    session_focus_t focus = session_get_focus();

    // TRACK press: pattern view on the focus.
    if (e1b == BTN_PRESS) {
        target_focus(focus);
        ui.mode = UI_PATTERN_VIEW;
    }

    // TRACK long: step the shown bank A-D.
    if (e1b == BTN_LONG_PRESS) {
        session_next_bank();
        char msg[17];
        snprintf(msg, sizeof(msg), "BANK %c", 'A' + session_bank());
        show_notify(msg);
    }

    // VALUE press: cycle the launch quantization.
    if (e2b == BTN_PRESS) {
        launch_quant_t q = (launch_quant_t)((transport_get_launch_quant() + 1u) %
                                            LAUNCH_QUANT_COUNT);
        transport_set_launch_quant(q);
        char msg[17];
        snprintf(msg, sizeof(msg), "QUANT %s", display_launch_quant_name(q));
        show_notify(msg);
    }

    // VALUE long: pattern selector on the focus.
    if (e2b == BTN_LONG_PRESS) {
        ui.selected_track = focus.track;
        ui.select_pattern = focus.location;
        ui.mode = UI_PATTERN_SELECT;
    }

    // A step pressed here and released on another screen does nothing.
    if (ui.mode != UI_SESSION) return;

    // Only keys pressed in this view count, on the cell and lane focused at
    // the press; steps beyond that cell's length do nothing. A short press acts
    // on release, a long press opens the editor on that step and lane without
    // changing it: the kit step editor on a kit, else the note editor.
    uint16_t long_presses = handle_bound_steps(
        focus, steps_within_length(focus.track, focus.location), steps);
    open_editor_on_long_press(long_presses, UI_SESSION);
}

// ─── Pattern View Handler ──────────────────────────────────────

// The cell the Pattern view edits: the selected track's active location, on a
// kit its remembered lane.
static session_focus_t pattern_view_cell(void) {
    uint8_t track = ui.selected_track;
    return (session_focus_t){track, g_seq.active_pattern[track], session_kit_lane(track)};
}

// A turn of whole clicks, whatever it is worth.
static bool turned(encoder_turn_t turn) {
    return turn.detents != 0 || turn.amount != 0;
}

// TRACK in the Pattern view: walks the stops of the tracks' active patterns, a
// kit's lanes included; the lane reached is the kit track's remembered one.
static void walk_pattern_view(int8_t detents) {
    uint8_t kit_mask = kit_cell_mask(g_seq.patterns, g_seq.active_pattern);
    uint8_t track = ui.selected_track;
    uint8_t lane = session_kit_lane(track);
    session_walk_stops(&track, &lane, detents, kit_mask);
    ui.selected_track = track;
    if (kit_mask & (1u << track)) session_set_kit_lane(track, lane);
}

// The Pattern view's encoders when no hold gesture owns them: TRACK walks,
// VALUE sets the tempo, TRACK press opens the kit step editor on step 1 of a
// kit cell, else the note editor, TRACK long returns to the Session view and
// VALUE long opens Pattern Select. VALUE press has no action here.
static void navigate_pattern_view(encoder_turn_t e1, encoder_turn_t e2,
                                  button_event_t e1b, button_event_t e2b) {
    if (e1.detents != 0) walk_pattern_view(e1.detents);

    if (e2.amount != 0) {
        float new_bpm = g_seq.transport.bpm + (float)e2.amount * 0.1f;
        transport_set_bpm(new_bpm);
    }

    session_focus_t cell = pattern_view_cell();
    if (e1b == BTN_PRESS) {
        if (is_kit(cell)) {
            open_kit_editor(0, cell.lane, UI_PATTERN_VIEW);
        } else {
            ui.mode = UI_STEP_EDIT;
            ui.edit_step = 0;
            ui.step_edit_return = UI_PATTERN_VIEW;
        }
    }

    if (e1b == BTN_LONG_PRESS) {
        ui.mode = UI_SESSION;
    }

    if (e2b == BTN_LONG_PRESS) {
        ui.mode = UI_PATTERN_SELECT;
        ui.select_pattern = cell.location;
    }
}

// Applies Step Edit encoder input to one step: e1_amount and e2_amount are the
// turns' accelerated amounts (note, velocity). Returns true if the step changed.
static bool apply_step_edit(step_t *st, int8_t e1_amount, int8_t e2_amount,
                            button_event_t e1b, button_event_t e2b) {
    bool changed = false;

    // Enc1 rotate: change note
    if (e1_amount != 0) {
        int16_t note = (int16_t)st->note + e1_amount;
        uint8_t next_note = clamp_i8(note, 0, 127);
        if (st->note != next_note) {
            st->note = next_note;
            changed = true;
        }
    }

    // Enc2 rotate: change velocity
    if (e2_amount != 0) {
        int16_t vel = (int16_t)st->velocity + e2_amount;
        uint8_t next_velocity = clamp_i8(vel, 0, 127);
        if (st->velocity != next_velocity) {
            st->velocity = next_velocity;
            changed = true;
        }
    }

    // Accent is independent of the outgoing connection type.
    if (e1b == BTN_PRESS) {
        st->flags ^= STEP_FLAG_ACCENT;
        changed = true;
    }
    if (e1b == BTN_LONG_PRESS) {
        st->flags = (st->flags ^ STEP_FLAG_SLIDE) & (uint8_t)~STEP_FLAG_TIE;
        changed = true;
    }
    if (e2b == BTN_LONG_PRESS) {
        st->flags = (st->flags ^ STEP_FLAG_TIE) & (uint8_t)~STEP_FLAG_SLIDE;
        changed = true;
    }

    return changed;
}

// For every step in mask: activate it if inactive, then apply_step_edit().
// Returns true if any step changed; an activation counts. Caller holds seq_lock().
static bool edit_held_steps(pattern_t *pat, uint8_t track, uint16_t mask,
                            int8_t e1_amount, int8_t e2_amount,
                            button_event_t e1b, button_event_t e2b) {
    bool changed = false;
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (!(mask & (1u << s))) continue;
        step_t *st = &pat->steps[track][s];
        if (!(st->flags & STEP_FLAG_ACTIVE)) {
            pattern_toggle_step(pat, track, s);
            changed = true;
        }
        if (apply_step_edit(st, e1_amount, e2_amount, e1b, e2b)) changed = true;
    }
    return changed;
}

// Pattern view on a SYNTH or DRUM cell, with hold-step editing: a tap toggles
// on release, held steps take the encoders, and with no hold they navigate.
// `walked`: TRACK walked here from a kit in this update, which counts as a turn
// for the steps pressed in it. Presses carried here from a kit keep their kit
// binding: they act on release or long press as on the kit.
static void handle_step_grid(encoder_turn_t e1, encoder_turn_t e2,
                             button_event_t e1b, button_event_t e2b,
                             step_buttons_t *steps, bool walked) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint16_t down_or_released = (uint16_t)(steps->held | steps->released);

    // 1. Steps already held when this update began (still down, or released
    //    just now). Rotation edits only these.
    uint16_t turn_targets = ui.grid_hold & down_or_released;

    // 2. Holds that start in this view.
    for (uint8_t s = 0; s < NUM_STEP_BUTTONS; s++) {
        if (steps->pressed & (1u << s)) ui.grid_press_ms[s] = now;
    }
    ui.grid_hold |= steps->pressed;
    ui.grid_tap_pending |= steps->pressed;

    // 3. Keep only buttons that are down now or were released in this update.
    ui.grid_hold &= down_or_released;
    ui.grid_tap_pending &= ui.grid_hold;

    // 4. An encoder button that is down while steps are held joins the gesture.
    for (uint8_t enc = 0; enc < NUM_ENCODERS; enc++) {
        if (encoder_button_held(enc) && ui.grid_hold) {
            ui.grid_enc_target[enc] |= ui.grid_hold;
            ui.grid_tap_pending &= (uint16_t)~ui.grid_hold;
        }
    }
    // A button event belongs only to the steps that were held while its
    // button was down.
    uint16_t e1_targets = ui.grid_enc_target[0];
    uint16_t e2_targets = ui.grid_enc_target[1];

    // A long press that already fired reaches, once, each step that joined
    // its button after it fired.
    uint16_t long_late[NUM_ENCODERS];
    for (uint8_t enc = 0; enc < NUM_ENCODERS; enc++) {
        long_late[enc] = 0;
        if (ui.grid_enc_long_done[enc] && encoder_button_held(enc)) {
            long_late[enc] = ui.grid_enc_target[enc] &
                             (uint16_t)~ui.grid_enc_long_done[enc];
            ui.grid_enc_long_done[enc] |= long_late[enc];
        }
    }
    bool gesture = ui.grid_hold || ui.grid_enc_target[0] || ui.grid_enc_target[1];

    // 5. Dispatch. With a gesture active, input edits its targets or is
    //    ignored; without one, the encoders navigate. A turn is whole clicks;
    //    note and velocity move by its accelerated amount.
    bool turn = turned(e1) || turned(e2);
    bool turn_edit = turn && turn_targets;
    bool e1_edit = (e1b == BTN_PRESS || e1b == BTN_LONG_PRESS) && e1_targets;
    bool e2_edit = e2b == BTN_LONG_PRESS && e2_targets;
    if (turn_edit) ui.grid_tap_pending &= (uint16_t)~turn_targets;
    // A turn in the same update as a first press does not edit that step,
    // but its order relative to the press is unknown: cancel the tap.
    if (turn || walked) ui.grid_tap_pending &= (uint16_t)~steps->pressed;
    if (e1_edit) ui.grid_tap_pending &= (uint16_t)~e1_targets;
    if (e2_edit) ui.grid_tap_pending &= (uint16_t)~e2_targets;
    if (e1_edit && e1b == BTN_LONG_PRESS) ui.grid_enc_long_done[0] |= e1_targets;
    if (e2_edit) ui.grid_enc_long_done[1] |= e2_targets;
    bool late_edit = long_late[0] || long_late[1];

    if (!gesture) navigate_pattern_view(e1, e2, e1b, e2b);

    // 6. Taps: a short hold with no encoder input toggles on release.
    uint16_t taps = 0;
    uint16_t tap_releases = steps->released & ui.grid_tap_pending;
    for (uint8_t s = 0; s < NUM_STEP_BUTTONS; s++) {
        if ((tap_releases & (1u << s)) &&
            (uint32_t)(now - ui.grid_press_ms[s]) < MCP_LONG_PRESS_MS) {
            taps |= (uint16_t)(1u << s);
        }
    }

    if (turn_edit || e1_edit || e2_edit || late_edit || taps) {
        bool changed = false;
        uint8_t track = ui.selected_track;
        uint8_t pattern_index;
        uint32_t lock_state = seq_lock();
        pattern_index = g_seq.active_pattern[track];
        pattern_t *pat = &g_seq.patterns[pattern_index];
        if (turn_edit &&
            edit_held_steps(pat, track, turn_targets, e1.amount, e2.amount,
                            BTN_NONE, BTN_NONE)) {
            changed = true;
        }
        if (e1_edit &&
            edit_held_steps(pat, track, e1_targets, 0, 0, e1b, BTN_NONE)) {
            changed = true;
        }
        if (e2_edit &&
            edit_held_steps(pat, track, e2_targets, 0, 0, BTN_NONE, e2b)) {
            changed = true;
        }
        if (long_late[0] &&
            edit_held_steps(pat, track, long_late[0], 0, 0, BTN_LONG_PRESS, BTN_NONE)) {
            changed = true;
        }
        if (long_late[1] &&
            edit_held_steps(pat, track, long_late[1], 0, 0, BTN_NONE, BTN_LONG_PRESS)) {
            changed = true;
        }
        for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
            if (taps & (1u << s)) {
                pattern_toggle_step(pat, track, s);
                changed = true;
            }
        }
        seq_unlock(lock_state);
        if (changed) {
            sequencer_refresh_track_playback(track, pattern_index);
        }
    }

    // 7. Released steps leave the hold.
    ui.grid_hold &= (uint16_t)~steps->released;
    ui.grid_tap_pending &= (uint16_t)~steps->released;

    // 8. An encoder whose button is up gives up its targets. This runs after
    //    the dispatch because a short press arrives once the button is up.
    for (uint8_t enc = 0; enc < NUM_ENCODERS; enc++) {
        if (!encoder_button_held(enc)) {
            ui.grid_enc_target[enc] = 0;
            ui.grid_enc_long_done[enc] = 0;
        }
    }

    // 9. Kit presses carried here by TRACK: a kit step pressed here and
    //    released on another screen does nothing.
    if (ui.mode != UI_PATTERN_VIEW) return;
    uint16_t long_presses = handle_bound_steps(pattern_view_cell(), 0, steps);
    open_editor_on_long_press(long_presses, UI_PATTERN_VIEW);
}

// SYNTH and DRUM cells: hold-step editing (handle_step_grid). A kit cell
// follows the Session rule on its remembered lane, its presses bound to the
// cell and lane focused when they were made; nothing there holds the encoders.
static void handle_pattern_view(encoder_turn_t e1, encoder_turn_t e2,
                                 button_event_t e1b, button_event_t e2b,
                                 step_buttons_t *steps) {
    // On a kit TRACK walks first, and the rest of the update belongs to the
    // cell it reaches.
    bool walked = false;
    if (is_kit(pattern_view_cell()) && e1.detents != 0) {
        walk_pattern_view(e1.detents);
        e1 = (encoder_turn_t){0, 0};
        walked = true;
    }

    session_focus_t cell = pattern_view_cell();
    if (!is_kit(cell)) {
        handle_step_grid(e1, e2, e1b, e2b, steps, walked);
        return;
    }

    navigate_pattern_view(e1, e2, e1b, e2b);

    // A kit step pressed here and released on another screen does nothing.
    if (ui.mode != UI_PATTERN_VIEW) return;
    // Steps beyond the cell's length do nothing; a long press opens the kit
    // step editor on its step and lane.
    uint16_t long_presses = handle_bound_steps(
        cell, steps_within_length(cell.track, cell.location), steps);
    open_editor_on_long_press(long_presses, UI_PATTERN_VIEW);
}

// ─── Kit Step Edit Handler ─────────────────────────────────────

// One kit step of the selected track's active cell. Nothing here touches its
// step_t data.
static void handle_kit_step_edit(encoder_turn_t e1, encoder_turn_t e2,
                                 button_event_t e1b, button_event_t e2b,
                                 step_buttons_t *steps) {
    (void)e1b;
    uint8_t track = ui.selected_track;
    uint8_t location = g_seq.active_pattern[track];

    // TRACK rotate: move the lane cursor, AC..RC.
    if (e1.detents != 0) {
        ui.kit_cursor = (uint8_t)clamp_i8((int16_t)ui.kit_cursor + e1.detents, KIT_LANE_AC,
                                          KIT_LANES - 1);
    }

    // VALUE rotate: one value step per click, clamped (off/note/accent/flam;
    // AC off/on), never accelerated; one kit_edit, with the CH/OH rule.
    if (e2.detents != 0) {
        bool changed = false;
        uint8_t lane = ui.kit_cursor;
        uint8_t top = lane == KIT_LANE_AC ? 1 : KIT_FLAM;
        uint32_t lock_state = seq_lock();
        uint32_t *word = &g_seq.patterns[location].kit_steps[track][ui.edit_step];
        uint8_t value = kit_get(*word, lane);
        uint8_t next = (uint8_t)clamp_i8((int16_t)value + e2.detents, KIT_OFF, top);
        if (next != value) {
            *word = kit_edit(*word, lane, next);
            changed = true;
        }
        seq_unlock(lock_state);
        if (changed) sequencer_refresh_track_playback(track, location);
    }

    // VALUE press: back to the screen the editor was opened from.
    if (e2b == BTN_PRESS) {
        ui.mode = ui.step_edit_return;
    }

    // Step buttons: another step within the length.
    uint16_t selectable = steps->pressed & steps_within_length(track, location);
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (selectable & (1u << s)) {
            ui.edit_step = s;
            break;
        }
    }
}

// ─── Step Edit Handler ─────────────────────────────────────────

static void handle_step_edit(encoder_turn_t e1, encoder_turn_t e2,
                              button_event_t e1b, button_event_t e2b,
                              step_buttons_t *steps) {
    uint32_t lock_state = seq_lock();
    uint8_t track = ui.selected_track;
    uint8_t pattern_index = g_seq.active_pattern[track];
    bool changed = false;
    pattern_t *pat = &g_seq.patterns[pattern_index];
    step_t *st = &pat->steps[track][ui.edit_step];

    if (apply_step_edit(st, e1.amount, e2.amount, e1b, e2b)) changed = true;

    // Enc2 press: back to the screen the editor was opened from
    if (e2b == BTN_PRESS) {
        ui.mode = ui.step_edit_return;
    }

    // Step buttons: select which step to edit
    if (steps->pressed) {
        for (int s = 0; s < STEPS_PER_PATTERN; s++) {
            if (steps->pressed & (1u << s)) {
                ui.edit_step = s;
                break;
            }
        }
    }

    // Step button long press: toggle step active
    if (steps->long_pressed) {
        for (int s = 0; s < STEPS_PER_PATTERN; s++) {
            if (steps->long_pressed & (1u << s)) {
                pattern_toggle_step(pat, track, s);
                changed = true;
            }
        }
    }

    seq_unlock(lock_state);
    if (changed) {
        sequencer_refresh_track_playback(track, pattern_index);
    }
}

// ─── Pattern Select Handler ────────────────────────────────────

static void handle_pattern_select(encoder_turn_t e1, encoder_turn_t e2,
                                   button_event_t e1b, button_event_t e2b,
                                   step_buttons_t *steps) {
    // Enc1 rotate: select pattern slot 1-8 within the current bank.
    if (e1.detents != 0) {
        int16_t slot = (int16_t)pattern_slot(ui.select_pattern) + e1.detents;
        if (slot < 0) slot = 0;
        if (slot >= PATTERNS_PER_BANK) slot = PATTERNS_PER_BANK - 1;
        ui.select_pattern = pattern_make_index(pattern_bank(ui.select_pattern),
                                               (uint8_t)slot);
    }

    // Enc2 rotate: select bank A-D, one per detent, wrapping.
    if (e2.detents != 0) {
        int16_t bank = (int16_t)pattern_bank(ui.select_pattern) + (e2.detents > 0 ? 1 : -1);
        if (bank < 0) bank = PATTERN_BANKS - 1;
        if (bank >= PATTERN_BANKS) bank = 0;
        ui.select_pattern = pattern_make_index((uint8_t)bank,
                                               pattern_slot(ui.select_pattern));
    }

    // Enc1 press: confirm, go to pattern view
    if (e1b == BTN_PRESS) {
        uint32_t lock_state = seq_lock();
        g_seq.active_pattern[ui.selected_track] = ui.select_pattern;
        seq_unlock(lock_state);
        ui.mode = UI_PATTERN_VIEW;
    }

    // Enc1 long press: enter song edit
    if (e1b == BTN_LONG_PRESS) {
        ui.mode = UI_SONG_EDIT;
        ui.song_cursor = 0;
        ui.song_field = 0;
    }

    // Enc2 press: queue pattern
    if (e2b == BTN_PRESS) {
        if (!transport_queue_track_pattern(ui.selected_track, ui.select_pattern)) {
            show_notify("SONG MODE");
        }
    }

    // Enc2 long press: back to the Session view (escape)
    if (e2b == BTN_LONG_PRESS) {
        ui.mode = UI_SESSION;
    }

    // In the selector only, defer short presses until release so opening
    // track configuration never changes the audible mute state.
    uint16_t short_releases = take_short_releases(&ui.select_short_pending, steps,
                                                  0xFFu, NULL);
    if (short_releases) {
        for (int t = 0; t < NUM_TRACKS; t++) {
            if (short_releases & (1u << t)) {
                seq_toggle_track_mute(t);
            }
        }
    }

    // Step button long press on 1–8: enter track config
    if (steps->long_pressed) {
        for (int t = 0; t < NUM_TRACKS; t++) {
            if (steps->long_pressed & (1u << t)) {
                ui.mode = UI_TRACK_CONFIG;
                ui.config_track = t;
                ui.config_param = 0;
                ui.select_short_pending = 0;
                break;
            }
        }
    }

    // Step button 9 (index 8): save to flash
    if (steps->pressed & (1u << 8)) {
        if (g_seq.transport.state != TRANSPORT_STOPPED) {
            show_notify("STOP TO SAVE");
        } else {
            show_notify(flash_storage_save() ? "SAVED OK" : "SAVE FAILED");
            drop_step_gestures();   // steps held through the stall do nothing
        }
    }

    // Step button 10 (index 9): load from flash
    if (steps->pressed & (1u << 9)) {
        if (g_seq.transport.state != TRANSPORT_STOPPED) {
            show_notify("STOP TO LOAD");
        } else {
            show_notify(flash_storage_load() ? "LOADED OK" : "LOAD FAILED");
            drop_step_gestures();
        }
    }

    // Step button 11 (index 10): shorten pattern length
    if (steps->pressed & (1u << 10)) {
        bool changed = false;
        uint32_t lock_state = seq_lock();
        pattern_t *selected = &g_seq.patterns[ui.select_pattern];
        uint8_t len = pattern_get_track_length(selected, ui.selected_track);
        if (len > 1) {
            pattern_set_track_length(selected, ui.selected_track, len - 1);
            changed = true;
        }
        seq_unlock(lock_state);
        if (changed) {
            sequencer_refresh_track_playback(ui.selected_track, ui.select_pattern);
        }
    }

    // Step button 12 (index 11): lengthen pattern length
    if (steps->pressed & (1u << 11)) {
        bool changed = false;
        uint32_t lock_state = seq_lock();
        pattern_t *selected = &g_seq.patterns[ui.select_pattern];
        uint8_t len = pattern_get_track_length(selected, ui.selected_track);
        if (len < STEPS_PER_PATTERN) {
            pattern_set_track_length(selected, ui.selected_track, len + 1);
            changed = true;
        }
        seq_unlock(lock_state);
        if (changed) {
            sequencer_refresh_track_playback(ui.selected_track, ui.select_pattern);
        }
    }

    // Step button 13 (index 12): reset jitter measurement counters
    if (steps->pressed & (1u << 12)) {
        sequencer_reset_jitter_stats();
    }
}

static uint16_t build_step_activity_mask(const pattern_t *pat, uint8_t track) {
    uint16_t mask = 0;
    uint8_t len = clamp_pattern_length(pattern_get_track_length(pat, track));

    for (uint8_t s = 0; s < len; s++) {
        if (pat->steps[track][s].flags & STEP_FLAG_ACTIVE) {
            mask |= (uint16_t)(1u << s);
        }
    }
    return mask;
}

// Session, Pattern view and kit editor: on a kit cell the steps of `lane` that
// are not off, else the active steps; within the length either way.
static uint16_t build_cell_mask(const pattern_t *pat, uint8_t track, uint8_t lane) {
    if (!kit_is_cell(pat, track)) return build_step_activity_mask(pat, track);
    uint16_t mask = 0;
    uint8_t len = clamp_pattern_length(pattern_get_track_length(pat, track));
    for (uint8_t s = 0; s < len; s++) {
        if (kit_get(pat->kit_steps[track][s], lane) != KIT_OFF) mask |= (uint16_t)(1u << s);
    }
    return mask;
}

// On for the first 8th note of every quarter, like the panel's fast blink.
static bool eighth_blink_on(uint32_t pulse) {
    return (pulse / (PPQN / 2)) % 2 == 0;
}

// A cell's playhead: its step while the transport plays it (the playing
// location of its track) within `length`, else STEPS_PER_PATTERN (none). Uses
// the frame's snapshot; takes no lock.
static uint8_t cell_playhead(const transport_snapshot_t *snap, uint8_t track,
                             uint8_t location, uint8_t length) {
    uint8_t step = snap->track_step[track];
    bool playing = snap->state == TRANSPORT_PLAYING && snap->current[track] == location;
    return playing && step < length ? step : STEPS_PER_PATTERN;
}

// Session step LEDs: the focused cell's active steps (on a kit, the focused
// lane's steps) within its length. While the cell is the playing location of
// its track, its playhead blinks on 8th notes of the transport. Uses the
// frame's snapshot; takes no lock.
static uint16_t build_session_mask(const transport_snapshot_t *snap) {
    session_focus_t focus = session_get_focus();
    const pattern_t *pat = &g_seq.patterns[focus.location];
    uint16_t mask = build_cell_mask(pat, focus.track, focus.lane);

    uint8_t playhead = cell_playhead(
        snap, focus.track, focus.location,
        clamp_pattern_length(pattern_get_track_length(pat, focus.track)));
    if (playhead < STEPS_PER_PATTERN && eighth_blink_on(snap->pulse_count)) {
        mask ^= (uint16_t)(1u << playhead);
    }
    return mask;
}

static uint16_t build_pattern_select_mask(void) {
    uint16_t mask = 0;
    uint8_t mutes = seq_get_track_mutes();

    // Buttons 1-8 represent track mute state (lit = audible: neither the live
    // mute nor the stored mute of the playing location is set).
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        if (!(mutes & (1u << t))) {
            mask |= (uint16_t)(1u << t);
        }
    }
    // Buttons 11/12 indicate length controls.
    mask |= (1u << 10);
    mask |= (1u << 11);
    // Button 13 resets jitter stats.
    mask |= (1u << 12);
    return mask;
}

// Per-pass step LEDs for every screen but the Session view, whose mask needs
// the transport and is written once per frame by ui_render: the playback lock
// is never taken on every pass of the UI loop.
static void update_step_leds(void) {
    if (!step_leds_available()) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - ui.led_anim_ms >= 120) {
        ui.led_anim_ms = now;
        ui.led_anim_phase = !ui.led_anim_phase;
    }

    uint16_t mask = 0;

    switch (ui.mode) {
        case UI_SESSION:
            return;     // written once per frame by ui_render

        case UI_PATTERN_VIEW: {
            pattern_t *p = seq_active_pattern(ui.selected_track);
            mask = build_cell_mask(p, ui.selected_track, session_kit_lane(ui.selected_track));
            if (g_seq.transport.state == TRANSPORT_PLAYING &&
                g_seq.active_pattern[ui.selected_track] == g_seq.transport.current_pattern[ui.selected_track] &&
                ui.led_anim_phase &&
                g_seq.transport.current_track_step[ui.selected_track] <
                    clamp_pattern_length(pattern_get_track_length(p, ui.selected_track))) {
                mask ^= (uint16_t)(1u << g_seq.transport.current_track_step[ui.selected_track]);
            }
            break;
        }

        case UI_STEP_EDIT:
        case UI_KIT_STEP_EDIT: {
            // The note editor shows the active steps, the kit editor the
            // cursor lane's steps.
            pattern_t *p = seq_active_pattern(ui.selected_track);
            mask = ui.mode == UI_KIT_STEP_EDIT
                ? build_cell_mask(p, ui.selected_track, ui.kit_cursor)
                : build_step_activity_mask(p, ui.selected_track);

            // Keep selected step obvious while editing.
            if (ui.edit_step < STEPS_PER_PATTERN) {
                if (ui.led_anim_phase) {
                    mask |= (uint16_t)(1u << ui.edit_step);
                } else {
                    mask &= (uint16_t)~(1u << ui.edit_step);
                }
            }
            break;
        }

        case UI_PATTERN_SELECT:
            mask = build_pattern_select_mask();
            break;

        case UI_TRACK_CONFIG:
            // Indicate currently configured track on buttons 1-8.
            if (ui.config_track < NUM_TRACKS) {
                mask = (uint16_t)(1u << ui.config_track);
            }
            break;

        case UI_SONG_EDIT:
            // Button 1 = loop/stop, button 2 = song, button 3 = pattern playback.
            mask = (1u << 0) | (1u << 1) | (1u << 2);
            break;
    }

    step_leds_set_mask(mask);
}

// ─── Track Config Handler ──────────────────────────────────────

// Rows of a SYNTH or DRUM track's config screen (NOTE on a drum only).
enum { TRACK_PARAM_TYPE, TRACK_PARAM_CHANNEL, TRACK_PARAM_OUTPUT, TRACK_PARAM_GATE,
       TRACK_PARAM_NOTE };

#define TRACK_TYPE_COUNT 3
_Static_assert(TRACK_TYPE_KIT + 1 == TRACK_TYPE_COUNT,
               "TRACK_TYPE_COUNT must follow the last track type");

// SYNTH -> DRUM -> KIT -> SYNTH; backwards on a left turn.
static track_type_t next_track_type(track_type_t type, int8_t direction) {
    return (track_type_t)((type + (direction > 0 ? 1 : TRACK_TYPE_COUNT - 1)) %
                          TRACK_TYPE_COUNT);
}

// value + delta, within lo..hi.
static uint8_t step_value(uint8_t value, int8_t delta, uint8_t lo, uint8_t hi) {
    int16_t next = (int16_t)value + delta;
    return (uint8_t)(next < lo ? lo : (next > hi ? hi : next));
}

// TYPE moves one type per event. seq_set_track_config refuses a third kit track
// before it changes anything; KIT is then skipped with a message.
static void step_track_type(uint8_t track, track_config_t config, int8_t direction) {
    config.type = next_track_type(config.type, direction);
    if (seq_set_track_config(track, &config)) return;
    show_notify("MAX 2 KITS");
    config.type = next_track_type(config.type, direction);
    seq_set_track_config(track, &config);
}

// OUTPUT moves one mode per event, cycling through the three; written to all
// 32 locations.
static void step_output_mode(uint8_t track, track_config_t config, int8_t direction) {
    int16_t m = (int16_t)config.output_mode + (direction > 0 ? 1 : -1);
    if (m < 0) m = 2;
    if (m > 2) m = 0;
    config.output_mode = (output_mode_t)m;
    seq_set_track_config(track, &config);
}

// CHANNEL, GATE and a drum's NOTE: the track's routing, written to all 32
// locations when it changes.
static void edit_track_param(uint8_t track, track_config_t config, uint8_t param,
                             int8_t delta) {
    uint8_t *field;
    uint8_t lo = 0, hi = 127;
    switch (param) {
        case TRACK_PARAM_CHANNEL: field = &config.midi_channel; hi = 15; break;
        case TRACK_PARAM_GATE: field = &config.gate_length_pct; lo = 10; hi = 100; break;
        case TRACK_PARAM_NOTE: field = &config.drum_note; break;
        default: return;
    }
    uint8_t next = step_value(*field, delta, lo, hi);
    if (next == *field) return;
    *field = next;
    seq_set_track_config(track, &config);
}

// A kit's CHANNEL and GATE are its routing (seq_set_track_config); FLAM, the
// velocities and the notes are its kit settings (seq_set_kit_config). Each is
// written only when it changes.
static void edit_kit_param(uint8_t track, track_config_t config, uint8_t param,
                           int8_t delta) {
    uint32_t lock_state = seq_lock();
    kit_config_t kit = g_seq.kit_config[track];
    seq_unlock(lock_state);
    uint8_t *field;
    uint8_t lo = 1, hi = 127;
    bool routing = false;
    switch (param) {
        case KIT_PARAM_CHANNEL:
            field = &config.midi_channel; lo = 0; hi = 15; routing = true; break;
        case KIT_PARAM_GATE:
            field = &config.gate_length_pct; lo = 10; hi = 100; routing = true; break;
        case KIT_PARAM_FLAM:
            field = &kit.flam_pulses; lo = KIT_FLAM_MIN; hi = KIT_FLAM_MAX; break;
        case KIT_PARAM_VEL_NORMAL: field = &kit.vel_normal; break;
        case KIT_PARAM_VEL_ACCENT: field = &kit.vel_accent; break;
        case KIT_PARAM_VEL_MAX: field = &kit.vel_max; break;
        default:
            if (param < KIT_PARAM_NOTE || param >= KIT_PARAM_COUNT) return;
            field = &kit.note[param - KIT_PARAM_NOTE];
            lo = 0;
            break;
    }
    uint8_t next = step_value(*field, delta, lo, hi);
    if (next == *field) return;
    *field = next;
    if (routing) {
        seq_set_track_config(track, &config);
    } else {
        seq_set_kit_config(track, &kit);
    }
}

static void handle_track_config(encoder_turn_t e1, encoder_turn_t e2,
                                 button_event_t e1b, button_event_t e2b,
                                 step_buttons_t *steps) {
    (void)steps;
    uint8_t edited_track = ui.config_track;
    uint32_t lock_state = seq_lock();
    const pattern_t *cell = &g_seq.patterns[g_seq.active_pattern[edited_track]];
    track_config_t config = cell->tracks[edited_track];
    bool kit = kit_is_cell(cell, edited_track);
    seq_unlock(lock_state);

    // A kit lists TYPE, CHANNEL, GATE, FLAM, the velocities and the notes;
    // the others TYPE, CHANNEL, OUTPUT, GATE and, on a drum, NOTE.
    uint8_t last = kit ? KIT_PARAM_COUNT - 1
                       : (config.type == TRACK_TYPE_DRUM ? TRACK_PARAM_NOTE : TRACK_PARAM_GATE);
    if (ui.config_param > last) ui.config_param = last;

    // Enc1 rotate: select parameter
    if (e1.detents != 0) {
        ui.config_param = (uint8_t)clamp_i8((int16_t)ui.config_param + e1.detents, 0, last);
    }

    // Enc2 rotate: TYPE (the first row of both lists) and OUTPUT are choices,
    // the other rows numbers.
    if (ui.config_param == TRACK_PARAM_TYPE) {
        if (e2.detents != 0) step_track_type(edited_track, config, e2.detents);
    } else if (kit) {
        if (e2.amount != 0) edit_kit_param(edited_track, config, ui.config_param, e2.amount);
    } else if (ui.config_param == TRACK_PARAM_OUTPUT) {
        if (e2.detents != 0) step_output_mode(edited_track, config, e2.detents);
    } else if (e2.amount != 0) {
        edit_track_param(edited_track, config, ui.config_param, e2.amount);
    }

    // Enc1 press: next track
    if (e1b == BTN_PRESS) {
        ui.config_track = (ui.config_track + 1) % NUM_TRACKS;
        ui.config_param = 0;
    }

    // Enc2 press: back to pattern select
    if (e2b == BTN_PRESS) {
        ui.mode = UI_PATTERN_SELECT;
    }
}

// ─── Song Edit Handler ─────────────────────────────────────────

static void handle_song_edit(encoder_turn_t e1, encoder_turn_t e2,
                              button_event_t e1b, button_event_t e2b,
                              step_buttons_t *steps) {
    (void)steps;
    song_t *s = &g_seq.song;

    // Enc1 rotate: navigate entries
    if (e1.detents != 0) {
        uint32_t lock_state = seq_lock();
        uint16_t song_len = s->length;
        seq_unlock(lock_state);

        int16_t pos = (int16_t)ui.song_cursor + e1.detents;
        if (pos < 0) pos = 0;
        if (pos >= (int16_t)song_len && song_len > 0) {
            pos = song_len - 1;
        }
        ui.song_cursor = (uint16_t)pos;
    }

    // Enc2 rotate: modify current entry based on field: its pattern is a
    // choice, its repeats a number.
    if (e2.detents != 0 || e2.amount != 0) {
        uint32_t lock_state = seq_lock();
        if (ui.song_cursor < s->length) {
            song_entry_t *entry = &s->entries[ui.song_cursor];
            switch (ui.song_field) {
                case 0: {
                    // Pattern index for the currently selected track.
                    int16_t p = (int16_t)entry->pattern_index[ui.selected_track] + e2.detents;
                    if (p < 0) p = 0;
                    if (p >= MAX_PATTERNS) p = MAX_PATTERNS - 1;
                    entry->pattern_index[ui.selected_track] = (uint8_t)p;
                    break;
                }
                case 1: {
                    // Repeats 1–255
                    int16_t r = (int16_t)entry->repeats + e2.amount;
                    if (r < 1) r = 1;
                    if (r > 255) r = 255;
                    entry->repeats = (uint8_t)r;
                    break;
                }
            }
        }
        seq_unlock(lock_state);
    }

    // Enc1 press: insert new entry / cycle field
    if (e1b == BTN_PRESS) {
        // Cycle edit field: pattern -> repeats
        ui.song_field = (ui.song_field + 1) % 2;
    }

    // Enc1 long press: insert new entry after cursor
    if (e1b == BTN_LONG_PRESS) {
        uint32_t lock_state = seq_lock();
        uint16_t song_len = s->length;
        uint8_t active_patterns[NUM_TRACKS];
        memcpy(active_patterns, g_seq.active_pattern, sizeof(active_patterns));
        seq_unlock(lock_state);

        uint16_t insert_pos = (song_len > 0) ? ui.song_cursor + 1 : 0;
        if (song_insert_patterns(insert_pos, active_patterns, 1)) {
            ui.song_cursor = insert_pos;
        }
    }

    // Enc2 press: back to pattern select
    if (e2b == BTN_PRESS) {
        ui.mode = UI_PATTERN_SELECT;
    }

    // Enc2 long press: delete current entry
    if (e2b == BTN_LONG_PRESS) {
        uint32_t lock_state = seq_lock();
        bool can_delete = (s->length > 0 && ui.song_cursor < s->length);
        seq_unlock(lock_state);

        if (can_delete) {
            song_remove(ui.song_cursor);

            lock_state = seq_lock();
            uint16_t song_len = s->length;
            seq_unlock(lock_state);
            if (song_len == 0) {
                ui.song_cursor = 0;
            } else if (ui.song_cursor > 0 && ui.song_cursor >= song_len) {
                ui.song_cursor = song_len - 1;
            }
        }
    }

    // Step button 1: toggle end mode (loop/stop)
    if (steps->pressed & (1u << 0)) {
        uint32_t lock_state = seq_lock();
        song_end_mode_t next_mode =
            (s->end_mode == SONG_END_LOOP) ? SONG_END_STOP : SONG_END_LOOP;
        seq_unlock(lock_state);

        song_set_end_mode(
            next_mode
        );
    }

    // Step buttons 2/3: Song or Pattern playback; PLAY starts it.
    if (steps->pressed & (1u << 1)) {
        transport_set_mode(PLAY_MODE_SONG);
    }
    if (steps->pressed & (1u << 2)) {
        transport_set_mode(PLAY_MODE_PATTERN);
    }
}

// ─── Notification overlay ──────────────────────────────────────

static void render_notify(void) {
    if (ui.notify_ticks == 0) return;
    // Draw a centred box over the middle of the screen
    ssd1309_fill_rect(8, 24, 112, 16, false);   // Clear background
    ssd1309_draw_rect(8, 24, 112, 16, true);     // Border
    ssd1309_draw_string(14, 28, ui.notify_msg, false);
    ui.notify_ticks--;
}

// ─── Panel lights (RGB chain) ──────────────────────────────────

// Builds the 82-LED frame from the frame's transport snapshot and starts
// sending it; a frame the chain is still busy with is skipped. Called with no
// lock held: sequencer_clip_mask takes seq_lock itself.
static void update_panel_lights(const transport_snapshot_t *snap) {
    led_state_t state;
    state.bank = session_bank();
    state.clip_mask = sequencer_clip_mask(state.bank);
    memcpy(state.current, snap->current, sizeof(state.current));
    memcpy(state.queued, snap->queued, sizeof(state.queued));
    state.muted = snap->live_mutes | snap->stored_mutes;
    state.focus_track = session_get_focus().track;
    state.playing = snap->state == TRANSPORT_PLAYING;
    if (state.playing) {
        state.pulse = snap->pulse_count;
    } else {
        // Blinks keep a 120 BPM clock while stopped.
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        state.pulse = (uint32_t)(((uint64_t)now_ms * 2 * PPQN) / 1000);
    }

    rgb_t frame[LED_COUNT];
    led_frame_build(&state, frame);
    led_frame_apply_budget(frame, LED_RGB_BUDGET_MA);
    rgb_leds_show(frame);
}

// On a kit focus the focus line names the lane and a strip shows its steps
// and the cell's playhead.
static void render_session_view(const transport_snapshot_t *snap) {
    session_focus_t focus = session_get_focus();
    const pattern_t *pat = &g_seq.patterns[focus.location];
    uint8_t length = clamp_pattern_length(pattern_get_track_length(pat, focus.track));
    uint8_t bar_step = snap->state == TRANSPORT_PLAYING
        ? (uint8_t)((snap->pulse_count / PULSES_PER_STEP) % STEPS_PER_PATTERN)
        : STEPS_PER_PATTERN;    // none
    const char *lane = NULL;
    char strip[STEPS_PER_PATTERN + 1];
    uint8_t playhead = STEPS_PER_PATTERN;
    if (kit_is_cell(pat, focus.track)) {
        lane = kit_lane_name(focus.lane);
        kit_lane_glyphs(pat->kit_steps[focus.track], focus.lane, length, strip);
        playhead = cell_playhead(snap, focus.track, focus.location, length);
    }
    display_draw_session_view(session_bank(), session_common_row(snap), snap->mode,
                              focus.track, focus.location, length,
                              g_seq.transport.bpm, transport_get_launch_quant(),
                              bar_step, lane, lane ? strip : NULL, playhead);
}

// A kit cell shows its remembered lane's steps, any other cell its step_t grid
// with the values of the lowest held step (hold-step editing).
static void render_pattern_view(const transport_snapshot_t *snap) {
    uint8_t track = ui.selected_track;
    uint8_t location = g_seq.active_pattern[track];
    const pattern_t *pat = &g_seq.patterns[location];
    if (kit_is_cell(pat, track)) {
        uint8_t lane = session_kit_lane(track);
        uint8_t length = clamp_pattern_length(pattern_get_track_length(pat, track));
        char strip[STEPS_PER_PATTERN + 1];
        kit_lane_glyphs(pat->kit_steps[track], lane, length, strip);
        display_draw_kit_lane_view(track, location, kit_lane_name(lane), strip,
                                   cell_playhead(snap, track, location, length));
    } else {
        int8_t held_step = -1;              // lowest held step, -1 = none
        for (int8_t s = 0; s < STEPS_PER_PATTERN; s++) {
            if (ui.grid_hold & (1u << s)) { held_step = s; break; }
        }
        display_draw_pattern_view(pat, track, g_seq.transport.current_track_step[track],
                                  held_step);
    }
    display_draw_bpm(g_seq.transport.bpm);
}

// A kit track's config lists its kit settings; the others keep their list.
static void render_track_config(void) {
    uint8_t track = ui.config_track;
    const pattern_t *cell = seq_active_pattern(track);
    if (kit_is_cell(cell, track)) {
        display_draw_kit_config(&cell->tracks[track], &g_seq.kit_config[track], track,
                                ui.config_param);
    } else {
        display_draw_track_config(&cell->tracks[track], track, ui.config_param);
    }
}

// ─── UI Render (dispatches to display functions) ───────────────

void ui_render(void) {
    // One copy of the transport per frame, taken with no lock held.
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    update_panel_lights(&snap);

    switch (ui.mode) {
        case UI_SESSION:
            render_session_view(&snap);
            if (step_leds_available()) step_leds_set_mask(build_session_mask(&snap));
            break;

        case UI_PATTERN_VIEW:
            render_pattern_view(&snap);
            break;

        case UI_STEP_EDIT:
            display_draw_step_edit(
                seq_active_pattern(ui.selected_track),
                ui.selected_track,
                ui.edit_step);
            break;

        case UI_PATTERN_SELECT:
            display_draw_pattern_select(
                ui.select_pattern,
                pattern_get_track_length(&g_seq.patterns[ui.select_pattern],
                                         ui.selected_track),
                g_seq.transport.queued_pattern[ui.selected_track],
                seq_get_track_mutes());
            break;

        case UI_TRACK_CONFIG:
            render_track_config();
            break;

        case UI_SONG_EDIT:
            display_draw_song_view(&g_seq.song, ui.song_cursor, ui.selected_track);
            break;

        case UI_KIT_STEP_EDIT:
            display_draw_kit_step_edit(
                ui.selected_track,
                ui.edit_step,
                seq_active_pattern(ui.selected_track)->kit_steps[ui.selected_track][ui.edit_step],
                ui.kit_cursor);
            break;
    }

    // Overlay flash notification if active
    render_notify();
}

// ─── Initialization ────────────────────────────────────────────

void ui_init(void) {
    encoder_init();
    mcp23017_init();
    panel_input_init();     // TCA8418 shares I2C0, set up by mcp23017_init
    step_leds_init();
    rgb_leds_init();        // Sends an all-off frame

    // Tempo LED
    gpio_init(TEMPO_LED_PIN);
    gpio_set_dir(TEMPO_LED_PIN, GPIO_OUT);
    gpio_put(TEMPO_LED_PIN, 0);

    // Default state
    memset(&ui, 0, sizeof(ui));
    ui.mode = UI_SESSION;
    ui.step_edit_return = UI_SESSION;
    session_init();         // Reads the transport: call after sequencer_init
}

// ─── Main Update Loop ──────────────────────────────────────────

void ui_update(void) {
    // 1. Poll inputs: the panel on every pass, then encoders and step buttons
    panel_input_poll(to_ms_since_boot(get_absolute_time()));
    encoder_poll();
    mcp23017_poll();

    // 2. Read encoder events, once per pass. Every screen reads whole clicks:
    // a choice moves one per click (`detents`), a number by `amount`, the same
    // clicks accelerated on a fast turn (encoder_accel_detent).
    encoder_turn_t e1 = encoder_get_turn(0);
    encoder_turn_t e2 = encoder_get_turn(1);
    button_event_t e1b = encoder_get_button(0);
    button_event_t e2b = encoder_get_button(1);
    step_buttons_t *steps = mcp23017_get_buttons();

    // 3. Tempo LED
    handle_tempo_led();

    if (handle_panic_combo(e1b, e2b)) {
        // Panel presses during the panic gesture are consumed like the
        // encoder and step events, so none fires once it is released.
        discard_panel_events();
        update_step_leds();
        steps->pressed = 0;
        steps->released = 0;
        steps->long_pressed = 0;
        return;
    }

    // 4. Global panel controls (no lock held), then the screen's own handler
    handle_panel_events();

    // A long press counts only for a step pressed on the screen it fires on.
    ui_mode_t screen = ui.mode;
    ui.step_gestures |= steps->pressed;
    steps->long_pressed &= ui.step_gestures;

    switch (ui.mode) {
        case UI_SESSION:
            handle_session_view(e1, e2, e1b, e2b, steps);
            break;
        case UI_PATTERN_VIEW:
            handle_pattern_view(e1, e2, e1b, e2b, steps);
            break;
        case UI_STEP_EDIT:
            handle_step_edit(e1, e2, e1b, e2b, steps);
            break;
        case UI_PATTERN_SELECT:
            handle_pattern_select(e1, e2, e1b, e2b, steps);
            break;
        case UI_TRACK_CONFIG:
            handle_track_config(e1, e2, e1b, e2b, steps);
            break;
        case UI_SONG_EDIT:
            handle_song_edit(e1, e2, e1b, e2b, steps);
            break;
        case UI_KIT_STEP_EDIT:
            handle_kit_step_edit(e1, e2, e1b, e2b, steps);
            break;
    }

    // 5. Update step-button LEDs for current UI mode.
    update_step_leds();

    // 6. Clear consumed button events; a screen change ends every gesture.
    ui.step_gestures &= (uint16_t)~steps->released;
    if (ui.mode != screen) drop_step_gestures();
    steps->pressed = 0;
    steps->released = 0;
    steps->long_pressed = 0;
}
