#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stdint.h>

/* Session view model (Core 0 only). The grid shows one bank of 8 slots per
 * track: column = track, row = slot, location = bank * 8 + row. */

typedef struct {
    uint8_t track;
    uint8_t location;
} session_focus_t;

void session_init(void);              // bank 0; focus = (0, current location of track 0)
uint8_t session_bank(void);
void session_next_bank(void);         // 0 -> 1 -> 2 -> 3 -> 0; focus unchanged
session_focus_t session_get_focus(void);

// Moves the focus by whole tracks (clamped to 0..7). The location is the
// track's queued location if one is pending, else its current one.
void session_move_focus(int8_t delta);

// Launches slot `row` of the shown bank on `track`: a pad on the queued
// location cancels it, a pad on the playing one drops any queue, else queues.
// The decision and the write are one transport_launch call, atomic with the
// timer. With move_focus the focus follows the pad, accepted or not. False =
// rejected (Song mode).
bool session_pad(uint8_t track, uint8_t row, bool move_focus);

// Scene launch of slot `row`; a press while any track is queued to it cancels
// every queue. One transport_launch call. False = rejected (Song mode).
bool session_scene(uint8_t row);

void session_mute(uint8_t track);

// Slot 0..7 of the shown bank when every track's current location is on it in
// the same slot, else 0xFF.
uint8_t session_common_row(void);

#endif
