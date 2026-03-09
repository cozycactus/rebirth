#include <string.h>
#include <stdio.h>
#include "sequencer.h"

void pattern_init(pattern_t *p, uint8_t index) {
    memset(p->steps, 0, sizeof(p->steps));

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
    p->swing = 50;  // Straight (no swing)
    snprintf(p->name, sizeof(p->name), "PT%02d", index);
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
