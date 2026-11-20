#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kit.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define RUN(test) do { test(); printf("PASS %s\n", #test); } while (0)

static void test_get_set_roundtrip(void) {
    for (uint8_t lane = 0; lane < KIT_LANES; lane++) {
        for (uint8_t v = 0; v <= 3; v++) {
            uint32_t w = kit_set(0, lane, v);
            CHECK(kit_get(w, lane) == (lane == 0 ? (v != 0) : v));
            for (uint8_t other = 0; other < KIT_LANES; other++) {
                if (other != lane) CHECK(kit_get(w, other) == 0);
            }
        }
    }
    CHECK(kit_set(0, 11, 3) == 0x00C00000u);
}

static void test_press_cycles(void) {
    uint32_t w = 0;
    w = kit_press(w, 2); CHECK(kit_get(w, 2) == 1);
    w = kit_press(w, 2); CHECK(kit_get(w, 2) == 2);
    w = kit_press(w, 2); CHECK(kit_get(w, 2) == 0);
    w = kit_set(0, 2, 3);
    w = kit_press(w, 2); CHECK(kit_get(w, 2) == 0);
    w = kit_press(0, KIT_LANE_AC); CHECK(kit_get(w, KIT_LANE_AC) == 1);
    w = kit_press(w, KIT_LANE_AC); CHECK(kit_get(w, KIT_LANE_AC) == 0);
}

static void test_ch_clears_oh(void) {
    uint32_t w = kit_set(0, KIT_LANE_OH, 1);
    w = kit_press(w, KIT_LANE_CH);
    CHECK(kit_get(w, KIT_LANE_CH) == 1);
    CHECK(kit_get(w, KIT_LANE_OH) == 0);

    w = kit_press(w, KIT_LANE_OH);
    CHECK(kit_get(w, KIT_LANE_OH) == 1);
    CHECK(kit_get(w, KIT_LANE_CH) == 0);

    w = kit_set(0, KIT_LANE_OH, 2);
    w = kit_edit(w, KIT_LANE_CH, 0);
    CHECK(kit_get(w, KIT_LANE_OH) == 2);
    w = kit_edit(w, KIT_LANE_CH, 3);
    CHECK(kit_get(w, KIT_LANE_CH) == 3);
    CHECK(kit_get(w, KIT_LANE_OH) == 0);
}

static void test_defaults_and_clamp(void) {
    static const uint8_t notes[KIT_VOICES] = {36, 38, 45, 47, 50, 37, 39, 42, 46, 49, 51};
    kit_config_t c;
    kit_config_defaults(&c);
    CHECK(memcmp(c.note, notes, sizeof(notes)) == 0);
    CHECK(c.vel_normal == 80 && c.vel_accent == 110 && c.vel_max == 127);
    CHECK(c.flam_pulses == 3);
    CHECK(kit_config_valid(&c));

    kit_config_t bad = c;
    bad.note[4] = 200;
    CHECK(!kit_config_valid(&bad));
    kit_config_clamp(&bad);
    CHECK(bad.note[4] == 127 && kit_config_valid(&bad));

    bad = c; bad.vel_normal = 0;
    CHECK(!kit_config_valid(&bad));
    kit_config_clamp(&bad);
    CHECK(bad.vel_normal == 1);

    bad = c; bad.flam_pulses = 0;
    CHECK(!kit_config_valid(&bad));
    kit_config_clamp(&bad);
    CHECK(bad.flam_pulses == 1);

    bad = c; bad.flam_pulses = 13;
    CHECK(!kit_config_valid(&bad));
    kit_config_clamp(&bad);
    CHECK(bad.flam_pulses == 12);
}

static void test_lane_names(void) {
    CHECK(strcmp(kit_lane_name(0), "AC") == 0);
    CHECK(strcmp(kit_lane_name(1), "BD") == 0);
    CHECK(strcmp(kit_lane_name(11), "RC") == 0);
    CHECK(strcmp(kit_lane_name(12), "??") == 0);
    static const uint8_t order[KIT_VOICES] = {0, 1, 6, 7, 8, 5, 2, 3, 4, 9, 10};
    CHECK(memcmp(kit_voice_order, order, sizeof(order)) == 0);
}

static void test_has_clip(void) {
    uint32_t steps[STEPS_PER_PATTERN] = {0};
    steps[3] = kit_set(0, KIT_LANE_AC, 1);
    CHECK(!kit_cell_has_clip(steps, STEPS_PER_PATTERN));
    steps[5] = kit_set(0, 2, 1);
    CHECK(!kit_cell_has_clip(steps, 5));
    CHECK(kit_cell_has_clip(steps, 6));
}

static void test_track_mask(void) {
    static pattern_t patterns[32];
    for (int i = 0; i < 32; i++) pattern_init(&patterns[i], (uint8_t)i);
    CHECK(kit_track_mask(patterns, 32) == 0);
    patterns[0].tracks[2].type = TRACK_TYPE_KIT;
    patterns[31].tracks[5].type = TRACK_TYPE_KIT;
    CHECK(kit_track_mask(patterns, 32) == 0x24);
    CHECK(kit_track_mask(patterns, 31) == 0x04);
}

static void test_cell_mask(void) {
    static pattern_t patterns[MAX_PATTERNS];
    for (int i = 0; i < MAX_PATTERNS; i++) pattern_init(&patterns[i], (uint8_t)i);
    patterns[3].tracks[1].type = TRACK_TYPE_KIT;
    patterns[0].tracks[6].type = TRACK_TYPE_KIT;
    CHECK(kit_is_cell(&patterns[3], 1));
    CHECK(!kit_is_cell(&patterns[0], 1));
    CHECK(!kit_is_cell(&patterns[3], NUM_TRACKS));
    uint8_t location[NUM_TRACKS] = {0};
    CHECK(kit_cell_mask(patterns, location) == 0x40);
    location[1] = 3;
    CHECK(kit_cell_mask(patterns, location) == 0x42);
    location[6] = 3;                                    // each track's own location
    CHECK(kit_cell_mask(patterns, location) == 0x02);
    location[1] = MAX_PATTERNS;                         // out of range: no kit
    CHECK(kit_cell_mask(patterns, location) == 0);
}

static void test_glyphs(void) {
    uint32_t steps[STEPS_PER_PATTERN] = {0};
    steps[0] = kit_set(0, 2, 1);
    steps[1] = kit_set(0, 2, 2);
    steps[2] = kit_set(0, 2, 3);
    char out[STEPS_PER_PATTERN + 1];
    kit_lane_glyphs(steps, 2, 6, out);
    CHECK(strlen(out) == 16);
    CHECK(memcmp(out, "oOf...          ", 17) == 0);
    CHECK(kit_value_glyph(KIT_OFF) == '.' && kit_value_glyph(KIT_NOTE) == 'o');
    CHECK(kit_value_glyph(KIT_ACCENT) == 'O' && kit_value_glyph(KIT_FLAM) == 'f');
}

static void test_pattern_helpers_move_kit_steps(void) {
    static pattern_t p;
    pattern_init(&p, 0);
    for (int t = 0; t < KIT_TRACK_SLOTS; t++) {
        p.kit_steps[t][0] = 0x10u + t;
        p.kit_steps[t][15] = 0x20u + t;
    }
    pattern_shift_track(&p, 1, +1);
    CHECK(p.kit_steps[1][0] == 0x21u && p.kit_steps[1][1] == 0x11u && p.kit_steps[1][15] == 0);
    CHECK(p.kit_steps[0][0] == 0x10u);

    pattern_init(&p, 0);
    for (int t = 0; t < KIT_TRACK_SLOTS; t++) p.kit_steps[t][0] = 0x10u + t;
    pattern_shift(&p, -1);
    for (int t = 0; t < KIT_TRACK_SLOTS; t++) {
        CHECK(p.kit_steps[t][15] == 0x10u + t);
        CHECK(p.kit_steps[t][0] == 0);
    }

    for (int t = 0; t < KIT_TRACK_SLOTS; t++) p.kit_steps[t][4] = 7;
    pattern_clear_step(&p, 2, 4);
    CHECK(p.kit_steps[2][4] == 0 && p.kit_steps[3][4] == 7);
    pattern_clear_track(&p, 3);
    CHECK(p.kit_steps[3][4] == 0 && p.kit_steps[3][15] == 0 && p.kit_steps[4][4] == 7);
    pattern_clear(&p);
    for (int t = 0; t < KIT_TRACK_SLOTS; t++)
        for (int s = 0; s < STEPS_PER_PATTERN; s++) CHECK(p.kit_steps[t][s] == 0);

    memset(p.kit_steps, 0xFF, sizeof(p.kit_steps));
    pattern_init(&p, 0);
    for (int t = 0; t < KIT_TRACK_SLOTS; t++)
        for (int s = 0; s < STEPS_PER_PATTERN; s++) CHECK(p.kit_steps[t][s] == 0);
}

int main(void) {
    RUN(test_get_set_roundtrip);
    RUN(test_press_cycles);
    RUN(test_ch_clears_oh);
    RUN(test_defaults_and_clamp);
    RUN(test_lane_names);
    RUN(test_has_clip);
    RUN(test_track_mask);
    RUN(test_cell_mask);
    RUN(test_glyphs);
    RUN(test_pattern_helpers_move_kit_steps);
    return 0;
}
