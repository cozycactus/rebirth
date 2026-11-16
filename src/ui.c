#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>
#include "sequencer.h"
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
} ui_mode_t;

// ─── UI State ──────────────────────────────────────────────────

typedef struct {
    ui_mode_t mode;
    bool panic_latched;
    uint16_t step_gestures;         // steps pressed on this screen, still held
    uint16_t select_short_pending;
    uint16_t step_short_pending;    // SESSION: steps pressed here, toggled on release
    session_focus_t step_cell[STEPS_PER_PATTERN]; // SESSION: cell focused at each press

    // PATTERN_VIEW
    uint8_t selected_track;         // 0–7

    // STEP_EDIT
    uint8_t edit_step;              // 0–15
    ui_mode_t step_edit_return;     // Screen the step editor was opened from

    // PATTERN_SELECT
    uint8_t select_pattern;         // 0–31 (bank A-D × slot 1-8)

    // TRACK_CONFIG
    uint8_t config_track;           // 0–7
    uint8_t config_param;           // 0–4

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

// Points the editing screens (selected_track, active_pattern) at the focus.
static void target_focus(session_focus_t focus) {
    ui.selected_track = focus.track;
    uint32_t lock_state = seq_lock();
    g_seq.active_pattern[focus.track] = focus.location;
    seq_unlock(lock_state);
}

static void handle_session_view(int8_t e1d, int8_t e2d,
                                button_event_t e1b, button_event_t e2b,
                                step_buttons_t *steps) {
    // TRACK rotate: focus the previous/next track.
    if (e1d != 0) {
        session_move_focus(e1d);
    }

    // VALUE rotate: tempo.
    if (e2d != 0) {
        float new_bpm = g_seq.transport.bpm + (float)e2d * 0.1f;
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

    // A step press belongs to the cell focused when it was made, whatever the
    // focus does before release; steps beyond that cell's length do nothing.
    uint8_t len = clamp_pattern_length(
        pattern_get_track_length(&g_seq.patterns[focus.location], focus.track));
    uint16_t in_length = (uint16_t)((1u << len) - 1u);
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (steps->pressed & in_length & (1u << s)) ui.step_cell[s] = focus;
    }

    // Only keys pressed in this view count: a short press toggles on release,
    // a long press opens the step editor on that step without toggling it.
    uint16_t long_presses;
    uint16_t short_releases = take_short_releases(&ui.step_short_pending, steps,
                                                  in_length, &long_presses);
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (short_releases & (1u << s)) {
            toggle_steps(ui.step_cell[s].track, ui.step_cell[s].location,
                         (uint16_t)(1u << s));
        }
    }

    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        if (long_presses & (1u << s)) {
            target_focus(ui.step_cell[s]);
            ui.edit_step = s;
            ui.step_edit_return = UI_SESSION;
            ui.mode = UI_STEP_EDIT;
            break;
        }
    }
}

// ─── Pattern View Handler ──────────────────────────────────────

static void handle_pattern_view(int8_t e1d, int8_t e2d,
                                 button_event_t e1b, button_event_t e2b,
                                 step_buttons_t *steps) {
    // Enc1 rotate: select track
    if (e1d != 0) {
        int16_t t = (int16_t)ui.selected_track + e1d;
        ui.selected_track = clamp_i8(t, 0, NUM_TRACKS - 1);
    }

    // Enc2 rotate: adjust BPM
    if (e2d != 0) {
        float new_bpm = g_seq.transport.bpm + (float)e2d * 0.1f;
        transport_set_bpm(new_bpm);
    }

    // Enc1 press: enter step edit
    if (e1b == BTN_PRESS) {
        ui.mode = UI_STEP_EDIT;
        ui.edit_step = 0;
        ui.step_edit_return = UI_PATTERN_VIEW;
    }

    // Enc1 long: back to the Session view
    if (e1b == BTN_LONG_PRESS) {
        ui.mode = UI_SESSION;
    }

    // Enc2 long press: enter pattern select
    if (e2b == BTN_LONG_PRESS) {
        ui.mode = UI_PATTERN_SELECT;
        ui.select_pattern = g_seq.active_pattern[ui.selected_track];
    }

    // Step buttons: toggle steps on current track (active_pattern is Core 0's)
    toggle_steps(ui.selected_track, g_seq.active_pattern[ui.selected_track],
                 steps->pressed);
}

// ─── Step Edit Handler ─────────────────────────────────────────

static void handle_step_edit(int8_t e1d, int8_t e2d,
                              button_event_t e1b, button_event_t e2b,
                              step_buttons_t *steps) {
    uint32_t lock_state = seq_lock();
    uint8_t track = ui.selected_track;
    uint8_t pattern_index = g_seq.active_pattern[track];
    bool changed = false;
    pattern_t *pat = &g_seq.patterns[pattern_index];
    step_t *st = &pat->steps[track][ui.edit_step];

    // Enc1 rotate: change note
    if (e1d != 0) {
        int16_t note = (int16_t)st->note + e1d;
        uint8_t next_note = clamp_i8(note, 0, 127);
        if (st->note != next_note) {
            st->note = next_note;
            changed = true;
        }
    }

    // Enc2 rotate: change velocity
    if (e2d != 0) {
        int16_t vel = (int16_t)st->velocity + e2d;
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

static void handle_pattern_select(int8_t e1d, int8_t e2d,
                                   button_event_t e1b, button_event_t e2b,
                                   step_buttons_t *steps) {
    // Enc1 rotate: select pattern slot 1-8 within the current bank.
    if (e1d != 0) {
        int16_t slot = (int16_t)pattern_slot(ui.select_pattern) + e1d;
        if (slot < 0) slot = 0;
        if (slot >= PATTERNS_PER_BANK) slot = PATTERNS_PER_BANK - 1;
        ui.select_pattern = pattern_make_index(pattern_bank(ui.select_pattern),
                                               (uint8_t)slot);
    }

    // Enc2 rotate: select bank A-D.
    if (e2d != 0) {
        int16_t bank = (int16_t)pattern_bank(ui.select_pattern) + (e2d > 0 ? 1 : -1);
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

// On for the first 8th note of every quarter, like the panel's fast blink.
static bool eighth_blink_on(uint32_t pulse) {
    return (pulse / (PPQN / 2)) % 2 == 0;
}

// Session step LEDs: the focused cell's active steps within its length. While
// the cell is the playing location of its track, its playhead blinks on 8th
// notes of the transport. Uses the frame's snapshot; takes no lock.
static uint16_t build_session_mask(const transport_snapshot_t *snap) {
    session_focus_t focus = session_get_focus();
    const pattern_t *pat = &g_seq.patterns[focus.location];
    uint16_t mask = build_step_activity_mask(pat, focus.track);

    uint8_t playhead = snap->track_step[focus.track];
    if (snap->state == TRANSPORT_PLAYING &&
        snap->current[focus.track] == focus.location &&
        playhead < clamp_pattern_length(pattern_get_track_length(pat, focus.track)) &&
        eighth_blink_on(snap->pulse_count)) {
        mask ^= (uint16_t)(1u << playhead);
    }
    return mask;
}

static uint16_t build_pattern_select_mask(void) {
    uint16_t mask = 0;
    uint8_t mutes = seq_get_track_mutes();

    // Buttons 1-8 represent track mute state (lit = unmuted).
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
            mask = build_step_activity_mask(p, ui.selected_track);
            if (g_seq.transport.state == TRANSPORT_PLAYING &&
                g_seq.active_pattern[ui.selected_track] == g_seq.transport.current_pattern[ui.selected_track] &&
                ui.led_anim_phase &&
                g_seq.transport.current_track_step[ui.selected_track] <
                    clamp_pattern_length(pattern_get_track_length(p, ui.selected_track))) {
                mask ^= (uint16_t)(1u << g_seq.transport.current_track_step[ui.selected_track]);
            }
            break;
        }

        case UI_STEP_EDIT: {
            pattern_t *p = seq_active_pattern(ui.selected_track);
            mask = build_step_activity_mask(p, ui.selected_track);

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

static void handle_track_config(int8_t e1d, int8_t e2d,
                                 button_event_t e1b, button_event_t e2b,
                                 step_buttons_t *steps) {
    (void)steps;
    uint8_t edited_track = ui.config_track;
    uint32_t lock_state = seq_lock();
    track_config_t config = g_seq.patterns[g_seq.active_pattern[edited_track]]
                                .tracks[edited_track];
    seq_unlock(lock_state);
    track_config_t *tc = &config;
    bool changed = false;

    int max_params = (tc->type == TRACK_TYPE_DRUM) ? 4 : 3;

    // Enc1 rotate: select parameter
    if (e1d != 0) {
        int16_t p = (int16_t)ui.config_param + e1d;
        ui.config_param = clamp_i8(p, 0, max_params);
    }

    // Enc2 rotate: adjust value
    if (e2d != 0) {
        switch (ui.config_param) {
            case 0: {
                // Track type: synth / drum
                tc->type = (tc->type == TRACK_TYPE_SYNTH)
                           ? TRACK_TYPE_DRUM : TRACK_TYPE_SYNTH;
                changed = true;
                break;
            }
            case 1: {
                // MIDI channel 0–15
                int16_t ch = (int16_t)tc->midi_channel + e2d;
                uint8_t next_channel = clamp_i8(ch, 0, 15);
                if (tc->midi_channel != next_channel) {
                    tc->midi_channel = next_channel;
                    changed = true;
                }
                break;
            }
            case 2: {
                // Output mode: cycle through 3 options
                int16_t m = (int16_t)tc->output_mode + (e2d > 0 ? 1 : -1);
                if (m < 0) m = 2;
                if (m > 2) m = 0;
                tc->output_mode = (output_mode_t)m;
                changed = true;
                break;
            }
            case 3: {
                // Gate length 10–100%
                int16_t g = (int16_t)tc->gate_length_pct + e2d;
                if (g < 10) g = 10;
                if (g > 100) g = 100;
                if (tc->gate_length_pct != (uint8_t)g) {
                    tc->gate_length_pct = (uint8_t)g;
                    changed = true;
                }
                break;
            }
            case 4: {
                // Drum note (only for drum tracks)
                int16_t n = (int16_t)tc->drum_note + e2d;
                uint8_t next_note = clamp_i8(n, 0, 127);
                if (tc->drum_note != next_note) {
                    tc->drum_note = next_note;
                    changed = true;
                }
                break;
            }
        }
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

    if (changed) {
        seq_set_track_config(edited_track, &config);
    }
}

// ─── Song Edit Handler ─────────────────────────────────────────

static void handle_song_edit(int8_t e1d, int8_t e2d,
                              button_event_t e1b, button_event_t e2b,
                              step_buttons_t *steps) {
    (void)steps;
    song_t *s = &g_seq.song;

    // Enc1 rotate: navigate entries
    if (e1d != 0) {
        uint32_t lock_state = seq_lock();
        uint16_t song_len = s->length;
        seq_unlock(lock_state);

        int16_t pos = (int16_t)ui.song_cursor + e1d;
        if (pos < 0) pos = 0;
        if (pos >= (int16_t)song_len && song_len > 0) {
            pos = song_len - 1;
        }
        ui.song_cursor = (uint16_t)pos;
    }

    // Enc2 rotate: modify current entry based on field
    if (e2d != 0) {
        uint32_t lock_state = seq_lock();
        if (ui.song_cursor < s->length) {
            song_entry_t *entry = &s->entries[ui.song_cursor];
            switch (ui.song_field) {
                case 0: {
                    // Pattern index for the currently selected track.
                    int16_t p = (int16_t)entry->pattern_index[ui.selected_track] + e2d;
                    if (p < 0) p = 0;
                    if (p >= MAX_PATTERNS) p = MAX_PATTERNS - 1;
                    entry->pattern_index[ui.selected_track] = (uint8_t)p;
                    break;
                }
                case 1: {
                    // Repeats 1–255
                    int16_t r = (int16_t)entry->repeats + e2d;
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

static void render_session_view(const transport_snapshot_t *snap) {
    session_focus_t focus = session_get_focus();
    uint8_t length = clamp_pattern_length(
        pattern_get_track_length(&g_seq.patterns[focus.location], focus.track));
    uint8_t bar_step = snap->state == TRANSPORT_PLAYING
        ? (uint8_t)((snap->pulse_count / PULSES_PER_STEP) % STEPS_PER_PATTERN)
        : STEPS_PER_PATTERN;    // none
    display_draw_session_view(session_bank(), session_common_row(snap),
                              focus.track, focus.location, length,
                              g_seq.transport.bpm, transport_get_launch_quant(),
                              bar_step);
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
            display_draw_pattern_view(
                seq_active_pattern(ui.selected_track),
                ui.selected_track,
                g_seq.transport.current_track_step[ui.selected_track]);
            display_draw_bpm(g_seq.transport.bpm);
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
            display_draw_track_config(
                &seq_active_pattern(ui.config_track)->tracks[ui.config_track],
                ui.config_track,
                ui.config_param);
            break;

        case UI_SONG_EDIT:
            display_draw_song_view(&g_seq.song, ui.song_cursor, ui.selected_track);
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

    // 2. Read encoder events
    int8_t e1d = encoder_get_delta(0);
    int8_t e2d = encoder_get_delta(1);
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
            handle_session_view(e1d, e2d, e1b, e2b, steps);
            break;
        case UI_PATTERN_VIEW:
            handle_pattern_view(e1d, e2d, e1b, e2b, steps);
            break;
        case UI_STEP_EDIT:
            handle_step_edit(e1d, e2d, e1b, e2b, steps);
            break;
        case UI_PATTERN_SELECT:
            handle_pattern_select(e1d, e2d, e1b, e2b, steps);
            break;
        case UI_TRACK_CONFIG:
            handle_track_config(e1d, e2d, e1b, e2b, steps);
            break;
        case UI_SONG_EDIT:
            handle_song_edit(e1d, e2d, e1b, e2b, steps);
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
