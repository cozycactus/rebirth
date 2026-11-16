#include "session.h"
#include "sequencer.h"

#define SESSION_BANKS 4
#define SLOTS_PER_BANK 8
#define NO_COMMON_ROW 0xFF

static uint8_t bank;
static session_focus_t focus;
static uint8_t kit_lane[NUM_TRACKS];

static uint8_t location(uint8_t row) {
    return pattern_make_index(bank, row);
}

// The location a track is edited in when the focus moves onto it.
static uint8_t queued_or_current(const transport_snapshot_t *snap, uint8_t track) {
    return snap->queued[track] != NO_QUEUED_PATTERN ? snap->queued[track] : snap->current[track];
}

// Patterns are written by Core 0 only: read without a lock.
static bool is_kit_cell(uint8_t track, uint8_t location) {
    return g_seq.patterns[location].tracks[track].type == TRACK_TYPE_KIT;
}

void session_init(void) {
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    bank = 0;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) kit_lane[track] = KIT_LANE_BD;
    focus.track = 0;
    focus.location = snap.current[0];
    focus.lane = kit_lane[0];
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

static bool kit_in_mask(uint8_t kit_mask, uint8_t track) {
    return (kit_mask >> track) & 1u;
}

void session_walk_stops(uint8_t *track, uint8_t *lane, int8_t detents, uint8_t kit_mask) {
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

void session_move_focus(int8_t detents) {
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    uint8_t kit_mask = 0;
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        uint8_t edited = t == focus.track ? focus.location : queued_or_current(&snap, t);
        if (is_kit_cell(t, edited)) kit_mask |= (uint8_t)(1u << t);
    }

    uint8_t track = focus.track;
    uint8_t lane = focus.lane;
    session_walk_stops(&track, &lane, detents, kit_mask);
    if (track != focus.track) {
        focus.track = track;
        focus.location = queued_or_current(&snap, track);
    }
    focus.lane = lane;
    if (kit_in_mask(kit_mask, track)) session_set_kit_lane(track, lane);
}

bool session_pad(uint8_t track, uint8_t row, bool move_focus) {
    if (track >= NUM_TRACKS || row >= SLOTS_PER_BANK) return false;
    uint8_t target = location(row);
    if (move_focus) {
        focus.track = track;
        focus.location = target;
        focus.lane = kit_lane[track];
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

uint8_t session_common_row(const transport_snapshot_t *snap) {
    uint8_t first = snap->current[0];
    if (first / SLOTS_PER_BANK != bank) return NO_COMMON_ROW;
    for (uint8_t track = 1; track < NUM_TRACKS; track++) {
        if (snap->current[track] != first) return NO_COMMON_ROW;
    }
    return first % SLOTS_PER_BANK;
}
