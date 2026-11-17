// Stands in for src/rgb_leds.c (PIO + DMA): the same interface and the same
// refusal of a frame while the previous one is still on the wire.

#include <string.h>
#include "rgb_leds.h"
#include "sim_internal.h"

#define FRAME_TIME_US 3000u     // as in src/rgb_leds.c: 82 x 30 us plus the reset gap

static uint8_t shown[LED_COUNT * 3];
static uint32_t frames_shown;
static uint32_t frames_skipped;
static uint64_t frame_accepted;
static bool frame_sent;

void sim_rgb_reset(void) {
    memset(shown, 0, sizeof(shown));
    frames_shown = 0;
    frames_skipped = 0;
    frame_sent = false;
}

void rgb_leds_init(void) {
    memset(shown, 0, sizeof(shown));    // an all-off frame goes out at once
    frame_accepted = sim_now_us;
    frame_sent = true;
}

bool rgb_leds_show(const rgb_t frame[LED_COUNT]) {
    if (frame_sent && sim_now_us - frame_accepted < FRAME_TIME_US) {
        frames_skipped++;
        return false;
    }
    for (int i = 0; i < LED_COUNT; i++) {
        shown[i * 3] = frame[i].r;
        shown[i * 3 + 1] = frame[i].g;
        shown[i * 3 + 2] = frame[i].b;
    }
    frame_accepted = sim_now_us;
    frame_sent = true;
    frames_shown++;
    return true;
}

const uint8_t *sim_leds(void) { return shown; }
uint32_t sim_leds_seq(void) { return frames_shown; }
uint32_t sim_leds_skipped(void) { return frames_skipped; }
