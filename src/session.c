#include "session.h"
#include "sequencer.h"
#include "kit.h"

#define SESSION_BANKS 4
#define SLOTS_PER_BANK 8
#define NO_COMMON_ROW 0xFF

static uint8_t bank;
static uint8_t focus_track, focus_location;
// The only record of a kit track's lane: the focus reads it on a kit cell.
static uint8_t kit_lane[NUM_TRACKS];

static uint8_t location(uint8_t row) {
    return pattern_make_index(bank, row);
}

// The location a track is edited in when the focus moves onto it.
static uint8_t queued_or_current(const transport_snapshot_t *snap, uint8_t track) {
    return snap->queued[track] != NO_QUEUED_PATTERN ? snap->queued[track] : snap->current[track];
}

void session_init(void) {
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    bank = 0;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) kit_lane[track] = KIT_LANE_BD;
    focus_track = 0;
    focus_location = snap.current[0];
}

uint8_t session_bank(void) {
    return bank;
}

void session_next_bank(void) {
    bank = (bank + 1) % SESSION_BANKS;
}

// Patterns are written by Core 0 only: the type is read without a lock.
session_focus_t session_get_focus(void) {
    session_focus_t focus = {focus_track, focus_location, 0};
    if (kit_is_cell(&g_seq.patterns[focus_location], focus_track)) {
        focus.lane = kit_lane[focus_track];
    }
    return focus;
}

static bool kit_in_mask(track_mask_t kit_mask, uint8_t track) {
    return (kit_mask >> track) & 1u;
}

void session_walk_stops(uint8_t *track, uint8_t *lane, int8_t detents, track_mask_t kit_mask) {
    uint8_t t = *track < NUM_TRACKS ? *track : NUM_TRACKS - 1;
    uint8_t l = !kit_in_mask(kit_mask, t) ? 0 : (*lane < KIT_LANES ? *lane : KIT_LANES - 1);
    for (; detents > 0; detents--) {
        if (kit_in_mask(kit_mask, t) && l < KIT_LANES - 1) {
            l++;
        } else if (t < NUM_TRACKS - 1) {
            t++;
            l = KIT_LANE_AC;    // a kit entered from the left starts on AC
        } else {
            break;
        }
    }
    for (; detents < 0; detents++) {
        if (kit_in_mask(kit_mask, t) && l > KIT_LANE_AC) {
            l--;
        } else if (t > 0) {
            t--;
            l = kit_in_mask(kit_mask, t) ? KIT_LANES - 1 : 0;
        } else {
            break;
        }
    }
    *track = t;
    *lane = l;
}

uint8_t session_kit_lane(uint8_t track) {
    return track < NUM_TRACKS ? kit_lane[track] : KIT_LANE_BD;
}

void session_set_kit_lane(uint8_t track, uint8_t lane) {
    if (track < NUM_TRACKS && lane < KIT_LANES) kit_lane[track] = lane;
}

void session_set_focus(uint8_t track, uint8_t location) {
    if (track >= NUM_TRACKS || location >= MAX_PATTERNS) return;
    focus_track = track;
    focus_location = location;
}

void session_move_focus(int8_t detents) {
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    uint8_t edited[NUM_TRACKS];
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        edited[t] = t == focus_track ? focus_location : queued_or_current(&snap, t);
    }
    track_mask_t kit_mask = kit_cell_mask(g_seq.patterns, edited);

    uint8_t track = focus_track;
    uint8_t lane = session_get_focus().lane;
    session_walk_stops(&track, &lane, detents, kit_mask);
    if (track != focus_track) {
        focus_track = track;
        focus_location = edited[track];
    }
    if (kit_in_mask(kit_mask, track)) session_set_kit_lane(track, lane);
}

bool session_pad(uint8_t track, uint8_t row, bool move_focus) {
    if (track >= NUM_TRACKS || row >= SLOTS_PER_BANK) return false;
    uint8_t target = location(row);
    if (move_focus) {
        focus_track = track;
        focus_location = target;
    }
    return transport_launch(TRACK_BIT(track), target);
}

bool session_scene(uint8_t row) {
    if (row >= SLOTS_PER_BANK) return false;
    return transport_launch(ALL_TRACKS, location(row));
}

void session_mute(uint8_t track) {
    if (track < NUM_TRACKS) seq_toggle_track_mute(track);
}

uint8_t session_common_row(const transport_snapshot_t *snap) {
    uint8_t first = snap->current[0];
    if (first / SLOTS_PER_BANK != bank) return NO_COMMON_ROW;
    for (uint8_t track = 1; track < NUM_TRACKS; track++) {
        if (snap->current[track] != first) return NO_COMMON_ROW;
    }
    return first % SLOTS_PER_BANK;
}
