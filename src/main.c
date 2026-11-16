#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "sequencer.h"
#include "kit.h"

// ─── Global shared state ────────────────────────────────────────

sequencer_state_t g_seq;

// ─── Initialization ─────────────────────────────────────────────

static void init_defaults(void) {
    seq_lock_init();

    // Transport defaults
    g_seq.transport.state = TRANSPORT_STOPPED;
    g_seq.transport.mode = PLAY_MODE_PATTERN;
    g_seq.transport.bpm = DEFAULT_BPM;
    g_seq.launch_quant = LAUNCH_QUANT_BAR;
    g_seq.transport.pulse_count = 0;
    g_seq.transport.current_step = 0;
    for (int t = 0; t < NUM_TRACKS; t++) {
        g_seq.transport.current_track_step[t] = 0;
        g_seq.transport.track_step_count[t] = 0;
        g_seq.transport.current_pattern[t] = 0;
        g_seq.transport.queued_pattern[t] = NO_QUEUED_PATTERN;
        g_seq.active_pattern[t] = 0;
    }
    g_seq.transport.song_position = 0;
    g_seq.transport.song_repeat_count = 0;
    g_seq.transport.step_triggered = false;

    g_seq.track_mutes = 0;

    // Initialize all ReBirth-style pattern locations: banks A-D, slots 1-8.
    for (int i = 0; i < MAX_PATTERNS; i++) {
        pattern_init(&g_seq.patterns[i], i);
    }

    // Initialize song
    g_seq.song.length = 0;
    g_seq.song.end_mode = SONG_END_LOOP;
    g_seq.song.name[0] = '\0';

    // Set up a basic demo pattern on bank A, slot 1: the kick on its own DRUM
    // track and a kit on the same channel with its BD lane empty, so no other
    // track plays one of the kit's notes on its channel (a kit release would cut it).
    pattern_t *p = &g_seq.patterns[0];
    p->tracks[0].type = TRACK_TYPE_DRUM;
    p->tracks[0].drum_note = 36;  // GM Kick
    p->tracks[0].midi_channel = 9; // GM drums
    for (int s = 0; s < 16; s += 4) {
        pattern_set_step(p, 0, s, 36, 100, STEP_FLAG_ACTIVE);
    }

    // Track 1: a kit, its settings at the defaults sequencer_init() sets.
    // SD on steps 5 and 13, CH on every step.
    p->tracks[1].type = TRACK_TYPE_KIT;
    p->tracks[1].midi_channel = 9;
    for (int s = 0; s < 16; s++) {
        uint32_t word = kit_edit(0, KIT_LANE_CH, KIT_NOTE);
        if (s == 4 || s == 12) word = kit_edit(word, KIT_LANE_SD, KIT_NOTE);
        p->kit_steps[1][s] = word;
    }

    // Routing belongs to the track: every location gets location 0's config.
    for (int i = 1; i < MAX_PATTERNS; i++) {
        for (int t = 0; t < NUM_TRACKS; t++) {
            g_seq.patterns[i].tracks[t] = p->tracks[t];
        }
    }
}

// ─── Main (Core 0) ─────────────────────────────────────────────

int main(void) {
    stdio_init_all();

    // Initialize subsystems
    init_defaults();
    midi_init();
    cv_gate_init();
    display_init();
    sequencer_init();   // Claims the playback lock; ui_init's session_init takes it
    ui_init();

    // Launch sequencer engine on Core 1
    multicore_launch_core1(sequencer_core1_entry);

    // Core 0: UI loop
    while (true) {
        ui_update();

        // Refresh display at ~30fps
        static uint32_t last_display_ms = 0;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_display_ms >= 33) {
            last_display_ms = now;
            display_clear();
            ui_render();
            display_update();
        }
    }
}
