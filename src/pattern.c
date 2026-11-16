#include <string.h>
#include <stdio.h>
#include "sequencer.h"

void pattern_init(pattern_t *p, uint8_t index) {
    memset(p->steps, 0, sizeof(p->steps));
    memset(p->kit_steps, 0, sizeof(p->kit_steps));

    for (int t = 0; t < NUM_TRACKS; t++) {
        p->tracks[t].type = (t < 4) ? TRACK_TYPE_SYNTH : TRACK_TYPE_DRUM;
        p->tracks[t].midi_channel = t;
        p->tracks[t].drum_note = 36 + t;   // C1, C#1, D1...
        p->tracks[t].output_mode = (t < NUM_CV_CHANNELS) ? OUTPUT_BOTH : OUTPUT_MIDI_ONLY;
        p->tracks[t].cv_channel = (t < NUM_CV_CHANNELS) ? t : 0;
        p->tracks[t].gate_length_pct = 50;
        p->tracks[t].muted = false;
    }

    p->length = STEPS_PER_PATTERN;
    for (int t = 0; t < NUM_TRACKS; t++) {
        p->track_length[t] = STEPS_PER_PATTERN;
    }
    p->swing = 50;  // Straight (no swing)
    snprintf(p->name, sizeof(p->name), "P%c%d",
             'A' + pattern_bank(index), pattern_slot(index) + 1);
}

void pattern_set_step(pattern_t *p, uint8_t track, uint8_t step,
                      uint8_t note, uint8_t velocity, uint8_t flags) {
    if (track >= NUM_TRACKS || step >= STEPS_PER_PATTERN) return;
    p->steps[track][step].note = note;
    p->steps[track][step].velocity = velocity;
    p->steps[track][step].flags = flags;
}

void pattern_clear_step(pattern_t *p, uint8_t track, uint8_t step) {
    if (track >= NUM_TRACKS || step >= STEPS_PER_PATTERN) return;
    memset(&p->steps[track][step], 0, sizeof(step_t));
    p->kit_steps[track][step] = 0;
}

void pattern_toggle_step(pattern_t *p, uint8_t track, uint8_t step) {
    if (track >= NUM_TRACKS || step >= STEPS_PER_PATTERN) return;
    step_t *s = &p->steps[track][step];
    s->flags ^= STEP_FLAG_ACTIVE;
    if (s->flags & STEP_FLAG_ACTIVE) {
        // Set defaults when activating
        if (s->velocity == 0) s->velocity = 100;
        if (s->note == 0 && p->tracks[track].type == TRACK_TYPE_DRUM) {
            s->note = p->tracks[track].drum_note;
        }
        if (s->note == 0) s->note = 60;  // Middle C default for synth
    }
}

void pattern_copy(pattern_t *dst, const pattern_t *src) {
    memcpy(dst, src, sizeof(pattern_t));
}

void pattern_clear(pattern_t *p) {
    memset(p->steps, 0, sizeof(p->steps));
    memset(p->kit_steps, 0, sizeof(p->kit_steps));
}

void pattern_clear_track(pattern_t *p, uint8_t track) {
    if (track >= NUM_TRACKS) return;
    memset(p->steps[track], 0, sizeof(p->steps[track]));
    memset(p->kit_steps[track], 0, sizeof(p->kit_steps[track]));
}

static void shift_track_steps(step_t *steps, int8_t direction) {
    if (direction == 0) return;

    step_t tmp;
    if (direction > 0) {
        tmp = steps[STEPS_PER_PATTERN - 1];
        memmove(&steps[1], &steps[0], (STEPS_PER_PATTERN - 1) * sizeof(step_t));
        steps[0] = tmp;
    } else {
        tmp = steps[0];
        memmove(&steps[0], &steps[1], (STEPS_PER_PATTERN - 1) * sizeof(step_t));
        steps[STEPS_PER_PATTERN - 1] = tmp;
    }
}

static void shift_kit_steps(uint32_t *words, int8_t direction) {
    if (direction == 0) return;

    uint32_t tmp;
    if (direction > 0) {
        tmp = words[STEPS_PER_PATTERN - 1];
        memmove(&words[1], &words[0], (STEPS_PER_PATTERN - 1) * sizeof(uint32_t));
        words[0] = tmp;
    } else {
        tmp = words[0];
        memmove(&words[0], &words[1], (STEPS_PER_PATTERN - 1) * sizeof(uint32_t));
        words[STEPS_PER_PATTERN - 1] = tmp;
    }
}

void pattern_shift(pattern_t *p, int8_t direction) {
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        shift_track_steps(p->steps[track], direction);
        shift_kit_steps(p->kit_steps[track], direction);
    }
}

void pattern_shift_track(pattern_t *p, uint8_t track, int8_t direction) {
    if (track >= NUM_TRACKS) return;
    shift_track_steps(p->steps[track], direction);
    shift_kit_steps(p->kit_steps[track], direction);
}

uint8_t pattern_get_track_length(const pattern_t *p, uint8_t track) {
    uint8_t len = p->length;
    if (track < NUM_TRACKS && p->track_length[track] != 0) {
        len = p->track_length[track];
    }
    if (len < 1) return 1;
    if (len > STEPS_PER_PATTERN) return STEPS_PER_PATTERN;
    return len;
}

void pattern_set_track_length(pattern_t *p, uint8_t track, uint8_t length) {
    if (track >= NUM_TRACKS) return;
    if (length < 1) length = 1;
    if (length > STEPS_PER_PATTERN) length = STEPS_PER_PATTERN;
    p->track_length[track] = length;
}

uint8_t pattern_bank(uint8_t pattern_index) {
    if (pattern_index >= MAX_PATTERNS) pattern_index = MAX_PATTERNS - 1;
    return pattern_index / PATTERNS_PER_BANK;
}

uint8_t pattern_slot(uint8_t pattern_index) {
    if (pattern_index >= MAX_PATTERNS) pattern_index = MAX_PATTERNS - 1;
    return pattern_index % PATTERNS_PER_BANK;
}

uint8_t pattern_make_index(uint8_t bank, uint8_t slot) {
    if (bank >= PATTERN_BANKS) bank = PATTERN_BANKS - 1;
    if (slot >= PATTERNS_PER_BANK) slot = PATTERNS_PER_BANK - 1;
    return (uint8_t)(bank * PATTERNS_PER_BANK + slot);
}
