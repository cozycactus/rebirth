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
static int8_t delta_scale = 1;   // encoder_get_delta returns deltas * delta_scale
static bool held[2];
static step_buttons_t step_buttons;
static uint16_t led_mask;
static rgb_t shown[LED_COUNT];      // last frame sent to the RGB chain
static unsigned rgb_inits;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

void encoder_init(void) {}
void encoder_poll(void) {}
int8_t encoder_get_delta(uint8_t encoder) {
    int8_t value = deltas[encoder] * delta_scale; deltas[encoder] = 0; return value;
}
int8_t encoder_get_detents(uint8_t encoder) {
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
    delta_scale = 1;
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
    display_draw_session_view(0, 0, 0, 0, 16, DEFAULT_BPM, LAUNCH_QUANT_TRACK_LOOP, 0xFF,
                              NULL, NULL, STEPS_PER_PATTERN);
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
    display_draw_session_view(0, 0xFF, 1, 1, 7, DEFAULT_BPM, LAUNCH_QUANT_TRACK_LOOP, 5,
                              NULL, NULL, STEPS_PER_PATTERN);
    CHECK(memcmp(rendered, ssd1309_get_framebuffer(), sizeof(rendered)) == 0);
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

// ─── Kit lanes ─────────────────────────────────────────────────

#define LANE_SD 2
#define LANE_LT 3
#define LANE_MT 4
#define LANE_CP 7

static void make_kit(uint8_t track) {
    track_config_t config = g_seq.patterns[0].tracks[track];
    config.type = TRACK_TYPE_KIT;
    CHECK(seq_set_track_config(track, &config));
}

static uint8_t lane_at(uint8_t location, uint8_t track, uint8_t lane, uint8_t step) {
    return kit_get(g_seq.patterns[location].kit_steps[track][step], lane);
}

static bool cell_has_clip(uint8_t track, uint8_t location) {
    return (sequencer_clip_mask(pattern_bank(location)) >> (track * 8 + pattern_slot(location))) & 1u;
}

// Session view: a pad on the kit lands on BD, then TRACK walks to `lane`.
static void focus_lane(uint8_t track, uint8_t row, uint8_t lane) {
    panel(PANEL_PAD, track, row);
    CHECK(session_get_focus().lane == KIT_LANE_BD);
    deltas[0] = (int8_t)(lane - KIT_LANE_BD);
    ui_update();
    session_focus_t f = session_get_focus();
    CHECK(f.track == track && f.lane == lane);
}

static void tap_step(uint8_t index) {
    press_step(index);
    release_step(index);
}

static void test_kit_step_cycles_on_release(void) {
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    step_t steps_before[STEPS_PER_PATTERN];
    memcpy(steps_before, g_seq.patterns[0].steps[1], sizeof(steps_before));
    const uint8_t expected[3] = {KIT_NOTE, KIT_ACCENT, KIT_OFF};
    uint8_t value = KIT_OFF;
    for (unsigned i = 0; i < 3; i++) {
        press_step(3);
        CHECK(lane_at(0, 1, LANE_SD, 3) == value);      // acts on release
        release_step(3);
        value = expected[i];
        CHECK(lane_at(0, 1, LANE_SD, 3) == value);
        CHECK(cell_has_clip(1, 0) == (value != KIT_OFF)); // refreshed for playback
    }
    CHECK(memcmp(steps_before, g_seq.patterns[0].steps[1], sizeof(steps_before)) == 0);
    CHECK(ui.mode == UI_SESSION);
}

static void test_kit_ac_toggle_and_ch_clears_oh(void) {
    make_kit(1);
    focus_lane(1, 0, KIT_LANE_AC);
    tap_step(0);
    CHECK(lane_at(0, 1, KIT_LANE_AC, 0) == 1);
    tap_step(0);
    CHECK(lane_at(0, 1, KIT_LANE_AC, 0) == 0);
    deltas[0] = KIT_LANE_OH;
    ui_update();
    CHECK(session_get_focus().lane == KIT_LANE_OH);
    tap_step(5);
    CHECK(lane_at(0, 1, KIT_LANE_OH, 5) == KIT_NOTE);
    deltas[0] = -1;                                     // CH
    ui_update();
    tap_step(5);
    CHECK(lane_at(0, 1, KIT_LANE_CH, 5) == KIT_NOTE);
    CHECK(lane_at(0, 1, KIT_LANE_OH, 5) == KIT_OFF);
    deltas[0] = 1;                                      // OH
    ui_update();
    tap_step(5);
    CHECK(lane_at(0, 1, KIT_LANE_OH, 5) == KIT_NOTE);
    CHECK(lane_at(0, 1, KIT_LANE_CH, 5) == KIT_OFF);
}

static void test_kit_step_beyond_length_does_nothing(void) {
    make_kit(1);
    pattern_set_track_length(&g_seq.patterns[0], 1, 4);
    sequencer_refresh_track_playback(1, 0);
    focus_lane(1, 0, LANE_SD);
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    tap_step(5);
    press_step(5);
    long_step(5);
    release_step(5);
    CHECK(memcmp(patterns_before, g_seq.patterns, sizeof(patterns_before)) == 0);
    CHECK(ui.mode == UI_SESSION);
}

static void test_held_step_stays_on_its_lane(void) {
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    uint32_t words[STEPS_PER_PATTERN];
    step_t steps_before[STEPS_PER_PATTERN];
    memcpy(words, g_seq.patterns[0].kit_steps[1], sizeof(words));
    memcpy(steps_before, g_seq.patterns[0].steps[1], sizeof(steps_before));
    press_step(2);                                      // made on SD
    deltas[0] = 1;
    ui_update();
    CHECK(session_get_focus().lane == LANE_LT);
    release_step(2);
    words[2] = kit_set(words[2], LANE_SD, KIT_NOTE);
    CHECK(memcmp(words, g_seq.patterns[0].kit_steps[1], sizeof(words)) == 0);
    CHECK(memcmp(steps_before, g_seq.patterns[0].steps[1], sizeof(steps_before)) == 0);
    session_focus_t f = session_get_focus();
    CHECK(f.track == 1 && f.location == 0 && f.lane == LANE_LT);
    // A long press made on SD fires on LT: the kit editor opens on SD and
    // changes nothing by itself.
    deltas[0] = -1;
    ui_update();
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    press_step(6);
    deltas[0] = 1;
    ui_update();
    long_step(6);
    CHECK(ui.mode == UI_KIT_STEP_EDIT);
    CHECK(ui.edit_step == 6 && ui.kit_cursor == LANE_SD);
    release_step(6);
    CHECK(memcmp(patterns_before, g_seq.patterns, sizeof(patterns_before)) == 0);
    press_encoder(1, BTN_PRESS);
    CHECK(ui.mode == UI_SESSION && session_get_focus().lane == LANE_LT);
    // Made on the kit's SD, released on track 2: still the kit's SD.
    deltas[0] = -1;
    ui_update();
    CHECK(session_get_focus().lane == LANE_SD);
    press_step(9);
    deltas[0] = 10;                                     // SD..RC, then track 2
    ui_update();
    CHECK(session_get_focus().track == 2);
    release_step(9);
    CHECK(lane_at(0, 1, LANE_SD, 9) == KIT_NOTE);
    CHECK(!(g_seq.patterns[0].steps[2][9].flags & STEP_FLAG_ACTIVE));
    CHECK(!(g_seq.patterns[0].steps[1][9].flags & STEP_FLAG_ACTIVE));
}

static void test_eight_detents_move_eight_stops(void) {
    make_kit(1);
    delta_scale = 4;                // TRACK reads detents, not accelerated edges
    deltas[0] = 8;
    ui_update();
    session_focus_t f = session_get_focus();
    CHECK(f.track == 1 && f.lane == LANE_CP);
    panel(PANEL_PAD, 0, 0);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW && ui.selected_track == 0);
    session_set_kit_lane(1, KIT_LANE_BD);
    deltas[0] = 8;
    ui_update();
    CHECK(ui.selected_track == 1 && session_kit_lane(1) == LANE_CP);
}

static void test_pattern_view_on_kit_edits_lane(void) {
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW && ui.selected_track == 1);
    step_t steps_before[STEPS_PER_PATTERN];
    memcpy(steps_before, g_seq.patterns[0].steps[1], sizeof(steps_before));
    press_step(4);
    CHECK(lane_at(0, 1, LANE_SD, 4) == KIT_OFF);        // acts on release
    release_step(4);
    CHECK(lane_at(0, 1, LANE_SD, 4) == KIT_NOTE);
    CHECK(cell_has_clip(1, 0));
    tap_step(4);
    CHECK(lane_at(0, 1, LANE_SD, 4) == KIT_ACCENT);
    CHECK(memcmp(steps_before, g_seq.patterns[0].steps[1], sizeof(steps_before)) == 0);
    deltas[0] = 1;
    ui_update();
    CHECK(ui.selected_track == 1 && session_kit_lane(1) == LANE_LT);
    tap_step(4);
    CHECK(lane_at(0, 1, LANE_LT, 4) == KIT_NOTE);
    CHECK(lane_at(0, 1, LANE_SD, 4) == KIT_ACCENT);
    // A press belongs to the lane it was made on; a long press opens the kit
    // editor there and changes nothing by itself.
    press_step(5);
    deltas[0] = 1;
    ui_update();
    CHECK(session_kit_lane(1) == LANE_MT);
    release_step(5);
    CHECK(lane_at(0, 1, LANE_LT, 5) == KIT_NOTE);
    CHECK(lane_at(0, 1, LANE_MT, 5) == KIT_OFF);
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    press_step(7);
    long_step(7);
    CHECK(ui.mode == UI_KIT_STEP_EDIT);
    CHECK(ui.edit_step == 7 && ui.kit_cursor == LANE_MT);
    release_step(7);
    press_encoder(1, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    CHECK(memcmp(patterns_before, g_seq.patterns, sizeof(patterns_before)) == 0);
    // Beyond the length nothing; a press released on another screen nothing.
    pattern_set_track_length(&g_seq.patterns[0], 1, 8);
    sequencer_refresh_track_playback(1, 0);
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    tap_step(10);
    press_step(2);
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_SESSION);
    release_step(2);
    CHECK(memcmp(patterns_before, g_seq.patterns, sizeof(patterns_before)) == 0);
    CHECK(memcmp(steps_before, g_seq.patterns[0].steps[1], sizeof(steps_before)) == 0);
}

static void test_step_leds_show_focused_lane(void) {
    make_kit(1);
    uint32_t *words = g_seq.patterns[0].kit_steps[1];
    words[0] = kit_set(words[0], LANE_SD, KIT_NOTE);
    words[3] = kit_set(words[3], LANE_SD, KIT_ACCENT);
    words[5] = kit_set(words[5], LANE_SD, KIT_FLAM);
    words[6] = kit_set(words[6], LANE_LT, KIT_NOTE);
    words[12] = kit_set(words[12], LANE_SD, KIT_NOTE);  // beyond the length
    pattern_set_step(&g_seq.patterns[0], 1, 1, 60, 100, STEP_FLAG_ACTIVE); // step_t: not shown
    pattern_set_track_length(&g_seq.patterns[0], 1, 8);
    sequencer_refresh_track_playback(1, 0);
    const uint16_t sd_mask = (1u << 0) | (1u << 3) | (1u << 5);
    focus_lane(1, 0, LANE_SD);
    ui_render();
    CHECK(led_mask == sd_mask);
    deltas[0] = 1;
    ui_update();
    ui_render();
    CHECK(led_mask == (1u << 6));
    deltas[0] = -1;
    ui_update();
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    CHECK(led_mask == sd_mask);
    deltas[0] = 1;
    ui_update();
    CHECK(led_mask == (1u << 6));
}

static void test_mixed_type_track_uses_cell_type(void) {
    g_seq.patterns[31].tracks[1].type = TRACK_TYPE_KIT; // SYNTH in location 0
    sequencer_refresh_track_playback(1, 31);
    panel(PANEL_PAD, 1, 0);
    tap_step(3);
    CHECK(g_seq.patterns[0].steps[1][3].flags & STEP_FLAG_ACTIVE);
    CHECK(g_seq.patterns[0].kit_steps[1][3] == 0);
    deltas[0] = 1;                                      // one stop
    ui_update();
    CHECK(session_get_focus().track == 2);
    for (unsigned b = 0; b < 3; b++) press_encoder(0, BTN_LONG_PRESS);
    CHECK(session_bank() == 3);
    panel(PANEL_PAD, 1, 7);                             // location 31
    session_focus_t f = session_get_focus();
    CHECK(f.track == 1 && f.location == 31 && f.lane == KIT_LANE_BD);
    tap_step(3);
    CHECK(lane_at(31, 1, KIT_LANE_BD, 3) == KIT_NOTE);
    CHECK(!(g_seq.patterns[31].steps[1][3].flags & STEP_FLAG_ACTIVE));
    deltas[0] = 1;
    ui_update();
    f = session_get_focus();
    CHECK(f.track == 1 && f.location == 31 && f.lane == LANE_SD);
}

static bool kit_steps_untouched(void) {
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        for (uint8_t t = 0; t < NUM_TRACKS; t++) {
            for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
                if (g_seq.patterns[p].kit_steps[t][s]) return false;
            }
        }
    }
    return true;
}

static void test_synth_cell_unchanged(void) {
    make_kit(2);
    panel(PANEL_PAD, 1, 0);
    press_step(3);
    CHECK(!(g_seq.patterns[0].steps[1][3].flags & STEP_FLAG_ACTIVE)); // on release
    release_step(3);
    CHECK(g_seq.patterns[0].steps[1][3].flags & STEP_FLAG_ACTIVE);
    ui_render();
    CHECK(led_mask == (1u << 3));
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW && ui.selected_track == 1);
    press_step(5);
    CHECK(g_seq.patterns[0].steps[1][5].flags & STEP_FLAG_ACTIVE);   // on press
    release_step(5);
    CHECK(g_seq.patterns[0].steps[1][5].flags & STEP_FLAG_ACTIVE);
    CHECK(led_mask == ((1u << 3) | (1u << 5)));
    CHECK(kit_steps_untouched());
}

// ─── Kit step editor ───────────────────────────────────────────

static void turn_value(int8_t detents) {
    deltas[1] = detents;
    ui_update();
}

static void turn_track(int8_t detents) {
    deltas[0] = detents;
    ui_update();
}

static void test_long_press_on_kit_lane_opens_kit_editor(void) {
    make_kit(1);
    pattern_set_track_length(&g_seq.patterns[0], 1, 8);
    sequencer_refresh_track_playback(1, 0);
    focus_lane(1, 0, LANE_SD);
    step_t steps_before[STEPS_PER_PATTERN];
    memcpy(steps_before, g_seq.patterns[0].steps[1], sizeof(steps_before));
    press_step(5);
    long_step(5);
    CHECK(ui.mode == UI_KIT_STEP_EDIT);
    CHECK(ui.edit_step == 5 && ui.kit_cursor == LANE_SD);
    CHECK(ui.selected_track == 1 && g_seq.active_pattern[1] == 0);
    release_step(5);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_OFF);       // the long press sets nothing

    // VALUE: one step per event, whatever its size, clamped at flam and off.
    delta_scale = 4;                                    // VALUE accelerates, TRACK does not
    const uint8_t expected[3] = {KIT_NOTE, KIT_ACCENT, KIT_FLAM};
    for (unsigned i = 0; i < 3; i++) {
        turn_value(1);
        CHECK(lane_at(0, 1, LANE_SD, 5) == expected[i]);
    }
    CHECK(cell_has_clip(1, 0));                         // refreshed for playback
    turn_value(1);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_FLAM);
    turn_value(-8);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_ACCENT);
    turn_value(-1);
    turn_value(-1);
    turn_value(-1);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_OFF);
    CHECK(!cell_has_clip(1, 0));

    // A TRACK detent is one lane; from off, a large VALUE turn is one step.
    turn_track(1);
    CHECK(ui.kit_cursor == LANE_LT);
    delta_scale = 1;
    turn_value(8);
    CHECK(lane_at(0, 1, LANE_LT, 5) == KIT_NOTE);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_OFF);

    // The cursor is clamped to AC..RC; AC is off or on.
    turn_track(-20);
    CHECK(ui.kit_cursor == KIT_LANE_AC);
    turn_value(3);
    turn_value(3);
    CHECK(lane_at(0, 1, KIT_LANE_AC, 5) == 1);
    turn_track(20);
    CHECK(ui.kit_cursor == KIT_LANES - 1);

    // Setting CH clears OH, and the other way round.
    turn_track(KIT_LANE_OH - (KIT_LANES - 1));
    CHECK(ui.kit_cursor == KIT_LANE_OH);
    turn_value(1);
    CHECK(lane_at(0, 1, KIT_LANE_OH, 5) == KIT_NOTE);
    turn_track(-1);
    turn_value(1);
    CHECK(lane_at(0, 1, KIT_LANE_CH, 5) == KIT_NOTE);
    CHECK(lane_at(0, 1, KIT_LANE_OH, 5) == KIT_OFF);

    // Step keys select another step within the length.
    tap_step(2);
    CHECK(ui.edit_step == 2 && ui.mode == UI_KIT_STEP_EDIT);
    tap_step(12);
    CHECK(ui.edit_step == 2);
    turn_value(2);
    CHECK(lane_at(0, 1, KIT_LANE_CH, 2) == KIT_NOTE);
    press_step(3);
    long_step(3);                                       // no step_t gestures here
    release_step(3);
    CHECK(ui.edit_step == 3 && ui.mode == UI_KIT_STEP_EDIT);

    press_encoder(1, BTN_PRESS);
    CHECK(ui.mode == UI_SESSION);
    CHECK(memcmp(steps_before, g_seq.patterns[0].steps[1], sizeof(steps_before)) == 0);
}

static void test_pattern_view_track_press_on_kit_opens_kit_editor(void) {
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW && ui.selected_track == 1);
    turn_track(1);                                      // LT
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_KIT_STEP_EDIT);
    CHECK(ui.edit_step == 0 && ui.kit_cursor == LANE_LT);
    turn_value(1);
    CHECK(lane_at(0, 1, LANE_LT, 0) == KIT_NOTE);
    CHECK(!(g_seq.patterns[0].steps[1][0].flags & STEP_FLAG_ACTIVE));
    press_encoder(1, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    // The note editor still opens on a SYNTH cell.
    turn_track(KIT_LANES - LANE_LT);                    // LT..RC, then track 2
    CHECK(ui.selected_track == 2);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_STEP_EDIT && ui.edit_step == 0);
    press_encoder(1, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
}

static void test_pattern_view_walk_moves_session_lane(void) {
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    press_encoder(0, BTN_PRESS);
    turn_track(2);                                      // MT in the Pattern view
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_SESSION);
    session_focus_t f = session_get_focus();
    CHECK(f.track == 1 && f.location == 0 && f.lane == LANE_MT);
    tap_step(1);
    CHECK(lane_at(0, 1, LANE_MT, 1) == KIT_NOTE);
    CHECK(lane_at(0, 1, LANE_SD, 1) == KIT_OFF);
}

// ─── Track config on a kit ─────────────────────────────────────

// The pattern selector, then a long press on step `track + 1`.
static void open_track_config(uint8_t track) {
    press_encoder(1, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_PATTERN_SELECT);
    press_step(track);
    long_step(track);
    release_step(track);
    CHECK(ui.mode == UI_TRACK_CONFIG && ui.config_track == track);
}

static bool type_everywhere(uint8_t track, track_type_t type) {
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        if (g_seq.patterns[p].tracks[track].type != type) return false;
    }
    return true;
}

static void test_track_config_kit_params(void) {
    track_config_t before = g_seq.patterns[0].tracks[1];
    kit_config_t defaults;
    kit_config_defaults(&defaults);
    open_track_config(1);
    turn_value(1);
    CHECK(type_everywhere(1, TRACK_TYPE_DRUM));
    turn_value(1);
    CHECK(type_everywhere(1, TRACK_TYPE_KIT));

    // TYPE, CHANNEL, GATE, FLAM, VEL NORM, VEL ACC, VEL MAX, NOTE BD..NOTE RC.
    turn_track(1);                                      // CHANNEL
    turn_value(3);
    turn_track(1);                                      // GATE
    turn_value(-100);
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        CHECK(g_seq.patterns[p].tracks[1].midi_channel == before.midi_channel + 3);
        CHECK(g_seq.patterns[p].tracks[1].gate_length_pct == 10);
    }
    turn_track(1);                                      // FLAM
    turn_value(-20);
    CHECK(g_seq.kit_config[1].flam_pulses == KIT_FLAM_MIN);
    turn_value(100);
    CHECK(g_seq.kit_config[1].flam_pulses == KIT_FLAM_MAX);
    turn_track(1);                                      // VEL NORM
    turn_value(100);
    CHECK(g_seq.kit_config[1].vel_normal == 127);
    turn_value(-127);
    CHECK(g_seq.kit_config[1].vel_normal == 1);
    turn_value(30);
    CHECK(g_seq.kit_config[1].vel_normal == 31);
    turn_track(4);                                      // VEL ACC, VEL MAX, NOTE BD, NOTE SD
    turn_value(-100);
    CHECK(g_seq.kit_config[1].note[LANE_SD - 1] == 0);
    turn_value(127);
    turn_value(10);
    CHECK(g_seq.kit_config[1].note[LANE_SD - 1] == 127);
    turn_value(-90);
    CHECK(g_seq.kit_config[1].note[LANE_SD - 1] == 37);
    CHECK(g_seq.kit_config[1].vel_accent == defaults.vel_accent);
    CHECK(g_seq.kit_config[1].vel_max == defaults.vel_max);
    CHECK(g_seq.kit_config[1].note[0] == defaults.note[0]);

    // NOTE LT..NOTE RC end the list: OUTPUT and CV are never reached.
    for (uint8_t voice = LANE_LT - 1; voice < KIT_VOICES; voice++) {
        turn_track(1);
        turn_value(1);
    }
    turn_track(5);                                      // clamped on NOTE RC
    turn_value(1);
    for (uint8_t voice = LANE_LT - 1; voice < KIT_VOICES - 1; voice++) {
        CHECK(g_seq.kit_config[1].note[voice] == defaults.note[voice] + 1);
    }
    CHECK(g_seq.kit_config[1].note[KIT_VOICES - 1] == defaults.note[KIT_VOICES - 1] + 2);
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        const track_config_t *tc = &g_seq.patterns[p].tracks[1];
        CHECK(tc->type == TRACK_TYPE_KIT);
        CHECK(tc->output_mode == before.output_mode && tc->cv_channel == before.cv_channel);
        CHECK(tc->drum_note == before.drum_note);
    }
    CHECK(memcmp(&g_seq.kit_config[0], &defaults, sizeof(defaults)) == 0);
    CHECK(memcmp(&g_seq.kit_config[2], &defaults, sizeof(defaults)) == 0);
}

static void test_track_config_third_kit_refused(void) {
    make_kit(1);
    make_kit(4);
    open_track_config(6);
    CHECK(type_everywhere(6, TRACK_TYPE_DRUM));         // tracks 5-8 start as DRUM
    ui.notify_msg[0] = '\0';
    turn_value(1);                                      // KIT skipped
    CHECK(type_everywhere(6, TRACK_TYPE_SYNTH));
    CHECK(strcmp(ui.notify_msg, "MAX 2 KITS") == 0);
    ui.notify_msg[0] = '\0';
    turn_value(1);
    CHECK(type_everywhere(6, TRACK_TYPE_DRUM));
    CHECK(ui.notify_msg[0] == '\0');
    turn_value(-1);
    CHECK(type_everywhere(6, TRACK_TYPE_SYNTH));
    turn_value(-1);                                     // backwards too
    CHECK(type_everywhere(6, TRACK_TYPE_DRUM));
    CHECK(strcmp(ui.notify_msg, "MAX 2 KITS") == 0);
    CHECK(seq_kit_tracks() == ((1u << 1) | (1u << 4)));
    // A kit track itself still cycles through KIT.
    ui.notify_msg[0] = '\0';
    for (unsigned i = 0; i < 3; i++) press_encoder(0, BTN_PRESS);
    CHECK(ui.config_track == 1);
    turn_value(1);
    CHECK(type_everywhere(1, TRACK_TYPE_SYNTH));
    turn_value(-1);
    CHECK(type_everywhere(1, TRACK_TYPE_KIT));
    CHECK(ui.notify_msg[0] == '\0');
}

// ─── Kit screens ───────────────────────────────────────────────

typedef bool frame_t[OLED_HEIGHT][OLED_WIDTH];

// One rendered frame, read pixel by pixel.
static void render_frame(frame_t frame) {
    ui.notify_ticks = 0;
    display_clear();
    ui_render();
    for (uint8_t y = 0; y < OLED_HEIGHT; y++) {
        for (uint8_t x = 0; x < OLED_WIDTH; x++) frame[y][x] = ssd1309_get_pixel(x, y);
    }
}

static bool frames_differ(frame_t a, frame_t b) {
    return memcmp(a, b, sizeof(frame_t)) != 0;
}

static void test_render_kit_session_screen(void) {
    static frame_t sd, ch;
    make_kit(1);
    uint32_t *words = g_seq.patterns[0].kit_steps[1];
    words[0] = kit_set(words[0], LANE_SD, KIT_NOTE);
    words[4] = kit_set(words[4], LANE_SD, KIT_FLAM);
    words[2] = kit_set(words[2], KIT_LANE_CH, KIT_ACCENT);
    sequencer_refresh_track_playback(1, 0);
    focus_lane(1, 0, LANE_SD);
    render_frame(sd);
    CHECK(!oled_blank());
    turn_track(KIT_LANE_CH - LANE_SD);
    CHECK(session_get_focus().lane == KIT_LANE_CH);
    render_frame(ch);
    CHECK(frames_differ(sd, ch));

    // Playing, length 5: one bar apart the global bar step is the same and the
    // kit's playhead is not; only the strip changes.
    pattern_set_track_length(&g_seq.patterns[0], 1, 5);
    sequencer_refresh_track_playback(1, 0);
    transport_start();
    pulses(PULSES_PER_STEP + 1);
    CHECK(g_seq.transport.current_track_step[1] == 1);
    render_frame(sd);
    pulses(STEPS_PER_PATTERN * PULSES_PER_STEP);
    CHECK(g_seq.transport.current_track_step[1] == 2);
    render_frame(ch);
    CHECK(frames_differ(sd, ch));
    for (unsigned y = 0; y < OLED_HEIGHT; y++) {
        for (unsigned x = 0; x < OLED_WIDTH; x++) {
            if (sd[y][x] == ch[y][x]) continue;
            CHECK(y >= DISPLAY_KIT_STRIP_Y && y < DISPLAY_KIT_STRIP_Y + DISPLAY_KIT_STRIP_H);
        }
    }
}

static void test_render_kit_editor_and_lane_view(void) {
    static frame_t before, after;
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    render_frame(before);
    CHECK(!oled_blank());
    tap_step(3);
    CHECK(lane_at(0, 1, LANE_SD, 3) == KIT_NOTE);
    render_frame(after);
    CHECK(frames_differ(before, after));

    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_KIT_STEP_EDIT);
    render_frame(before);
    CHECK(!oled_blank());
    turn_value(1);
    CHECK(lane_at(0, 1, LANE_SD, 0) == KIT_NOTE);
    render_frame(after);
    CHECK(frames_differ(before, after));
    turn_track(1);                                      // the cursor shows too
    render_frame(before);
    CHECK(frames_differ(before, after));
}

static void test_render_kit_config(void) {
    static frame_t before, after;
    make_kit(1);
    open_track_config(1);
    turn_track(KIT_PARAM_FLAM);
    render_frame(before);
    CHECK(!oled_blank());
    turn_value(1);
    CHECK(g_seq.kit_config[1].flam_pulses == 4);
    render_frame(after);
    CHECK(frames_differ(before, after));
    transport_set_bpm(150.0f);                          // FLAM shows its ms at the tempo
    render_frame(before);
    CHECK(frames_differ(before, after));
    turn_track(KIT_PARAM_COUNT);                        // the list scrolls to NOTE RC
    render_frame(after);
    CHECK(frames_differ(before, after));
}

static void test_pattern_select_shows_flam_drops(void) {
    static frame_t none, one;
    press_encoder(1, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_PATTERN_SELECT);
    jitter_stats.flam_drops = 0;
    render_frame(none);
    jitter_stats.flam_drops = 1;
    render_frame(one);
    CHECK(frames_differ(none, one));
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
    RUN(test_pad_launch_decides_and_writes_atomically);
    RUN(test_scene_cancel_decides_and_writes_atomically);
    RUN(test_step_release_toggles_the_cell_focused_at_press);
    RUN(test_step_long_press_opens_the_cell_focused_at_press);
    RUN(test_long_press_begun_on_another_screen_is_dropped);
    RUN(test_second_held_step_does_not_toggle_in_the_editor);
    RUN(test_play_pressed_during_panic_does_not_fire_after_it);
    RUN(test_step_held_through_save_or_load_does_not_mute);
    RUN(test_kit_step_cycles_on_release);
    RUN(test_kit_ac_toggle_and_ch_clears_oh);
    RUN(test_kit_step_beyond_length_does_nothing);
    RUN(test_held_step_stays_on_its_lane);
    RUN(test_eight_detents_move_eight_stops);
    RUN(test_pattern_view_on_kit_edits_lane);
    RUN(test_step_leds_show_focused_lane);
    RUN(test_mixed_type_track_uses_cell_type);
    RUN(test_synth_cell_unchanged);
    RUN(test_long_press_on_kit_lane_opens_kit_editor);
    RUN(test_pattern_view_track_press_on_kit_opens_kit_editor);
    RUN(test_pattern_view_walk_moves_session_lane);
    RUN(test_track_config_kit_params);
    RUN(test_track_config_third_kit_refused);
    RUN(test_render_kit_session_screen);
    RUN(test_render_kit_editor_and_lane_view);
    RUN(test_render_kit_config);
    RUN(test_pattern_select_shows_flam_drops);
    reset();
    render_views(argc > 1 ? argv[1] : NULL);
    puts("PASS all six OLED views render");
    return 0;
}
