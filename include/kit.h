#ifndef KIT_H
#define KIT_H

#include <stdint.h>
#include <stdbool.h>
#include "sequencer.h"

// Pure helpers for kit tracks: no locks, no hardware.

// Lane value of a step word: 0..3 (AC reads 0 or 1).
uint8_t  kit_get(uint32_t word, uint8_t lane);
// Store a lane value; AC stores (value != 0).
uint32_t kit_set(uint32_t word, uint8_t lane, uint8_t value);
// kit_set plus the CH/OH rule.
uint32_t kit_edit(uint32_t word, uint8_t lane, uint8_t value);
// Button press: AC toggles; a voice goes 0 -> 1 -> 2 -> 0, flam -> 0. CH/OH rule applies.
uint32_t kit_press(uint32_t word, uint8_t lane);

void kit_config_defaults(kit_config_t *c);
void kit_config_clamp(kit_config_t *c);
bool kit_config_valid(const kit_config_t *c);

const char *kit_lane_name(uint8_t lane);

// True when any voice lane (not AC) is non-zero within the first `length` steps.
bool kit_cell_has_clip(const uint32_t steps[STEPS_PER_PATTERN], uint8_t length);
// A kit cell: track `track` of pattern `p` has its own type KIT. The one rule for
// which data a cell plays and which editor the panel shows for it.
bool kit_is_cell(const pattern_t *p, uint8_t track);
// Bit t set when track t is a kit cell at location[t] of `patterns` (a location
// >= MAX_PATTERNS counts as none).
track_mask_t kit_cell_mask(const pattern_t *patterns, const uint8_t location[NUM_TRACKS]);
// Bit t set when track t has type KIT in any of the `count` patterns.
track_mask_t kit_track_mask(const pattern_t *patterns, uint8_t count);
// The glyph of a lane value: '.' off, 'o' note (AC on), 'O' accent, 'f' flam.
char kit_value_glyph(uint8_t value);
// One glyph per step: '.', 'o', 'O', 'f'; ' ' beyond length; NUL-terminated.
void kit_lane_glyphs(const uint32_t steps[STEPS_PER_PATTERN], uint8_t lane, uint8_t length,
                     char out[STEPS_PER_PATTERN + 1]);

// Order the voices play inside one kit step (voice indices).
extern const uint8_t kit_voice_order[KIT_VOICES];

#endif
