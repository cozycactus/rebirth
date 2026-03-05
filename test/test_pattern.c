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
    ASSERT_STR_EQ(p.name, "PT00");

    pattern_init(&p, 7);
    ASSERT_STR_EQ(p.name, "PT07");

    pattern_init(&p, 63);
    ASSERT_STR_EQ(p.name, "PT63");
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
    src.swing = 65;

    pattern_copy(&dst, &src);

    ASSERT_EQ(dst.steps[0][0].note, 60);
    ASSERT_EQ(dst.steps[3][7].note, 48);
    ASSERT_EQ(dst.steps[3][7].flags, STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT);
    ASSERT_TRUE(dst.tracks[1].muted);
    ASSERT_EQ(dst.swing, 65);
    ASSERT_STR_EQ(dst.name, "PT05");
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

/* ── Main ────────────────────────────────────────────────── */

int main(void) {
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

    printf("\n  %d/%d tests passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
