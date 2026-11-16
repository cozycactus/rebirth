#include "led_frame.h"

static const rgb_t TRACK_HUE[NUM_TRACKS] = {
    {255, 0, 0},     // red
    {255, 80, 0},    // orange
    {255, 200, 0},   // yellow
    {0, 255, 40},    // green
    {0, 220, 255},   // cyan
    {0, 60, 255},    // blue
    {140, 0, 255},   // violet
    {255, 0, 120},   // pink
};
static const rgb_t WHITE = {255, 255, 255};
static const rgb_t SCENE_GREEN = {0, 255, 0};
static const rgb_t OFF = {0, 0, 0};

// Rounds up, so a channel that is part of a hue never vanishes: at the dim
// level orange stays (3,1,0) instead of collapsing into red's (3,0,0).
static uint8_t shade_channel(uint8_t channel, uint8_t level) {
    return (uint8_t)((channel * level + 254) / 255);
}

static rgb_t shade(rgb_t hue, uint8_t level) {
    rgb_t c = {
        shade_channel(hue.r, level),
        shade_channel(hue.g, level),
        shade_channel(hue.b, level),
    };
    return c;
}

static bool is_off(rgb_t c) { return c.r == 0 && c.g == 0 && c.b == 0; }

// 8th-note blink phase and half-note (slow) blink phase.
static bool blink_fast(uint32_t pulse) { return (pulse / (PPQN / 2)) % 2 == 0; }
static bool blink_slow(uint32_t pulse) { return (pulse / (PPQN * 2)) % 2 == 0; }

static bool has_clip(const led_state_t *s, uint8_t track, uint8_t row) {
    return (s->clip_mask >> (track * 8 + row)) & 1u;
}

static uint8_t location(const led_state_t *s, uint8_t row) {
    return (uint8_t)(s->bank * PATTERNS_PER_BANK + row);
}

static bool is_queued(const led_state_t *s, uint8_t track) {
    return s->queued[track] != NO_QUEUED_PATTERN;
}

static bool outside_bank(const led_state_t *s, uint8_t location_) {
    return location_ / PATTERNS_PER_BANK != s->bank;
}

static rgb_t pad_light(const led_state_t *s, uint8_t track, uint8_t row) {
    uint8_t loc = location(s, row);
    bool clip = has_clip(s, track, row);
    rgb_t hue = TRACK_HUE[track];

    if (s->queued[track] == loc) {
        if (clip) return blink_fast(s->pulse) ? shade(hue, LED_LEVEL_FULL) : OFF;
        return blink_fast(s->pulse) ? shade(WHITE, LED_LEVEL_DIM) : OFF;
    }
    if (s->current[track] == loc) {
        if (s->playing) {
            return clip ? shade(hue, LED_LEVEL_FULL) : shade(WHITE, LED_LEVEL_DIM);
        }
        if (clip) return shade(hue, LED_LEVEL_HALF);
    }
    if (clip) return shade(hue, LED_LEVEL_DIM);
    return OFF;
}

static rgb_t scene_light(const led_state_t *s, uint8_t row) {
    uint8_t loc = location(s, row);
    uint8_t playing_row = 0;
    bool pending = false;
    bool clip = false;
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        if (s->queued[t] == loc) pending = true;
        if (s->playing && s->current[t] == loc) playing_row++;
        if (has_clip(s, t, row)) clip = true;
    }
    if (pending) return blink_fast(s->pulse) ? shade(SCENE_GREEN, LED_LEVEL_FULL) : OFF;
    if (playing_row == NUM_TRACKS) return shade(SCENE_GREEN, LED_LEVEL_FULL);
    if (playing_row > 0) return shade(SCENE_GREEN, LED_LEVEL_DIM);
    if (clip) return shade(WHITE, LED_LEVEL_DIM);
    return OFF;
}

static rgb_t mute_light(const led_state_t *s, uint8_t track) {
    bool muted = (s->muted >> track) & 1u;
    bool focus = track == s->focus_track;
    rgb_t base;
    if (focus) base = muted ? shade(WHITE, LED_LEVEL_DIM) : shade(TRACK_HUE[track], LED_LEVEL_FULL);
    else       base = muted ? OFF : shade(TRACK_HUE[track], LED_LEVEL_HALF);

    bool blink;
    bool on;
    if (is_queued(s, track) && outside_bank(s, s->queued[track])) {
        blink = true;
        on = blink_fast(s->pulse);
    } else if (outside_bank(s, s->current[track])) {
        blink = true;
        on = blink_slow(s->pulse);
    } else {
        blink = false;
        on = true;
    }
    if (!blink) return base;
    if (!on) return OFF;
    // A muted, unfocused track has no light of its own; the blink still has
    // to show, so it uses dim white.
    return is_off(base) ? shade(WHITE, LED_LEVEL_DIM) : base;
}

static rgb_t beat_light(const led_state_t *s) {
    if (!s->playing) return OFF;
    if (s->pulse % (PPQN * 4) < PPQN / 8) return shade(WHITE, LED_LEVEL_FULL);
    if (s->pulse % PPQN < PPQN / 8) return shade(WHITE, LED_LEVEL_HALF);
    return OFF;
}

void led_frame_build(const led_state_t *s, rgb_t out[LED_COUNT]) {
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        for (uint8_t row = 0; row < PATTERNS_PER_BANK; row++) {
            out[LED_PAD(track, row)] = pad_light(s, track, row);
        }
        out[LED_MUTE(track)] = mute_light(s, track);
    }
    for (uint8_t row = 0; row < PATTERNS_PER_BANK; row++) {
        out[LED_SCENE(row)] = scene_light(s, row);
    }
    out[LED_PLAY] = s->playing ? shade(SCENE_GREEN, LED_LEVEL_FULL) : OFF;
    out[LED_BEAT] = beat_light(s);
}

static uint64_t channel_sum(const rgb_t frame[LED_COUNT]) {
    uint64_t sum = 0;
    for (int i = 0; i < LED_COUNT; i++) sum += frame[i].r + frame[i].g + frame[i].b;
    return sum;
}

// Dynamic current in uA, rounded up; a full channel (255) draws LED_CHANNEL_MA_FULL.
static uint64_t dynamic_ua(const rgb_t frame[LED_COUNT]) {
    return (channel_sum(frame) * LED_CHANNEL_MA_FULL * 1000u + 254u) / 255u;
}

uint32_t led_frame_estimate_ma(const rgb_t frame[LED_COUNT]) {
    uint64_t total_ua = (uint64_t)LED_COUNT * LED_IDLE_UA + dynamic_ua(frame);
    return (uint32_t)((total_ua + 999u) / 1000u);
}

void led_frame_apply_budget(rgb_t frame[LED_COUNT], uint32_t budget_ma) {
    uint64_t dynamic = dynamic_ua(frame);
    uint64_t idle = (uint64_t)LED_COUNT * LED_IDLE_UA;
    uint64_t budget = (uint64_t)budget_ma * 1000u;
    if (dynamic == 0 || idle + dynamic <= budget) return;
    uint64_t available = budget > idle ? budget - idle : 0;
    for (int i = 0; i < LED_COUNT; i++) {
        frame[i].r = (uint8_t)(frame[i].r * available / dynamic);
        frame[i].g = (uint8_t)(frame[i].g * available / dynamic);
        frame[i].b = (uint8_t)(frame[i].b * available / dynamic);
    }
}
