#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "sequencer.h"

// ─── Global shared state ────────────────────────────────────────

sequencer_state_t g_seq;

// ─── Initialization ─────────────────────────────────────────────

static void init_defaults(void) {
    // Claim a hardware spinlock for shared data access
    g_seq.spinlock_num = spin_lock_claim_unused(true);

    // Transport defaults
    g_seq.transport.state = TRANSPORT_STOPPED;
    g_seq.transport.mode = PLAY_MODE_PATTERN;
    g_seq.transport.bpm = DEFAULT_BPM;
    g_seq.transport.pulse_count = 0;
    g_seq.transport.current_step = 0;
    g_seq.transport.current_pattern = 0;
    g_seq.transport.current_variation = 0;
    g_seq.transport.queued_pattern = 0xFF;
    g_seq.transport.queued_variation = 0;
    g_seq.transport.song_position = 0;
    g_seq.transport.song_repeat_count = 0;
    g_seq.transport.step_triggered = false;

    g_seq.active_pattern = 0;
    g_seq.active_variation = 0;
    g_seq.track_mutes = 0;

    // Initialize all patterns with defaults (both A and B)
    for (int i = 0; i < MAX_PATTERNS; i++) {
        pattern_init(&g_seq.patterns[i].variation[0], i);
        pattern_init(&g_seq.patterns[i].variation[1], i);
    }

    // Initialize song
    g_seq.song.length = 0;
    g_seq.song.end_mode = SONG_END_LOOP;
    g_seq.song.name[0] = '\0';

    // Set up a basic demo pattern on pattern 0, variation A
    pattern_t *p = &g_seq.patterns[0].variation[0];
    p->tracks[0].type = TRACK_TYPE_DRUM;
    p->tracks[0].drum_note = 36;  // GM Kick
    p->tracks[0].midi_channel = 9; // GM drums
    for (int s = 0; s < 16; s += 4) {
        pattern_set_step(p, 0, s, 36, 100, STEP_FLAG_ACTIVE);
    }

    // Track 1: Snare on 5, 13
    p->tracks[1].type = TRACK_TYPE_DRUM;
    p->tracks[1].drum_note = 38;  // GM Snare
    p->tracks[1].midi_channel = 9;
    pattern_set_step(p, 1, 4, 38, 100, STEP_FLAG_ACTIVE);
    pattern_set_step(p, 1, 12, 38, 100, STEP_FLAG_ACTIVE);

    // Track 2: Closed hi-hat on every step
    p->tracks[2].type = TRACK_TYPE_DRUM;
    p->tracks[2].drum_note = 42;  // GM Closed HH
    p->tracks[2].midi_channel = 9;
    for (int s = 0; s < 16; s++) {
        pattern_set_step(p, 2, s, 42, 80, STEP_FLAG_ACTIVE);
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
