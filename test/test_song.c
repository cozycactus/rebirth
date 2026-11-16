#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sequencer.h"

/* Global required by sequencer.h */
sequencer_state_t g_seq;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do {                              \
    tests_run++;                                    \
    printf("  %-55s", #name);                       \
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
#define ASSERT_TRUE(x)  ASSERT_EQ(!!(x), 1)
#define ASSERT_FALSE(x) ASSERT_EQ(!!(x), 0)

static void reset_song(void) {
    song_clear();
    g_seq.song.end_mode = SONG_END_LOOP;
}

static void assert_entry_pattern_all(uint16_t entry, uint8_t pattern_index) {
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        ASSERT_EQ(g_seq.song.entries[entry].pattern_index[track], pattern_index);
    }
}

/* ── song_append ───────────────────────────────────────── */

TEST(test_song_append_basic) {
    reset_song();
    ASSERT_TRUE(song_append(0, 4));
    ASSERT_EQ(g_seq.song.length, 1);
    assert_entry_pattern_all(0, 0);
    ASSERT_EQ(g_seq.song.entries[0].repeats, 4);
}

TEST(test_song_append_multiple) {
    reset_song();
    ASSERT_TRUE(song_append(0, 2));
    ASSERT_TRUE(song_append(3, 1));
    ASSERT_TRUE(song_append(7, 8));
    ASSERT_EQ(g_seq.song.length, 3);
    assert_entry_pattern_all(1, 3);
    ASSERT_EQ(g_seq.song.entries[2].repeats, 8);
}

TEST(test_song_append_patterns_per_track) {
    reset_song();
    uint8_t patterns[NUM_TRACKS];
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        patterns[track] = track;
    }

    ASSERT_TRUE(song_append_patterns(patterns, 4));
    ASSERT_EQ(g_seq.song.length, 1);
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        ASSERT_EQ(g_seq.song.entries[0].pattern_index[track], track);
    }
    ASSERT_EQ(g_seq.song.entries[0].repeats, 4);
}

TEST(test_song_append_rejects_invalid) {
    reset_song();
    ASSERT_FALSE(song_append(MAX_PATTERNS, 1));  /* bad pattern */
    ASSERT_FALSE(song_append(0, 0));             /* 0 repeats */
    ASSERT_EQ(g_seq.song.length, 0);
}

TEST(test_song_append_full) {
    reset_song();
    for (int i = 0; i < MAX_SONG_ENTRIES; i++) {
        ASSERT_TRUE(song_append(i % MAX_PATTERNS, 1));
    }
    ASSERT_FALSE(song_append(0, 1));  /* full */
    ASSERT_EQ(g_seq.song.length, MAX_SONG_ENTRIES);
}

/* ── song_insert ───────────────────────────────────────── */

TEST(test_song_insert_at_start) {
    reset_song();
    song_append(1, 1);
    song_append(2, 1);
    ASSERT_TRUE(song_insert(0, 0, 3));
    ASSERT_EQ(g_seq.song.length, 3);
    assert_entry_pattern_all(0, 0);
    ASSERT_EQ(g_seq.song.entries[0].repeats, 3);
    assert_entry_pattern_all(1, 1);
    assert_entry_pattern_all(2, 2);
}

TEST(test_song_insert_in_middle) {
    reset_song();
    song_append(0, 1);
    song_append(2, 1);
    ASSERT_TRUE(song_insert(1, 1, 4));
    ASSERT_EQ(g_seq.song.length, 3);
    assert_entry_pattern_all(1, 1);
    assert_entry_pattern_all(2, 2);
}

TEST(test_song_insert_patterns_per_track) {
    reset_song();
    song_append(0, 1);

    uint8_t patterns[NUM_TRACKS];
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        patterns[track] = (uint8_t)(MAX_PATTERNS - 1 - track);
    }

    ASSERT_TRUE(song_insert_patterns(1, patterns, 2));
    ASSERT_EQ(g_seq.song.length, 2);
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        ASSERT_EQ(g_seq.song.entries[1].pattern_index[track],
                  MAX_PATTERNS - 1 - track);
    }
}

TEST(test_song_insert_at_end) {
    reset_song();
    song_append(0, 1);
    /* Insert at position == length is same as append */
    ASSERT_TRUE(song_insert(1, 5, 2));
    ASSERT_EQ(g_seq.song.length, 2);
    assert_entry_pattern_all(1, 5);
}

TEST(test_song_insert_rejects_invalid) {
    reset_song();
    song_append(0, 1);
    ASSERT_FALSE(song_insert(5, 0, 1));  /* pos > length */
    ASSERT_FALSE(song_insert(0, 0, 0));  /* 0 repeats */

    uint8_t patterns[NUM_TRACKS] = {0};
    patterns[3] = MAX_PATTERNS;
    ASSERT_FALSE(song_insert_patterns(1, patterns, 1));
}

/* ── song_remove ───────────────────────────────────────── */

TEST(test_song_remove_first) {
    reset_song();
    song_append(0, 1);
    song_append(1, 2);
    song_append(2, 3);
    ASSERT_TRUE(song_remove(0));
    ASSERT_EQ(g_seq.song.length, 2);
    assert_entry_pattern_all(0, 1);
    assert_entry_pattern_all(1, 2);
}

TEST(test_song_remove_last) {
    reset_song();
    song_append(0, 1);
    song_append(1, 2);
    ASSERT_TRUE(song_remove(1));
    ASSERT_EQ(g_seq.song.length, 1);
    assert_entry_pattern_all(0, 0);
}

TEST(test_song_remove_only) {
    reset_song();
    song_append(0, 1);
    ASSERT_TRUE(song_remove(0));
    ASSERT_EQ(g_seq.song.length, 0);
}

TEST(test_song_remove_rejects_invalid) {
    reset_song();
    ASSERT_FALSE(song_remove(0));  /* empty */
    song_append(0, 1);
    ASSERT_FALSE(song_remove(1));  /* out of range */
}

/* ── song_move ─────────────────────────────────────────── */

TEST(test_song_move_forward) {
    reset_song();
    song_append(0, 1);
    song_append(1, 1);
    song_append(2, 1);
    ASSERT_TRUE(song_move(0, 2));
    /* [1, 2, 0] */
    assert_entry_pattern_all(0, 1);
    assert_entry_pattern_all(1, 2);
    assert_entry_pattern_all(2, 0);
}

TEST(test_song_move_backward) {
    reset_song();
    song_append(0, 1);
    song_append(1, 1);
    song_append(2, 1);
    ASSERT_TRUE(song_move(2, 0));
    /* [2, 0, 1] */
    assert_entry_pattern_all(0, 2);
    assert_entry_pattern_all(1, 0);
    assert_entry_pattern_all(2, 1);
}

TEST(test_song_move_same) {
    reset_song();
    song_append(5, 1);
    ASSERT_TRUE(song_move(0, 0));
    assert_entry_pattern_all(0, 5);
}

TEST(test_song_move_rejects_invalid) {
    reset_song();
    song_append(0, 1);
    ASSERT_FALSE(song_move(0, 1));  /* to >= length */
    ASSERT_FALSE(song_move(1, 0));  /* from >= length */
}

/* ── song_clear ────────────────────────────────────────── */

TEST(test_song_clear) {
    reset_song();
    song_append(0, 4);
    song_append(1, 2);
    song_clear();
    ASSERT_EQ(g_seq.song.length, 0);
}

/* ── song_set_end_mode ─────────────────────────────────── */

TEST(test_song_end_mode) {
    song_set_end_mode(SONG_END_STOP);
    ASSERT_EQ(g_seq.song.end_mode, SONG_END_STOP);
    song_set_end_mode(SONG_END_LOOP);
    ASSERT_EQ(g_seq.song.end_mode, SONG_END_LOOP);
}

/* ── Pattern queue ─────────────────────────────────────── */

TEST(test_transport_queue_pattern) {
    transport_t *t = (transport_t *)&g_seq.transport;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        t->queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    transport_queue_pattern(5);
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        ASSERT_EQ(t->queued_pattern[track], 5);
    }
}

TEST(test_transport_queue_track_pattern) {
    transport_t *t = (transport_t *)&g_seq.transport;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        t->queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    transport_queue_track_pattern(3, 9);
    ASSERT_EQ(t->queued_pattern[3], 9);
    ASSERT_EQ(t->queued_pattern[2], NO_QUEUED_PATTERN);
}

TEST(test_transport_clear_queue) {
    transport_queue_pattern(5);
    transport_clear_queue();
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        ASSERT_EQ(((transport_t *)&g_seq.transport)->queued_pattern[track],
                  NO_QUEUED_PATTERN);
    }
}

TEST(test_transport_clear_track_queue) {
    transport_queue_pattern(5);
    transport_clear_track_queue(2);
    ASSERT_EQ(((transport_t *)&g_seq.transport)->queued_pattern[2],
              NO_QUEUED_PATTERN);
    ASSERT_EQ(((transport_t *)&g_seq.transport)->queued_pattern[3], 5);
}

TEST(test_transport_queue_rejects_invalid) {
    transport_t *t = (transport_t *)&g_seq.transport;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        t->queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    transport_queue_pattern(MAX_PATTERNS);  /* bad index */
    ASSERT_EQ(t->queued_pattern[0], NO_QUEUED_PATTERN);  /* not set */
    transport_queue_track_pattern(NUM_TRACKS, 0);
    ASSERT_EQ(t->queued_pattern[0], NO_QUEUED_PATTERN);
}

/* ── Track mutes ───────────────────────────────────────── */

TEST(test_track_mute_set) {
    g_seq.track_mutes = 0;
    seq_set_track_mute(0, true);
    seq_set_track_mute(3, true);
    ASSERT_EQ(seq_get_track_mutes(), 0x09);  /* bits 0 and 3 */
}

TEST(test_track_mute_unset) {
    g_seq.track_mutes = 0xFF;
    seq_set_track_mute(0, false);
    ASSERT_EQ(seq_get_track_mutes(), 0xFE);
}

TEST(test_track_mute_toggle) {
    g_seq.track_mutes = 0;
    seq_toggle_track_mute(5);
    ASSERT_TRUE(seq_get_track_mutes() & (1 << 5));
    seq_toggle_track_mute(5);
    ASSERT_FALSE(seq_get_track_mutes() & (1 << 5));
}

TEST(test_track_mute_bounds) {
    g_seq.track_mutes = 0;
    seq_set_track_mute(NUM_TRACKS, true);  /* out of range */
    ASSERT_EQ(seq_get_track_mutes(), 0);
    seq_toggle_track_mute(NUM_TRACKS);     /* out of range */
    ASSERT_EQ(seq_get_track_mutes(), 0);
}

TEST(test_track_config_applies_to_every_location) {
    for (int i = 0; i < MAX_PATTERNS; i++) pattern_init(&g_seq.patterns[i], i);
    uint8_t other = g_seq.patterns[5].tracks[1].midi_channel;
    track_config_t cfg = g_seq.patterns[0].tracks[2];
    cfg.midi_channel = 7;
    seq_set_track_config(2, &cfg);
    for (int p = 0; p < MAX_PATTERNS; p++) {
        ASSERT_EQ(g_seq.patterns[p].tracks[2].midi_channel, 7);
    }
    ASSERT_EQ(g_seq.patterns[5].tracks[1].midi_channel, other);
}

/* ── Pattern access helpers ────────────────────────────── */

TEST(test_seq_active_pattern) {
    g_seq.active_pattern[3] = 7;
    pattern_t *p = seq_active_pattern(3);
    ASSERT_TRUE(p == &g_seq.patterns[7]);

    g_seq.active_pattern[0] = 3;
    p = seq_active_pattern(NUM_TRACKS);
    ASSERT_TRUE(p == &g_seq.patterns[3]);
}

TEST(test_seq_playing_pattern) {
    transport_t *t = (transport_t *)&g_seq.transport;
    t->current_pattern[2] = 7;
    pattern_t *p = seq_playing_pattern(2);
    ASSERT_TRUE(p == &g_seq.patterns[7]);
}

TEST(test_pattern_locations_independent) {
    pattern_t *a = &g_seq.patterns[0];
    pattern_t *b = &g_seq.patterns[1];
    pattern_init(a, 0);
    pattern_init(b, 1);

    pattern_set_step(a, 0, 0, 60, 100, STEP_FLAG_ACTIVE);
    /* A1 and A2 should be independent locations. */
    ASSERT_EQ(b->steps[0][0].flags, 0);
    ASSERT_EQ(a->steps[0][0].note, 60);
}

/* ── ReBirth bank helpers ──────────────────────────────── */

TEST(test_pattern_bank_helpers) {
    ASSERT_EQ(pattern_bank(0), 0);
    ASSERT_EQ(pattern_slot(0), 0);
    ASSERT_EQ(pattern_bank(7), 0);
    ASSERT_EQ(pattern_slot(7), 7);
    ASSERT_EQ(pattern_bank(8), 1);
    ASSERT_EQ(pattern_slot(8), 0);
    ASSERT_EQ(pattern_make_index(3, 7), 31);
}

/* ── Main ────────────────────────────────────────────────── */

int main(void) {
    printf("\n=== rebirth song mode unit tests ===\n\n");

    /* Initialize patterns so helpers work */
    for (int i = 0; i < MAX_PATTERNS; i++) {
        pattern_init(&g_seq.patterns[i], i);
    }
    seq_lock_init();
    midi_init();
    g_seq.transport.bpm = DEFAULT_BPM;
    sequencer_init();
    transport_set_launch_quant(LAUNCH_QUANT_TRACK_LOOP);
    transport_start();

    /* song_append */
    RUN(test_song_append_basic);
    RUN(test_song_append_multiple);
    RUN(test_song_append_patterns_per_track);
    RUN(test_song_append_rejects_invalid);
    RUN(test_song_append_full);

    /* song_insert */
    RUN(test_song_insert_at_start);
    RUN(test_song_insert_in_middle);
    RUN(test_song_insert_patterns_per_track);
    RUN(test_song_insert_at_end);
    RUN(test_song_insert_rejects_invalid);

    /* song_remove */
    RUN(test_song_remove_first);
    RUN(test_song_remove_last);
    RUN(test_song_remove_only);
    RUN(test_song_remove_rejects_invalid);

    /* song_move */
    RUN(test_song_move_forward);
    RUN(test_song_move_backward);
    RUN(test_song_move_same);
    RUN(test_song_move_rejects_invalid);

    /* song_clear / end mode */
    RUN(test_song_clear);
    RUN(test_song_end_mode);

    /* Pattern queue */
    RUN(test_transport_queue_pattern);
    RUN(test_transport_queue_track_pattern);
    RUN(test_transport_clear_queue);
    RUN(test_transport_clear_track_queue);
    RUN(test_transport_queue_rejects_invalid);

    /* Track mutes */
    RUN(test_track_mute_set);
    RUN(test_track_mute_unset);
    RUN(test_track_mute_toggle);
    RUN(test_track_mute_bounds);

    RUN(test_track_config_applies_to_every_location);

    /* Pattern access */
    RUN(test_seq_active_pattern);
    RUN(test_seq_playing_pattern);
    RUN(test_pattern_locations_independent);
    RUN(test_pattern_bank_helpers);

    printf("\n  %d/%d tests passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
