#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "fakes/fake_hardware.h"
#include "hardware/flash.h"
#include "../src/ui.c"
#include "../src/sequencer.c"
#include "rgb_leds.h"

sequencer_state_t g_seq;
static button_event_t buttons[2];
// Scripted detents for the next read: `deltas` detents, worth `delta_scale`
// each to a number (a scripted fast turn when above 1).
static int8_t deltas[2];
static int8_t delta_scale = 1;
// Edges the way the panel delivers them (turn_edges): one per encoder_poll,
// i.e. one per UI pass. As in encoder.c, ENC_EDGES_PER_DETENT edges the same
// way make a detent, timed by encoder_accel_detent on the fake clock.
static int16_t edges_to_come[2], edge_count[2], turn_detents[2], turn_amount[2];
static encoder_accel_t accel[2];
static bool held[2];
static step_buttons_t step_buttons;
static uint16_t led_mask;
static rgb_t shown[LED_COUNT];      // last frame sent to the RGB chain
static unsigned rgb_inits;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

void encoder_init(void) {}
void encoder_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (unsigned e = 0; e < 2; e++) {
        if (edges_to_come[e] == 0) continue;
        int8_t edge = edges_to_come[e] > 0 ? 1 : -1;
        edges_to_come[e] -= edge;
        edge_count[e] += edge;
        if (edge_count[e] == edge * ENC_EDGES_PER_DETENT) {
            edge_count[e] = 0;
            turn_detents[e] += edge;
            turn_amount[e] += edge * encoder_accel_detent(&accel[e], edge, now);
        }
    }
}
encoder_turn_t encoder_get_turn(uint8_t encoder) {
    encoder_turn_t turn = {
        (int8_t)(deltas[encoder] + turn_detents[encoder]),
        (int8_t)(deltas[encoder] * delta_scale + turn_amount[encoder]),
    };
    deltas[encoder] = 0;
    turn_detents[encoder] = turn_amount[encoder] = 0;
    return turn;
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
// What the panel layer reports for each pad, and its sync generation. A queued
// press sets the pad down, a queued release clears it; a resync forgets both.
static bool pads_down[NUM_TRACKS][8];
static uint32_t panel_generation;
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
bool panel_input_pad_down(uint8_t track, uint8_t row) {
    return track < NUM_TRACKS && row < 8 && pads_down[track][row];
}
uint32_t panel_input_generation(void) { return panel_generation; }
void panel_input_resync(void) {
    panel_head = panel_tail = 0;
    debounce_polls = 0;
    memset(pads_down, 0, sizeof(pads_down));
    panel_generation++;
    panel_resyncs++;
}
static void push_panel_event(panel_kind_t kind, uint8_t track, uint8_t row, bool down) {
    CHECK(panel_tail + 1 <= PANEL_QUEUE);
    panel_queue[panel_tail++] = (panel_event_t){kind, track, row, down};
    if (kind == PANEL_PAD) pads_down[track][row] = down;
}
static void push_panel(panel_kind_t kind, uint8_t track, uint8_t row) {
    push_panel_event(kind, track, row, true);
    push_panel_event(kind, track, row, false);
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
    memset(edges_to_come, 0, sizeof(edges_to_come));
    memset(edge_count, 0, sizeof(edge_count));
    memset(turn_detents, 0, sizeof(turn_detents));
    memset(turn_amount, 0, sizeof(turn_amount));
    memset(accel, 0, sizeof(accel));
    memset(held, 0, sizeof(held));
    panel_head = panel_tail = panel_resyncs = 0;
    debounce_polls = 0;
    memset(pads_down, 0, sizeof(pads_down));
    panel_generation = 1;
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

// Note Ons on MIDI channel `channel` (0-15) with this note and velocity, from
// the decoded output: channel messages may go out with running status.
static unsigned count_channel_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    static fake_midi_msg_t messages[FAKE_MIDI_CAPACITY];
    unsigned decoded = fake_midi_decode(messages, FAKE_MIDI_CAPACITY);
    unsigned found = 0;
    for (unsigned i = 0; i < decoded; i++) {
        if (messages[i].status == (MIDI_NOTE_ON | channel) && messages[i].data1 == note &&
            messages[i].data2 == velocity) found++;
    }
    return found;
}

static unsigned count_note_on(uint8_t note, uint8_t velocity) {
    return count_channel_note_on(0, note, velocity);
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
    g_seq.patterns[3].tracks[0].type = TRACK_TYPE_DRUM;     // the step grid; the roll has its own test
    transport_start();
    g_seq.active_pattern[0] = 3;
    g_seq.transport.current_track_step[0] = 0;
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[3], 0, 0, -1);
    uint8_t without_playhead[OLED_BUF_SIZE];
    memcpy(without_playhead, ssd1309_get_framebuffer(), sizeof(without_playhead));
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[3], 0, NO_QUEUED_PATTERN, -1);
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
                              LAUNCH_QUANT_TRACK_LOOP, 0xFF, NULL, NULL, STEPS_PER_PATTERN);
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
                              LAUNCH_QUANT_TRACK_LOOP, 5, NULL, NULL, STEPS_PER_PATTERN);
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
    turn(0, 2);                             // track 3
    CHECK(ui.selected_track == 2);
    g_seq.active_pattern[2] = 3;            // editor on A4 while A1 plays
    pattern_set_step(&g_seq.patterns[3], 2, 4, 60, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(&g_seq.patterns[0], 2, 4, 70, 100, STEP_FLAG_ACTIVE);
    step_down(1u << 4);
    turn(0, 3);
    CHECK(g_seq.patterns[3].steps[2][4].note == 63);
    CHECK(STEP(2, 4).note == 70);
    CHECK(g_seq.patterns[3].steps[0][4].flags == 0);    // track 0 untouched
    CHECK(STEP(0, 4).flags == 0);
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
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
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
static void test_grid_encoder2_short_during_hold_only_cancels_the_tap(void) {
    enter_pattern_view();
    step_down(1u << 4);
    encoder_down(1);
    step_up(1u << 4);
    encoder_up(1, BTN_PRESS);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
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
    step_down(1u << 0);                     // Session view: toggles on release only
    CHECK(ui.mode == UI_SESSION);
    enter_pattern_view();                   // button 1 is still down
    turn(0, 1);
    CHECK(ui.selected_track == 1);          // not a hold gesture
    step_up(1u << 0);
    CHECK(STEP(0, 0).flags == 0);
    CHECK(STEP(1, 0).flags == 0);
}

static void test_grid_panic_forgets_held_steps(void) {
    enter_pattern_view();
    step_down((1u << 4) | (1u << 5));
    held[0] = held[1] = true;
    press_encoder(0, BTN_LONG_PRESS);       // Panic returns to the Session view
    CHECK(ui.mode == UI_SESSION);
    held[0] = held[1] = false;
    ui_update();                            // unlatch
    CHECK(!ui.panic_latched);
    step_up(1u << 5);                       // released in the Session view
    CHECK(STEP(0, 5).flags == 0);
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

// ─── The last note ─────────────────────────────────────────────

static void test_last_note_starts_at_c4_in_the_session_view(void) {
    for (uint8_t t = 0; t < NUM_TRACKS; t++) CHECK(ui.last_note[t] == 60);
    press_step(3);
    release_step(3);
    CHECK(STEP(0, 3).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(0, 3).note == 60);
}

static void test_last_note_follows_a_turn_per_track(void) {
    enter_pattern_view();
    step_down(1u << 4);
    turn(0, -27);
    CHECK(STEP(0, 4).note == 33);
    CHECK(ui.last_note[0] == 33);
    step_up(1u << 4);
    press_encoder(0, BTN_LONG_PRESS);       // back to the Session view
    CHECK(ui.mode == UI_SESSION);
    press_step(9);
    release_step(9);
    CHECK(STEP(0, 9).note == 33);
    CHECK(ui.last_note[0] == 33);
    panel(PANEL_PAD, 1, 0);                 // another SYNTH track keeps its own
    press_step(2);
    release_step(2);
    CHECK(STEP(1, 2).note == 60);
    CHECK(ui.last_note[1] == 60);
}

static void test_last_note_seeds_a_pattern_view_tap(void) {
    ui.last_note[0] = 33;
    enter_pattern_view();
    step_down(1u << 6);
    step_up(1u << 6);
    CHECK(STEP(0, 6).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(0, 6).note == 33);
}

static void test_last_note_seeds_the_step_edit_long_press(void) {
    ui.last_note[0] = 33;
    press_encoder(0, BTN_PRESS);            // Pattern View
    press_encoder(0, BTN_PRESS);            // Step Edit, step 1
    CHECK(ui.mode == UI_STEP_EDIT);
    press_step(5);
    long_step(5);
    release_step(5);
    CHECK(STEP(0, 5).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(0, 5).note == 33);
    CHECK(ui.last_note[0] == 33);
}

static void test_step_edit_turn_sets_the_last_note(void) {
    press_encoder(0, BTN_PRESS);
    press_encoder(0, BTN_PRESS);
    pattern_set_step(&g_seq.patterns[0], 0, 0, 50, 100, STEP_FLAG_ACTIVE);
    turn(0, 4);
    CHECK(STEP(0, 0).note == 54);
    CHECK(ui.last_note[0] == 54);
    turn(1, 5);                             // velocity leaves it alone
    CHECK(ui.last_note[0] == 54);
}

static void test_last_note_two_held_empty_steps_turn_together(void) {
    ui.last_note[0] = 48;
    enter_pattern_view();
    step_down((1u << 2) | (1u << 9));
    turn(0, 2);
    CHECK(STEP(0, 2).note == 50);
    CHECK(STEP(0, 9).note == 50);
    CHECK(ui.last_note[0] == 50);
    step_up((1u << 2) | (1u << 9));
}

static void test_last_note_does_not_touch_a_step_that_has_a_note(void) {
    pattern_set_step(&g_seq.patterns[0], 0, 3, 45, 100, STEP_FLAG_ACTIVE);
    press_step(3);
    release_step(3);                        // off
    CHECK(STEP(0, 3).flags == 0);
    press_step(3);
    release_step(3);                        // on again
    CHECK(STEP(0, 3).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(0, 3).note == 45);
    CHECK(ui.last_note[0] == 60);
}

static void test_last_note_comes_from_the_lowest_held_step(void) {
    pattern_set_step(&g_seq.patterns[0], 0, 2, 50, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(&g_seq.patterns[0], 0, 9, 70, 100, STEP_FLAG_ACTIVE);
    enter_pattern_view();
    step_down((1u << 2) | (1u << 9));
    turn(0, 3);
    CHECK(STEP(0, 2).note == 53);
    CHECK(STEP(0, 9).note == 73);
    CHECK(ui.last_note[0] == 53);
    step_up((1u << 2) | (1u << 9));
}

static void test_last_note_zero_is_a_note(void) {
    enter_pattern_view();
    step_down(1u << 4);
    turn(0, -100);                          // 60 down to 0
    CHECK(STEP(0, 4).note == 0);
    CHECK(ui.last_note[0] == 0);
    step_up(1u << 4);
    step_down(1u << 5);
    step_up(1u << 5);                       // a tap: seeded with 0, not C4
    CHECK(STEP(0, 5).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(0, 5).note == 0);
    step_down(1u << 7);
    turn(0, 2);
    CHECK(STEP(0, 7).note == 2);
    CHECK(ui.last_note[0] == 2);
    step_up(1u << 7);
}

static void test_last_note_joining_step_is_not_edited(void) {
    ui.last_note[0] = 40;
    enter_pattern_view();
    step_down(1u << 10);
    deltas[0] = 2;
    step_down(1u << 3);                     // S03 joins in the update of the turn
    CHECK(STEP(0, 10).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(0, 10).note == 42);
    CHECK(STEP(0, 3).flags == 0);
    CHECK(STEP(0, 3).note == 0);
    CHECK(ui.last_note[0] == 42);
    step_up((1u << 10) | (1u << 3));
}

static void test_last_note_ignores_drums_velocity_and_clamped_turns(void) {
    ui.last_note[0] = 50;
    enter_pattern_view();
    step_down(1u << 4);
    turn(1, -10);                           // velocity only
    CHECK(ui.last_note[0] == 50);
    step_up(1u << 4);
    pattern_set_step(&g_seq.patterns[0], 0, 8, 127, 100, STEP_FLAG_ACTIVE);
    step_down(1u << 8);
    turn(0, 5);                             // clamped: no note changed
    CHECK(STEP(0, 8).note == 127);
    CHECK(ui.last_note[0] == 50);
    step_up(1u << 8);
    turn(0, 4);                             // track 5: drum, drum_note 40
    CHECK(ui.selected_track == 4);
    step_down(1u << 3);
    CHECK(STEP(4, 3).note == 0);
    turn(0, 3);                             // a drum note turn
    step_up(1u << 3);
    CHECK(STEP(4, 3).flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(4, 3).note == 43);
    for (uint8_t t = 4; t < NUM_TRACKS; t++) CHECK(ui.last_note[t] == 60);
    CHECK(ui.last_note[0] == 50);
}

static void test_last_note_drum_tap_takes_drum_note(void) {
    enter_pattern_view();
    turn(0, 4);
    step_down(1u << 6);
    step_up(1u << 6);
    CHECK(STEP(4, 6).note == 40);
    CHECK(ui.last_note[4] == 60);
}

// ─── Clip copy ─────────────────────────────────────────────────

static void hold_pad(uint8_t track, uint8_t row) {
    push_panel_event(PANEL_PAD, track, row, true);
    ui_update();
}

static void let_go_pad(uint8_t track, uint8_t row) {
    push_panel_event(PANEL_PAD, track, row, false);
    ui_update();
}

static transport_snapshot_t transport_now(void) {
    transport_snapshot_t snapshot;
    sequencer_snapshot(&snapshot);
    return snapshot;
}

static bool same_transport(const transport_snapshot_t *a, const transport_snapshot_t *b) {
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        if (a->current[t] != b->current[t] || a->queued[t] != b->queued[t]) return false;
    }
    return true;
}

// Location 0 of track 0 gets two steps, a kit word and a length of 8; every
// other location is left as pattern_init made it.
static void fill_source_clip(uint8_t location) {
    pattern_t *p = &g_seq.patterns[location];
    pattern_set_step(p, 0, 3, 70, 90, STEP_FLAG_ACTIVE | STEP_FLAG_SLIDE);
    pattern_set_step(p, 0, 9, 55, 40, STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT);
    p->kit_steps[0][2] = 0x155u;
    pattern_set_track_length(p, 0, 8);
}

static bool clip_equals(uint8_t a, uint8_t b, uint8_t track) {
    const pattern_t *pa = &g_seq.patterns[a];
    const pattern_t *pb = &g_seq.patterns[b];
    return memcmp(pa->steps[track], pb->steps[track], sizeof(pa->steps[track])) == 0 &&
           memcmp(pa->kit_steps[track], pb->kit_steps[track], sizeof(pa->kit_steps[track])) == 0 &&
           pattern_get_track_length(pa, track) == pattern_get_track_length(pb, track);
}

static void test_copy_hold_then_press_copies_the_clip(void) {
    fill_source_clip(0);
    hold_pad(0, 0);
    transport_snapshot_t before = transport_now();
    sleep_ms(PAD_COPY_HOLD_MS);
    hold_pad(0, 2);
    CHECK(clip_equals(0, 2, 0));
    CHECK(g_seq.patterns[2].steps[0][3].note == 70);
    CHECK(g_seq.patterns[2].kit_steps[0][2] == 0x155u);
    CHECK(pattern_get_track_length(&g_seq.patterns[2], 0) == 8);
    transport_snapshot_t after = transport_now();
    CHECK(same_transport(&before, &after));
    session_focus_t focus = session_get_focus();
    CHECK(focus.track == 0 && focus.location == 2);
    CHECK(strcmp(ui.notify_msg, "COPY A1>A3") == 0);
    CHECK(clip_equals(0, 0, 0));
    CHECK(sequencer_clip_mask(0) & (1ull << 2));       // the target's row is prepared
}

static void test_copy_leaves_a_playing_or_queued_target_alone(void) {
    fill_source_clip(1);
    transport_start();
    tick();
    hold_pad(0, 1);                                     // queues A2 behind the loop
    CHECK(transport_now().queued[0] == 1);
    transport_snapshot_t before = transport_now();
    sleep_ms(PAD_COPY_HOLD_MS);
    hold_pad(0, 0);                                     // the playing clip is the target
    CHECK(clip_equals(1, 0, 0));
    transport_snapshot_t after = transport_now();
    CHECK(same_transport(&before, &after));
    CHECK(before.current[0] == 0 && before.queued[0] == 1);

    // A queued target: queue row 4 by hand, then copy onto it.
    reset();
    fill_source_clip(1);
    transport_start();
    tick();
    hold_pad(0, 1);
    CHECK(transport_queue_track_pattern(0, 3));
    before = transport_now();
    CHECK(before.queued[0] == 3);
    sleep_ms(PAD_COPY_HOLD_MS);
    hold_pad(0, 3);
    CHECK(clip_equals(1, 3, 0));
    after = transport_now();
    CHECK(same_transport(&before, &after));
}

static void test_copy_needs_the_full_hold_time(void) {
    fill_source_clip(0);
    hold_pad(0, 0);
    sleep_ms(PAD_COPY_HOLD_MS - 1);
    hold_pad(0, 2);
    CHECK(!clip_equals(0, 2, 0));
    CHECK(transport_now().current[0] == 2);             // an ordinary launch
    CHECK(strcmp(ui.notify_msg, "") == 0);
}

static void test_copy_two_presses_in_one_update_are_two_launches(void) {
    fill_source_clip(0);
    sleep_ms(10000);
    push_panel_event(PANEL_PAD, 0, 0, true);
    push_panel_event(PANEL_PAD, 0, 2, true);
    ui_update();
    CHECK(!clip_equals(0, 2, 0));
    CHECK(transport_now().current[0] == 2);
}

static void test_copy_ignores_a_pad_of_another_track(void) {
    fill_source_clip(0);
    hold_pad(0, 0);
    sleep_ms(PAD_COPY_HOLD_MS);
    hold_pad(1, 2);
    CHECK(transport_now().current[1] == 2);
    CHECK(clip_equals(0, 2, 0) == false);
    CHECK(strcmp(ui.notify_msg, "") == 0);
}

static void test_copy_needs_the_panel_to_report_the_source_down(void) {
    fill_source_clip(0);
    hold_pad(0, 0);
    sleep_ms(PAD_COPY_HOLD_MS);
    pads_down[0][0] = false;                            // forgotten without a release
    hold_pad(0, 2);
    CHECK(!clip_equals(0, 2, 0));
    CHECK(transport_now().current[0] == 2);
}

static void test_copy_ignores_a_hold_from_before_a_recovery(void) {
    fill_source_clip(0);
    hold_pad(0, 0);
    sleep_ms(PAD_COPY_HOLD_MS);
    panel_generation++;                                 // the pad still reads down
    CHECK(pads_down[0][0]);
    hold_pad(0, 2);
    CHECK(!clip_equals(0, 2, 0));
    CHECK(transport_now().current[0] == 2);
}

static void test_copy_ignores_a_pad_reported_down_without_a_press(void) {
    fill_source_clip(0);
    hold_pad(0, 0);
    let_go_pad(0, 0);                                   // the release is handled
    sleep_ms(PAD_COPY_HOLD_MS);
    pads_down[0][0] = true;
    hold_pad(0, 2);
    CHECK(!clip_equals(0, 2, 0));
    CHECK(transport_now().current[0] == 2);
}

static void test_copy_takes_the_longest_held_source(void) {
    fill_source_clip(1);                                // the longer-held pad, a higher row
    pattern_set_step(&g_seq.patterns[0], 0, 5, 80, 100, STEP_FLAG_ACTIVE);
    hold_pad(0, 1);
    sleep_ms(200);
    hold_pad(0, 0);
    sleep_ms(600);
    hold_pad(0, 2);
    CHECK(clip_equals(1, 2, 0));
    CHECK(g_seq.patterns[2].steps[0][5].flags == 0);
    hold_pad(0, 3);                                     // a third press copies the same source
    CHECK(clip_equals(1, 3, 0));
    CHECK(strcmp(ui.notify_msg, "COPY A2>A4") == 0);
}

static void test_copy_source_stays_put_when_the_bank_changes(void) {
    fill_source_clip(0);
    hold_pad(0, 0);
    press_encoder(0, BTN_LONG_PRESS);                   // bank B
    CHECK(session_bank() == 1);
    sleep_ms(PAD_COPY_HOLD_MS);
    hold_pad(0, 2);
    CHECK(strcmp(ui.notify_msg, "COPY A1>B3") == 0);
    CHECK(clip_equals(0, 10, 0));
    CHECK(!clip_equals(0, 8, 0));
    CHECK(session_get_focus().location == 10);
}

static void test_copy_in_song_mode_still_copies(void) {
    fill_source_clip(0);
    transport_set_mode(PLAY_MODE_SONG);
    hold_pad(0, 0);
    CHECK(strcmp(ui.notify_msg, "SONG MODE") == 0);
    sleep_ms(PAD_COPY_HOLD_MS);
    hold_pad(0, 2);
    CHECK(strcmp(ui.notify_msg, "COPY A1>A3") == 0);
    CHECK(clip_equals(0, 2, 0));
    CHECK(transport_now().current[0] == 0);
}

static void test_copy_outside_the_session_view_keeps_the_focus(void) {
    fill_source_clip(0);
    enter_pattern_view();
    session_focus_t before = session_get_focus();
    hold_pad(0, 0);
    sleep_ms(PAD_COPY_HOLD_MS);
    hold_pad(0, 2);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    CHECK(clip_equals(0, 2, 0));
    session_focus_t after = session_get_focus();
    CHECK(after.track == before.track && after.location == before.location);
    CHECK(strcmp(ui.notify_msg, "COPY A1>A3") == 0);
}

static void test_copy_leaves_a_held_step_on_its_cell(void) {
    hold_pad(0, 0);
    step_down(1u << 4);                                 // bound to A1
    sleep_ms(PAD_COPY_HOLD_MS);
    hold_pad(0, 2);
    CHECK(session_get_focus().location == 2);
    step_up(1u << 4);
    CHECK(g_seq.patterns[0].steps[0][4].flags == STEP_FLAG_ACTIVE);
    CHECK(g_seq.patterns[2].steps[0][4].flags == 0);
    step_down(1u << 5);                                 // pressed after the copy
    step_up(1u << 5);
    CHECK(g_seq.patterns[2].steps[0][5].flags == STEP_FLAG_ACTIVE);
    CHECK(g_seq.patterns[0].steps[0][5].flags == 0);
}

static void test_copy_over_the_playing_clip_is_heard(void) {
    pattern_set_step(&g_seq.patterns[1], 0, 1, 72, 100, STEP_FLAG_ACTIVE);
    transport_start();
    timer_hw->alarm[0] = pulse_interval_us;
    tick();                                             // step 1 plays; nothing is on it
    hold_pad(0, 1);
    sleep_ms(PAD_COPY_HOLD_MS);
    hold_pad(0, 0);                                     // onto the clip that is playing
    fake_midi_length = 0;
    for (unsigned i = 0; i < PULSES_PER_STEP; i++) tick();
    CHECK(count_note_on(72, 100) == 1);
}

static void test_copy_takes_a_kit_clips_kit_steps(void) {
    g_seq.patterns[0].tracks[1].type = TRACK_TYPE_KIT;
    g_seq.patterns[0].kit_steps[1][3] = kit_press(0, KIT_LANE_BD);
    CHECK(g_seq.patterns[0].kit_steps[1][3] != 0);
    hold_pad(1, 0);
    sleep_ms(PAD_COPY_HOLD_MS);
    hold_pad(1, 2);
    CHECK(g_seq.patterns[2].kit_steps[1][3] == g_seq.patterns[0].kit_steps[1][3]);
    CHECK(clip_equals(0, 2, 1));
    CHECK(strcmp(ui.notify_msg, "COPY A1>A3") == 0);
}

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
    g_seq.patterns[0].tracks[0].type = TRACK_TYPE_DRUM;     // the step grid; the roll has its own test
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
    g_seq.patterns[0].tracks[0].type = TRACK_TYPE_DRUM;     // the step grid; the roll has its own test
    enter_pattern_view();
    step_down(1u << 9);
    STEP(0, 9).note = 1;                    // a drum step has a fixed note: set it directly
    STEP(0, 9).velocity = 127;
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

// ─── Piano roll ────────────────────────────────────────────────
//
// Pixel tests of the Pattern view of a SYNTH clip: info column x 0..23, roll
// x 24..127, step s on rows 4s..4s+3, semitone n of the window on columns
// 24+4n..24+4n+3.

#define ROLL_BEAT_PIXELS 26    // one per semitone on each beat line
// FNV-1a of the frames the pre-roll code drew
#define GOLDEN_DRUM_IDLE 505614909u
#define GOLDEN_DRUM_HELD 2794909885u

static bool px(int x, int y) { return ssd1309_get_pixel((uint8_t)x, (uint8_t)y); }

static void roll_draw(uint8_t step, int8_t held_step) {
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[0], 0, step, held_step);
}

static void roll_note(uint8_t step, uint8_t note, uint8_t flags) {
    pattern_set_step(&g_seq.patterns[0], 0, step, note, 100, STEP_FLAG_ACTIVE | flags);
}

// True when every pixel of the rectangle is `on`.
static bool rect_is(int x0, int y0, int x1, int y1, bool on) {
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) if (px(x, y) != on) return false;
    }
    return true;
}

static unsigned lit_in(int x0, int y0, int x1, int y1) {
    unsigned n = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) if (px(x, y)) n++;
    }
    return n;
}

// Info line `line` (columns 0..22) is `text` drawn alone. The frame survives.
static bool info_is(unsigned line, const char *text) {
    uint8_t saved[OLED_BUF_SIZE];
    bool actual[FONT_CHAR_H][23];
    memcpy(saved, ssd1309_get_framebuffer(), sizeof(saved));
    for (unsigned y = 0; y < FONT_CHAR_H; y++) {
        for (unsigned x = 0; x < 23; x++) actual[y][x] = px((int)x, (int)(line * 8 + y));
    }
    display_clear();
    ssd1309_draw_string(0, (uint8_t)(line * 8), text, false);
    bool same = true;
    for (unsigned y = 0; y < FONT_CHAR_H; y++) {
        for (unsigned x = 0; x < 23; x++) {
            if (actual[y][x] != px((int)x, (int)(line * 8 + y))) same = false;
        }
    }
    memcpy(ssd1309_get_framebuffer(), saved, sizeof(saved));
    return same;
}

static bool info_is_blank(unsigned line) {
    return rect_is(0, (int)(line * 8), 22, (int)(line * 8 + 7), false);
}

static void test_roll_one_note_is_a_block_in_a_centred_window(void) {
    roll_note(2, 60, 0);                    // window 48..73: note 60 is semitone 12
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(rect_is(72, 8, 74, 10, true));    // 3x3 at x = 24 + 4*12, y = 4*2
    CHECK(rect_is(72, 11, 74, 11, false));  // row y+3 stays clear
    CHECK(rect_is(75, 8, 75, 11, false));
    CHECK(rect_is(71, 8, 71, 11, false));
    CHECK(lit_in(24, 0, 127, 63) == 9 + 3 * ROLL_BEAT_PIXELS);
    roll_note(2, 60, STEP_FLAG_ACCENT);
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(rect_is(72, 8, 75, 10, true));    // accent: 4 wide
    CHECK(rect_is(72, 11, 75, 11, false));  // and row y+3 still clear
    CHECK(rect_is(76, 8, 76, 11, false));
}

static void test_roll_window_holds_25_semitones_and_not_26(void) {
    roll_note(0, 40, 0);
    roll_note(1, 65, 0);                    // 25 apart: window 40..65
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(rect_is(24, 0, 26, 2, true));     // the low note in semitone 0
    CHECK(rect_is(124, 4, 126, 6, true));   // the high one in semitone 25
    CHECK(!px(127, 4));
    roll_note(1, 66, 0);                    // 26 apart: the high one is an edge bar
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(rect_is(24, 0, 26, 2, true));
    CHECK(rect_is(127, 4, 127, 6, true));
    CHECK(rect_is(126, 4, 126, 7, false));
    CHECK(rect_is(127, 7, 127, 7, false));
}

static void test_roll_notes_0_and_127_alone_sit_at_the_window_ends(void) {
    roll_note(0, 0, 0);                     // window 0..25
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(rect_is(24, 0, 26, 2, true));
    roll_note(0, 127, 0);                   // window 102..127
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(rect_is(124, 0, 126, 2, true));   // semitone 25
    CHECK(rect_is(127, 0, 127, 2, false));
    CHECK(rect_is(24, 0, 27, 3, false));
}

static void test_roll_held_note_stays_in_the_window(void) {
    roll_note(0, 30, 0);
    roll_note(1, 90, 0);
    roll_note(2, 60, 0);
    roll_draw(NO_QUEUED_PATTERN, 2);        // window 35..60
    CHECK(rect_is(24, 0, 24, 2, true));     // 30: bar at column 24
    CHECK(rect_is(25, 0, 27, 3, false));
    CHECK(rect_is(127, 4, 127, 6, true));   // 90: bar at column 127
    CHECK(rect_is(124, 8, 126, 10, true));  // 60: the last semitone
    roll_note(0, 40, 0);                    // 40 and 90 with 60 held: 40 is
    roll_draw(NO_QUEUED_PATTERN, 2);        // inside window 40..65 (the plan said
    CHECK(rect_is(24, 0, 26, 2, true));     // two edge bars; the formula gives one)
    CHECK(rect_is(127, 4, 127, 6, true));
    CHECK(rect_is(104, 8, 106, 10, true));  // 60: semitone 20
    // A held step beyond the length is not "inside the length": no effect.
    reset();
    roll_note(0, 30, 0);
    roll_note(1, 90, 0);
    pattern_set_track_length(&g_seq.patterns[0], 0, 4);
    roll_note(9, 60, 0);
    roll_draw(NO_QUEUED_PATTERN, -1);
    uint8_t free_window[OLED_BUF_SIZE];
    memcpy(free_window, ssd1309_get_framebuffer(), sizeof(free_window));
    roll_draw(NO_QUEUED_PATTERN, 9);
    for (int y = 0; y < 64; y++) {
        for (int x = 24; x < 128; x++) {
            bool before = (free_window[(y / 8) * OLED_WIDTH + x] >> (y & 7)) & 1u;
            CHECK(before == px(x, y));
        }
    }
}

static void test_roll_slide_runs_from_the_lower_to_the_higher_column(void) {
    roll_note(0, 40, STEP_FLAG_SLIDE);
    roll_note(1, 50, 0);                    // window 33..58: columns 52 and 92
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(rect_is(52, 3, 94, 3, true));
    CHECK(rect_is(51, 3, 51, 3, false) && rect_is(95, 3, 95, 3, false));
    roll_note(0, 50, STEP_FLAG_SLIDE);
    roll_note(1, 40, 0);                    // downwards: the same row
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(rect_is(52, 3, 94, 3, true));
    CHECK(rect_is(51, 3, 51, 3, false) && rect_is(95, 3, 95, 3, false));
    // To an edge bar: 100 is above the window 40..65.
    roll_note(0, 40, STEP_FLAG_SLIDE);
    roll_note(1, 100, 0);
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(rect_is(24, 3, 127, 3, true));
    // From an edge bar: 30 is below the window 35..60 (held step 2 on 60).
    reset();
    roll_note(0, 30, STEP_FLAG_SLIDE);
    roll_note(1, 60, 0);
    roll_note(2, 90, 0);
    roll_draw(NO_QUEUED_PATTERN, 1);        // window 35..60
    CHECK(rect_is(24, 3, 126, 3, true));
    CHECK(rect_is(127, 3, 127, 3, false));
}

static void test_roll_slide_needs_a_next_step_that_is_on_and_inside_the_length(void) {
    roll_note(0, 40, STEP_FLAG_SLIDE);      // next step off
    roll_note(2, 50, 0);
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(lit_in(24, 3, 127, 3) == 0);
    reset();
    roll_note(1, 40, STEP_FLAG_SLIDE);      // next step on but beyond the length
    roll_note(2, 50, 0);
    pattern_set_track_length(&g_seq.patterns[0], 0, 2);
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(lit_in(24, 7, 127, 7) == 0);
    reset();
    roll_note(15, 40, STEP_FLAG_SLIDE);     // the last step does not wrap round
    roll_note(0, 50, 0);
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(lit_in(24, 63, 127, 63) == 0);
}

static void test_roll_slide_from_40_to_100_writes_nothing_outside_the_roll(void) {
    roll_note(0, 40, 0);
    roll_note(1, 100, 0);
    roll_draw(NO_QUEUED_PATTERN, -1);
    uint8_t plain[OLED_BUF_SIZE];
    memcpy(plain, ssd1309_get_framebuffer(), sizeof(plain));
    roll_note(0, 40, STEP_FLAG_SLIDE);
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(lit_in(24, 3, 127, 3) == 104);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 24; x++) {      // the info column is the same with the slide
            bool before = (plain[(y / 8) * OLED_WIDTH + x] >> (y & 7)) & 1u;
            CHECK(before == px(x, y));
        }
    }
    CHECK(lit_in(0, 0, 127, 63) == (unsigned)lit_in(0, 0, 23, 63) + lit_in(24, 0, 127, 63));
}

static void test_roll_tie_is_one_pixel_with_or_without_accent(void) {
    roll_note(1, 60, STEP_FLAG_TIE);        // x = 72
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(px(73, 7));
    CHECK(!px(72, 7) && !px(74, 7) && !px(75, 7));
    roll_note(1, 60, STEP_FLAG_TIE | STEP_FLAG_ACCENT);
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(px(73, 7));
    CHECK(!px(72, 7) && !px(74, 7) && !px(75, 7));
    CHECK(rect_is(72, 4, 75, 6, true));
}

static void test_roll_steps_beyond_the_length_are_dotted_and_empty(void) {
    roll_note(1, 60, 0);
    roll_note(6, 100, 0);                   // beyond the length: no block, no window effect
    pattern_set_track_length(&g_seq.patterns[0], 0, 4);
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(rect_is(72, 4, 74, 6, true));     // window still centred on 60
    for (int s = 4; s < 16; s++) {
        int y = 4 * s;
        CHECK(lit_in(24, y, 127, y) == 0);
        CHECK(lit_in(24, y + 2, 127, y + 2) == 0);
        for (int x = 24; x < 128; x++) CHECK(px(x, y + 1) == ((x - 24) % 2 == 0));
    }
    CHECK(lit_in(24, 0, 127, 2) == 0);      // inside the length the row is plain
    CHECK(lit_in(24, 1, 127, 1) == 0);
}

static void test_roll_beat_lines_sit_in_row_y3_of_steps_4_8_12(void) {
    roll_draw(NO_QUEUED_PATTERN, -1);       // an empty clip
    for (int s = 0; s < 16; s++) {
        bool beat = s == 3 || s == 7 || s == 11;
        for (int n = 0; n < 26; n++) {
            CHECK(px(24 + 4 * n + 3, 4 * s + 3) == beat);
            CHECK(!px(24 + 4 * n, 4 * s + 3));
        }
    }
    CHECK(lit_in(24, 0, 127, 63) == 3 * ROLL_BEAT_PIXELS);
}

static void roll_expect_inversion(uint8_t step, bool inverted) {
    uint8_t plain[OLED_BUF_SIZE];
    roll_draw(NO_QUEUED_PATTERN, -1);
    memcpy(plain, ssd1309_get_framebuffer(), sizeof(plain));
    roll_draw(step, -1);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 128; x++) {
            bool before = (plain[(y / 8) * OLED_WIDTH + x] >> (y & 7)) & 1u;
            bool flipped = inverted && x >= 24 && y / 4 == step;
            CHECK(px(x, y) == (before != flipped));
        }
    }
}

static void test_roll_inverts_only_the_playing_step(void) {
    roll_note(0, 60, 0);
    roll_note(2, 64, STEP_FLAG_ACCENT);
    pattern_set_track_length(&g_seq.patterns[0], 0, 8);
    roll_expect_inversion(2, false);                    // stopped
    transport_start();
    CHECK(seq_playing_pattern(0) == &g_seq.patterns[0]);
    roll_expect_inversion(2, true);
    roll_expect_inversion(0, true);
    roll_expect_inversion(7, true);
    roll_expect_inversion(8, false);                    // beyond the length
    roll_expect_inversion(12, false);
    // Viewing another location than the one the track plays.
    g_seq.patterns[3].tracks[0].type = TRACK_TYPE_SYNTH;
    roll_note(2, 64, 0);
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[3], 0, NO_QUEUED_PATTERN, -1);
    uint8_t plain[OLED_BUF_SIZE];
    memcpy(plain, ssd1309_get_framebuffer(), sizeof(plain));
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[3], 0, 2, -1);
    CHECK(memcmp(plain, ssd1309_get_framebuffer(), sizeof(plain)) == 0);
}

static void test_roll_held_step_marks_column_23_and_fills_info_lines_2_to_5(void) {
    pattern_set_step(&g_seq.patterns[0], 0, 5, 70, 100,
                     STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT | STEP_FLAG_SLIDE | STEP_FLAG_TIE);
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(lit_in(23, 0, 23, 63) == 0);
    for (unsigned line = 2; line <= 5; line++) CHECK(info_is_blank(line));
    roll_draw(NO_QUEUED_PATTERN, 5);        // step 6
    CHECK(rect_is(23, 20, 23, 23, true));
    CHECK(lit_in(23, 0, 23, 63) == 4);
    CHECK(info_is(2, "S06"));
    CHECK(info_is(3, "A#4"));
    CHECK(info_is(4, "v100"));
    CHECK(info_is(5, "AST"));
    pattern_set_step(&g_seq.patterns[0], 0, 5, 70, 100, STEP_FLAG_ACTIVE | STEP_FLAG_TIE);
    roll_draw(NO_QUEUED_PATTERN, 5);
    CHECK(info_is(5, "T"));
    // An empty held step still shows its values.
    roll_draw(NO_QUEUED_PATTERN, 11);
    CHECK(rect_is(23, 44, 23, 47, true));
    CHECK(info_is(2, "S12"));
    CHECK(info_is(3, "C-1"));
    CHECK(info_is(4, "v0"));
    CHECK(info_is_blank(5));
}

static void test_roll_empty_clip_centres_the_window_on_c4(void) {
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(info_is(6, "C3"));                // window 48..73, so C4 is semitone 12
    roll_draw(NO_QUEUED_PATTERN, 3);        // a held step that is off changes nothing
    CHECK(info_is(6, "C3"));
    roll_note(0, 60, 0);
    roll_draw(NO_QUEUED_PATTERN, -1);
    CHECK(info_is(6, "C3"));
    CHECK(rect_is(72, 0, 74, 2, true));
}

static void test_roll_info_column_lines_0_1_6_7(void) {
    g_seq.active_pattern[2] = 9;            // B2
    g_seq.transport.queued_pattern[2] = 3;  // a queue and the mutes are not drawn here
    pattern_set_track_length(&g_seq.patterns[9], 2, 7);
    pattern_set_step(&g_seq.patterns[9], 2, 0, 1, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(&g_seq.patterns[9], 2, 1, 27, 100, STEP_FLAG_ACTIVE);  // 26 apart: window from C#-1
    g_seq.transport.bpm = 145.4f;
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[9], 2, NO_QUEUED_PATTERN, -1);
    CHECK(info_is(0, "B2T3"));
    CHECK(info_is(1, "L7"));
    CHECK(info_is_blank(2) && info_is_blank(3) && info_is_blank(4) && info_is_blank(5));
    CHECK(info_is(6, "C#-1"));
    CHECK(info_is(7, "145"));
    g_seq.transport.bpm = 99.6f;
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[9], 2, NO_QUEUED_PATTERN, -1);
    CHECK(info_is(7, "100"));
}

static void test_roll_info_text_stays_left_of_column_24(void) {
    // An empty clip draws nothing in the roll but its beat lines, so text that
    // overran the info column would show up as extra pixels. The held step is
    // off, so it adds none either, yet its flags and values still print.
    g_seq.patterns[0].steps[0][4].note = 1;
    g_seq.patterns[0].steps[0][4].velocity = 127;
    g_seq.patterns[0].steps[0][4].flags = STEP_FLAG_ACCENT | STEP_FLAG_SLIDE | STEP_FLAG_TIE;
    g_seq.transport.bpm = 300.0f;
    g_seq.active_pattern[0] = 31;           // D8
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[0], 0, NO_QUEUED_PATTERN, 4);
    CHECK(lit_in(24, 0, 127, 63) == 3 * ROLL_BEAT_PIXELS);
    CHECK(info_is(0, "D8T1"));
    CHECK(info_is(2, "S05"));
    CHECK(info_is(3, "C#-1"));
    CHECK(info_is(4, "v127"));
    CHECK(info_is(5, "AST"));
    CHECK(info_is(7, "300"));
}

// The DRUM picture is the step grid it always was: this frame was captured
// from the code before the roll existed.
static uint32_t frame_hash(void) {
    const uint8_t *frame = ssd1309_get_framebuffer();
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < OLED_BUF_SIZE; i++) hash = (hash ^ frame[i]) * 16777619u;
    return hash;
}

static void test_drum_cell_keeps_the_step_grid_picture(void) {
    g_seq.patterns[0].tracks[0].type = TRACK_TYPE_DRUM;
    pattern_set_step(&g_seq.patterns[0], 0, 0, 36, 100, STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT);
    pattern_set_step(&g_seq.patterns[0], 0, 3, 36, 100, STEP_FLAG_ACTIVE | STEP_FLAG_SLIDE);
    pattern_set_step(&g_seq.patterns[0], 0, 5, 36, 100, STEP_FLAG_ACTIVE | STEP_FLAG_TIE);
    pattern_set_track_length(&g_seq.patterns[0], 0, 12);
    g_seq.transport.queued_pattern[0] = 4;
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[0], 0, NO_QUEUED_PATTERN, -1);
    CHECK(frame_hash() == GOLDEN_DRUM_IDLE);
    display_clear();
    display_draw_pattern_view(&g_seq.patterns[0], 0, NO_QUEUED_PATTERN, 5);
    CHECK(frame_hash() == GOLDEN_DRUM_HELD);
    // Not a roll: the old info lines and the grid are there, column 23 is text.
    CHECK(px(32, 20) || px(32, 25));
}

static void test_roll_held_step_through_the_ui(void) {
    enter_pattern_view();                   // track 0 is a SYNTH track
    step_down(1u << 4);
    render();
    CHECK(rect_is(23, 16, 23, 19, true));   // step 5, shown from the first moment
    CHECK(info_is(2, "S05"));
    CHECK(info_is(3, "C-1"));
    CHECK(info_is(4, "v0"));
    step_up(1u << 4);
    step_down(1u << 9);
    turn(0, -59);                           // step 10: note 1
    turn(1, 27);                            // velocity 127
    render();
    CHECK(info_is(3, "C#-1"));
    CHECK(info_is(4, "v127"));
    step_down(1u << 2);                     // a lower step joins: the lowest is shown
    render();
    CHECK(rect_is(23, 8, 23, 11, true));
    CHECK(lit_in(23, 0, 23, 63) == 4);
    CHECK(info_is(2, "S03"));
    step_up(1u << 2);
    step_up(1u << 9);
    render();
    CHECK(lit_in(23, 0, 23, 63) == 0);
    // The tempo is in the info column, not at the top right.
    g_seq.transport.bpm = 123.0f;
    render();
    CHECK(info_is(7, "123"));
    CHECK(lit_in(78, 0, 127, 7) == 0);
}

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
    CHECK(g_seq.patterns[3].steps[0][3].flags == 0);    // track 0 untouched
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
    panel(PANEL_PAD, 4, 3);                 // Session focus on track 5, A4: selected at once while stopped
    CHECK(g_seq.transport.current_pattern[4] == 3);
    enter_pattern_view();
    CHECK(ui.selected_track == 4);
    CHECK(g_seq.active_pattern[4] == 3);
    step_down(1u << 0);
    turn(1, -10);                           // drum hit at velocity 90
    step_up(1u << 0);
    CHECK(g_seq.patterns[3].steps[4][0].flags == STEP_FLAG_ACTIVE);
    CHECK(STEP(4, 0).flags == 0);           // pattern 0 untouched
    CHECK(g_seq.patterns[3].steps[0][0].flags == 0);    // track 0 untouched
    transport_start();
    fake_midi_length = 0;
    timer_hw->alarm[0] = pulse_interval_us;
    tick();
    CHECK(count_channel_note_on(4, 40, 90) == 1);
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

// Packs the current frame into 1 bit per pixel rows, leftmost pixel in the
// top bit, and checks that something is lit.
static void capture_frame(uint8_t frame[OLED_HEIGHT][OLED_WIDTH / 8]) {
    unsigned pixels = 0;
    for (unsigned y = 0; y < OLED_HEIGHT; y++) {
        for (unsigned x = 0; x < OLED_WIDTH; x += 8) {
            uint8_t byte = 0;
            for (unsigned bit = 0; bit < 8; bit++) {
                if (ssd1309_get_pixel(x + bit, y)) { byte |= 1u << (7 - bit); pixels++; }
            }
            frame[y][x / 8] = byte;
        }
    }
    CHECK(pixels > 0);
}

static void render_views(const char *path) {
    // Six screens, then the piano roll not holding a step and holding one.
    uint8_t image[8][OLED_HEIGHT][OLED_WIDTH / 8];
    for (unsigned entry = 0; entry < MAX_SONG_ENTRIES; entry++) CHECK(song_append(0, 255));
    ui.song_cursor = MAX_SONG_ENTRIES - 1;
    for (unsigned mode = UI_SESSION; mode <= UI_SONG_EDIT; mode++) {
        ui.mode = (ui_mode_t)mode;
        ui.notify_ticks = 0;
        display_clear();
        ui_render();
        capture_frame(image[mode]);
    }

    // A clip that shows what the roll can draw: notes, an accent, a slide, a
    // tie, a note out of the window (100), and steps beyond the length (12..15,
    // one of them on). The window runs 48..73.
    pattern_t *clip = &g_seq.patterns[0];
    pattern_set_step(clip, 0, 0, 60, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(clip, 0, 1, 64, 100, STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT);
    pattern_set_step(clip, 0, 2, 67, 100, STEP_FLAG_ACTIVE | STEP_FLAG_SLIDE);
    pattern_set_step(clip, 0, 3, 72, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(clip, 0, 4, 55, 100, STEP_FLAG_ACTIVE | STEP_FLAG_TIE);
    pattern_set_step(clip, 0, 5, 100, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(clip, 0, 6, 48, 100, STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT | STEP_FLAG_SLIDE);
    pattern_set_step(clip, 0, 7, 52, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(clip, 0, 9, 62, 100, STEP_FLAG_ACTIVE | STEP_FLAG_SLIDE | STEP_FLAG_TIE);
    pattern_set_step(clip, 0, 10, 100, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(clip, 0, 13, 70, 100, STEP_FLAG_ACTIVE);
    pattern_set_track_length(clip, 0, 12);
    sequencer_refresh_track_playback(0, 0);
    transport_start();
    pulses(3 * PULSES_PER_STEP + 1);        // the playhead is on step 4
    CHECK(g_seq.transport.current_track_step[0] == 3);
    ui.mode = UI_PATTERN_VIEW;
    ui.selected_track = 0;
    ui.grid_hold = 0;
    display_clear();
    ui_render();
    capture_frame(image[6]);
    ui.grid_hold = 1u << 2;                 // step 3 held
    display_clear();
    ui_render();
    capture_frame(image[7]);
    ui.grid_hold = 0;
    if (path) {
        FILE *file = fopen(path, "wb");
        CHECK(file != NULL);
        fprintf(file, "P4\n256 256\n");
        for (unsigned pair = 0; pair < 4; pair++) {
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
#define LANE_RC (KIT_LANES - 1)

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
    delta_scale = 4;                // TRACK reads detents, not the accelerated amount
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
    press_step(5);                          // hold-step editing: a tap toggles
    CHECK(!(g_seq.patterns[0].steps[1][5].flags & STEP_FLAG_ACTIVE)); // on release
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

    // VALUE: one step per click, not accelerated, clamped at flam and off.
    delta_scale = 4;                                    // the editor reads detents
    const uint8_t expected[3] = {KIT_NOTE, KIT_ACCENT, KIT_FLAM};
    for (unsigned i = 0; i < 3; i++) {
        turn_value(1);
        CHECK(lane_at(0, 1, LANE_SD, 5) == expected[i]);
    }
    CHECK(cell_has_clip(1, 0));                         // refreshed for playback
    turn_value(1);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_FLAM);
    turn_value(-8);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_OFF);
    turn_value(-1);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_OFF);
    CHECK(!cell_has_clip(1, 0));

    // A TRACK detent is one lane; from off, eight clicks reach flam and stop there.
    turn_track(1);
    CHECK(ui.kit_cursor == LANE_LT);
    delta_scale = 1;
    turn_value(8);
    CHECK(lane_at(0, 1, LANE_LT, 5) == KIT_FLAM);
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
    CHECK(lane_at(0, 1, KIT_LANE_CH, 2) == KIT_ACCENT);
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

// ─── Clicks, the way the panel delivers them ───────────────────

// `count` edges of encoder `e`, one per UI pass, as the panel's loop sees them.
static void turn_edges(uint8_t e, int16_t count) {
    edges_to_come[e] = count;
    while (edges_to_come[e] != 0) ui_update();
}

// `detents` clicks of encoder `e`, ENC_EDGES_PER_DETENT edges each, the first
// after `first_gap_ms`, the others `gap_ms` after the one before.
static void clicks_apart(uint8_t e, int8_t detents, uint32_t first_gap_ms, uint32_t gap_ms) {
    int8_t dir = detents > 0 ? 1 : -1;
    for (uint32_t gap = first_gap_ms; detents != 0; gap = gap_ms) {
        sleep_ms(gap);
        turn_edges(e, (int16_t)(dir * ENC_EDGES_PER_DETENT));
        detents = (int8_t)(detents - dir);
    }
}

// `detents` slow clicks of encoder `e`: each one ENC_ACCEL_FAST_MS after the
// one before, too late to count as fast.
static void click(uint8_t e, int8_t detents) {
    clicks_apart(e, detents, ENC_ACCEL_FAST_MS, ENC_ACCEL_FAST_MS);
}

// A fast turn of `detents` clicks from rest: the first slow, the others
// FAST_CLICK_MS apart. Numbers move 1, 1, 1, 1, 4, 4, 4, then 8 per click.
#define FAST_CLICK_MS 20
static void fast_clicks(uint8_t e, int8_t detents) {
    clicks_apart(e, detents, ENC_ACCEL_FAST_MS, FAST_CLICK_MS);
}

static void test_kit_editor_value_click_is_one_step(void) {
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    press_step(5);
    long_step(5);
    release_step(5);
    CHECK(ui.mode == UI_KIT_STEP_EDIT && ui.kit_cursor == LANE_SD);
    turn_edges(1, ENC_EDGES_PER_DETENT - 1);            // not a whole click yet
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_OFF);
    turn_edges(1, 1);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_NOTE);
    click(1, 1);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_ACCENT);
    click(1, -1);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_NOTE);
    click(0, 1);
    CHECK(ui.kit_cursor == LANE_LT);
    click(1, 1);
    CHECK(lane_at(0, 1, LANE_LT, 5) == KIT_NOTE);
}

// VALUE moves by the detent count, one value step per click, clamped (off..flam; AC
// off/on), with no acceleration; the CH/OH rule holds.
static void test_kit_editor_value_moves_by_detents(void) {
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    press_step(5);
    long_step(5);
    release_step(5);
    CHECK(ui.mode == UI_KIT_STEP_EDIT && ui.kit_cursor == LANE_SD);
    delta_scale = 8;                                    // a fast turn: no acceleration
    turn_value(2);                                      // two clicks in one pass
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_ACCENT);
    turn_value(5);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_FLAM);       // clamped at flam
    turn_value(-2);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_NOTE);
    turn_value(-5);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_OFF);        // clamped at off
    turn_track(KIT_LANE_AC - LANE_SD);
    CHECK(ui.kit_cursor == KIT_LANE_AC);
    turn_value(3);
    CHECK(lane_at(0, 1, KIT_LANE_AC, 5) == 1);          // AC: off or on
    turn_value(-2);
    CHECK(lane_at(0, 1, KIT_LANE_AC, 5) == 0);
    turn_track(KIT_LANE_OH);
    turn_value(1);
    CHECK(lane_at(0, 1, KIT_LANE_OH, 5) == KIT_NOTE);
    turn_track(-1);                                     // CH
    turn_value(2);
    CHECK(lane_at(0, 1, KIT_LANE_CH, 5) == KIT_ACCENT);
    CHECK(lane_at(0, 1, KIT_LANE_OH, 5) == KIT_OFF);
    CHECK(cell_has_clip(1, 0));                         // refreshed for playback
}

static void test_track_config_type_click_is_one_type(void) {
    open_track_config(2);                               // a SYNTH track, no kits
    turn_edges(1, 2);                                   // half a click: no change
    CHECK(type_everywhere(2, TRACK_TYPE_SYNTH));
    turn_edges(1, ENC_EDGES_PER_DETENT - 2);
    CHECK(type_everywhere(2, TRACK_TYPE_DRUM));
    click(1, 1);
    CHECK(type_everywhere(2, TRACK_TYPE_KIT));
    click(1, 1);
    CHECK(type_everywhere(2, TRACK_TYPE_SYNTH));
    click(1, -1);
    CHECK(type_everywhere(2, TRACK_TYPE_KIT));
    click(1, 1);
    CHECK(type_everywhere(2, TRACK_TYPE_SYNTH));

    // Kits on two other tracks: one click from SYNTH is DRUM, the next skips
    // KIT once with one message.
    make_kit(1);
    make_kit(4);
    ui.notify_msg[0] = '\0';
    click(1, 1);
    CHECK(type_everywhere(2, TRACK_TYPE_DRUM));
    CHECK(ui.notify_msg[0] == '\0');
    click(1, 1);
    CHECK(type_everywhere(2, TRACK_TYPE_SYNTH));
    CHECK(strcmp(ui.notify_msg, "MAX 2 KITS") == 0);
}

static void test_track_config_rows_move_by_clicks(void) {
    make_kit(1);
    open_track_config(1);
    click(0, 1);
    CHECK(ui.config_param == KIT_PARAM_CHANNEL);
    click(0, KIT_PARAM_VEL_NORMAL - KIT_PARAM_CHANNEL);
    CHECK(ui.config_param == KIT_PARAM_VEL_NORMAL);
    delta_scale = 4;                                    // a fast turn
    deltas[0] = 1;
    ui_update();
    CHECK(ui.config_param == KIT_PARAM_VEL_ACCENT);     // rows: one per detent
    deltas[0] = -1;
    ui_update();
    CHECK(ui.config_param == KIT_PARAM_VEL_NORMAL);
    // Numbers take the accelerated amount.
    uint8_t vel = g_seq.kit_config[1].vel_normal;
    turn_value(1);
    CHECK(g_seq.kit_config[1].vel_normal == vel + 4);
    // A number moves on whole clicks too: nothing until the click is complete,
    // then one step for a slow click.
    delta_scale = 1;
    turn_edges(1, ENC_EDGES_PER_DETENT - 1);
    CHECK(g_seq.kit_config[1].vel_normal == vel + 4);
    turn_edges(1, 1);
    CHECK(g_seq.kit_config[1].vel_normal == vel + 5);
}

static bool bpm_is(float bpm) {
    return fabsf(g_seq.transport.bpm - bpm) < 0.001f;
}

static void test_slow_click_moves_tempo_and_patterns_by_one(void) {
    // Tempo, in the Session view and in the Pattern view: 0.1 BPM per click.
    float bpm = g_seq.transport.bpm;
    click(1, 1);
    CHECK(bpm_is(bpm + 0.1f));
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW);
    click(1, -1);
    CHECK(bpm_is(bpm));
    // Pattern select: one slot, one bank.
    press_encoder(1, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_PATTERN_SELECT && ui.select_pattern == 0);
    click(0, 1);
    CHECK(pattern_bank(ui.select_pattern) == 0 && pattern_slot(ui.select_pattern) == 1);
    click(1, 1);
    CHECK(pattern_bank(ui.select_pattern) == 1 && pattern_slot(ui.select_pattern) == 1);
    click(1, -1);
    click(1, -1);                                       // A wraps round to D
    CHECK(pattern_bank(ui.select_pattern) == PATTERN_BANKS - 1);
    click(0, -1);
    CHECK(pattern_slot(ui.select_pattern) == 0);
}

static void test_slow_click_moves_kit_config_by_one(void) {
    make_kit(2);
    open_track_config(2);
    track_config_t before = g_seq.patterns[0].tracks[2];
    kit_config_t kit = g_seq.kit_config[2];
    click(0, KIT_PARAM_CHANNEL);
    click(1, 1);
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        CHECK(g_seq.patterns[p].tracks[2].midi_channel == before.midi_channel + 1);
    }
    click(0, KIT_PARAM_GATE - KIT_PARAM_CHANNEL);
    click(1, -1);
    CHECK(g_seq.patterns[5].tracks[2].gate_length_pct == before.gate_length_pct - 1);
    click(0, KIT_PARAM_FLAM - KIT_PARAM_GATE);
    click(1, -1);
    CHECK(g_seq.kit_config[2].flam_pulses == kit.flam_pulses - 1);
    click(0, KIT_PARAM_VEL_NORMAL - KIT_PARAM_FLAM);
    click(1, 1);
    CHECK(g_seq.kit_config[2].vel_normal == kit.vel_normal + 1);
    click(0, KIT_PARAM_NOTE + LANE_MT - 1 - KIT_PARAM_VEL_NORMAL);
    click(1, -1);
    CHECK(g_seq.kit_config[2].note[LANE_MT - 1] == kit.note[LANE_MT - 1] - 1);
    click(1, -1);
    CHECK(g_seq.kit_config[2].note[LANE_MT - 1] == kit.note[LANE_MT - 1] - 2);
}

static void test_slow_click_moves_synth_and_drum_config_by_one(void) {
    open_track_config(0);                               // a SYNTH track
    track_config_t before = g_seq.patterns[0].tracks[0];
    click(0, TRACK_PARAM_CHANNEL);
    click(1, 1);
    CHECK(g_seq.patterns[9].tracks[0].midi_channel == before.midi_channel + 1);
    click(0, TRACK_PARAM_OUTPUT - TRACK_PARAM_CHANNEL);
    click(1, 1);
    CHECK(g_seq.patterns[9].tracks[0].output_mode == (before.output_mode + 1) % 3);
    click(1, -1);
    CHECK(g_seq.patterns[9].tracks[0].output_mode == before.output_mode);
    click(0, TRACK_PARAM_GATE - TRACK_PARAM_OUTPUT);
    click(1, 1);
    CHECK(g_seq.patterns[9].tracks[0].gate_length_pct == before.gate_length_pct + 1);
    for (unsigned i = 0; i < 4; i++) press_encoder(0, BTN_PRESS);
    CHECK(ui.config_track == 4 && ui.config_param == 0);
    CHECK(type_everywhere(4, TRACK_TYPE_DRUM));
    uint8_t note = g_seq.patterns[0].tracks[4].drum_note;
    click(0, TRACK_PARAM_NOTE);
    click(1, -1);
    CHECK(g_seq.patterns[9].tracks[4].drum_note == note - 1);
}

static void test_slow_click_moves_note_editor_and_song_by_one(void) {
    open_editor_from_session();
    CHECK(ui.mode == UI_STEP_EDIT);
    const step_t *st = &g_seq.patterns[3].steps[2][4];
    uint8_t note = st->note, velocity = st->velocity;
    click(0, 1);
    click(1, 1);
    CHECK(st->note == note + 1 && st->velocity == velocity + 1);
    press_encoder(1, BTN_PRESS);
    CHECK(ui.mode == UI_SESSION);

    CHECK(song_append(0, 1) && song_append(0, 1) && song_append(0, 1));
    press_encoder(1, BTN_LONG_PRESS);
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_SONG_EDIT && ui.song_cursor == 0);
    click(0, 1);
    CHECK(ui.song_cursor == 1);
    click(1, 1);                                        // the entry's pattern
    CHECK(g_seq.song.entries[1].pattern_index[ui.selected_track] == 1);
    press_encoder(0, BTN_PRESS);                        // its repeats
    click(1, 1);
    CHECK(g_seq.song.entries[1].repeats == 2);
}

// Numbers accelerate on a fast turn; choices move one per click however fast.
static void test_fast_clicks_accelerate_numbers_only(void) {
    float bpm = g_seq.transport.bpm;
    fast_clicks(1, 8);                                  // 1+1+1+1+4+4+4+8 tenths
    CHECK(bpm_is(bpm + 2.4f));
    click(1, -1);                                       // slow again: one tenth
    CHECK(bpm_is(bpm + 2.3f));
    fast_clicks(0, 6);                                  // TRACK: six stops
    CHECK(session_get_focus().track == 6);

    make_kit(1);
    open_track_config(1);
    fast_clicks(0, KIT_PARAM_VEL_NORMAL);               // rows: one per click
    CHECK(ui.config_param == KIT_PARAM_VEL_NORMAL);
    uint8_t vel = g_seq.kit_config[1].vel_normal;
    fast_clicks(1, 8);
    CHECK(g_seq.kit_config[1].vel_normal == vel + 24);
    fast_clicks(1, -5);                                 // 1+1+1+1+4
    CHECK(g_seq.kit_config[1].vel_normal == vel + 16);
    fast_clicks(0, -KIT_PARAM_VEL_NORMAL);              // TYPE: one type per click
    CHECK(ui.config_param == KIT_PARAM_TYPE);
    fast_clicks(1, 3);
    CHECK(type_everywhere(1, TRACK_TYPE_KIT));
    fast_clicks(1, 1);
    CHECK(type_everywhere(1, TRACK_TYPE_SYNTH));
    fast_clicks(0, TRACK_PARAM_OUTPUT);                 // OUTPUT: one mode per click
    uint8_t mode = g_seq.patterns[0].tracks[1].output_mode;
    fast_clicks(1, 5);
    CHECK(g_seq.patterns[0].tracks[1].output_mode == (mode + 5) % 3);

    press_encoder(1, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_SELECT);
    ui.select_pattern = 0;
    fast_clicks(0, 6);
    CHECK(pattern_slot(ui.select_pattern) == 6);
    fast_clicks(1, 5);                                  // A..D, then B
    CHECK(pattern_bank(ui.select_pattern) == 1);
}

static void test_fast_clicks_in_editors(void) {
    open_editor_from_session();
    const step_t *st = &g_seq.patterns[3].steps[2][4];
    fast_clicks(0, 9);                                  // 1+1+1+1+4+4+4+8+8
    fast_clicks(1, 7);                                  // 1+1+1+1+4+4+4
    CHECK(st->note == 32 && st->velocity == 16);
    press_encoder(1, BTN_PRESS);

    // The kit step editor: one value step per click.
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    press_step(5);
    long_step(5);
    release_step(5);
    CHECK(ui.mode == UI_KIT_STEP_EDIT);
    fast_clicks(1, 2);
    CHECK(lane_at(0, 1, LANE_SD, 5) == KIT_ACCENT);
    fast_clicks(0, 5);                                  // the lane cursor too
    CHECK(ui.kit_cursor == LANE_SD + 5);
    press_encoder(1, BTN_PRESS);

    // The Song editor: the cursor and an entry's pattern are choices, its
    // repeats a number.
    for (unsigned i = 0; i < 8; i++) CHECK(song_append(0, 1));
    press_encoder(1, BTN_LONG_PRESS);
    press_encoder(0, BTN_LONG_PRESS);
    CHECK(ui.mode == UI_SONG_EDIT);
    fast_clicks(0, 6);
    CHECK(ui.song_cursor == 6);
    fast_clicks(1, 6);
    CHECK(g_seq.song.entries[6].pattern_index[ui.selected_track] == 6);
    press_encoder(0, BTN_PRESS);
    fast_clicks(1, 8);
    CHECK(g_seq.song.entries[6].repeats == 1 + 24);
}

// ─── Hold-step editing beside kits ─────────────────────────────

// No step_t of any location differs from patterns_before.
static bool step_data_untouched(void) {
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        if (memcmp(patterns_before[p].steps, g_seq.patterns[p].steps,
                   sizeof(g_seq.patterns[p].steps)) != 0) return false;
    }
    return true;
}

// A held SYNTH step moves by the accelerated amount: a slow click is exactly 1,
// a fast turn more; the clicks never move the track.
static void test_grid_hold_slow_click_moves_note_by_one(void) {
    enter_pattern_view();
    step_down(1u << 4);
    step_up(1u << 4);                       // active, note 60, velocity 100
    step_down(1u << 4);
    turn_edges(0, ENC_EDGES_PER_DETENT - 1);    // not a whole click yet
    CHECK(STEP(0, 4).note == 60);
    turn_edges(0, 1);
    CHECK(STEP(0, 4).note == 61);
    click(0, 1);
    CHECK(STEP(0, 4).note == 62);
    click(1, -1);
    CHECK(STEP(0, 4).velocity == 99);
    fast_clicks(0, 8);                      // 1+1+1+1+4+4+4+8
    CHECK(STEP(0, 4).note == 62 + 24);
    delta_scale = 4;                        // one detent worth 4
    turn(0, 1);
    CHECK(STEP(0, 4).note == 62 + 28);
    CHECK(ui.selected_track == 0);
    step_up(1u << 4);
    CHECK(STEP(0, 4).flags == STEP_FLAG_ACTIVE);    // edited: no toggle
}

// On a kit cell a held step takes no encoder: VALUE stays the tempo, TRACK
// keeps walking the lanes and an encoder press navigates; no step_t changes,
// and the release cycles the lane the press was bound to.
static void test_kit_hold_in_pattern_view_takes_no_encoder(void) {
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW && ui.selected_track == 1);
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    float bpm = g_seq.transport.bpm;
    step_down(1u << 4);
    click(1, 1);
    CHECK(bpm_is(bpm + 0.1f));
    turn(1, 3);
    CHECK(bpm_is(bpm + 0.4f));
    CHECK(ui.grid_hold == 0);
    click(0, 1);                            // SD -> LT
    CHECK(ui.selected_track == 1 && session_kit_lane(1) == LANE_LT);
    step_up(1u << 4);
    CHECK(lane_at(0, 1, LANE_SD, 4) == KIT_NOTE);
    CHECK(lane_at(0, 1, LANE_LT, 4) == KIT_OFF);
    CHECK(step_data_untouched());
    // An encoder press during a kit hold is navigation, not a flag edit.
    step_down(1u << 6);
    encoder_short(0);                       // the kit step editor on step 1
    CHECK(ui.mode == UI_KIT_STEP_EDIT && ui.edit_step == 0 && ui.kit_cursor == LANE_LT);
    step_up(1u << 6);                       // released on another screen: nothing
    CHECK(lane_at(0, 1, LANE_LT, 6) == KIT_OFF);
    CHECK(step_data_untouched());
}

static void test_pattern_view_switch_between_synth_and_kit_with_a_step_held(void) {
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    press_encoder(0, BTN_PRESS);
    CHECK(ui.mode == UI_PATTERN_VIEW && ui.selected_track == 1);
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));

    // A kit step held while TRACK walks onto track 3, a SYNTH: the press stays
    // bound to SD, and nothing holds the encoders on track 3.
    step_down(1u << 2);
    click(0, KIT_LANES - LANE_SD);          // SD..RC, then track 3
    CHECK(ui.selected_track == 2);
    CHECK(ui.grid_hold == 0 && ui.step_short_pending == 1u << 2);
    float bpm = g_seq.transport.bpm;
    click(1, 1);                            // tempo, not a velocity
    CHECK(bpm_is(bpm + 0.1f));
    click(0, 1);                            // TRACK still walks
    CHECK(ui.selected_track == 3);
    click(0, -1);
    CHECK(ui.selected_track == 2);
    step_up(1u << 2);
    CHECK(lane_at(0, 1, LANE_SD, 2) == KIT_NOTE);
    CHECK(step_data_untouched());
    CHECK(ui.step_short_pending == 0);

    // A SYNTH step held on track 3: TRACK edits its note and cannot leave it.
    step_down(1u << 5);
    click(0, -1);
    CHECK(ui.selected_track == 2);
    CHECK(STEP(2, 5).flags == STEP_FLAG_ACTIVE && STEP(2, 5).note == 59);
    step_up(1u << 5);
    CHECK(STEP(2, 5).flags == STEP_FLAG_ACTIVE);
    click(0, -1);                           // back onto the kit, on RC
    CHECK(ui.selected_track == 1 && session_kit_lane(1) == LANE_RC);
    CHECK(ui.grid_hold == 0 && ui.grid_tap_pending == 0);
    step_down(1u << 5);
    step_up(1u << 5);
    CHECK(lane_at(0, 1, LANE_RC, 5) == KIT_NOTE);
    CHECK(STEP(1, 5).flags == 0);

    // TRACK leaves the kit in the update a step goes down: the step joins the
    // hold on track 3 with no tap, as for any turn that shares an update with
    // a first press (the order is unknown).
    deltas[0] = 1;
    step_down(1u << 7);
    CHECK(ui.selected_track == 2);
    CHECK(ui.grid_hold == 1u << 7 && ui.step_short_pending == 0);
    step_up(1u << 7);
    CHECK(STEP(2, 7).flags == 0);
    CHECK(lane_at(0, 1, LANE_RC, 7) == KIT_OFF);
}

// A panic drops both kinds of step gesture at once: a kit press carried onto a
// SYNTH track and the hold on that track. Neither acts afterwards.
static void test_drop_step_gestures_forgets_kit_presses_and_holds(void) {
    make_kit(1);
    focus_lane(1, 0, LANE_SD);
    press_encoder(0, BTN_PRESS);
    memcpy(patterns_before, g_seq.patterns, sizeof(patterns_before));
    step_down(1u << 3);                     // bound to the kit's SD
    click(0, KIT_LANES - LANE_SD);          // SD..RC, then track 3
    CHECK(ui.selected_track == 2);
    step_down(1u << 9);                     // a hold on track 3
    CHECK(ui.grid_hold == 1u << 9 && ui.step_short_pending == 1u << 3);
    held[0] = held[1] = true;
    ui_update();
    CHECK(ui.grid_enc_target[0] == 1u << 9 && ui.grid_enc_target[1] == 1u << 9);
    press_encoder(0, BTN_LONG_PRESS);       // Panic
    CHECK(ui.mode == UI_SESSION);
    CHECK(ui.grid_hold == 0 && ui.grid_tap_pending == 0 && ui.step_short_pending == 0);
    CHECK(ui.grid_enc_target[0] == 0 && ui.grid_enc_target[1] == 0);
    CHECK(ui.grid_enc_long_done[0] == 0 && ui.grid_enc_long_done[1] == 0);
    held[0] = held[1] = false;
    ui_update();                            // unlatch
    step_up((1u << 3) | (1u << 9));
    CHECK(memcmp(patterns_before, g_seq.patterns, sizeof(patterns_before)) == 0);
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
    RUN(test_kit_editor_value_click_is_one_step);
    RUN(test_kit_editor_value_moves_by_detents);
    RUN(test_track_config_type_click_is_one_type);
    RUN(test_track_config_rows_move_by_clicks);
    RUN(test_slow_click_moves_tempo_and_patterns_by_one);
    RUN(test_slow_click_moves_kit_config_by_one);
    RUN(test_slow_click_moves_synth_and_drum_config_by_one);
    RUN(test_slow_click_moves_note_editor_and_song_by_one);
    RUN(test_fast_clicks_accelerate_numbers_only);
    RUN(test_fast_clicks_in_editors);
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
    RUN(test_grid_encoder2_short_during_hold_only_cancels_the_tap);
    RUN(test_grid_track_is_frozen_while_an_encoder_owns_the_gesture);
    RUN(test_grid_ignores_button_held_before_entering);
    RUN(test_grid_panic_forgets_held_steps);
    RUN(test_grid_hold_edit_reaches_playback);
    RUN(test_grid_hold_edit_while_playing_is_heard);
    RUN(test_grid_tap_within_one_update_toggles_once);
    RUN(test_grid_turn_and_release_in_one_update_is_an_edit);
    RUN(test_grid_turn_in_the_same_update_as_the_press_cancels_the_tap);
    RUN(test_grid_hold_turn_clamps_and_still_cancels_tap);
    RUN(test_grid_hold_creates_drum_hit_with_kit_note);
    RUN(test_held_step_line_is_rendered);
    RUN(test_held_step_line_shows_lowest_held_step);
    RUN(test_roll_one_note_is_a_block_in_a_centred_window);
    RUN(test_roll_window_holds_25_semitones_and_not_26);
    RUN(test_roll_notes_0_and_127_alone_sit_at_the_window_ends);
    RUN(test_roll_held_note_stays_in_the_window);
    RUN(test_roll_slide_runs_from_the_lower_to_the_higher_column);
    RUN(test_roll_slide_needs_a_next_step_that_is_on_and_inside_the_length);
    RUN(test_roll_slide_from_40_to_100_writes_nothing_outside_the_roll);
    RUN(test_roll_tie_is_one_pixel_with_or_without_accent);
    RUN(test_roll_steps_beyond_the_length_are_dotted_and_empty);
    RUN(test_roll_beat_lines_sit_in_row_y3_of_steps_4_8_12);
    RUN(test_roll_inverts_only_the_playing_step);
    RUN(test_roll_held_step_marks_column_23_and_fills_info_lines_2_to_5);
    RUN(test_roll_empty_clip_centres_the_window_on_c4);
    RUN(test_roll_info_column_lines_0_1_6_7);
    RUN(test_roll_info_text_stays_left_of_column_24);
    RUN(test_drum_cell_keeps_the_step_grid_picture);
    RUN(test_roll_held_step_through_the_ui);
    RUN(test_grid_long_press_applies_to_later_steps);
    RUN(test_grid_encoder_press_ending_as_a_step_goes_down_is_not_part_of_it);
    RUN(test_grid_tap_edits_selected_track_and_pattern);
    RUN(test_grid_driver_long_press_event_does_not_toggle);
    RUN(test_grid_hold_edit_refreshes_the_edited_track_and_pattern);
    RUN(test_step_edit_note_and_velocity_still_work);
    RUN(test_pattern_view_navigation_without_a_hold);
    RUN(test_grid_hold_suspends_every_screen_change);
    RUN(test_grid_panel_keys_during_a_hold_leave_the_tap);
    RUN(test_grid_step_that_vanishes_without_a_release_ends_the_hold);
    RUN(test_grid_step_held_without_a_press_event_never_joins);
    RUN(test_grid_hold_slow_click_moves_note_by_one);
    RUN(test_kit_hold_in_pattern_view_takes_no_encoder);
    RUN(test_pattern_view_switch_between_synth_and_kit_with_a_step_held);
    RUN(test_drop_step_gestures_forgets_kit_presses_and_holds);
    RUN(test_last_note_starts_at_c4_in_the_session_view);
    RUN(test_last_note_follows_a_turn_per_track);
    RUN(test_last_note_seeds_a_pattern_view_tap);
    RUN(test_last_note_seeds_the_step_edit_long_press);
    RUN(test_step_edit_turn_sets_the_last_note);
    RUN(test_last_note_two_held_empty_steps_turn_together);
    RUN(test_last_note_does_not_touch_a_step_that_has_a_note);
    RUN(test_last_note_comes_from_the_lowest_held_step);
    RUN(test_last_note_zero_is_a_note);
    RUN(test_last_note_joining_step_is_not_edited);
    RUN(test_last_note_ignores_drums_velocity_and_clamped_turns);
    RUN(test_last_note_drum_tap_takes_drum_note);
    RUN(test_copy_hold_then_press_copies_the_clip);
    RUN(test_copy_leaves_a_playing_or_queued_target_alone);
    RUN(test_copy_needs_the_full_hold_time);
    RUN(test_copy_two_presses_in_one_update_are_two_launches);
    RUN(test_copy_ignores_a_pad_of_another_track);
    RUN(test_copy_needs_the_panel_to_report_the_source_down);
    RUN(test_copy_ignores_a_hold_from_before_a_recovery);
    RUN(test_copy_ignores_a_pad_reported_down_without_a_press);
    RUN(test_copy_takes_the_longest_held_source);
    RUN(test_copy_source_stays_put_when_the_bank_changes);
    RUN(test_copy_in_song_mode_still_copies);
    RUN(test_copy_outside_the_session_view_keeps_the_focus);
    RUN(test_copy_leaves_a_held_step_on_its_cell);
    RUN(test_copy_over_the_playing_clip_is_heard);
    RUN(test_copy_takes_a_kit_clips_kit_steps);
    reset();
    render_views(argc > 1 ? argv[1] : NULL);
    puts("PASS the six base OLED screens render (the four kit screens: test_render_kit_*)");
    return 0;
}
