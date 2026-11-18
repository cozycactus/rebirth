#include "kit.h"

// The timer ISR reads it on every kit step, flam and release. On the RP2040 it is
// copied to RAM with .data, so the kit path makes no flash access.
#if defined(__arm__)
__attribute__((section(".data.kit_voice_order")))
#endif
const uint8_t kit_voice_order[KIT_VOICES] = {0, 1, 6, 7, 8, 5, 2, 3, 4, 9, 10};

static const char *const lane_names[KIT_LANES] = {
    "AC", "BD", "SD", "LT", "MT", "HT", "RS", "CP", "CH", "OH", "CC", "RC",
};

static const uint8_t default_notes[KIT_VOICES] = {36, 38, 45, 47, 50, 37, 39, 42, 46, 49, 51};

uint8_t kit_get(uint32_t word, uint8_t lane) {
    if (lane >= KIT_LANES) return 0;
    uint8_t v = (uint8_t)((word >> (2u * lane)) & 3u);
    return lane == KIT_LANE_AC ? (v != 0) : v;
}

uint32_t kit_set(uint32_t word, uint8_t lane, uint8_t value) {
    if (lane >= KIT_LANES) return word;
    value = lane == KIT_LANE_AC ? (value != 0) : (value & 3u);
    word &= ~(3u << (2u * lane));
    return word | ((uint32_t)value << (2u * lane));
}

static uint32_t apply_hat_rule(uint32_t word, uint8_t lane) {
    if (kit_get(word, lane) == 0) return word;
    if (lane == KIT_LANE_CH) return kit_set(word, KIT_LANE_OH, 0);
    if (lane == KIT_LANE_OH) return kit_set(word, KIT_LANE_CH, 0);
    return word;
}

uint32_t kit_edit(uint32_t word, uint8_t lane, uint8_t value) {
    return apply_hat_rule(kit_set(word, lane, value), lane);
}

uint32_t kit_press(uint32_t word, uint8_t lane) {
    uint8_t v = kit_get(word, lane);
    uint8_t next;
    if (lane == KIT_LANE_AC) next = !v;
    else next = (v == KIT_NOTE) ? KIT_ACCENT : (v == KIT_OFF ? KIT_NOTE : KIT_OFF);
    return kit_edit(word, lane, next);
}

void kit_config_defaults(kit_config_t *c) {
    for (uint8_t i = 0; i < KIT_VOICES; i++) c->note[i] = default_notes[i];
    c->vel_normal = 80;
    c->vel_accent = 110;
    c->vel_max = 127;
    c->flam_pulses = 3;
}

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void kit_config_clamp(kit_config_t *c) {
    for (uint8_t i = 0; i < KIT_VOICES; i++) c->note[i] = clamp_u8(c->note[i], 0, 127);
    c->vel_normal = clamp_u8(c->vel_normal, 1, 127);
    c->vel_accent = clamp_u8(c->vel_accent, 1, 127);
    c->vel_max = clamp_u8(c->vel_max, 1, 127);
    c->flam_pulses = clamp_u8(c->flam_pulses, KIT_FLAM_MIN, KIT_FLAM_MAX);
}

bool kit_config_valid(const kit_config_t *c) {
    for (uint8_t i = 0; i < KIT_VOICES; i++) {
        if (c->note[i] > 127) return false;
    }
    if (c->vel_normal < 1 || c->vel_normal > 127) return false;
    if (c->vel_accent < 1 || c->vel_accent > 127) return false;
    if (c->vel_max < 1 || c->vel_max > 127) return false;
    return c->flam_pulses >= KIT_FLAM_MIN && c->flam_pulses <= KIT_FLAM_MAX;
}

const char *kit_lane_name(uint8_t lane) {
    return lane < KIT_LANES ? lane_names[lane] : "??";
}

bool kit_cell_has_clip(const uint32_t steps[STEPS_PER_PATTERN], uint8_t length) {
    if (length > STEPS_PER_PATTERN) length = STEPS_PER_PATTERN;
    for (uint8_t s = 0; s < length; s++) {
        // Bits 2..23 are the voice lanes; AC (bits 0..1) alone is not a clip.
        if (steps[s] & 0x00FFFFFCu) return true;
    }
    return false;
}

bool kit_is_cell(const pattern_t *p, uint8_t track) {
    return track < KIT_TRACK_SLOTS && p->tracks[track].type == TRACK_TYPE_KIT;
}

track_mask_t kit_cell_mask(const pattern_t *patterns, const uint8_t location[NUM_TRACKS]) {
    track_mask_t mask = 0;
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        if (location[t] < MAX_PATTERNS && kit_is_cell(&patterns[location[t]], t)) {
            mask |= TRACK_BIT(t);
        }
    }
    return mask;
}

track_mask_t kit_track_mask(const pattern_t *patterns, uint8_t count) {
    track_mask_t mask = 0;
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t t = 0; t < NUM_TRACKS; t++) {
            if (kit_is_cell(&patterns[i], t)) mask |= TRACK_BIT(t);
        }
    }
    return mask;
}

char kit_value_glyph(uint8_t value) {
    static const char glyph[4] = {'.', 'o', 'O', 'f'};
    return glyph[value & 3u];
}

void kit_lane_glyphs(const uint32_t steps[STEPS_PER_PATTERN], uint8_t lane, uint8_t length,
                     char out[STEPS_PER_PATTERN + 1]) {
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        out[s] = s < length ? kit_value_glyph(kit_get(steps[s], lane)) : ' ';
    }
    out[STEPS_PER_PATTERN] = '\0';
}
