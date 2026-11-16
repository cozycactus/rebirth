#include <stdio.h>
#include <stdlib.h>
#include "fakes/fake_hardware.h"
#include "hardware/flash.h"
#include "../src/ui.c"
#include "../src/sequencer.c"

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
    memset(&step_buttons, 0, sizeof(step_buttons));
    memset(buttons, 0, sizeof(buttons));
    memset(deltas, 0, sizeof(deltas));
    memset(held, 0, sizeof(held));
    ui_init();
}

static void press_encoder(uint8_t encoder, button_event_t event) {
    buttons[encoder] = event;
    ui_update();
}

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

static void test_performance_mutes_do_not_edit_notes(void) {
    step_buttons.pressed = 1;
    ui_update();
    CHECK(g_seq.track_mutes == 1);
    CHECK(g_seq.patterns[0].steps[0][0].flags == 0);
    CHECK(ui.mode == UI_PERFORMANCE);
}

static void test_editor_follows_performance_selection(void) {
    step_buttons.pressed = 1u << 13; // A6
    ui_update();
    CHECK(g_seq.transport.current_pattern[0] == 5);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    CHECK(g_seq.active_pattern[0] == 5);
    step_down(1);
    step_up(1);
    CHECK(g_seq.patterns[5].steps[0][0].flags & STEP_FLAG_ACTIVE);
    CHECK(g_seq.patterns[0].steps[0][0].flags == 0);
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
    CHECK(ui.mode == UI_PERFORMANCE);
    CHECK(fake_midi_length == bytes);
    held[1] = false;
    press_encoder(1, BTN_PRESS); // Also covers a short release of the second button.
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    CHECK(!ui.panic_latched);
    press_encoder(1, BTN_PRESS); // A fresh press may start again.
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

static void render_views(const char *path) {
    uint8_t image[6][OLED_HEIGHT][OLED_WIDTH / 8];
    for (unsigned entry = 0; entry < MAX_SONG_ENTRIES; entry++) CHECK(song_append(0, 255));
    ui.song_cursor = MAX_SONG_ENTRIES - 1;
    for (unsigned mode = UI_PERFORMANCE; mode <= UI_SONG_EDIT; mode++) {
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
    RUN(test_performance_mutes_do_not_edit_notes);
    RUN(test_editor_follows_performance_selection);
    RUN(test_panic_consumes_staggered_holds_and_releases);
    RUN(test_accent_can_combine_with_slide_or_tie);
    RUN(test_long_config_press_never_mutes);
    RUN(test_selector_short_press_mutes_on_release);
    RUN(test_flash_controls_are_blocked_during_playback);
    RUN(test_song_mode_has_a_return_to_pattern_playback);
    RUN(test_song_editor_lights_its_three_buttons);
    RUN(test_nonplaying_editor_hides_playhead);
    RUN(test_grid_tap_toggles_on_release);
    RUN(test_grid_hold_turn_edits_note_without_toggling);
    RUN(test_grid_hold_turn_edits_velocity_not_tempo);
    RUN(test_grid_hold_turn_creates_empty_step);
    RUN(test_grid_hold_time_decides_tap_or_peek);
    RUN(test_grid_tap_beside_a_held_step);
    RUN(test_grid_two_held_steps_edit_independently);
    RUN(test_grid_hold_leaves_unheld_steps_alone);
    RUN(test_grid_hold_edits_the_edited_pattern_not_the_playing_one);
    RUN(test_grid_hold_encoder_presses_set_flags);
    RUN(test_grid_encoder_press_outlives_step_release);
    RUN(test_grid_encoder_long_press_outlives_step_release);
    RUN(test_grid_play_during_hold_cancels_the_tap);
    RUN(test_grid_track_is_frozen_while_an_encoder_owns_the_gesture);
    RUN(test_grid_ignores_button_held_before_entering);
    RUN(test_grid_panic_forgets_held_steps);
    RUN(test_grid_hold_edit_reaches_playback);
    RUN(test_grid_hold_edit_while_playing_is_heard);
    RUN(test_grid_tap_within_one_update_toggles_once);
    RUN(test_grid_turn_and_release_in_one_update_is_an_edit);
    RUN(test_grid_turn_in_the_same_update_as_the_press_is_not_an_edit);
    RUN(test_grid_hold_turn_clamps_and_still_cancels_tap);
    RUN(test_grid_hold_creates_drum_hit_with_kit_note);
    reset();
    render_views(argc > 1 ? argv[1] : NULL);
    puts("PASS all six OLED views render");
    return 0;
}
