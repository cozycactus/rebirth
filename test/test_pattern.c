#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "sequencer.h"

/* Minimal global required by sequencer.h */
sequencer_state_t g_seq;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do {                              \
    tests_run++;                                    \
    printf("  %-50s", #name);                       \
    name();                                         \
    tests_passed++;                                 \
    printf("PASS\n");                               \
} while (0)

#define ASSERT_EQ(a, b) do {                         \
    if ((a) != (b)) {                                \
        printf("FAIL\n    %s:%d: %d != %d\n",       \
               __FILE__, __LINE__, (int)(a), (int)(b)); \
        exit(1);                                     \
    }                                                \
} while (0)

#define ASSERT_STR_EQ(a, b) do {                     \
    if (strcmp((a), (b)) != 0) {                     \
        printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n",\
               __FILE__, __LINE__, (a), (b));        \
        exit(1);                                     \
    }                                                \
} while (0)

#define ASSERT_TRUE(x)  ASSERT_EQ(!!(x), 1)
#define ASSERT_FALSE(x) ASSERT_EQ(!!(x), 0)

/* ── pattern_init tests ─────────────────────────────────── */

TEST(test_pattern_init_name) {
    pattern_t p;
    pattern_init(&p, 0);
    ASSERT_STR_EQ(p.name, "PA1");

    pattern_init(&p, 7);
    ASSERT_STR_EQ(p.name, "PA8");

    pattern_init(&p, 31);
    ASSERT_STR_EQ(p.name, "PD8");
}

TEST(test_pattern_init_tracks) {
    pattern_t p;
    pattern_init(&p, 0);

    /* First 4 tracks are synth, rest are drum */
    for (int t = 0; t < 4; t++) {
        ASSERT_EQ(p.tracks[t].type, TRACK_TYPE_SYNTH);
    }
    for (int t = 4; t < NUM_TRACKS; t++) {
        ASSERT_EQ(p.tracks[t].type, TRACK_TYPE_DRUM);
    }

    /* MIDI channels match track index */
    for (int t = 0; t < NUM_TRACKS; t++) {
        ASSERT_EQ(p.tracks[t].midi_channel, t);
    }

    /* Drum notes increment from 36 */
    for (int t = 0; t < NUM_TRACKS; t++) {
        ASSERT_EQ(p.tracks[t].drum_note, 36 + t);
    }

    /* Gate length defaults to 50% */
    for (int t = 0; t < NUM_TRACKS; t++) {
        ASSERT_EQ(p.tracks[t].gate_length_pct, 50);
    }

    /* No tracks muted */
    for (int t = 0; t < NUM_TRACKS; t++) {
        ASSERT_FALSE(p.tracks[t].muted);
    }

    /* Swing defaults to 50 (straight) */
    ASSERT_EQ(p.swing, 50);
    ASSERT_EQ(p.length, STEPS_PER_PATTERN);
    for (int t = 0; t < NUM_TRACKS; t++) {
        ASSERT_EQ(pattern_get_track_length(&p, t), STEPS_PER_PATTERN);
    }
}

TEST(test_pattern_init_steps_zeroed) {
    pattern_t p;
    /* Fill with garbage first */
    memset(&p, 0xFF, sizeof(p));
    pattern_init(&p, 0);

    for (int t = 0; t < NUM_TRACKS; t++) {
        for (int s = 0; s < STEPS_PER_PATTERN; s++) {
            ASSERT_EQ(p.steps[t][s].note, 0);
            ASSERT_EQ(p.steps[t][s].velocity, 0);
            ASSERT_EQ(p.steps[t][s].flags, 0);
        }
    }
}

TEST(test_pattern_init_cv_output_mode) {
    pattern_t p;
    pattern_init(&p, 0);

    /* First NUM_CV_CHANNELS tracks output both */
    for (int t = 0; t < NUM_CV_CHANNELS; t++) {
        ASSERT_EQ(p.tracks[t].output_mode, OUTPUT_BOTH);
        ASSERT_EQ(p.tracks[t].cv_channel, t);
    }
    /* Remaining tracks MIDI only */
    for (int t = NUM_CV_CHANNELS; t < NUM_TRACKS; t++) {
        ASSERT_EQ(p.tracks[t].output_mode, OUTPUT_MIDI_ONLY);
    }
}

/* ── pattern_set_step tests ─────────────────────────────── */

TEST(test_set_step_basic) {
    pattern_t p;
    pattern_init(&p, 0);

    pattern_set_step(&p, 0, 0, 60, 100, STEP_FLAG_ACTIVE);
    ASSERT_EQ(p.steps[0][0].note, 60);
    ASSERT_EQ(p.steps[0][0].velocity, 100);
    ASSERT_EQ(p.steps[0][0].flags, STEP_FLAG_ACTIVE);
}

TEST(test_set_step_with_flags) {
    pattern_t p;
    pattern_init(&p, 0);

    uint8_t flags = STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT | STEP_FLAG_SLIDE;
    pattern_set_step(&p, 2, 15, 72, 127, flags);
    ASSERT_EQ(p.steps[2][15].note, 72);
    ASSERT_EQ(p.steps[2][15].velocity, 127);
    ASSERT_EQ(p.steps[2][15].flags, flags);
}

TEST(test_set_step_overwrite) {
    pattern_t p;
    pattern_init(&p, 0);

    pattern_set_step(&p, 1, 5, 60, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(&p, 1, 5, 72, 50, STEP_FLAG_ACTIVE | STEP_FLAG_TIE);
    ASSERT_EQ(p.steps[1][5].note, 72);
    ASSERT_EQ(p.steps[1][5].velocity, 50);
    ASSERT_TRUE(p.steps[1][5].flags & STEP_FLAG_TIE);
}

TEST(test_set_step_bounds_check) {
    pattern_t p;
    pattern_init(&p, 0);

    /* Out-of-bounds track — should be a no-op */
    pattern_set_step(&p, NUM_TRACKS, 0, 60, 100, STEP_FLAG_ACTIVE);
    /* Out-of-bounds step */
    pattern_set_step(&p, 0, STEPS_PER_PATTERN, 60, 100, STEP_FLAG_ACTIVE);

    /* Verify nothing changed (all steps still zero) */
    for (int t = 0; t < NUM_TRACKS; t++) {
        for (int s = 0; s < STEPS_PER_PATTERN; s++) {
            ASSERT_EQ(p.steps[t][s].flags, 0);
        }
    }
}

/* ── pattern_clear_step tests ───────────────────────────── */

TEST(test_clear_step) {
    pattern_t p;
    pattern_init(&p, 0);

    pattern_set_step(&p, 0, 3, 60, 100, STEP_FLAG_ACTIVE);
    ASSERT_TRUE(p.steps[0][3].flags & STEP_FLAG_ACTIVE);

    pattern_clear_step(&p, 0, 3);
    ASSERT_EQ(p.steps[0][3].note, 0);
    ASSERT_EQ(p.steps[0][3].velocity, 0);
    ASSERT_EQ(p.steps[0][3].flags, 0);
}

TEST(test_clear_step_bounds) {
    pattern_t p;
    pattern_init(&p, 0);
    pattern_set_step(&p, 0, 0, 60, 100, STEP_FLAG_ACTIVE);

    /* Out-of-bounds clear should be no-op */
    pattern_clear_step(&p, NUM_TRACKS, 0);
    pattern_clear_step(&p, 0, STEPS_PER_PATTERN);

    /* Original step untouched */
    ASSERT_EQ(p.steps[0][0].note, 60);
}

/* ── pattern_toggle_step tests ──────────────────────────── */

TEST(test_toggle_step_activate_drum) {
    pattern_t p;
    pattern_init(&p, 0);

    /* Track 4 is drum (type TRACK_TYPE_DRUM, drum_note=40) */
    pattern_toggle_step(&p, 4, 0);
    ASSERT_TRUE(p.steps[4][0].flags & STEP_FLAG_ACTIVE);
    ASSERT_EQ(p.steps[4][0].velocity, 100);
    ASSERT_EQ(p.steps[4][0].note, p.tracks[4].drum_note);
}

TEST(test_toggle_step_activate_synth) {
    pattern_t p;
    pattern_init(&p, 0);

    /* Track 0 is synth — should get note=60 (middle C) */
    pattern_toggle_step(&p, 0, 0);
    ASSERT_TRUE(p.steps[0][0].flags & STEP_FLAG_ACTIVE);
    ASSERT_EQ(p.steps[0][0].note, 60);
    ASSERT_EQ(p.steps[0][0].velocity, 100);
}

TEST(test_toggle_step_deactivate) {
    pattern_t p;
    pattern_init(&p, 0);

    pattern_toggle_step(&p, 0, 0);
    ASSERT_TRUE(p.steps[0][0].flags & STEP_FLAG_ACTIVE);

    pattern_toggle_step(&p, 0, 0);
    ASSERT_FALSE(p.steps[0][0].flags & STEP_FLAG_ACTIVE);
}

TEST(test_toggle_step_preserves_note_on_deactivate) {
    pattern_t p;
    pattern_init(&p, 0);

    pattern_set_step(&p, 0, 0, 72, 110, STEP_FLAG_ACTIVE);
    pattern_toggle_step(&p, 0, 0);  /* deactivate */
    ASSERT_FALSE(p.steps[0][0].flags & STEP_FLAG_ACTIVE);
    /* Note and velocity should remain */
    ASSERT_EQ(p.steps[0][0].note, 72);
    ASSERT_EQ(p.steps[0][0].velocity, 110);
}

TEST(test_toggle_step_activate_with_existing_velocity) {
    pattern_t p;
    pattern_init(&p, 0);

    /* Pre-set velocity, then toggle on — should keep existing velocity */
    p.steps[0][0].velocity = 80;
    pattern_toggle_step(&p, 0, 0);
    ASSERT_EQ(p.steps[0][0].velocity, 80);
}

TEST(test_toggle_step_bounds) {
    pattern_t p;
    pattern_init(&p, 0);

    /* Out-of-bounds toggle should be no-op */
    pattern_toggle_step(&p, NUM_TRACKS, 0);
    pattern_toggle_step(&p, 0, STEPS_PER_PATTERN);

    for (int t = 0; t < NUM_TRACKS; t++) {
        for (int s = 0; s < STEPS_PER_PATTERN; s++) {
            ASSERT_EQ(p.steps[t][s].flags, 0);
        }
    }
}

/* ── pattern_copy tests ─────────────────────────────────── */

TEST(test_pattern_copy) {
    pattern_t src, dst;
    pattern_init(&src, 5);
    pattern_init(&dst, 0);

    pattern_set_step(&src, 0, 0, 60, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(&src, 3, 7, 48, 80, STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT);
    src.tracks[1].muted = true;
    src.length = 7;
    pattern_set_track_length(&src, 3, 11);
    src.swing = 65;

    pattern_copy(&dst, &src);

    ASSERT_EQ(dst.steps[0][0].note, 60);
    ASSERT_EQ(dst.steps[3][7].note, 48);
    ASSERT_EQ(dst.steps[3][7].flags, STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT);
    ASSERT_TRUE(dst.tracks[1].muted);
    ASSERT_EQ(dst.length, 7);
    ASSERT_EQ(pattern_get_track_length(&dst, 3), 11);
    ASSERT_EQ(dst.swing, 65);
    ASSERT_STR_EQ(dst.name, "PA6");
}

TEST(test_pattern_copy_independence) {
    pattern_t src, dst;
    pattern_init(&src, 1);
    pattern_set_step(&src, 0, 0, 60, 100, STEP_FLAG_ACTIVE);

    pattern_copy(&dst, &src);

    /* Modify source — destination should be unaffected */
    pattern_set_step(&src, 0, 0, 72, 127, STEP_FLAG_ACTIVE);
    ASSERT_EQ(dst.steps[0][0].note, 60);
    ASSERT_EQ(dst.steps[0][0].velocity, 100);
}

/* ── Pattern length / edit helpers ─────────────────────── */

TEST(test_track_length_per_track) {
    pattern_t p;
    pattern_init(&p, 0);

    pattern_set_track_length(&p, 0, 7);
    pattern_set_track_length(&p, 1, 13);

    ASSERT_EQ(pattern_get_track_length(&p, 0), 7);
    ASSERT_EQ(pattern_get_track_length(&p, 1), 13);
    ASSERT_EQ(pattern_get_track_length(&p, 2), STEPS_PER_PATTERN);
}

TEST(test_track_length_clamps) {
    pattern_t p;
    pattern_init(&p, 0);

    pattern_set_track_length(&p, 0, 0);
    ASSERT_EQ(pattern_get_track_length(&p, 0), 1);

    pattern_set_track_length(&p, 0, 99);
    ASSERT_EQ(pattern_get_track_length(&p, 0), STEPS_PER_PATTERN);
}

TEST(test_track_length_does_not_change_other_tracks_fallback) {
    pattern_t p;
    pattern_init(&p, 0);
    p.length = 12;
    p.track_length[1] = 0;
    pattern_set_track_length(&p, 0, 5);
    ASSERT_EQ(pattern_get_track_length(&p, 0), 5);
    ASSERT_EQ(pattern_get_track_length(&p, 1), 12);
}

TEST(test_pattern_clear_track) {
    pattern_t p;
    pattern_init(&p, 0);

    pattern_set_step(&p, 0, 0, 60, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(&p, 1, 0, 62, 100, STEP_FLAG_ACTIVE);
    pattern_clear_track(&p, 0);

    ASSERT_EQ(p.steps[0][0].flags, 0);
    ASSERT_EQ(p.steps[1][0].flags, STEP_FLAG_ACTIVE);
}

TEST(test_pattern_shift_track_right_wraps_16_steps) {
    pattern_t p;
    pattern_init(&p, 0);
    pattern_set_track_length(&p, 0, 7);
    pattern_set_step(&p, 0, 15, 72, 90, STEP_FLAG_ACTIVE);

    pattern_shift_track(&p, 0, 1);

    ASSERT_EQ(p.steps[0][0].note, 72);
    ASSERT_EQ(p.steps[0][0].flags, STEP_FLAG_ACTIVE);
    ASSERT_EQ(p.steps[0][15].flags, 0);
}

TEST(test_pattern_shift_track_left_wraps_16_steps) {
    pattern_t p;
    pattern_init(&p, 0);
    pattern_set_track_length(&p, 0, 7);
    pattern_set_step(&p, 0, 0, 72, 90, STEP_FLAG_ACTIVE);

    pattern_shift_track(&p, 0, -1);

    ASSERT_EQ(p.steps[0][15].note, 72);
    ASSERT_EQ(p.steps[0][15].flags, STEP_FLAG_ACTIVE);
    ASSERT_EQ(p.steps[0][0].flags, 0);
}

TEST(test_pattern_copy_track_copies_effective_length) {
    pattern_t src, dst;
    pattern_init(&src, 0);
    pattern_init(&dst, 1);
    src.length = 8;
    src.track_length[2] = 0;
    pattern_set_step(&src, 2, 3, 70, 90, STEP_FLAG_ACTIVE | STEP_FLAG_SLIDE);
    src.kit_steps[2][5] = 0x1234u;
    ASSERT_EQ(dst.length, 16);

    pattern_copy_track(&dst, &src, 2);

    ASSERT_EQ(dst.track_length[2], 8);
    ASSERT_EQ(pattern_get_track_length(&dst, 2), pattern_get_track_length(&src, 2));
    ASSERT_TRUE(memcmp(dst.steps[2], src.steps[2], sizeof(dst.steps[2])) == 0);
    ASSERT_TRUE(memcmp(dst.kit_steps[2], src.kit_steps[2], sizeof(dst.kit_steps[2])) == 0);
    ASSERT_EQ(dst.length, 16);
}

TEST(test_pattern_copy_track_leaves_everything_else) {
    pattern_t src, dst, before;
    pattern_init(&src, 0);
    pattern_init(&dst, 1);
    src.length = 4;
    src.track_length[1] = 0;
    src.swing = 40;
    src.tracks[1].type = TRACK_TYPE_KIT;
    src.tracks[1].muted = true;
    pattern_set_step(&src, 1, 0, 65, 80, STEP_FLAG_ACTIVE);
    pattern_set_step(&dst, 0, 0, 50, 60, STEP_FLAG_ACTIVE);
    pattern_set_step(&dst, 3, 4, 52, 60, STEP_FLAG_ACTIVE);
    dst.swing = 10;
    dst.track_length[3] = 5;
    before = dst;

    pattern_copy_track(&dst, &src, 1);

    ASSERT_TRUE(memcmp(dst.steps[0], before.steps[0], sizeof(dst.steps[0])) == 0);
    ASSERT_TRUE(memcmp(dst.steps[3], before.steps[3], sizeof(dst.steps[3])) == 0);
    for (int t = 0; t < NUM_TRACKS; t++) {
        ASSERT_TRUE(memcmp(&dst.tracks[t], &before.tracks[t], sizeof(dst.tracks[t])) == 0);
        if (t != 1) {
            ASSERT_EQ(dst.track_length[t], before.track_length[t]);
            ASSERT_TRUE(memcmp(dst.kit_steps[t], before.kit_steps[t], sizeof(dst.kit_steps[t])) == 0);
        }
    }
    ASSERT_EQ(dst.length, before.length);
    ASSERT_EQ(dst.swing, 10);
    ASSERT_STR_EQ(dst.name, before.name);
    ASSERT_EQ(dst.steps[1][0].note, 65);
    ASSERT_EQ(dst.track_length[1], 4);
}

TEST(test_pattern_copy_track_invalid_changes_nothing) {
    pattern_t src, dst, before;
    pattern_init(&src, 0);
    pattern_init(&dst, 1);
    pattern_set_step(&src, 0, 0, 70, 90, STEP_FLAG_ACTIVE);
    before = dst;

    pattern_copy_track(&dst, &src, NUM_TRACKS);
    ASSERT_TRUE(memcmp(&dst, &before, sizeof(dst)) == 0);

    before = src;
    pattern_copy_track(&src, &src, 0);
    ASSERT_TRUE(memcmp(&src, &before, sizeof(src)) == 0);
}

/* ── Main ────────────────────────────────────────────────── */

int main(void) {
    RUN(test_track_length_does_not_change_other_tracks_fallback);
    printf("\n=== rebirth pattern unit tests ===\n\n");

    /* pattern_init */
    RUN(test_pattern_init_name);
    RUN(test_pattern_init_tracks);
    RUN(test_pattern_init_steps_zeroed);
    RUN(test_pattern_init_cv_output_mode);

    /* pattern_set_step */
    RUN(test_set_step_basic);
    RUN(test_set_step_with_flags);
    RUN(test_set_step_overwrite);
    RUN(test_set_step_bounds_check);

    /* pattern_clear_step */
    RUN(test_clear_step);
    RUN(test_clear_step_bounds);

    /* pattern_toggle_step */
    RUN(test_toggle_step_activate_drum);
    RUN(test_toggle_step_activate_synth);
    RUN(test_toggle_step_deactivate);
    RUN(test_toggle_step_preserves_note_on_deactivate);
    RUN(test_toggle_step_activate_with_existing_velocity);
    RUN(test_toggle_step_bounds);

    /* pattern_copy */
    RUN(test_pattern_copy);
    RUN(test_pattern_copy_independence);

    /* pattern length / edit helpers */
    RUN(test_track_length_per_track);
    RUN(test_track_length_clamps);
    RUN(test_pattern_clear_track);
    RUN(test_pattern_shift_track_right_wraps_16_steps);
    RUN(test_pattern_shift_track_left_wraps_16_steps);
    RUN(test_pattern_copy_track_copies_effective_length);
    RUN(test_pattern_copy_track_leaves_everything_else);
    RUN(test_pattern_copy_track_invalid_changes_nothing);

    printf("\n  %d/%d tests passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
