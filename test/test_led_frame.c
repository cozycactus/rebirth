#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "led_frame.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define RUN(name) do { name(); printf("PASS %s\n", #name); } while (0)

static const rgb_t HUE[NUM_TRACKS] = {
    {255, 0, 0}, {255, 80, 0}, {255, 200, 0}, {0, 255, 40},
    {0, 220, 255}, {0, 60, 255}, {140, 0, 255}, {255, 0, 120},
};

// The frame builder rounds every non-zero channel up.
static rgb_t scaled(rgb_t hue, int level) {
    rgb_t c = {(uint8_t)((hue.r * level + 254) / 255), (uint8_t)((hue.g * level + 254) / 255),
               (uint8_t)((hue.b * level + 254) / 255)};
    return c;
}
static rgb_t white(int level) { return scaled((rgb_t){255, 255, 255}, level); }
static rgb_t green(int level) { return scaled((rgb_t){0, 255, 0}, level); }
static const rgb_t BLACK = {0, 0, 0};

static bool same(rgb_t a, rgb_t b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static led_state_t base_state(void) {
    led_state_t s;
    memset(&s, 0, sizeof(s));
    memset(s.queued, NO_QUEUED_PATTERN, sizeof(s.queued));
    return s;
}

static void set_clip(led_state_t *s, int track, int row) {
    s->clip_mask |= 1ull << (track * 8 + row);
}

static rgb_t light(const led_state_t *s, int index) {
    rgb_t frame[LED_COUNT];
    led_frame_build(s, frame);
    return frame[index];
}

static void test_empty_pad_is_off(void) {
    led_state_t s = base_state();
    rgb_t frame[LED_COUNT];
    led_frame_build(&s, frame);
    CHECK(same(frame[LED_PAD(2, 5)], BLACK));
    CHECK(same(frame[LED_PAD(7, 7)], BLACK));
}

static void test_stored_clip_is_dim_hue(void) {
    led_state_t s = base_state();
    set_clip(&s, 3, 4);
    s.current[3] = 1;
    CHECK(same(light(&s, LED_PAD(3, 4)), scaled(HUE[3], LED_LEVEL_DIM)));
    s.bank = 2;                              // same mask bit, other bank: still a stored clip
    CHECK(same(light(&s, LED_PAD(3, 4)), scaled(HUE[3], LED_LEVEL_DIM)));
}

static void test_playing_clip_is_full_hue(void) {
    led_state_t s = base_state();
    s.playing = true;
    set_clip(&s, 5, 2);
    s.current[5] = 2;
    CHECK(same(light(&s, LED_PAD(5, 2)), scaled(HUE[5], LED_LEVEL_FULL)));
    s.bank = 1;                              // location 10 in bank 1, row 2
    s.current[5] = 10;
    CHECK(same(light(&s, LED_PAD(5, 2)), scaled(HUE[5], LED_LEVEL_FULL)));
}

static void test_stopped_current_clip_is_half_hue(void) {
    led_state_t s = base_state();
    set_clip(&s, 6, 3);
    s.current[6] = 3;
    CHECK(same(light(&s, LED_PAD(6, 3)), scaled(HUE[6], LED_LEVEL_HALF)));
}

static void test_queued_clip_blinks_on_eighths(void) {
    led_state_t s = base_state();
    set_clip(&s, 1, 6);
    s.queued[1] = 6;
    s.pulse = 0;
    CHECK(same(light(&s, LED_PAD(1, 6)), scaled(HUE[1], LED_LEVEL_FULL)));
    s.pulse = 47;
    CHECK(same(light(&s, LED_PAD(1, 6)), scaled(HUE[1], LED_LEVEL_FULL)));
    s.pulse = 48;
    CHECK(same(light(&s, LED_PAD(1, 6)), BLACK));
    s.pulse = 96;
    CHECK(same(light(&s, LED_PAD(1, 6)), scaled(HUE[1], LED_LEVEL_FULL)));
}

static void test_queued_beats_stored(void) {
    led_state_t s = base_state();
    s.playing = true;
    set_clip(&s, 4, 0);
    s.queued[4] = 0;
    for (uint32_t pulse = 0; pulse < 192; pulse += 6) {
        s.pulse = pulse;
        rgb_t c = light(&s, LED_PAD(4, 0));
        CHECK(same(c, scaled(HUE[4], LED_LEVEL_FULL)) || same(c, BLACK));
    }
}

static void test_queued_empty_blinks_dim_white(void) {
    led_state_t s = base_state();
    s.queued[2] = 7;
    s.pulse = 0;
    CHECK(same(light(&s, LED_PAD(2, 7)), white(LED_LEVEL_DIM)));
    s.pulse = 48;
    CHECK(same(light(&s, LED_PAD(2, 7)), BLACK));
}

static void test_current_empty_is_dim_white_while_playing(void) {
    led_state_t s = base_state();
    s.current[0] = 3;
    CHECK(same(light(&s, LED_PAD(0, 3)), BLACK));        // stopped: off
    s.playing = true;
    CHECK(same(light(&s, LED_PAD(0, 3)), white(LED_LEVEL_DIM)));
}

static void test_scene_rules(void) {
    led_state_t s = base_state();
    s.playing = true;
    CHECK(same(light(&s, LED_SCENE(3)), BLACK));          // empty row

    set_clip(&s, 5, 3);
    CHECK(same(light(&s, LED_SCENE(3)), white(LED_LEVEL_DIM)));   // has a clip

    for (int t = 0; t < NUM_TRACKS; t++) s.current[t] = 3;
    CHECK(same(light(&s, LED_SCENE(3)), green(LED_LEVEL_FULL)));  // all play the row
    s.current[7] = 0;
    CHECK(same(light(&s, LED_SCENE(3)), green(LED_LEVEL_DIM)));   // some do

    s.playing = false;                                            // stopped: not "plays"
    CHECK(same(light(&s, LED_SCENE(3)), white(LED_LEVEL_DIM)));
    for (int t = 0; t < NUM_TRACKS; t++) s.current[t] = 3;
    CHECK(same(light(&s, LED_SCENE(3)), white(LED_LEVEL_DIM)));   // all on the row, has clip
    CHECK(same(light(&s, LED_SCENE(0)), BLACK));
    s.current[0] = 0;
    CHECK(same(light(&s, LED_SCENE(3)), white(LED_LEVEL_DIM)));
    led_state_t empty = base_state();
    for (int t = 0; t < NUM_TRACKS; t++) empty.current[t] = 3;
    CHECK(same(light(&empty, LED_SCENE(3)), BLACK));              // empty row, stopped

    s.playing = true;                                             // pending beats the rest
    for (int t = 0; t < NUM_TRACKS; t++) s.current[t] = 3;
    s.queued[2] = 3;
    s.pulse = 0;
    CHECK(same(light(&s, LED_SCENE(3)), green(LED_LEVEL_FULL)));
    s.pulse = 48;
    CHECK(same(light(&s, LED_SCENE(3)), BLACK));

    s.queued[6] = 5;                                              // two rows pending
    s.pulse = 0;
    CHECK(same(light(&s, LED_SCENE(3)), green(LED_LEVEL_FULL)));
    CHECK(same(light(&s, LED_SCENE(5)), green(LED_LEVEL_FULL)));
    s.pulse = 48;
    CHECK(same(light(&s, LED_SCENE(3)), BLACK));
    CHECK(same(light(&s, LED_SCENE(5)), BLACK));
}

static void test_mute_rules(void) {
    for (int focus = 0; focus < NUM_TRACKS; focus++) {
        led_state_t s = base_state();
        s.focus_track = (uint8_t)focus;
        s.muted = 0x40;                                           // track 6 muted
        for (int t = 0; t < NUM_TRACKS; t++) {
            rgb_t want;
            if (t == focus) want = (t == 6) ? white(LED_LEVEL_DIM) : scaled(HUE[t], LED_LEVEL_FULL);
            else            want = (t == 6) ? BLACK : scaled(HUE[t], LED_LEVEL_HALF);
            CHECK(same(light(&s, LED_MUTE(t)), want));
        }
    }
    led_state_t s = base_state();                                 // plays another bank: slow blink
    s.current[2] = 9;
    s.pulse = 0;
    CHECK(same(light(&s, LED_MUTE(2)), scaled(HUE[2], LED_LEVEL_HALF)));
    s.pulse = 191;
    CHECK(same(light(&s, LED_MUTE(2)), scaled(HUE[2], LED_LEVEL_HALF)));
    s.pulse = 192;
    CHECK(same(light(&s, LED_MUTE(2)), BLACK));
    s.pulse = 384;
    CHECK(same(light(&s, LED_MUTE(2)), scaled(HUE[2], LED_LEVEL_HALF)));
    s.focus_track = 2;
    s.pulse = 0;
    CHECK(same(light(&s, LED_MUTE(2)), scaled(HUE[2], LED_LEVEL_FULL)));
}

static void test_queue_in_other_bank_blinks_mute_fast(void) {
    led_state_t s = base_state();
    s.queued[4] = 17;                                             // bank 2, not shown
    s.pulse = 0;
    CHECK(same(light(&s, LED_MUTE(4)), scaled(HUE[4], LED_LEVEL_HALF)));
    s.pulse = 48;
    CHECK(same(light(&s, LED_MUTE(4)), BLACK));
    s.current[4] = 9;                                             // also plays elsewhere: fast wins
    s.pulse = 192;                                                // slow blink off, fast blink on
    CHECK(same(light(&s, LED_MUTE(4)), scaled(HUE[4], LED_LEVEL_HALF)));
    s.pulse = 48;
    CHECK(same(light(&s, LED_MUTE(4)), BLACK));
    s.queued[4] = 3;                                              // queue inside the shown bank
    s.current[4] = 0;
    s.pulse = 48;
    CHECK(same(light(&s, LED_MUTE(4)), scaled(HUE[4], LED_LEVEL_HALF)));
    s.queued[4] = 17;                                             // a muted track still shows it
    s.muted = 1 << 4;
    s.pulse = 0;
    CHECK(same(light(&s, LED_MUTE(4)), white(LED_LEVEL_DIM)));
}

static void test_play_and_beat(void) {
    led_state_t s = base_state();
    CHECK(same(light(&s, LED_PLAY), BLACK));
    CHECK(same(light(&s, LED_BEAT), BLACK));
    s.pulse = 0;
    CHECK(same(light(&s, LED_BEAT), BLACK));                      // stopped: no beat
    s.playing = true;
    CHECK(same(light(&s, LED_PLAY), green(LED_LEVEL_FULL)));
    CHECK(same(light(&s, LED_BEAT), white(LED_LEVEL_FULL)));      // bar line
    s.pulse = 11;
    CHECK(same(light(&s, LED_BEAT), white(LED_LEVEL_FULL)));
    s.pulse = 12;
    CHECK(same(light(&s, LED_BEAT), BLACK));
    s.pulse = 96;
    CHECK(same(light(&s, LED_BEAT), white(LED_LEVEL_HALF)));      // beat 2
    s.pulse = 108;
    CHECK(same(light(&s, LED_BEAT), BLACK));
    s.pulse = 384;
    CHECK(same(light(&s, LED_BEAT), white(LED_LEVEL_FULL)));
}

static bool is_black(rgb_t c) { return same(c, BLACK); }

// Pads of all eight tracks at one level: a stored clip (dim), the current clip
// while stopped (half) or while playing (full).
static void track_pads_at(int level_index, rgb_t out[NUM_TRACKS]) {
    led_state_t s = base_state();
    for (int t = 0; t < NUM_TRACKS; t++) {
        set_clip(&s, t, 0);
        s.current[t] = level_index == 0 ? 1 : 0;
    }
    s.playing = level_index == 2;
    for (int t = 0; t < NUM_TRACKS; t++) out[t] = light(&s, LED_PAD(t, 0));
}

static void test_track_hues_stay_distinct_at_every_level(void) {
    for (int level = 0; level < 3; level++) {
        rgb_t pads[NUM_TRACKS];
        track_pads_at(level, pads);
        for (int a = 0; a < NUM_TRACKS; a++) {
            CHECK(!is_black(pads[a]));
            for (int b = a + 1; b < NUM_TRACKS; b++) CHECK(!same(pads[a], pads[b]));
        }
    }
    // Dim track green (a stored clip of track 3) is not dim scene green (a row
    // some tracks play).
    led_state_t s = base_state();
    s.playing = true;
    set_clip(&s, 3, 2);
    s.current[0] = 5;
    CHECK(!same(light(&s, LED_PAD(3, 2)), light(&s, LED_SCENE(5))));
    CHECK(same(light(&s, LED_SCENE(5)), green(LED_LEVEL_DIM)));
}

static led_state_t busiest_state(void) {
    led_state_t s = base_state();
    s.playing = true;
    s.clip_mask = ~0ull;
    s.focus_track = 0;
    s.pulse = 0;
    return s;
}

static void test_busiest_designed_frame_fits_budget(void) {
    led_state_t s = busiest_state();
    rgb_t frame[LED_COUNT], copy[LED_COUNT];
    led_frame_build(&s, frame);
    memcpy(copy, frame, sizeof(frame));
    uint32_t ma = led_frame_estimate_ma(frame);
    CHECK(ma <= LED_RGB_BUDGET_MA);
    led_frame_apply_budget(frame, LED_RGB_BUDGET_MA);
    CHECK(memcmp(frame, copy, sizeof(frame)) == 0);
}

static void test_budget_scales_only_dynamic_current(void) {
    rgb_t frame[LED_COUNT], copy[LED_COUNT];
    memset(frame, 255, sizeof(frame));
    uint32_t idle_ma = LED_COUNT * LED_IDLE_UA / 1000;
    CHECK(led_frame_estimate_ma(frame) > LED_RGB_BUDGET_MA);
    led_frame_apply_budget(frame, LED_RGB_BUDGET_MA);
    uint32_t ma = led_frame_estimate_ma(frame);
    CHECK(ma <= LED_RGB_BUDGET_MA);
    CHECK(ma > idle_ma);
    CHECK(ma >= LED_RGB_BUDGET_MA - 2);                           // uses the budget, not far below it

    rgb_t dim[LED_COUNT];
    memset(dim, 0, sizeof(dim));
    dim[0].r = 10;
    memcpy(copy, dim, sizeof(dim));
    led_frame_apply_budget(dim, LED_RGB_BUDGET_MA);               // under budget: unchanged
    CHECK(memcmp(dim, copy, sizeof(dim)) == 0);

    rgb_t black[LED_COUNT];
    memset(black, 0, sizeof(black));
    CHECK(led_frame_estimate_ma(black) == LED_COUNT * LED_IDLE_UA / 1000);
    led_frame_apply_budget(black, LED_RGB_BUDGET_MA);             // all black: unchanged
    for (int i = 0; i < LED_COUNT; i++) CHECK(same(black[i], BLACK));
}

int main(void) {
    RUN(test_empty_pad_is_off);
    RUN(test_stored_clip_is_dim_hue);
    RUN(test_playing_clip_is_full_hue);
    RUN(test_stopped_current_clip_is_half_hue);
    RUN(test_queued_clip_blinks_on_eighths);
    RUN(test_queued_beats_stored);
    RUN(test_queued_empty_blinks_dim_white);
    RUN(test_current_empty_is_dim_white_while_playing);
    RUN(test_scene_rules);
    RUN(test_mute_rules);
    RUN(test_queue_in_other_bank_blinks_mute_fast);
    RUN(test_play_and_beat);
    RUN(test_busiest_designed_frame_fits_budget);
    RUN(test_budget_scales_only_dynamic_current);
    RUN(test_track_hues_stay_distinct_at_every_level);
    return 0;
}
