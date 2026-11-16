#include <stdio.h>
#include <stdlib.h>
#include "fakes/fake_hardware.h"
#include "hardware/flash.h"
#include "../src/ui.c"
#include "rgb_leds.h"

sequencer_state_t g_seq;
static button_event_t buttons[2];
static int8_t deltas[2];
static bool held[2];
static step_buttons_t step_buttons;
static uint16_t led_mask;

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
void mcp23017_init(void) {}
void mcp23017_poll(void) {}
step_buttons_t *mcp23017_get_buttons(void) { return &step_buttons; }
void step_leds_init(void) {}
bool step_leds_available(void) { return true; }
void step_leds_set_mask(uint16_t mask) { led_mask = mask; }
void step_leds_clear(void) { led_mask = 0; }
void rgb_leds_init(void) {}
bool rgb_leds_show(const rgb_t frame[LED_COUNT]) { (void)frame; return true; }

// Scripted panel input: push_panel() queues a press and its release.
#define PANEL_QUEUE 32
static panel_event_t panel_queue[PANEL_QUEUE];
static unsigned panel_head, panel_tail, panel_resyncs;
void panel_input_init(void) {}
void panel_input_poll(uint32_t now_ms) { (void)now_ms; }
bool panel_input_next(panel_event_t *out) {
    if (panel_head == panel_tail) return false;
    *out = panel_queue[panel_head++];
    return true;
}
void panel_input_resync(void) {
    panel_head = panel_tail = 0;
    panel_resyncs++;
}
static void push_panel(panel_kind_t kind, uint8_t track, uint8_t row) {
    CHECK(panel_tail + 2 <= PANEL_QUEUE);
    panel_queue[panel_tail++] = (panel_event_t){kind, track, row, true};
    panel_queue[panel_tail++] = (panel_event_t){kind, track, row, false};
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

static void test_track_config_edit_reaches_all_locations(void) {
    uint8_t old = g_seq.patterns[0].tracks[0].midi_channel;
    press_encoder(1, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_PATTERN_SELECT);
    step_buttons.long_pressed = 1;
    ui_update();
    CHECK(ui.mode == UI_TRACK_CONFIG);
    step_buttons.long_pressed = 0;
    deltas[0] = 1;
    deltas[1] = 1;
    ui_update();
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        CHECK(g_seq.patterns[p].tracks[0].midi_channel == old + 1);
    }
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
    RUN(test_flash_controls_are_blocked_during_playback);
    RUN(test_song_mode_has_a_return_to_pattern_playback);
    RUN(test_song_editor_lights_its_three_buttons);
    RUN(test_nonplaying_editor_hides_playhead);
    RUN(test_track_config_edit_reaches_all_locations);
    reset();
    render_views(argc > 1 ? argv[1] : NULL);
    puts("PASS all six OLED views render");
    return 0;
}
