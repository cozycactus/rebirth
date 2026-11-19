#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fakes/fake_hardware.h"
#include "sequencer.h"
#include "session.h"

sequencer_state_t g_seq;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define RUN(name) do { name(); printf("PASS %s\n", #name); } while (0)

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
    session_init();
}

static transport_snapshot_t snap(void) {
    transport_snapshot_t s;
    sequencer_snapshot(&s);
    return s;
}

static bool no_queues(void) {
    transport_snapshot_t s = snap();
    for (uint8_t t = 0; t < NUM_TRACKS; t++) if (s.queued[t] != NO_QUEUED_PATTERN) return false;
    return true;
}

static bool all_queued(uint8_t location) {
    transport_snapshot_t s = snap();
    for (uint8_t t = 0; t < NUM_TRACKS; t++) if (s.queued[t] != location) return false;
    return true;
}

static void test_pad_queues_and_focuses(void) {
    reset();
    transport_start();
    CHECK(session_pad(2, 3, true));
    CHECK(snap().queued[2] == 3);
    session_focus_t f = session_get_focus();
    CHECK(f.track == 2 && f.location == 3);
}

static void test_pad_on_queued_location_cancels(void) {
    reset();
    transport_start();
    session_pad(2, 3, true);
    session_pad(2, 3, true);
    CHECK(snap().queued[2] == NO_QUEUED_PATTERN);
}

static void test_pad_on_playing_location_without_queue_is_noop(void) {
    reset();
    transport_start();
    CHECK(session_pad(2, 0, true));
    CHECK(no_queues());
    CHECK(snap().current[2] == 0);
}

static void test_pad_on_playing_location_cancels_other_queue(void) {
    reset();
    transport_start();
    session_pad(2, 3, true);
    session_pad(2, 0, true);
    CHECK(no_queues());
    CHECK(snap().current[2] == 0);
}

static void test_pad_while_stopped_selects_immediately(void) {
    reset();
    session_pad(4, 6, true);
    CHECK(snap().current[4] == 6);
}

static void test_empty_pad_launches_silence(void) {
    reset();
    transport_start();
    session_pad(1, 7, true);
    CHECK(snap().queued[1] == 7);
}

static void test_scene_launches_row_of_shown_bank(void) {
    reset();
    session_next_bank();
    transport_start();
    CHECK(session_scene(3));
    CHECK(all_queued(11));
}

static void test_empty_row_scene_queues_every_track(void) {
    reset();
    transport_start();
    CHECK(session_scene(5));
    CHECK(all_queued(5));
}

static void test_scene_press_while_pending_cancels_all(void) {
    reset();
    transport_start();
    session_scene(3);
    session_pad(0, 5, false);
    CHECK(session_scene(3));
    CHECK(no_queues());
}

static void test_stop_ends_pending_scene(void) {
    reset();
    transport_start();
    session_scene(3);
    transport_stop();
    transport_start();
    session_scene(3);
    CHECK(all_queued(3));
}

static void test_song_mode_rejects_launch_but_moves_focus(void) {
    reset();
    transport_set_mode(PLAY_MODE_SONG);
    CHECK(!session_pad(5, 2, true));
    session_focus_t f = session_get_focus();
    CHECK(f.track == 5 && f.location == 2);
    CHECK(!session_scene(2));
}

static void test_move_focus_follows_queue_then_current(void) {
    reset();
    transport_start();
    session_pad(3, 6, false);
    session_move_focus(3);
    session_focus_t f = session_get_focus();
    CHECK(f.track == 3 && f.location == 6);
    session_move_focus(1);
    f = session_get_focus();
    CHECK(f.track == 4 && f.location == snap().current[4]);
}

static void test_bank_change_keeps_focus(void) {
    reset();
    session_pad(1, 2, true);
    session_next_bank();
    session_focus_t f = session_get_focus();
    CHECK(f.track == 1 && f.location == 2);
    CHECK(session_bank() == 1);
}

static uint8_t common_row(void) {
    transport_snapshot_t s = snap();
    return session_common_row(&s);
}

static void test_common_row(void) {
    reset();
    for (uint8_t t = 0; t < NUM_TRACKS; t++) session_pad(t, 2, false);
    CHECK(common_row() == 2);
    session_pad(5, 4, false);
    CHECK(common_row() == 0xFF);
    session_pad(5, 2, false);
    CHECK(common_row() == 2);
    session_next_bank();
    CHECK(common_row() == 0xFF);
}

#define LANE_SD 2
#define LANE_RC 11

static void test_walk_stops_across_kit(void) {
    uint8_t kits = 1u << 2;
    uint8_t track = 1, lane = 0;
    session_walk_stops(&track, &lane, 1, kits);         // right into the kit: AC
    CHECK(track == 2 && lane == KIT_LANE_AC);
    session_walk_stops(&track, &lane, 11, kits);
    CHECK(track == 2 && lane == LANE_RC);
    session_walk_stops(&track, &lane, 1, kits);         // out of it: one stop per track
    CHECK(track == 3 && lane == 0);
    session_walk_stops(&track, &lane, -1, kits);        // left into the kit: RC
    CHECK(track == 2 && lane == LANE_RC);
    track = 0;
    lane = 0;
    session_walk_stops(&track, &lane, 100, kits);       // clamped at the last stop
    CHECK(track == NUM_TRACKS - 1 && lane == 0);
    session_walk_stops(&track, &lane, -100, kits);      // and at the first
    CHECK(track == 0 && lane == 0);
}

static void make_kit(uint8_t track) {
    track_config_t config = g_seq.patterns[0].tracks[track];
    config.type = TRACK_TYPE_KIT;
    CHECK(seq_set_track_config(track, &config));
}

static void test_pad_on_kit_uses_remembered_lane(void) {
    reset();
    make_kit(1);
    for (uint8_t t = 0; t < NUM_TRACKS; t++) CHECK(session_kit_lane(t) == KIT_LANE_BD);
    session_pad(1, 0, true);
    session_focus_t f = session_get_focus();
    CHECK(f.track == 1 && f.location == 0 && f.lane == KIT_LANE_BD);
    session_move_focus(1);
    f = session_get_focus();
    CHECK(f.track == 1 && f.lane == LANE_SD);
    CHECK(session_kit_lane(1) == LANE_SD);
    session_move_focus(-3);                             // BD, AC, then track 0
    CHECK(session_get_focus().track == 0);
    session_pad(1, 0, true);
    f = session_get_focus();
    CHECK(f.track == 1 && f.location == 0 && f.lane == LANE_SD);
    session_set_kit_lane(1, LANE_RC);
    CHECK(session_kit_lane(1) == LANE_RC);
    session_init();
    CHECK(session_kit_lane(1) == KIT_LANE_BD);
}

static void test_lane_walk_keeps_location(void) {
    reset();
    g_seq.patterns[5].tracks[1].type = TRACK_TYPE_KIT;  // a kit in location 5 only
    transport_set_mode(PLAY_MODE_SONG);
    CHECK(!session_pad(1, 5, true));                     // rejected: track 1 stays on 0
    CHECK(snap().current[1] == 0);
    session_move_focus(1);
    session_focus_t f = session_get_focus();
    CHECK(f.track == 1 && f.location == 5 && f.lane == LANE_SD);
}

static void test_set_focus_moves_the_cell(void) {
    reset();
    session_set_focus(3, 13);
    session_focus_t f = session_get_focus();
    CHECK(f.track == 3 && f.location == 13 && f.lane == 0);
    session_set_focus(NUM_TRACKS, 2);
    f = session_get_focus();
    CHECK(f.track == 3 && f.location == 13);
    session_set_focus(1, MAX_PATTERNS);
    f = session_get_focus();
    CHECK(f.track == 3 && f.location == 13);
}

static void test_set_focus_on_kit_reads_remembered_lane(void) {
    reset();
    g_seq.patterns[9].tracks[1].type = TRACK_TYPE_KIT;
    session_set_kit_lane(1, LANE_RC);
    session_set_focus(1, 9);
    session_focus_t f = session_get_focus();
    CHECK(f.track == 1 && f.location == 9 && f.lane == LANE_RC);
}

int main(void) {
    seq_lock_init();
    RUN(test_pad_queues_and_focuses);
    RUN(test_pad_on_queued_location_cancels);
    RUN(test_pad_on_playing_location_without_queue_is_noop);
    RUN(test_pad_on_playing_location_cancels_other_queue);
    RUN(test_pad_while_stopped_selects_immediately);
    RUN(test_empty_pad_launches_silence);
    RUN(test_scene_launches_row_of_shown_bank);
    RUN(test_empty_row_scene_queues_every_track);
    RUN(test_scene_press_while_pending_cancels_all);
    RUN(test_stop_ends_pending_scene);
    RUN(test_song_mode_rejects_launch_but_moves_focus);
    RUN(test_move_focus_follows_queue_then_current);
    RUN(test_bank_change_keeps_focus);
    RUN(test_common_row);
    RUN(test_walk_stops_across_kit);
    RUN(test_pad_on_kit_uses_remembered_lane);
    RUN(test_lane_walk_keeps_location);
    RUN(test_set_focus_moves_the_cell);
    RUN(test_set_focus_on_kit_reads_remembered_lane);
    return 0;
}
