#ifndef LED_FRAME_H
#define LED_FRAME_H

#include <stdbool.h>
#include <stdint.h>
#include "sequencer.h"

/* Pure LED frame builder for the session panel: a state snapshot in, 82 RGB
 * values out. No locks, no hardware. Index layout: 64 pads (row-major), 8
 * SCENE, 8 MUTE, PLAY, BEAT. */

#define LED_COUNT 82
#define LED_PAD(track, row) ((row) * 8 + (track))
#define LED_SCENE(row)      (64 + (row))
#define LED_MUTE(track)     (72 + (track))
#define LED_PLAY            80
#define LED_BEAT            81

// Brightness codes; starting values, retuned on the bench.
#define LED_LEVEL_DIM   3
#define LED_LEVEL_HALF  12
#define LED_LEVEL_FULL  32

#define LED_RGB_BUDGET_MA   200
#define LED_CHANNEL_MA_FULL 20      // conservative until the part is measured
#define LED_IDLE_UA         1000    // per LED

typedef struct { uint8_t r, g, b; } rgb_t;

typedef struct {
    uint8_t  bank;                    // shown bank
    uint8_t  current[NUM_TRACKS];     // locations 0..31
    uint8_t  queued[NUM_TRACKS];      // NO_QUEUED_PATTERN when none
    uint64_t clip_mask;               // sequencer_clip_mask(bank)
    uint8_t  muted;                   // bit per track: live | stored mute
    uint8_t  focus_track;
    bool     playing;
    uint32_t pulse;                   // pulse_count while playing; a 120 BPM pulse clock when stopped
} led_state_t;

void led_frame_build(const led_state_t *s, rgb_t out[LED_COUNT]);

// Idle plus dynamic current for a frame, rounded up to whole mA.
uint32_t led_frame_estimate_ma(const rgb_t frame[LED_COUNT]);

// Scales every channel down so the frame fits budget_ma; a frame that already
// fits (or is all black) is left unchanged.
void led_frame_apply_budget(rgb_t frame[LED_COUNT], uint32_t budget_ma);

#endif
