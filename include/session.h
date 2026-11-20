#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include "sequencer.h"

/* Session view model (Core 0 only). The grid shows one bank of 8 slots per
 * track: column = track, row = slot, location = bank * 8 + row. */

typedef struct {
    uint8_t track;
    uint8_t location;
    uint8_t lane;       // on a kit cell the track's remembered lane, else 0
} session_focus_t;

// Bank 0; focus = (0, current location of track 0); every kit lane BD.
void session_init(void);
uint8_t session_bank(void);
void session_next_bank(void);         // 0 -> 1 -> 2 -> 3 -> 0; focus unchanged
session_focus_t session_get_focus(void);

// Walks `detents` stops: a track whose bit is clear in kit_mask is one stop
// (lane 0), a kit track KIT_LANES stops (AC..RC). Entering a kit moving right
// lands on AC, moving left on RC. Clamped at (0, first stop) and (7, last stop).
void session_walk_stops(uint8_t *track, uint8_t *lane, int8_t detents, track_mask_t kit_mask);

// The lane each kit track last had focused (UI RAM, not saved). The focus and
// the Pattern view both read and move this one record.
uint8_t session_kit_lane(uint8_t track);
void session_set_kit_lane(uint8_t track, uint8_t lane);

// Puts the focus on a cell. Ignored for a track or location out of range. The
// lane of a kit cell stays the track's remembered one.
void session_set_focus(uint8_t track, uint8_t location);

// Walks the focus by `detents` stops. A track is a kit for the walk when its
// type is KIT in the location it would be edited in: the focused location for
// the focused track, the queued location if one is pending, else the current
// one, for the others. On the same track the location stays; on a new track it
// is that track's queued-or-current location. A kit lane reached is remembered.
void session_move_focus(int8_t detents);

// Launches slot `row` of the shown bank on `track`: a pad on the queued
// location cancels it, a pad on the playing one drops any queue, else queues.
// The decision and the write are one transport_launch call, atomic with the
// timer. With move_focus the focus follows the pad, accepted or not, onto the
// track's remembered lane. False = rejected (Song mode).
bool session_pad(uint8_t track, uint8_t row, bool move_focus);

// Scene launch of slot `row`; a press while any track is queued to it cancels
// every queue. One transport_launch call. False = rejected (Song mode).
bool session_scene(uint8_t row);

void session_mute(uint8_t track);

// Slot 0..7 of the shown bank when every track's current location in `snap`
// is on it in the same slot, else 0xFF. Takes no lock: pass the frame's
// snapshot.
uint8_t session_common_row(const transport_snapshot_t *snap);

#endif
