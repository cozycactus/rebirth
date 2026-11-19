#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "sequencer.h"
#include "midi_in.h"
#include "kit.h"
#include "midi_song.h"
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

// MIDI keyboard: bytes taken from the input ring in one UI pass; the rest wait there.
#define MIDI_IN_BYTES_PER_PASS 48
// Output queue room a passed-through key may use. A full queue stops the transport, so a
// Note On (with the Note Off of a retrigger) leaves at most half the queue used; a Note
// Off may fill it, as it must not be refused for long.
#define THRU_ON_QUEUE_MAX  ((MIDI_TX_QUEUE_SIZE - 1u) / 2u)
// A release may use more of the queue than a new note, but never all of it:
// the sequencer's own sends must still fit, or the overflow rule stops it.
#define THRU_OFF_QUEUE_MAX ((MIDI_TX_QUEUE_SIZE - 1u) * 3u / 4u)
#define THRU_NONE 0xFF

// A pad held this long is a clip copy source for the next pad of its track; a
// MUTE key held this long collects the MUTE keys pressed next into a group.
#define PAD_COPY_HOLD_MS 500
#define PAD_ROWS 8
#define MUTE_NONE 0xFF          // no MUTE key is the held key

// ─── UI Modes ──────────────────────────────────────────────────

typedef enum {
    UI_SESSION,             // Home: pad grid, focus cell on the step row
    UI_PATTERN_VIEW,        // Main step grid
    UI_STEP_EDIT,           // Edit note/velocity/flags
    UI_PATTERN_SELECT,      // Choose bank A-D and pattern 1-8
    UI_TRACK_CONFIG,        // Track settings
    UI_SONG_EDIT,           // Song chain editor
    UI_KIT_STEP_EDIT,       // One kit step: all 12 lanes
    UI_MIDI_SONG,           // Timed MIDI Song playback, eight tracks per page
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
    uint8_t  midi_page;

    // Tempo LED
    uint32_t last_blink_ms;
    bool     led_on;
    uint32_t led_anim_ms;
    bool     led_anim_phase;

    // Pad presses the UI has handled and not yet seen released, per track (bit
    // = row), and what each was when pressed: the time, the location it stood
    // for and the panel layer's sync generation. Only a pad in pad_seen can
    // be a copy source; see find_copy_source().
    uint8_t  pad_seen[NUM_TRACKS];
    uint32_t pad_down_ms[NUM_TRACKS][PAD_ROWS];
    uint8_t  pad_location[NUM_TRACKS][PAD_ROWS];
    uint32_t pad_generation[NUM_TRACKS][PAD_ROWS];

    // The pads as a keyboard while a step is held: one snapshot per pass, taken
    // by update_pad_keys(). The pad routing of the pass and the frame of the
    // lights both read it; nothing works out "is the keyboard on" again later.
    // It holds whether the keyboard is on, the track and location it writes
    // to, the step whose note it marks and (in the step editor) whether that
    // is the edited step, and the C of its bottom-left pad, which is fixed when
    // the keyboard turns on.
    bool     pad_keys_on;
    bool     pad_keys_edit;
    uint8_t  pad_keys_base;
    uint8_t  pad_keys_track;
    uint8_t  pad_keys_location;
    uint8_t  pad_keys_step;
    // For each pad pressed as a key the note it stands for plus one (0 = not a
    // key) and the panel layer's generation then. A key is released by what
    // was remembered at its press, not by whether the keyboard is still on.
    uint8_t  pad_key[PANEL_TRACKS][PAD_ROWS];
    uint32_t pad_key_generation[PANEL_TRACKS][PAD_ROWS];
    // Steps a key has written to during the hold of their button in the step
    // editor: that hold is a modifier, so its long press does not toggle.
    uint16_t key_wrote;

    // The held MUTE key (MUTE_NONE when none): the physical key pressed while no
    // key was held, the logical track it stood for then, the time the UI handled
    // its press and the panel layer's generation then. Keys pressed after the
    // hold time join the group (logical tracks, each with its target: bit set =
    // muted), applied when the held key is released.
    uint8_t  mute_held;
    uint8_t  mute_held_track;
    uint32_t mute_held_ms;
    uint32_t mute_held_generation;
    track_mask_t mute_group;
    track_mask_t mute_group_target;

    // The pitch last entered on each track: a new SYNTH step starts there.
    // UI RAM only; not saved, not cleared by Stop, Panic or load.
    uint8_t last_note[NUM_TRACKS];

    // MIDI keyboard on the DIN input
    midi_in_parser_t midi_parser;
    uint32_t midi_dropped_seen;     // midi_in_dropped() when last looked at
    uint8_t  thru_channel[128];     // channel a passed-through note sounds on, THRU_NONE = off
    bool     thru_release_pending[128]; // its Note Off was refused: tried again every pass
    uint8_t  thru_pending_count;

    // The tempo the screens show this frame (update_shown_tempo): the prefix (NULL
    // with INT), the value, and the external tempo's shown tenth, which moves only
    // when the measurement leaves it by more than 0.07 BPM (0 = none shown).
    const char *tempo_prefix;
    float    tempo_bpm;
    uint16_t ext_tenths;

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
    ui.key_wrote = 0;
    for (uint8_t enc = 0; enc < NUM_ENCODERS; enc++) {
        ui.grid_enc_target[enc] = 0;
        ui.grid_enc_long_done[enc] = 0;
    }
}

// Forgets every pad hold: after a panic and after a save or load, whatever
// its result, a copy source has to be pressed again. The notes of pad keys
// have been released by then (the pass-through table is handled there), so
// only the memory of them goes.
static void drop_pad_holds(void) {
    memset(ui.pad_seen, 0, sizeof(ui.pad_seen));
    memset(ui.pad_key, 0, sizeof(ui.pad_key));
}

// Forgets the held MUTE key and its group, changing nothing in the transport:
// a change withdrawn into the group is gone with it. Wherever pad holds are
// dropped, and when the panel layer may have lost the hold.
static void drop_mute_hold(void) {
    ui.mute_held = MUTE_NONE;
    ui.mute_group = 0;
    ui.mute_group_target = 0;
}

// Toggles one step. A SYNTH step that is switched on with no stored note
// takes the track's last note: seeded after pattern_toggle_step(), which
// writes C4 over a note of 0, so a last note of 0 survives.
static void toggle_step(pattern_t *pat, uint8_t track, uint8_t step) {
    step_t *st = &pat->steps[track][step];
    bool seed = pat->tracks[track].type == TRACK_TYPE_SYNTH &&
                !(st->flags & STEP_FLAG_ACTIVE) && st->note == 0;
    pattern_toggle_step(pat, track, step);
    if (seed) st->note = ui.last_note[track];
}

// Toggles the steps in `mask` of one track at one location and refreshes its
// playback. Takes seq_lock: call with no lock held.
static void toggle_steps(uint8_t track, uint8_t location, uint16_t mask) {
    if (!mask) return;
    uint32_t lock_state = seq_lock();
    pattern_t *pat = &g_seq.patterns[location];
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (mask & (1u << s)) toggle_step(pat, track, s);
    }
    seq_unlock(lock_state);
    sequencer_refresh_track_playback(track, location);
}

// ─── MIDI keyboard ─────────────────────────────────────────────

// A key goes out on the channel of the track being edited, and is released on the channel
// it started on, whatever the focus did meanwhile. Only the capped send is used: the
// ordinary ones set the overflow flag, which stops the transport.

static void thru_forget(void) {
    memset(ui.thru_channel, THRU_NONE, sizeof(ui.thru_channel));
    memset(ui.thru_release_pending, 0, sizeof(ui.thru_release_pending));
    ui.thru_pending_count = 0;
}

static void thru_note_off(uint8_t note) {
    uint8_t channel = ui.thru_channel[note];
    if (channel == THRU_NONE) return;
    const uint8_t message[] = {(uint8_t)(MIDI_NOTE_OFF | channel), note, 0};
    if (midi_try_send_bytes_capped(message, sizeof(message), THRU_OFF_QUEUE_MAX)) {
        ui.thru_channel[note] = THRU_NONE;
        if (ui.thru_release_pending[note]) {
            ui.thru_release_pending[note] = false;
            ui.thru_pending_count--;
        }
    } else if (!ui.thru_release_pending[note]) {
        // The note stays in the table, marked: every pass tries again.
        ui.thru_release_pending[note] = true;
        ui.thru_pending_count++;
    }
}

// Ends every note the keys hold. The pads shown as keys own nothing after it:
// a pad still down then ends no note when it is let go.
static void thru_release_all(void) {
    for (uint8_t note = 0; note < 128; note++) thru_note_off(note);
    memset(ui.pad_key, 0, sizeof(ui.pad_key));
}

static void thru_retry_releases(void) {
    if (ui.thru_pending_count == 0) return;
    for (uint8_t note = 0; note < 128; note++) {
        if (ui.thru_release_pending[note]) thru_note_off(note);
    }
}

// A Note On, with the Note Off of a key still down on the same pitch as one batch. If the
// queue has no room the key is dropped and not entered in the table. Returns whether it
// was sent.
static bool thru_note_on(uint8_t note, uint8_t channel, uint8_t velocity) {
    uint8_t message[6];
    uint16_t length = 0;
    uint8_t old = ui.thru_channel[note];
    if (old != THRU_NONE) {
        message[length++] = (uint8_t)(MIDI_NOTE_OFF | old);
        message[length++] = note;
        message[length++] = 0;
    }
    message[length++] = (uint8_t)(MIDI_NOTE_ON | channel);
    message[length++] = note;
    message[length++] = velocity;
    if (!midi_try_send_bytes_capped(message, length, THRU_ON_QUEUE_MAX)) return false;
    ui.thru_channel[note] = channel;
    if (ui.thru_release_pending[note]) {
        ui.thru_release_pending[note] = false;
        ui.thru_pending_count--;
    }
    return true;
}

// Bytes may have been lost, so the stream cannot be trusted: with running status the next
// bytes could read as a wrong note, and a Note Off may have been among the lost ones.
static void midi_in_gap(void) {
    midi_in_flush();
    midi_in_parser_reset(&ui.midi_parser);
    ui.midi_dropped_seen = midi_in_dropped();
    thru_release_all();
}

// The cell a key goes to: the focused one in the Session view, the configured track on
// its config screen, the selected track elsewhere.
static void midi_in_cell(uint8_t *track, uint8_t *location) {
    if (ui.mode == UI_SESSION) {
        session_focus_t focus = session_get_focus();
        *track = focus.track;
        *location = focus.location;
        return;
    }
    *track = ui.mode == UI_TRACK_CONFIG ? ui.config_track : ui.selected_track;
    *location = g_seq.active_pattern[*track];
}

// Writes a key into the steps in `mask`: one lock, then the playback refresh. A step that
// is off is switched on first when `activate` (Pattern view), as with the encoders. A
// DRUM step keeps its stored note. A pad key has no velocity of its own, so it passes
// set_velocity false and leaves the step's velocity as it is.
static void write_key_to_steps(uint8_t track, uint8_t location, uint16_t mask, bool activate,
                               uint8_t note, uint8_t velocity, bool set_velocity) {
    uint32_t lock_state = seq_lock();
    pattern_t *pat = &g_seq.patterns[location];
    bool synth = pat->tracks[track].type == TRACK_TYPE_SYNTH;
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (!(mask & (1u << s))) continue;
        step_t *st = &pat->steps[track][s];
        if (activate && !(st->flags & STEP_FLAG_ACTIVE)) toggle_step(pat, track, s);
        if (synth) st->note = note;
        if (set_velocity) st->velocity = velocity;
    }
    seq_unlock(lock_state);
    sequencer_refresh_track_playback(track, location);
}

// A key of the MIDI input, or a pad shown as one (set_velocity false: see
// write_key_to_steps()). Returns whether a Note On was passed through, so that
// its sender owns the note now; the step is written either way.
static bool handle_midi_event(const midi_in_event_t *event, const step_buttons_t *steps,
                              bool set_velocity) {
    if (event->kind == MIDI_IN_ALL_OFF) {
        thru_release_all();
        return false;
    }
    if (event->kind == MIDI_IN_NOTE_OFF) {
        thru_note_off(event->note);
        return false;
    }

    uint8_t track, location;
    midi_in_cell(&track, &location);
    const track_config_t *config = &g_seq.patterns[location].tracks[track];
    bool sounded = thru_note_on(event->note, config->midi_channel, event->velocity);
    if (config->type == TRACK_TYPE_KIT) return sounded;
    if (config->type == TRACK_TYPE_SYNTH) ui.last_note[track] = event->note;

    // A step is held when its button is down now: a step pressed or released in this
    // pass does not count.
    uint16_t mask = 0;
    bool activate = false;
    if (ui.mode == UI_PATTERN_VIEW) {
        mask = (uint16_t)(ui.grid_hold & steps->held);
        activate = true;
        // A step a key wrote to is not toggled on release.
        ui.grid_tap_pending &= (uint16_t)~mask;
    } else if (ui.mode == UI_STEP_EDIT) {
        mask = (uint16_t)(steps->held & (uint16_t)~steps->pressed & (1u << ui.edit_step));
    }
    if (mask && ui.mode == UI_STEP_EDIT) ui.key_wrote |= mask;
    if (mask) write_key_to_steps(track, location, mask, activate, event->note, event->velocity,
                                    set_velocity);
    return sounded;
}

// Called at the start of every pass, before the panic check.
static void handle_midi_input(const step_buttons_t *steps) {
    uint8_t byte;
    if (ui.panic_latched) {
        // Keys pressed during a panic do nothing, not even once it is released.
        midi_in_flush();        // constant time, however fast bytes arrive
        midi_in_parser_reset(&ui.midi_parser);
        ui.midi_dropped_seen = midi_in_dropped();
        return;
    }
    thru_retry_releases();
    if (midi_in_dropped() != ui.midi_dropped_seen) midi_in_gap();
    midi_in_event_t event;
    for (unsigned n = 0; n < MIDI_IN_BYTES_PER_PASS && midi_in_read(&byte); n++) {
        if (midi_in_parse(&ui.midi_parser, byte, &event)) handle_midi_event(&event, steps, true);
    }
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

// PLAY. A start that waits for the external clock is cancelled like a stop.
static void toggle_transport(void) {
    if (transport_running()) {
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
            // A byte received since this pass emptied the ring must not start
            // a message that bytes after the panic would complete.
            midi_in_flush();
            midi_in_parser_reset(&ui.midi_parser);
            ui.midi_dropped_seen = midi_in_dropped();
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
        // All Notes Off has gone out: the keys held down are released.
        thru_forget();
        midi_in_flush();
        midi_in_parser_reset(&ui.midi_parser);
        if (ui.mode == UI_MIDI_SONG) transport_set_mode(PLAY_MODE_PATTERN);
        // Panel keys held through the panic must not fire afterwards.
        panel_input_resync();
        drop_pad_holds();
        drop_mute_hold();
        ui.pad_keys_on = false;     // the screen goes to Session: no keyboard is shown
        ui.panic_latched = true;
        drop_step_gestures();
        ui.mode = UI_SESSION;
        show_notify("PANIC: ALL OFF");
        return true;
    }
    return false;
}

// Which eight logical tracks the eight physical pad columns address.
static uint8_t panel_first_track(void) {
    uint8_t track = ui.mode == UI_SESSION ? session_get_focus().track :
                    ui.mode == UI_TRACK_CONFIG ? ui.config_track : ui.selected_track;
    return (track / PANEL_TRACKS) * PANEL_TRACKS;
}

// ─── Global panel controls (every screen) ──────────────────────

// The pad of `track` that is a copy source for a press now, or -1: one whose
// press the UI handled and whose release it has not, that the panel layer
// still reports down with no recovery since, held at least PAD_COPY_HOLD_MS.
// The longest held wins. Each condition covers a way a hold can be unreal: a
// pad accepted in the same poll as the one pressed now, a release the panel
// layer lost, a key state it forgot.
static int find_copy_source(uint8_t track, uint8_t target_row, uint32_t now) {
    int source = -1;
    uint32_t longest = 0;
    for (uint8_t row = 0; row < PAD_ROWS; row++) {
        if (row == target_row || !(ui.pad_seen[track] & (1u << row))) continue;
        if (!panel_input_pad_down(track % PANEL_TRACKS, row)) continue;
        if (ui.pad_generation[track][row] != panel_input_generation()) continue;
        uint32_t held = (uint32_t)(now - ui.pad_down_ms[track][row]);
        if (held < PAD_COPY_HOLD_MS || (source >= 0 && held <= longest)) continue;
        source = row;
        longest = held;
    }
    return source;
}

// Copies the clip of `track` at `src` over `dst` and refreshes the target's
// playback. Takes seq_lock: call with no lock held. The transport is not
// touched; in the Session view the focus moves to the copy.
static void copy_clip(uint8_t track, uint8_t src, uint8_t dst) {
    uint32_t lock_state = seq_lock();
    pattern_copy_track(&g_seq.patterns[dst], &g_seq.patterns[src], track);
    seq_unlock(lock_state);
    sequencer_refresh_track_playback(track, dst);
    if (ui.mode == UI_SESSION) session_set_focus(track, dst);
    char msg[17];
    snprintf(msg, sizeof(msg), "COPY %c%d>%c%d", 'A' + pattern_bank(src),
             pattern_slot(src) + 1, 'A' + pattern_bank(dst), pattern_slot(dst) + 1);
    show_notify(msg);
}

// Takes the snapshot of the keyboard, once per pass and before the panel
// events: the pads are a keyboard when a step of a SYNTH cell is held (Pattern
// view) or the edited step's button is down (step editor). A step pressed in
// this pass does not count, as for the MIDI keys. The reference step is the
// lowest held step (the edited one in the step editor). When the keyboard
// turns on, the window is fixed to that step's note (the track's last note if
// the step has none): its octave is the second pair of rows from the bottom.
// It then stays put until the keyboard turns off, so it does not jump when a
// pad changes the step's note.
static void update_pad_keys(const step_buttons_t *steps) {
    uint8_t track = ui.selected_track;
    uint8_t location = g_seq.active_pattern[track];
    const pattern_t *cell = &g_seq.patterns[location];
    uint16_t mask = 0;
    if (cell->tracks[track].type == TRACK_TYPE_SYNTH) {
        if (ui.mode == UI_PATTERN_VIEW) {
            mask = (uint16_t)(ui.grid_hold & steps->held);
        } else if (ui.mode == UI_STEP_EDIT) {
            mask = (uint16_t)(steps->held & (uint16_t)~steps->pressed & (1u << ui.edit_step));
        }
    }
    bool on = mask != 0;
    uint8_t step = ui.edit_step;
    if (on && ui.mode == UI_PATTERN_VIEW) {
        step = 0;
        while (!(mask & (1u << step))) step++;
    }
    if (on && !ui.pad_keys_on) {
        const step_t *st = &cell->steps[track][step];
        uint8_t reference = (st->flags & STEP_FLAG_ACTIVE) || st->note ? st->note
                                                                      : ui.last_note[track];
        int base = 12 * (reference / 12 - 1);
        ui.pad_keys_base = (uint8_t)(base < 0 ? 0 : base > 72 ? 72 : base);
    }
    ui.pad_keys_on = on;
    ui.pad_keys_edit = ui.mode == UI_STEP_EDIT;
    ui.pad_keys_track = track;
    ui.pad_keys_location = location;
    ui.pad_keys_step = step;
}

// Ends a pad key: its Note Off by the pass-through rule, and the pad is no
// longer a key.
static void release_pad_key(uint8_t track, uint8_t row) {
    uint8_t remembered = ui.pad_key[track][row];
    if (!remembered) return;
    ui.pad_key[track][row] = 0;
    thru_note_off((uint8_t)(remembered - 1));
}

// A pad shown as a key is a key of the MIDI input at velocity 100 that does
// not change a step's velocity. A press whose note could not be sent owns no
// note: its release must not end one another key holds on that pitch.
static void press_pad_key(uint8_t track, uint8_t row, uint8_t note, const step_buttons_t *steps) {
    midi_in_event_t event = {MIDI_IN_NOTE_ON, 0, note, 100};
    if (!handle_midi_event(&event, steps, false)) return;
    ui.pad_key[track][row] = (uint8_t)(note + 1);
    ui.pad_key_generation[track][row] = panel_input_generation();
}

// A pad press: with the keyboard on, a key; else a copy when another pad of
// the track is a source, or the ordinary launch. A launcher press is recorded,
// so it can be a source; a key press is not.
static void handle_pad_press(uint8_t track, uint8_t row, const step_buttons_t *steps) {
    // A release the panel layer lost and the sweep has not found yet: end the
    // old key before this press stands for the pad.
    if (track < PANEL_TRACKS && row < PAD_ROWS) release_pad_key(track, row);
    if (ui.pad_keys_on && track < PANEL_TRACKS && row < PAD_ROWS) {
        // A pad shown as a key is never a copy source, and a stale hold of this
        // very pad ends, on whichever page it was made; the holds of other
        // pads are left alone.
        for (uint8_t t = track; t < NUM_TRACKS; t += PANEL_TRACKS)
            ui.pad_seen[t] &= (uint8_t)~(1u << row);
        uint8_t note = pad_key_note(ui.pad_keys_base, track, row);
        if (note != 0xFF) press_pad_key(track, row, note, steps);
        return;
    }
    track += panel_first_track();
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint8_t target = pattern_make_index(session_bank(), row);
    int source = find_copy_source(track, row, now);
    uint8_t source_location = source >= 0 ? ui.pad_location[track][source] : 0;

    ui.pad_seen[track] |= (uint8_t)(1u << row);
    ui.pad_down_ms[track][row] = now;
    ui.pad_location[track][row] = target;
    ui.pad_generation[track][row] = panel_input_generation();

    if (source >= 0) {
        copy_clip(track, source_location, target);
        return;
    }
    // Outside the Session view a pad only launches.
    if (!session_pad(track, row, ui.mode == UI_SESSION)) show_notify("SONG MODE");
}

// A press of MUTE key `key`, which stands for the logical track of the page shown
// now. With no held key it is an ordinary press (session_mute) and becomes the
// held key. While a key is held, another key pressed less than
// PAD_COPY_HOLD_MS after it, or after the panel layer's generation changed, is
// an ordinary press too; a later one joins the group instead of acting. The
// join does not ask whether the held key is still reported down: every MUTE
// key is sampled in one column, so a member's press and the held key's release
// can come in one pass, and the panel layer already reports the key up while
// its release waits behind the press. Each transport call takes the locks
// itself, and what transport_cancel_mutes() returns decides, as the timer can
// land a change between two calls.
static void handle_mute_press(uint8_t key) {
    uint8_t track = (uint8_t)(panel_first_track() + key);
    track_mask_t bit = TRACK_BIT(track);
    uint32_t now = to_ms_since_boot(get_absolute_time());
    // A press of the held key itself means its release was lost.
    if (ui.mute_held == key) drop_mute_hold();
    if (ui.mute_held == MUTE_NONE) {
        session_mute(track);
        ui.mute_held = key;
        ui.mute_held_track = track;
        ui.mute_held_ms = now;
        ui.mute_held_generation = panel_input_generation();
        return;
    }
    if ((uint32_t)(now - ui.mute_held_ms) < PAD_COPY_HOLD_MS ||
        ui.mute_held_generation != panel_input_generation()) {
        session_mute(track);
        return;
    }
    if (ui.mute_group & bit) {                      // a member pressed again leaves
        ui.mute_group &= ~bit;
        return;
    }
    if (transport_cancel_mutes(bit, NULL)) return;  // a waiting change is cancelled only
    if (!ui.mute_group) {
        // The held key comes in with the target of its change if that change
        // still waits; one that landed, acted at once or was cancelled (Stop)
        // stays out. This is tried whenever the group is empty, so again after
        // every member has left, not only for the first key to join. That is
        // the same thing: the first try withdraws the held key's change or
        // finds none, and while the key is held it cannot get a new waiting
        // change (its own press ends the hold, the selector acts at once), so
        // a later try finds nothing to withdraw.
        track_mask_t held_bit = TRACK_BIT(ui.mute_held_track);
        track_mask_t target = 0;
        if (transport_cancel_mutes(held_bit, &target) & held_bit) {
            ui.mute_group |= held_bit;
            ui.mute_group_target = (ui.mute_group_target & ~held_bit) | (target & held_bit);
        }
    }
    bool muted = (seq_get_track_mutes() & bit) != 0;
    ui.mute_group |= bit;
    ui.mute_group_target = (ui.mute_group_target & ~bit) | (muted ? 0 : bit);
}

// A MUTE release. Only the held key's counts: it sets the group to its
// targets, at once or at the next launch point, unless the group is empty or
// the panel layer's generation changed since its press. Then no key is held.
static void handle_mute_release(uint8_t key) {
    if (ui.mute_held != key) return;
    if (ui.mute_group && ui.mute_held_generation == panel_input_generation()) {
        transport_set_mutes(ui.mute_group, ui.mute_group_target);
    }
    drop_mute_hold();
}

// Pads, SCENE, MUTE and PLAY. Called with no lock held: every session_* and
// transport call below takes the locks itself. A pad release only ends the
// hold the UI tracks for copying, a MUTE release only the MUTE hold; SCENE and
// PLAY act on press.
static void handle_panel_events(const step_buttons_t *steps) {
    panel_event_t ev;
    while (panel_input_next(&ev)) {
        if (ev.kind == PANEL_MUTE) {
            if (ev.track >= PANEL_TRACKS) continue;
            // The MIDI Song player has MUTE keys of its own, on press.
            if (ui.mode == UI_MIDI_SONG) {
                if (ev.down) transport_midi_song_toggle(ui.midi_page * 8 + ev.track, false);
                continue;
            }
            if (ev.down) {
                handle_mute_press(ev.track);
            } else {
                handle_mute_release(ev.track);
            }
            continue;
        }
        // The pad tables are 8 by 8: a pad outside them is not one.
        if (ev.kind == PANEL_PAD && (ev.track >= PANEL_TRACKS || ev.row >= PAD_ROWS)) continue;
        if (ev.kind == PANEL_PAD && !ev.down && ev.track < PANEL_TRACKS) {
            if (ev.row < PAD_ROWS && ui.pad_key[ev.track][ev.row]) {
                release_pad_key(ev.track, ev.row);
                continue;
            }
            for (uint8_t t = ev.track; t < NUM_TRACKS; t += PANEL_TRACKS)
                ui.pad_seen[t] &= (uint8_t)~(1u << ev.row);
        }
        if (!ev.down) continue;
        if (ui.mode == UI_MIDI_SONG) {
            if (ev.kind == PANEL_PLAY) toggle_transport();
            continue;
        }
        switch (ev.kind) {
            case PANEL_PAD:
                handle_pad_press(ev.track, ev.row, steps);
                break;
            case PANEL_SCENE:
                if (!session_scene(ev.row)) show_notify("SONG MODE");
                break;
            case PANEL_MUTE:        // handled above
                break;
            case PANEL_PLAY:
                toggle_transport();
                break;
        }
    }
}

// A key whose release the panel layer lost (a lost scanner, resync) is
// released here: a pad it no longer reports down is not held, and a pad that
// still reads down after the panel layer lost its key state (its generation
// changed) is not trusted either. That covers a key pressed in this very
// pass: the panel layer can lose its key state in a poll and still hand out a
// press it had accepted before, which would then carry the new generation and
// never look lost. `generation_before_poll` is the generation before this
// pass's poll. Run after the pass's events, so a release that did arrive has
// been handled first.
static void release_lost_pad_keys(uint32_t generation_before_poll) {
    bool lost_now = generation_before_poll != panel_input_generation();
    for (uint8_t track = 0; track < PANEL_TRACKS; track++) {
        for (uint8_t row = 0; row < PAD_ROWS; row++) {
            if (!ui.pad_key[track][row]) continue;
            if (lost_now || !panel_input_pad_down(track, row) ||
                ui.pad_key_generation[track][row] != panel_input_generation()) {
                release_pad_key(track, row);
            }
        }
    }
}

// The same for the held MUTE key, on the same terms: one the panel layer no
// longer reports down, or whose hold it may have lost, is dropped with its
// group. Run after the pass's events.
static void drop_lost_mute_hold(uint32_t generation_before_poll) {
    if (ui.mute_held == MUTE_NONE) return;
    if (generation_before_poll != panel_input_generation() ||
        !panel_input_mute_down(ui.mute_held) ||
        ui.mute_held_generation != panel_input_generation()) {
        drop_mute_hold();
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

// A VALUE turn on the tempo: 0.1 BPM a click, only while the internal timer is the
// source (INT, or AUTO with no external clock); otherwise it says so.
static void tempo_turn(int8_t amount) {
    if (amount == 0) return;
    if (!transport_tempo_is_internal()) {
        show_notify("TEMPO EXT");
        return;
    }
    transport_set_bpm(g_seq.transport.bpm + (float)amount * 0.1f);
}

static void handle_session_view(encoder_turn_t e1, encoder_turn_t e2,
                                button_event_t e1b, button_event_t e2b,
                                step_buttons_t *steps) {
    // TRACK rotate: walk the stops, a kit's lanes included.
    if (e1.detents != 0) {
        session_move_focus(e1.detents);
    }

    // VALUE rotate: tempo.
    tempo_turn(e2.amount);

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
    track_mask_t kit_mask = kit_cell_mask(g_seq.patterns, g_seq.active_pattern);
    uint8_t track = ui.selected_track;
    uint8_t lane = session_kit_lane(track);
    session_walk_stops(&track, &lane, detents, kit_mask);
    ui.selected_track = track;
    if (kit_mask & TRACK_BIT(track)) session_set_kit_lane(track, lane);
}

// The Pattern view's encoders when no hold gesture owns them: TRACK walks,
// VALUE sets the tempo, TRACK press opens the kit step editor on step 1 of a
// kit cell, else the note editor, TRACK long returns to the Session view and
// VALUE long opens Pattern Select. VALUE press has no action here.
static void navigate_pattern_view(encoder_turn_t e1, encoder_turn_t e2,
                                  button_event_t e1b, button_event_t e2b) {
    if (e1.detents != 0) walk_pattern_view(e1.detents);

    tempo_turn(e2.amount);

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
// Returns true if any step changed; an activation counts. A turn that changed
// a note on a SYNTH track makes the lowest target's resulting note the track's
// last note. Caller holds seq_lock().
static bool edit_held_steps(pattern_t *pat, uint8_t track, uint16_t mask,
                            int8_t e1_amount, int8_t e2_amount,
                            button_event_t e1b, button_event_t e2b) {
    bool changed = false;
    bool note_changed = false;
    int lowest = -1;
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (!(mask & (1u << s))) continue;
        step_t *st = &pat->steps[track][s];
        if (!(st->flags & STEP_FLAG_ACTIVE)) {
            toggle_step(pat, track, s);
            changed = true;
        }
        uint8_t note_before = st->note;
        if (apply_step_edit(st, e1_amount, e2_amount, e1b, e2b)) changed = true;
        if (st->note != note_before) note_changed = true;
        if (lowest < 0) lowest = s;
    }
    if (note_changed && pat->tracks[track].type == TRACK_TYPE_SYNTH) {
        ui.last_note[track] = pat->steps[track][lowest].note;
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
                toggle_step(pat, track, s);
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

    uint8_t note_before = st->note;
    if (apply_step_edit(st, e1.amount, e2.amount, e1b, e2b)) changed = true;
    if (st->note != note_before && pat->tracks[track].type == TRACK_TYPE_SYNTH) {
        ui.last_note[track] = st->note;
    }

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
                toggle_step(pat, track, s);
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
    // track configuration never changes the audible mute state. Buttons 1-8
    // act at once in every mode: the track leaves the mute group, then its
    // waiting change is cancelled and it toggles, in one engine call.
    uint16_t short_releases = take_short_releases(&ui.select_short_pending, steps,
                                                  0xFFu, NULL);
    if (short_releases) {
        track_mask_t tracks = (track_mask_t)(short_releases & 0xFFu) << panel_first_track();
        ui.mute_group &= ~tracks;
        transport_toggle_mutes_now(tracks);
    }

    // Step button long press on 1–8: enter track config
    if (steps->long_pressed) {
        for (int t = 0; t < PANEL_TRACKS; t++) {
            if (steps->long_pressed & (1u << t)) {
                ui.config_track = panel_first_track() + t;
                ui.mode = UI_TRACK_CONFIG;
                ui.config_param = 0;
                ui.select_short_pending = 0;
                break;
            }
        }
    }

    // Step button 9 (index 8): save to flash
    if (steps->pressed & (1u << 8)) {
        if (transport_running()) {
            show_notify("STOP TO SAVE");
        } else {
            thru_release_all();     // the stall would leave a held key sounding
            midi_wait_output_idle();    // a release the full queue refused goes out
            thru_retry_releases();      // now, not after the stall
            show_notify(flash_storage_save() ? "SAVED OK" : "SAVE FAILED");
            midi_in_gap();          // interrupts were off: bytes may be lost
            drop_step_gestures();   // steps held through the stall do nothing
            drop_pad_holds();
            drop_mute_hold();
        }
    }

    // Step button 10 (index 9): load from flash
    if (steps->pressed & (1u << 9)) {
        if (transport_running()) {
            show_notify("STOP TO LOAD");
        } else {
            thru_release_all();
            midi_wait_output_idle();
            thru_retry_releases();
            // The eight-track save is named: it may not be the last one made.
            static const char *const loaded[] = {
                [FLASH_LOAD_FAILED] = "LOAD FAILED",
                [FLASH_LOAD_OK] = "LOADED OK",
                [FLASH_LOAD_OLD_SAVE] = "OLD SAVE LOADED",
            };
            show_notify(loaded[flash_storage_load()]);
            midi_in_gap();
            drop_step_gestures();
            drop_pad_holds();       // also when nothing was loaded
            drop_mute_hold();
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
    // Step 14 enters the flash-backed MIDI Song player.
    if (steps->pressed & (1u << 13)) {
        if (g_seq.transport.state != TRANSPORT_STOPPED) show_notify("STOP FIRST");
        else if (!ui_open_midi_song()) show_notify("NO MIDI SONG");
    }

    // Step button 15 (index 14): the clock source, INT -> EXT -> AUTO (not saved)
    if (steps->pressed & (1u << 14)) {
        static const char *const names[CLOCK_SOURCE_COUNT] = {
            "CLOCK INT", "CLOCK EXT", "CLOCK AUTO",
        };
        clock_source_t source = (clock_source_t)((transport_get_clock_source() + 1u) %
                                                 CLOCK_SOURCE_COUNT);
        transport_set_clock_source(source);
        show_notify(names[source]);
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
    uint8_t mutes = (uint8_t)(seq_get_track_mutes() >> panel_first_track());

    // Buttons 1-8 represent track mute state (lit = audible: neither the live
    // mute nor the stored mute of the playing location is set).
    for (uint8_t t = 0; t < PANEL_TRACKS; t++) {
        if (!(mutes & (1u << t))) {
            mask |= (uint16_t)(1u << t);
        }
    }
    // Buttons 11/12 indicate length controls.
    mask |= (1u << 10);
    mask |= (1u << 11);
    // Button 13 resets jitter stats.
    mask |= (1u << 12);
    // Button 15 is lit while the clock source is not INT.
    if (transport_get_clock_source() != CLOCK_SOURCE_INT) mask |= (1u << 14);
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
                mask = (uint16_t)(1u << (ui.config_track % PANEL_TRACKS));
            }
            break;

        case UI_SONG_EDIT:
            // Button 1 = loop/stop, button 2 = song, button 3 = pattern playback.
            mask = (1u << 0) | (1u << 1) | (1u << 2);
            break;

        case UI_MIDI_SONG: {
            midi_song_view_t view;
            transport_midi_song_view(&view);
            for (uint8_t col = 0; col < 8; col++) {
                const uint8_t track = ui.midi_page * 8 + col;
                const uint64_t bit = UINT64_C(1) << track;
                if ((view.present & bit) && (view.solo >= 0 ? view.solo == track : !(view.muted & bit))) {
                    mask |= (uint16_t)(1u << col);
                }
            }
            break;
        }
    }

    step_leds_set_mask(mask);
}

// ─── Track Config Handler ──────────────────────────────────────

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
    if (track >= KIT_TRACK_SLOTS && config.type == TRACK_TYPE_KIT)
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

// CHANNEL, GATE, CV/Gate pair and a drum's NOTE are written to all 32
// locations when it changes.
static void edit_track_param(uint8_t track, track_config_t config, uint8_t param,
                             int8_t delta) {
    uint8_t *field;
    uint8_t lo = 0, hi = 127;
    switch (param) {
        case TRACK_PARAM_CHANNEL: field = &config.midi_channel; hi = 15; break;
        case TRACK_PARAM_GATE: field = &config.gate_length_pct; lo = 10; hi = 100; break;
        case TRACK_PARAM_NOTE:
            if (config.type == TRACK_TYPE_DRUM) {
                field = &config.drum_note;
                break;
            }
            // A synth has CV/Gate in the same position.
            // Falls through.
        case TRACK_PARAM_DRUM_CV: field = &config.cv_channel; hi = NUM_CV_CHANNELS - 1; break;
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
    // the others TYPE, CHANNEL, OUTPUT, GATE, (drum) NOTE, and CV/Gate.
    uint8_t last = kit ? KIT_PARAM_COUNT - 1
                       : (config.type == TRACK_TYPE_DRUM ? TRACK_PARAM_DRUM_CV : TRACK_PARAM_SYNTH_CV);
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

static void midi_song_lights(rgb_t frame[LED_COUNT], bool playing) {
    midi_song_view_t view;
    transport_midi_song_view(&view);
    memset(frame, 0, sizeof(rgb_t) * LED_COUNT);
    for (uint8_t track = 0; track < view.tracks; track++) {
        const uint64_t bit = UINT64_C(1) << track;
        if (!(view.present & bit)) continue;
        const bool enabled = view.solo >= 0 ? view.solo == track : !(view.muted & bit);
        const rgb_t color = !enabled ? (rgb_t){ LED_LEVEL_HALF, 0, 0 }
            : view.active & bit ? (rgb_t){ 0, LED_LEVEL_FULL, LED_LEVEL_HALF }
            : (rgb_t){ 0, LED_LEVEL_HALF, LED_LEVEL_DIM };
        if (track / 8 == ui.midi_page) frame[LED_MUTE(track % 8)] = color;
    }
    if (playing) frame[LED_PLAY] = (rgb_t){ 0, LED_LEVEL_FULL, 0 };
}

// Builds the 82-LED frame from the frame's transport snapshot and starts
// sending it; a frame the chain is still busy with is skipped. Called with no
// lock held: sequencer_clip_mask takes seq_lock itself.
static void update_panel_lights(const transport_snapshot_t *snap) {
    if (ui.mode == UI_MIDI_SONG) {
        rgb_t frame[LED_COUNT];
        midi_song_lights(frame, snap->state == TRANSPORT_PLAYING);
        led_frame_apply_budget(frame, LED_RGB_BUDGET_MA);
        rgb_leds_show(frame);
        return;
    }
    led_state_t state = {0};
    state.bank = session_bank();
    state.first_track = panel_first_track();
    state.clip_mask = sequencer_page_clips(state.bank, state.first_track / PANEL_TRACKS);
    uint64_t occupied = 0;
    for (uint8_t page = 0; page < NUM_TRACKS / PANEL_TRACKS; page++)
        occupied |= sequencer_page_clips(state.bank, page);
    occupied |= occupied >> 32;
    occupied |= occupied >> 16;
    state.occupied_rows = (uint8_t)(occupied | (occupied >> 8));
    memcpy(state.current, snap->current, sizeof(state.current));
    memcpy(state.queued, snap->queued, sizeof(state.queued));
    state.muted = snap->live_mutes | snap->stored_mutes;
    state.mute_pending = snap->mute_pending;
    state.mute_group = ui.mute_group;
    state.focus_track = session_get_focus().track;
    state.playing = snap->state == TRANSPORT_PLAYING;
    if (state.playing) {
        state.pulse = snap->pulse_count;
    } else {
        // Blinks keep a 120 BPM clock while stopped.
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        state.pulse = (uint32_t)(((uint64_t)now_ms * 2 * PPQN) / 1000);
    }

    // The keyboard of this pass's snapshot; the note it marks is read now, so
    // it follows a pad just pressed.
    state.keys = ui.pad_keys_on;
    state.keys_base = ui.pad_keys_base;
    state.keys_track = ui.pad_keys_track;
    state.keys_note = 0xFF;
    if (state.keys) {
        const step_t *st = &g_seq.patterns[ui.pad_keys_location]
                                .steps[ui.pad_keys_track][ui.pad_keys_step];
        bool marked = (st->flags & STEP_FLAG_ACTIVE) || (ui.pad_keys_edit && st->note != 0);
        if (marked) state.keys_note = st->note;
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
                              ui.tempo_bpm, transport_get_launch_quant(),
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
        // A SYNTH roll prints the tempo in its own info column.
        if (pat->tracks[track].type == TRACK_TYPE_SYNTH) return;
    }
    display_draw_bpm(ui.tempo_prefix, ui.tempo_bpm);
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

// The tempo the screens show, from the frame's snapshot. INT as always. With EXT and
// AUTO the source comes first: "EXT" and the measured tempo while the external clock
// is followed or (stopped) present, "EXT --.-" in EXT without one, "INT" and the
// internal tempo when AUTO runs on its own timer. The prefix blinks while a start
// waits for the first Clock.
static void update_shown_tempo(const transport_snapshot_t *snap) {
    if (snap->clock_source == CLOCK_SOURCE_INT) {
        ui.tempo_prefix = NULL;
        ui.tempo_bpm = snap->bpm;
        ui.ext_tenths = 0;
        display_set_tempo(NULL, 0.0f);      // the screens read transport.bpm
        return;
    }
    bool clock = snap->ext_following || snap->ext_present;
    bool external = snap->clock_source == CLOCK_SOURCE_EXT || snap->ext_following ||
                    (snap->state != TRANSPORT_PLAYING && snap->ext_present);
    float bpm = snap->bpm;
    if (external) {
        bpm = 0.0f;
        if (clock && snap->ext_shown_x16) {
            float measured = 40000000.0f / (float)snap->ext_shown_x16;  // 60e6 / (24 x interval)
            if (measured > 999.9f) measured = 999.9f;
            if (ui.ext_tenths == 0 || fabsf(measured * 10.0f - (float)ui.ext_tenths) > 0.7f) {
                ui.ext_tenths = (uint16_t)(measured * 10.0f + 0.5f);
            }
            bpm = (float)ui.ext_tenths / 10.0f;
        } else {
            ui.ext_tenths = 0;
        }
    } else {
        ui.ext_tenths = 0;
    }
    const char *prefix = external ? "EXT" : "INT";
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (snap->ext_armed && (now_ms / 250u) % 2u) prefix = "   ";
    ui.tempo_prefix = prefix;
    ui.tempo_bpm = bpm;
    display_set_tempo(prefix, bpm);
}

bool ui_open_midi_song(void) {
    if (!transport_open_midi_song()) return false;
    drop_mute_hold();       // its MUTE keys are the player's from here
    ui.midi_page = 0;
    ui.select_short_pending = 0;
    ui.mode = UI_MIDI_SONG;
    return true;
}

static void handle_midi_song(encoder_turn_t track, button_event_t value, const step_buttons_t *steps) {
    midi_song_view_t view;
    transport_midi_song_view(&view);
    const uint8_t last_page = view.tracks ? (view.tracks - 1) / 8 : 0;
    if (track.detents) {
        ui.midi_page = (uint8_t)clamp_i8(ui.midi_page + track.detents, 0, last_page);
        ui.select_short_pending = 0;
    }
    uint16_t long_presses = 0;
    const uint16_t short_presses = take_short_releases(&ui.select_short_pending, steps, 0xff, &long_presses);
    for (uint8_t i = 0; i < 8; i++) {
        if ((short_presses | long_presses) & (1u << i)) {
            transport_midi_song_toggle(ui.midi_page * 8 + i, (long_presses & (1u << i)) != 0);
        }
    }
    if (value == BTN_PRESS || value == BTN_LONG_PRESS) {
        transport_set_mode(PLAY_MODE_PATTERN);
        ui.mode = UI_PATTERN_SELECT;
        ui.select_short_pending = 0;
    }
}

static void render_midi_song(void) {
    midi_song_view_t view;
    transport_midi_song_view(&view);
    char line[22];
    snprintf(line, sizeof(line), "MIDI SONG  %u/%u", ui.midi_page + 1u, (view.tracks + 7u) / 8u);
    ssd1309_draw_string(0, 0, line, false);
    ssd1309_draw_string(0, 12, view.title, false);
    const unsigned sec = view.position_us / 1000000u, duration = view.duration_us / 1000000u;
    snprintf(line, sizeof(line), "%02u:%02u / %02u:%02u", sec / 60, sec % 60, duration / 60, duration % 60);
    ssd1309_draw_string(0, 24, line, false);
    for (uint8_t col = 0; col < 8 && ui.midi_page * 8 + col < view.tracks; col++) {
        const uint8_t track = ui.midi_page * 8 + col;
        snprintf(line, sizeof(line), "%2u", track + 1u);
        ssd1309_draw_string(col * 16, 38, line, view.solo >= 0 ? view.solo == track : !(view.muted & (UINT64_C(1) << track)));
    }
    ssd1309_draw_string(0, 54, "1-8 MUTE / HOLD SOLO", false);
}

void ui_render(void) {
    // One copy of the transport per frame, taken with no lock held.
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    update_panel_lights(&snap);
    update_shown_tempo(&snap);

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
                (uint8_t)(seq_get_track_mutes() >> panel_first_track()));
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
        case UI_MIDI_SONG:
            render_midi_song();
            break;
    }

    // Overlay flash notification if active
    render_notify();
}

// ─── Initialization ────────────────────────────────────────────

void ui_init(void) {
    encoder_init();
    mcp23017_init();
    panel_input_init();     // the key-scan MCP23017s share I2C0, set up by mcp23017_init
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
    for (uint8_t track = 0; track < NUM_TRACKS; track++) ui.last_note[track] = 60;
    drop_mute_hold();       // 0 would be MUTE 1 held
    thru_forget();
    // Bytes that arrived since midi_init() are dropped: if the ring overflowed
    // meanwhile, a Note On could be in it without its Note Off.
    midi_in_flush();
    midi_in_parser_reset(&ui.midi_parser);
    ui.midi_dropped_seen = midi_in_dropped();
    session_init();         // Reads the transport: call after sequencer_init
}

// ─── Main Update Loop ──────────────────────────────────────────

void ui_update(void) {
    // 1. Poll inputs: the panel on every pass, then encoders and step buttons
    uint32_t panel_generation = panel_input_generation();
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

    // Keys from the MIDI keyboard, before the panic check: a panic clears what they sound.
    handle_midi_input(steps);

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
    update_pad_keys(steps);
    handle_panel_events(steps);
    release_lost_pad_keys(panel_generation);
    drop_lost_mute_hold(panel_generation);

    // A long press counts only for a step pressed on the screen it fires on.
    ui_mode_t screen = ui.mode;
    ui.step_gestures |= steps->pressed;
    steps->long_pressed &= ui.step_gestures;
    steps->long_pressed &= (uint16_t)~ui.key_wrote;     // a button used as a key's modifier

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
        case UI_MIDI_SONG:
            handle_midi_song(e1, e2b, steps);
            break;
    }

    // 5. Update step-button LEDs for current UI mode.
    update_step_leds();

    // 6. Clear consumed button events; a screen change ends every gesture.
    ui.step_gestures &= (uint16_t)~steps->released;
    ui.key_wrote &= (uint16_t)~steps->released;
    if (ui.mode != screen) {
        drop_step_gestures();
        ui.pad_keys_on = false;
    }
    steps->pressed = 0;
    steps->released = 0;
    steps->long_pressed = 0;
}
