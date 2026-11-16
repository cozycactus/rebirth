#include <stdio.h>
#include <stdlib.h>
#include "fakes/fake_hardware.h"
#include "hardware/flash.h"
#include "../src/ui.c"
#include "../src/sequencer.c"
#include "rgb_leds.h"

sequencer_state_t g_seq;
static button_event_t buttons[2];
static int8_t deltas[2];
static bool held[2];
static step_buttons_t step_buttons;
static uint16_t led_mask;
static rgb_t shown[LED_COUNT];      // last frame sent to the RGB chain
static unsigned rgb_inits;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

void encoder_init(void) {}
void encoder_poll(void) {}
int8_t encoder_get_delta(uint8_t encoder) {
    int8_t value = deltas[encoder]; deltas[encoder] = 0; return value;
}
button_event_t encoder_get_button(uint8_t encoder) {
    button_event_t value = buttons[encoder]; buttons[encoder] = BTN_NONE; return value;
}
bool encoder_button_held(uint8_t encoder) { return held[encoder]; }
static unsigned encoder_resyncs, mcp_resyncs;
void encoder_resync(void) { encoder_resyncs++; }
void mcp23017_init(void) {}
void mcp23017_poll(void) {}
void mcp23017_resync(void) { mcp_resyncs++; }
step_buttons_t *mcp23017_get_buttons(void) { return &step_buttons; }
void step_leds_init(void) {}
bool step_leds_available(void) { return true; }
void step_leds_set_mask(uint16_t mask) { led_mask = mask; }
void step_leds_clear(void) { led_mask = 0; }
void rgb_leds_init(void) { rgb_inits++; }
bool rgb_leds_show(const rgb_t frame[LED_COUNT]) {
    memcpy(shown, frame, sizeof(shown));
    return true;
}

static void tick(void) {
    timer_hw->timerawl = timer_hw->alarm[0];
    sequencer_timer_isr();
}

static void pulses(unsigned count) {
    while (count--) tick();
}

static bool rgb_is(rgb_t c, uint8_t r, uint8_t g, uint8_t b) {
    return c.r == r && c.g == g && c.b == b;
}

// Scripted panel input: push_panel() queues a press and its release;
// debounce_panel() makes one appear only after a number of polls (a press
// still in its debounce time), unless a resync comes first.
#define PANEL_QUEUE 32
static panel_event_t panel_queue[PANEL_QUEUE];
static unsigned panel_head, panel_tail, panel_resyncs;
static panel_event_t debouncing;
static unsigned debounce_polls;
static void push_panel(panel_kind_t kind, uint8_t track, uint8_t row);
void panel_input_init(void) {}
void panel_input_poll(uint32_t now_ms) {
    (void)now_ms;
    if (debounce_polls > 0 && --debounce_polls == 0) {
        push_panel(debouncing.kind, debouncing.track, debouncing.row);
    }
}
bool panel_input_next(panel_event_t *out) {
    if (panel_head == panel_tail) return false;
    *out = panel_queue[panel_head++];
    return true;
}
void panel_input_resync(void) {
    panel_head = panel_tail = 0;
    debounce_polls = 0;
    panel_resyncs++;
}
static void push_panel(panel_kind_t kind, uint8_t track, uint8_t row) {
    CHECK(panel_tail + 2 <= PANEL_QUEUE);
    panel_queue[panel_tail++] = (panel_event_t){kind, track, row, true};
    panel_queue[panel_tail++] = (panel_event_t){kind, track, row, false};
}
static void debounce_panel(panel_kind_t kind, uint8_t track, uint8_t row, unsigned polls) {
    debouncing = (panel_event_t){kind, track, row, true};
    debounce_polls = polls;
}

static void reset(void) {
    uint32_t shared_lock = g_seq.spinlock_num;
    memset(&g_seq, 0, sizeof(g_seq));
    g_seq.spinlock_num = shared_lock;
    g_seq.spinlock_ready = true;
    fake_hardware_reset();
    midi_init();
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) pattern_init(&g_seq.patterns[p], p);
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        g_seq.transport.queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    g_seq.transport.bpm = DEFAULT_BPM;
    sequencer_init();
    transport_set_launch_quant(LAUNCH_QUANT_TRACK_LOOP);
    memset(&step_buttons, 0, sizeof(step_buttons));
    memset(buttons, 0, sizeof(buttons));
    memset(deltas, 0, sizeof(deltas));
    memset(held, 0, sizeof(held));
    panel_head = panel_tail = panel_resyncs = 0;
    debounce_polls = 0;
    encoder_resyncs = mcp_resyncs = 0;
    memset(shown, 0, sizeof(shown));
    rgb_inits = 0;
    timer_hw->alarm[0] = pulse_interval_us;
    ui_init();
}

static void press_encoder(uint8_t encoder, button_event_t event) {
    buttons[encoder] = event;
    ui_update();
}

static pattern_t patterns_before[MAX_PATTERNS];

static void press_step(uint8_t index) {
    step_buttons.pressed = step_buttons.held = (uint16_t)(1u << index);
    ui_update();
}

static void long_step(uint8_t index) {
    step_buttons.long_pressed = (uint16_t)(1u << index);
    ui_update();
}

static void release_step(uint8_t index) {
    step_buttons.held = 0;
    step_buttons.released = (uint16_t)(1u << index);
    ui_update();
}

static void panel(panel_kind_t kind, uint8_t track, uint8_t row) {
    push_panel(kind, track, row);
    ui_update();
}

static void test_session_is_home_and_steps_edit_focus(void) {
    CHECK(ui.mode == UI_SESSION);
    panel(PANEL_PAD, 2, 3);
    press_step(4);
    CHECK(!(g_seq.patterns[3].steps[2][4].flags & STEP_FLAG_ACTIVE)); // toggles on release
    release_step(4);
    CHECK(g_seq.patterns[3].steps[2][4].flags & STEP_FLAG_ACTIVE);
    CHECK(sequencer_clip_mask(0) & (1ull << (2 * 8 + 3))); // refreshed for playback
    // TRACK press opens the pattern view on the focus.
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    CHECK(ui.selected_track == 2);
    CHECK(g_seq.active_pattern[2] == 3);
}

static void test_mute_button_mutes_without_editing(void) {
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    panel(PANEL_MUTE, 0, 0);
    CHECK(g_seq.track_mutes == 1);
    CHECK(memcmp(patterns_before, g_seq.patterns, sizeof(patterns_before)) == 0);
}

static void open_editor_from_session(void) {
    panel(PANEL_PAD, 2, 3);
    press_step(4);
    long_step(4);
    release_step(4);
}

static void test_step_editor_targets_the_focus(void) {
    open_editor_from_session();
    CHECK(ui.mode == UI_STEP_EDIT);
    CHECK(ui.selected_track == 2);
    CHECK(g_seq.active_pattern[2] == 3);
    CHECK(ui.edit_step == 4);
    CHECK(!(g_seq.patterns[3].steps[2][4].flags & STEP_FLAG_ACTIVE));
    press_encoder(0, BTN_PRESS);
    CHECK(g_seq.patterns[3].steps[2][4].flags & STEP_FLAG_ACCENT);
}

static void test_step_edit_returns_to_session(void) {
    open_editor_from_session();
    CHECK(ui.mode == UI_STEP_EDIT);
    press_encoder(1, BTN_PRESS);
    CHECK(ui.mode == UI_SESSION);
    // Opened from the pattern view, it returns there.
    press_encoder(0, BTN_PRESS);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_STEP_EDIT);
    press_encoder(1, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
}

static void test_step_beyond_length_does_nothing(void) {
    pattern_set_track_length(&g_seq.patterns[0], 0, 4);
    sequencer_refresh_track_playback(0, 0);
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    press_step(5);
    release_step(5);
    press_step(5);
    long_step(5);
    release_step(5);
    CHECK(memcmp(patterns_before, g_seq.patterns, sizeof(patterns_before)) == 0);
    CHECK(ui.mode == UI_SESSION);
}

static void test_step_release_after_screen_change_does_not_toggle(void) {
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    press_step(2);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    release_step(2);
    CHECK(memcmp(patterns_before[0].steps[0], g_seq.patterns[0].steps[0],
                 sizeof(patterns_before[0].steps[0])) == 0);
    // Held through a trip to the pattern view and back: still no toggle.
    press_encoder(0, BTN_LONG_PRESS);
    press_step(2);
    press_encoder(0, BTN_PRESS);
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_SESSION);
    release_step(2);
    CHECK(memcmp(patterns_before[0].steps[0], g_seq.patterns[0].steps[0],
                 sizeof(patterns_before[0].steps[0])) == 0);
}

static void test_play_is_the_only_transport_toggle(void) {
    press_encoder(1, BTN_PRESS);
    CHECK(transport_get_launch_quant() == LAUNCH_QUANT_BAR);
    CHECK(strcmp(ui.notify_msg, "QUANT 1 BAR") == 0);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    press_encoder(1, BTN_PRESS);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    panel(PANEL_PLAY, 0, 0);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    panel(PANEL_PLAY, 0, 0);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
}

static void test_grid_works_in_step_editor_without_moving_its_target(void) {
    press_step(0);
    long_step(0);
    release_step(0);
    CHECK(ui.mode == UI_STEP_EDIT);
    transport_start();
    panel(PANEL_PAD, 0, 5);
    CHECK(g_seq.transport.queued_pattern[0] == 5);
    CHECK(ui.mode == UI_STEP_EDIT);
    CHECK(g_seq.active_pattern[0] == 0);
    session_focus_t focus = session_get_focus();
    CHECK(focus.track == 0 && focus.location == 0);
}

static void test_track_long_press_changes_bank(void) {
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(session_bank() == 1);
    CHECK(strcmp(ui.notify_msg, "BANK B") == 0);
    panel(PANEL_PAD, 0, 0);
    CHECK(g_seq.transport.current_pattern[0] == 8);
}

static void test_value_long_opens_pattern_select_on_focus(void) {
    panel(PANEL_PAD, 3, 2);
    press_encoder(1, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_PATTERN_SELECT);
    CHECK(ui.select_pattern == 2);
    CHECK(ui.selected_track == 3);
}

static void test_launch_in_song_mode_notifies(void) {
    transport_set_mode(PLAY_MODE_SONG);
    panel(PANEL_SCENE, 0, 1);
    CHECK(strcmp(ui.notify_msg, "SONG MODE") == 0);
    ui.notify_msg[0] = '\0';
    panel(PANEL_PAD, 1, 1);
    CHECK(strcmp(ui.notify_msg, "SONG MODE") == 0);
    session_focus_t focus = session_get_focus();
    CHECK(focus.track == 1 && focus.location == 1); // the focus still moves
}

static void test_panic_drops_queued_panel_events(void) {
    transport_start();
    press_step(2);
    push_panel(PANEL_PAD, 0, 4);
    held[0] = held[1] = true;
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(ui.panic_latched);
    CHECK(panel_resyncs == 1);
    // A pad and a step pressed while the panic is still held are dropped too.
    push_panel(PANEL_PAD, 0, 6);
    step_buttons.pressed = 1u << 6;
    step_buttons.held |= 1u << 6;
    ui_update();
    held[0] = held[1] = false;
    ui_update();
    CHECK(!ui.panic_latched);
    ui_update();
    CHECK(g_seq.transport.queued_pattern[0] == NO_QUEUED_PATTERN);
    CHECK(g_seq.transport.current_pattern[0] == 0);
    // Their long presses arrive after the latch clears: no step editor.
    long_step(2);
    CHECK(ui.mode == UI_SESSION);
    long_step(6);
    CHECK(ui.mode == UI_SESSION);
    step_buttons.held = 0;
    step_buttons.released = (1u << 2) | (1u << 6);
    ui_update();
    CHECK(!(g_seq.patterns[0].steps[0][2].flags & STEP_FLAG_ACTIVE));
    CHECK(!(g_seq.patterns[0].steps[0][6].flags & STEP_FLAG_ACTIVE));
}

static void test_step_held_from_editor_does_not_reopen_it(void) {
    open_editor_from_session();
    CHECK(ui.mode == UI_STEP_EDIT);
    press_step(6); // selects step 7 in the editor and stays held
    CHECK(ui.edit_step == 6);
    press_encoder(1, BTN_PRESS);
    CHECK(ui.mode == UI_SESSION);
    long_step(6);
    CHECK(ui.mode == UI_SESSION);
    release_step(6);
    CHECK(ui.mode == UI_SESSION);
    CHECK(!(g_seq.patterns[3].steps[2][6].flags & STEP_FLAG_ACTIVE));
}

static void test_panic_consumes_staggered_holds_and_releases(void) {
    transport_start();
    held[0] = held[1] = true;
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    CHECK(ui.panic_latched);
    unsigned bytes = fake_midi_length;
    // The second long event arrives after the first button is released.
    held[0] = false;
    buttons[0] = BTN_RELEASE;
    buttons[1] = BTN_LONG_PRESS;
    ui_update();
    CHECK(ui.mode == UI_SESSION);
    CHECK(fake_midi_length == bytes);
    held[1] = false;
    press_encoder(1, BTN_PRESS); // Also covers a short release of the second button.
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    CHECK(!ui.panic_latched);
    panel(PANEL_PLAY, 0, 0); // PLAY starts again.
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
}

static void test_accent_can_combine_with_slide_or_tie(void) {
    press_encoder(0, BTN_PRESS);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_STEP_EDIT);
    press_encoder(0, BTN_PRESS);
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(g_seq.patterns[0].steps[0][0].flags == (STEP_FLAG_ACCENT | STEP_FLAG_SLIDE));
    press_encoder(1, BTN_LONG_PRESS);
    CHECK(g_seq.patterns[0].steps[0][0].flags == (STEP_FLAG_ACCENT | STEP_FLAG_TIE));
    press_encoder(0, BTN_PRESS);
    CHECK(g_seq.patterns[0].steps[0][0].flags == STEP_FLAG_TIE);
}

static void test_long_config_press_never_mutes(void) {
    press_encoder(1, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_PATTERN_SELECT);
    step_buttons.held = step_buttons.pressed = 1;
    ui_update();
    CHECK(g_seq.track_mutes == 0);
    step_buttons.long_pressed = 1;
    ui_update();
    CHECK(ui.mode == UI_TRACK_CONFIG);
    CHECK(g_seq.track_mutes == 0);
    step_buttons.held = 0;
    step_buttons.released = 1;
    ui_update();
    CHECK(g_seq.track_mutes == 0);
}

static void test_selector_short_press_mutes_on_release(void) {
    press_encoder(1, BTN_LONG_PRESS);
    step_buttons.pressed = step_buttons.held = 4;
    ui_update();
    CHECK(g_seq.track_mutes == 0);
    step_buttons.held = 0;
    step_buttons.released = 4;
    ui_update();
    CHECK(g_seq.track_mutes == 4);
}

// Step LEDs 1-8 of the selector are lit for audible tracks: a stored mute at
// the playing location counts as muted, as on MUTE and the RGB lights.
static void test_selector_leds_show_stored_mutes(void) {
    press_encoder(1, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_PATTERN_SELECT);
    ui_update();
    CHECK((led_mask & 0xFF) == 0xFF);
    g_seq.patterns[g_seq.transport.current_pattern[2]].tracks[2].muted = true;
    g_seq.patterns[9].tracks[3].muted = true;   // not where track 4 plays
    seq_set_track_mute(5, true);
    ui_update();
    CHECK((led_mask & 0xFF) == (0xFF & ~((1u << 2) | (1u << 5))));
}

static void test_flash_controls_are_blocked_during_playback(void) {
    transport_start();
    press_encoder(1, BTN_LONG_PRESS);
    step_buttons.pressed = 1u << 8;
    ui_update();
    CHECK(fake_flash_erases == 0);
    CHECK(strcmp(ui.notify_msg, "STOP TO SAVE") == 0);
    step_buttons.pressed = 1u << 9;
    ui_update();
    CHECK(strcmp(ui.notify_msg, "STOP TO LOAD") == 0);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
}

static void test_song_mode_has_a_return_to_pattern_playback(void) {
    CHECK(song_append(0, 1));
    press_encoder(1, BTN_LONG_PRESS);
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_SONG_EDIT);
    step_buttons.pressed = 1u << 1;
    ui_update();
    CHECK(g_seq.transport.mode == PLAY_MODE_SONG);
    step_buttons.pressed = 1u << 2;
    ui_update();
    CHECK(g_seq.transport.mode == PLAY_MODE_PATTERN);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    panel(PANEL_PLAY, 0, 0);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    CHECK(transport_queue_track_pattern(0, 2));
}

static void test_song_editor_lights_its_three_buttons(void) {
    press_encoder(1, BTN_LONG_PRESS);
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_SONG_EDIT);
    ui_update();
    CHECK(led_mask == ((1u << 0) | (1u << 1) | (1u << 2)));
}

static void test_nonplaying_editor_hides_playhead(void) {
    transport_start();
    g_seq.active_pattern[0] = 3;
    g_seq.transport.current_track_step[0] = 0;
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[3], 0, 0);
    uint8_t without_playhead[OLED_BUF_SIZE];
    memcpy(without_playhead, ssd1309_get_framebuffer(), sizeof(without_playhead));
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[3], 0, NO_QUEUED_PATTERN);
    CHECK(memcmp(without_playhead, ssd1309_get_framebuffer(), sizeof(without_playhead)) == 0);
}

static void test_step_release_toggles_the_cell_focused_at_press(void) {
    press_step(4);                          // cell (track 0, location 0)
    panel(PANEL_PAD, 2, 3);                 // the focus moves by a pad
    release_step(4);
    CHECK(g_seq.patterns[0].steps[0][4].flags & STEP_FLAG_ACTIVE);
    CHECK(!(g_seq.patterns[3].steps[2][4].flags & STEP_FLAG_ACTIVE));
    press_step(5);                          // cell (2, 3)
    deltas[0] = 1;                          // the focus moves by TRACK
    ui_update();
    CHECK(session_get_focus().track == 3 && session_get_focus().location == 0);
    release_step(5);
    CHECK(g_seq.patterns[3].steps[2][5].flags & STEP_FLAG_ACTIVE);
    CHECK(!(g_seq.patterns[0].steps[3][5].flags & STEP_FLAG_ACTIVE));
}

static void test_step_long_press_opens_the_cell_focused_at_press(void) {
    panel(PANEL_PAD, 2, 3);
    press_step(6);                          // cell (2, 3)
    deltas[0] = 1;
    ui_update();                            // focus (3, 0)
    long_step(6);
    CHECK(ui.mode == UI_STEP_EDIT);
    CHECK(ui.selected_track == 2 && g_seq.active_pattern[2] == 3 && ui.edit_step == 6);
    release_step(6);
    CHECK(!(g_seq.patterns[3].steps[2][6].flags & STEP_FLAG_ACTIVE));
    CHECK(!(g_seq.patterns[0].steps[3][6].flags & STEP_FLAG_ACTIVE));
}

static void test_long_press_begun_on_another_screen_is_dropped(void) {
    press_step(0);                          // held in the Session view...
    press_encoder(1, BTN_LONG_PRESS);       // ...then the selector opens
    CHECK(ui.mode == UI_PATTERN_SELECT);
    long_step(0);
    CHECK(ui.mode == UI_PATTERN_SELECT);    // no track config
    release_step(0);
    CHECK(g_seq.track_mutes == 0);
    press_step(0);                          // pressed in the selector: its own
    long_step(0);                           // long press still opens track config
    CHECK(ui.mode == UI_TRACK_CONFIG && ui.config_track == 0);
}

static void test_second_held_step_does_not_toggle_in_the_editor(void) {
    step_buttons.pressed = step_buttons.held = (1u << 2) | (1u << 3);
    ui_update();
    long_step(2);
    CHECK(ui.mode == UI_STEP_EDIT && ui.edit_step == 2);
    long_step(3);                           // pressed in the Session view
    CHECK(ui.mode == UI_STEP_EDIT);
    CHECK(!(g_seq.patterns[0].steps[0][3].flags & STEP_FLAG_ACTIVE));
    CHECK(!(g_seq.patterns[0].steps[0][2].flags & STEP_FLAG_ACTIVE));
}

static void test_play_pressed_during_panic_does_not_fire_after_it(void) {
    held[0] = held[1] = true;
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(ui.panic_latched);
    unsigned panel_before = panel_resyncs;
    // PLAY pressed while the combo is held is still in its debounce time when
    // both encoder buttons are let go: it would be reported on the next poll.
    debounce_panel(PANEL_PLAY, 0, 0, 2);
    held[0] = held[1] = false;
    ui_update();                            // the latch clears
    CHECK(!ui.panic_latched);
    CHECK(panel_resyncs == panel_before + 1);
    CHECK(mcp_resyncs == 1);                // step buttons too
    ui_update();
    ui_update();
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    panel(PANEL_PLAY, 0, 0);                // a press after the panic works
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
}

static void test_step_held_through_save_or_load_does_not_mute(void) {
    press_encoder(1, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_PATTERN_SELECT);
    press_step(1);                          // step 2 held: mutes track 2 on release
    step_buttons.pressed = 1u << 8;         // step 9: save
    step_buttons.held |= 1u << 8;
    ui_update();
    CHECK(strcmp(ui.notify_msg, "SAVED OK") == 0);
    CHECK(panel_resyncs == 1 && mcp_resyncs == 1 && encoder_resyncs == 1);
    release_step(1);
    CHECK(g_seq.track_mutes == 0);
    press_step(2);                          // step 3 held through a load
    step_buttons.pressed = 1u << 9;
    step_buttons.held |= 1u << 9;
    ui_update();
    CHECK(strcmp(ui.notify_msg, "LOADED OK") == 0);
    release_step(2);
    CHECK(g_seq.track_mutes == 0);
    press_step(3);                          // a press after them still mutes
    release_step(3);
    CHECK(g_seq.track_mutes == 1u << 3);
}

static void test_track_config_edit_reaches_all_locations(void) {
    uint8_t old = g_seq.patterns[0].tracks[0].midi_channel;
    press_encoder(1, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_PATTERN_SELECT);
    press_step(0);                  // a long press needs a press on this screen
    long_step(0);
    CHECK(ui.mode == UI_TRACK_CONFIG);
    step_buttons.long_pressed = 0;
    deltas[0] = 1;
    deltas[1] = 1;
    ui_update();
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        CHECK(g_seq.patterns[p].tracks[0].midi_channel == old + 1);
    }
}

static void test_session_lights_reach_the_chain(void) {
    CHECK(rgb_inits == 1);
    pattern_set_step(&g_seq.patterns[0], 0, 0, 60, 100, STEP_FLAG_ACTIVE);
    sequencer_refresh_track_playback(0, 0);
    transport_start();
    tick();
    ui_render();
    CHECK(rgb_is(shown[LED_PAD(0, 0)], LED_LEVEL_FULL, 0, 0));
    CHECK(rgb_is(shown[LED_PAD(1, 0)], LED_LEVEL_DIM, LED_LEVEL_DIM, LED_LEVEL_DIM));
    CHECK(rgb_is(shown[LED_PAD(1, 1)], 0, 0, 0));
    CHECK(shown[LED_PLAY].g > 0);
    // The grid is global: other screens keep the lights going.
    ui.mode = UI_PATTERN_VIEW;
    memset(shown, 0, sizeof(shown));
    ui_render();
    CHECK(shown[LED_PLAY].g > 0);
}

static void test_stopped_lights_blink_on_a_120_bpm_clock(void) {
    // Bank B shown while every track plays bank A: MUTE blinks on half notes,
    // which while stopped means one second on, one second off.
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(session_bank() == 1);
    ui_render();
    CHECK(shown[LED_MUTE(1)].r > 0);
    sleep_ms(1000);
    ui_render();
    CHECK(rgb_is(shown[LED_MUTE(1)], 0, 0, 0));
    sleep_ms(1000);
    ui_render();
    CHECK(shown[LED_MUTE(1)].r > 0);
}

static void test_session_step_leds_show_focus_steps(void) {
    pattern_set_step(&g_seq.patterns[3], 2, 0, 60, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(&g_seq.patterns[3], 2, 4, 60, 100, STEP_FLAG_ACTIVE);
    sequencer_refresh_track_playback(2, 3);
    push_panel(PANEL_PAD, 2, 3);
    ui_update();
    ui_render();    // the Session mask is written once per frame
    CHECK(led_mask == 0x0011);
    // Steps beyond the cell's length stay dark.
    pattern_set_track_length(&g_seq.patterns[3], 2, 4);
    sequencer_refresh_track_playback(2, 3);
    ui_update();
    ui_render();
    CHECK(led_mask == 0x0001);
}

static void test_session_step_leds_blink_playhead(void) {
    // The focus (track 0, location 0) plays with length 3, so its step 2 comes
    // round at global step 2 (8th-note phase off) and again at 5 (phase on).
    pattern_set_step(&g_seq.patterns[0], 0, 0, 60, 100, STEP_FLAG_ACTIVE);
    pattern_set_track_length(&g_seq.patterns[0], 0, 3);
    sequencer_refresh_track_playback(0, 0);
    transport_start();
    pulses(2 * PULSES_PER_STEP + 1);
    CHECK(g_seq.transport.current_track_step[0] == 2);
    CHECK((g_seq.transport.pulse_count / (PPQN / 2)) % 2 == 1);
    ui_update();
    ui_render();
    uint16_t phase_off = led_mask;
    pulses(3 * PULSES_PER_STEP);
    CHECK(g_seq.transport.current_track_step[0] == 2);
    CHECK((g_seq.transport.pulse_count / (PPQN / 2)) % 2 == 0);
    ui_update();
    ui_render();
    uint16_t phase_on = led_mask;
    CHECK(phase_off == 0x0001);
    CHECK((phase_off ^ phase_on) == (1u << 2));
    // A focused cell that is not playing on its track shows no playhead.
    push_panel(PANEL_PAD, 1, 1);
    ui_update();
    ui_render();
    CHECK(session_get_focus().track == 1 && session_get_focus().location == 1);
    CHECK(led_mask == 0);
}

static unsigned playback_locks;
static void count_playback_lock(spin_lock_t *lock) {
    if (lock == spin_lock_instance(playback_spinlock_num)) playback_locks++;
}

static void test_session_passes_take_no_playback_lock(void) {
    // The timer ISR holds the playback lock for its whole body: the UI loop
    // must not take it on every pass, only once per frame.
    transport_start();
    pulses(PULSES_PER_STEP + 1);
    playback_locks = 0;
    fake_before_lock = count_playback_lock;
    for (int pass = 0; pass < 100; pass++) ui_update();
    CHECK(ui.mode == UI_SESSION);
    CHECK(playback_locks == 0);
    ui_render();    // one snapshot serves the lights, the OLED and the step LEDs
    fake_before_lock = NULL;
    CHECK(playback_locks == 1);
}

// The timer ISR runs through the bar line just before a session launch takes
// the playback lock for a second time, i.e. between a decision taken under one
// lock and a write under another.
static unsigned launch_playback_locks;
static void bar_line_on_second_playback_lock(spin_lock_t *lock) {
    if (lock != spin_lock_instance(playback_spinlock_num)) return;
    if (++launch_playback_locks != 2) return;
    fake_before_lock = NULL;            // the ISR takes the lock too
    while (g_seq.transport.pulse_count <= 16 * PULSES_PER_STEP) tick();
}

static void play_to_bar_end_with_race(void) {
    while (g_seq.transport.pulse_count <= 15 * PULSES_PER_STEP) tick();  // step 15 played
    launch_playback_locks = 0;
    fake_before_lock = bar_line_on_second_playback_lock;
}

static void test_pad_launch_decides_and_writes_atomically(void) {
    transport_set_launch_quant(LAUNCH_QUANT_BAR);
    transport_start();
    tick();                             // step 0 played: queues wait for step 16
    CHECK(transport_queue_track_pattern(2, 3));
    play_to_bar_end_with_race();
    // The playing pad drops the queue. Before the bar: track 2 stays on 0.
    // After it: track 2 is on 3 and the pad queues 0. Never a mix.
    CHECK(session_pad(2, 0, true));
    fake_before_lock = NULL;
    const volatile transport_t *t = &g_seq.transport;
    bool before = t->current_pattern[2] == 0 && t->queued_pattern[2] == NO_QUEUED_PATTERN;
    bool after = t->current_pattern[2] == 3 && t->queued_pattern[2] == 0;
    CHECK(before || after);
}

static void test_scene_cancel_decides_and_writes_atomically(void) {
    transport_set_launch_quant(LAUNCH_QUANT_BAR);
    transport_start();
    tick();
    CHECK(transport_queue_track_pattern(0, 3));
    CHECK(transport_queue_track_pattern(1, 5));
    play_to_bar_end_with_race();
    // Track 0 is queued to row 3: the press cancels every queue. Before the
    // bar nothing moves; after it, tracks 0/1 are on 3/5 and row 3 is launched.
    CHECK(session_scene(3));
    fake_before_lock = NULL;
    const volatile transport_t *t = &g_seq.transport;
    bool before = true, after = t->current_pattern[0] == 3 && t->current_pattern[1] == 5 &&
                                t->queued_pattern[0] == NO_QUEUED_PATTERN;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        before = before && t->current_pattern[track] == 0 &&
                 t->queued_pattern[track] == NO_QUEUED_PATTERN;
        if (track > 0) after = after && t->queued_pattern[track] == 3;
    }
    CHECK(before || after);
}

static bool oled_blank(void) {
    const uint8_t *frame = ssd1309_get_framebuffer();
    for (unsigned i = 0; i < OLED_BUF_SIZE; i++) {
        if (frame[i]) return false;
    }
    return true;
}

static void test_session_oled_renders(void) {
    uint8_t rendered[OLED_BUF_SIZE];
    CHECK(ui.mode == UI_SESSION);
    display_clear();
    ui_render();
    CHECK(!oled_blank());
    // Stopped, every track on A1, focus T1 A1 of length 16: no bar step.
    memcpy(rendered, ssd1309_get_framebuffer(), sizeof(rendered));
    display_clear();
    display_draw_session_view(0, 0, PLAY_MODE_PATTERN, 0, 0, 16, DEFAULT_BPM,
                              LAUNCH_QUANT_TRACK_LOOP, 0xFF);
    CHECK(memcmp(rendered, ssd1309_get_framebuffer(), sizeof(rendered)) == 0);
    // Track 1 moved to A2 (stopped: at once), then playing at global step 5.
    panel(PANEL_PAD, 1, 1);
    pattern_set_track_length(&g_seq.patterns[1], 1, 7);
    sequencer_refresh_track_playback(1, 1);
    transport_start();
    pulses(5 * PULSES_PER_STEP + 1);
    display_clear();
    ui_render();
    memcpy(rendered, ssd1309_get_framebuffer(), sizeof(rendered));
    display_clear();
    display_draw_session_view(0, 0xFF, PLAY_MODE_PATTERN, 1, 1, 7, DEFAULT_BPM,
                              LAUNCH_QUANT_TRACK_LOOP, 5);
    CHECK(memcmp(rendered, ssd1309_get_framebuffer(), sizeof(rendered)) == 0);
}

// True when the last character cell of OLED line 0 shows exactly `mark`.
static bool mode_mark_is(char mark) {
    const uint8_t x0 = (FONT_COLS - 1) * FONT_CHAR_W;
    bool rendered[FONT_CHAR_H][FONT_CHAR_W];
    for (uint8_t y = 0; y < FONT_CHAR_H; y++) {
        for (uint8_t x = 0; x < FONT_CHAR_W; x++) rendered[y][x] = ssd1309_get_pixel(x0 + x, y);
    }
    display_clear();
    const char text[2] = { mark, '\0' };
    ssd1309_draw_string(x0, 0, text, false);
    for (uint8_t y = 0; y < FONT_CHAR_H; y++) {
        for (uint8_t x = 0; x < FONT_CHAR_W; x++) {
            if (rendered[y][x] != ssd1309_get_pixel(x0 + x, y)) return false;
        }
    }
    return true;
}

// Line 0 of the Session OLED ends with the play mode: P (Pattern) or S (Song).
static void test_session_oled_shows_the_play_mode(void) {
    CHECK(ui.mode == UI_SESSION);
    display_clear();
    ui_render();
    CHECK(mode_mark_is('P'));
    CHECK(song_append(0, 1));
    transport_set_mode(PLAY_MODE_SONG);
    display_clear();
    ui_render();
    CHECK(mode_mark_is('S'));
}

static void render_views(const char *path) {
    uint8_t image[6][OLED_HEIGHT][OLED_WIDTH / 8];
    for (unsigned entry = 0; entry < MAX_SONG_ENTRIES; entry++) CHECK(song_append(0, 255));
    ui.song_cursor = MAX_SONG_ENTRIES - 1;
    for (unsigned mode = UI_SESSION; mode <= UI_SONG_EDIT; mode++) {
        ui.mode = (ui_mode_t)mode;
        ui.notify_ticks = 0;
        display_clear();
        ui_render();
        unsigned pixels = 0;
        for (unsigned y = 0; y < OLED_HEIGHT; y++) {
            for (unsigned x = 0; x < OLED_WIDTH; x += 8) {
                uint8_t byte = 0;
                for (unsigned bit = 0; bit < 8; bit++) {
                    bool on = ssd1309_get_pixel(x + bit, y);
                    if (on) { byte |= 1u << (7 - bit); pixels++; }
                }
                image[mode][y][x / 8] = byte;
            }
        }
        CHECK(pixels > 0);
    }
    if (path) {
        FILE *file = fopen(path, "wb");
        CHECK(file != NULL);
        fprintf(file, "P4\n256 192\n");
        for (unsigned pair = 0; pair < 3; pair++) {
            for (unsigned y = 0; y < OLED_HEIGHT; y++) {
                CHECK(fwrite(image[pair * 2][y], 1, 16, file) == 16);
                CHECK(fwrite(image[pair * 2 + 1][y], 1, 16, file) == 16);
            }
        }
        CHECK(fclose(file) == 0);
    }
}

#define RUN(test) do { reset(); test(); printf("PASS %s\n", #test); } while (0)
int main(int argc, char **argv) {
    seq_lock_init();
    RUN(test_session_is_home_and_steps_edit_focus);
    RUN(test_mute_button_mutes_without_editing);
    RUN(test_step_editor_targets_the_focus);
    RUN(test_step_edit_returns_to_session);
    RUN(test_step_beyond_length_does_nothing);
    RUN(test_step_release_after_screen_change_does_not_toggle);
    RUN(test_play_is_the_only_transport_toggle);
    RUN(test_grid_works_in_step_editor_without_moving_its_target);
    RUN(test_track_long_press_changes_bank);
    RUN(test_value_long_opens_pattern_select_on_focus);
    RUN(test_launch_in_song_mode_notifies);
    RUN(test_panic_drops_queued_panel_events);
    RUN(test_step_held_from_editor_does_not_reopen_it);
    RUN(test_panic_consumes_staggered_holds_and_releases);
    RUN(test_accent_can_combine_with_slide_or_tie);
    RUN(test_long_config_press_never_mutes);
    RUN(test_selector_short_press_mutes_on_release);
    RUN(test_selector_leds_show_stored_mutes);
    RUN(test_flash_controls_are_blocked_during_playback);
    RUN(test_song_mode_has_a_return_to_pattern_playback);
    RUN(test_song_editor_lights_its_three_buttons);
    RUN(test_nonplaying_editor_hides_playhead);
    RUN(test_track_config_edit_reaches_all_locations);
    RUN(test_session_lights_reach_the_chain);
    RUN(test_stopped_lights_blink_on_a_120_bpm_clock);
    RUN(test_session_step_leds_show_focus_steps);
    RUN(test_session_step_leds_blink_playhead);
    RUN(test_session_passes_take_no_playback_lock);
    RUN(test_session_oled_renders);
    RUN(test_session_oled_shows_the_play_mode);
    RUN(test_pad_launch_decides_and_writes_atomically);
    RUN(test_scene_cancel_decides_and_writes_atomically);
    RUN(test_step_release_toggles_the_cell_focused_at_press);
    RUN(test_step_long_press_opens_the_cell_focused_at_press);
    RUN(test_long_press_begun_on_another_screen_is_dropped);
    RUN(test_second_held_step_does_not_toggle_in_the_editor);
    RUN(test_play_pressed_during_panic_does_not_fire_after_it);
    RUN(test_step_held_through_save_or_load_does_not_mute);
    reset();
    render_views(argc > 1 ? argv[1] : NULL);
    puts("PASS all six OLED views render");
    return 0;
}
