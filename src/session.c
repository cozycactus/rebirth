#include "session.h"
#include "sequencer.h"

#define SESSION_BANKS 4
#define SLOTS_PER_BANK 8
#define NO_COMMON_ROW 0xFF

static uint8_t bank;
static session_focus_t focus;

static uint8_t location(uint8_t row) {
    return pattern_make_index(bank, row);
}

void session_init(void) {
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    bank = 0;
    focus.track = 0;
    focus.location = snap.current[0];
}

uint8_t session_bank(void) {
    return bank;
}

void session_next_bank(void) {
    bank = (bank + 1) % SESSION_BANKS;
}

session_focus_t session_get_focus(void) {
    return focus;
}

void session_move_focus(int8_t delta) {
    int track = (int)focus.track + delta;
    if (track < 0) track = 0;
    if (track > NUM_TRACKS - 1) track = NUM_TRACKS - 1;

    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    focus.track = (uint8_t)track;
    focus.location = snap.queued[track] != NO_QUEUED_PATTERN ? snap.queued[track]
                                                              : snap.current[track];
}

bool session_pad(uint8_t track, uint8_t row, bool move_focus) {
    if (track >= NUM_TRACKS || row >= SLOTS_PER_BANK) return false;
    uint8_t target = location(row);
    if (move_focus) {
        focus.track = track;
        focus.location = target;
    }
    return transport_launch((uint8_t)(1u << track), target);
}

bool session_scene(uint8_t row) {
    if (row >= SLOTS_PER_BANK) return false;
    return transport_launch(UINT8_MAX, location(row));
}

void session_mute(uint8_t track) {
    if (track < NUM_TRACKS) seq_toggle_track_mute(track);
}

uint8_t session_common_row(void) {
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    uint8_t first = snap.current[0];
    if (first / SLOTS_PER_BANK != bank) return NO_COMMON_ROW;
    for (uint8_t track = 1; track < NUM_TRACKS; track++) {
        if (snap.current[track] != first) return NO_COMMON_ROW;
    }
    return first % SLOTS_PER_BANK;
}
