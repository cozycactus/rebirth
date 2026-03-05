#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "sequencer.h"

// ─── Internal state ─────────────────────────────────────────────

static uint32_t timer_alarm_num;
static uint64_t pulse_interval_us;

// Track which notes are currently sounding (for note-off before re-trigger)
static uint8_t active_notes[NUM_TRACKS];
static bool    note_is_on[NUM_TRACKS];

// Gate timing: count down pulses until gate off
static uint8_t gate_off_countdown[NUM_CV_CHANNELS];

// ─── Helpers ────────────────────────────────────────────────────

static uint64_t bpm_to_pulse_interval_us(float bpm) {
    // microseconds per pulse = 60,000,000 / (BPM * PPQN)
    return (uint64_t)(60000000.0f / (bpm * (float)PPQN));
}

// Helper: get the pattern_t currently playing
static inline const pattern_t *get_playing_pattern(void) {
    return &g_seq.patterns[g_seq.transport.current_pattern]
                .variation[g_seq.transport.current_variation];
}

// Immediate note-off (used by transport_stop)
static void send_note_off_if_active(uint8_t track) {
    if (!note_is_on[track]) return;

    const track_config_t *tc = &get_playing_pattern()->tracks[track];

    if (tc->output_mode == OUTPUT_MIDI_ONLY || tc->output_mode == OUTPUT_BOTH) {
        midi_send_note_off(tc->midi_channel, active_notes[track]);
    }
    if ((tc->output_mode == OUTPUT_CV_GATE || tc->output_mode == OUTPUT_BOTH)
        && tc->cv_channel < NUM_CV_CHANNELS) {
        gate_off(tc->cv_channel);
    }
    note_is_on[track] = false;
}

// ─── Two-phase commit state ─────────────────────────────────────
// Phase 1 (collect): prepare_note_off / prepare_trigger queue MIDI
//   into a buffer and stage CV/gate changes.
// Phase 2 (commit): all gates fire simultaneously, then MIDI flushes.

static uint32_t pending_gate_on;    // bitmask of CV channels to set high
static uint32_t pending_gate_off;   // bitmask of CV channels to set low

static void prepare_note_off(uint8_t track) {
    if (!note_is_on[track]) return;

    const track_config_t *tc = &get_playing_pattern()->tracks[track];

    if (tc->output_mode == OUTPUT_MIDI_ONLY || tc->output_mode == OUTPUT_BOTH) {
        midi_buf_note_off(tc->midi_channel, active_notes[track]);
    }
    if ((tc->output_mode == OUTPUT_CV_GATE || tc->output_mode == OUTPUT_BOTH)
        && tc->cv_channel < NUM_CV_CHANNELS) {
        pending_gate_off |= (1u << tc->cv_channel);
    }
    note_is_on[track] = false;
}

static void prepare_trigger(uint8_t track, const step_t *step) {
    const pattern_t *pat = get_playing_pattern();
    const track_config_t *tc = &pat->tracks[track];

    // Check global mute bitmask
    if (g_seq.track_mutes & (1u << track)) return;
    if (tc->muted) return;
    if (!(step->flags & STEP_FLAG_ACTIVE)) return;

    uint8_t note = (tc->type == TRACK_TYPE_DRUM) ? tc->drum_note : step->note;
    uint8_t vel  = (step->flags & STEP_FLAG_ACCENT) ? 127 : step->velocity;

    // Tie — keep the note ringing, no retrigger
    if (step->flags & STEP_FLAG_TIE && note_is_on[track] && active_notes[track] == note) {
        return;
    }

    // Turn off previous note (unless slide)
    if (!(step->flags & STEP_FLAG_SLIDE)) {
        prepare_note_off(track);
    }

    // Queue MIDI messages
    if (tc->output_mode == OUTPUT_MIDI_ONLY || tc->output_mode == OUTPUT_BOTH) {
        if (step->flags & STEP_FLAG_SLIDE && note_is_on[track]) {
            midi_buf_note_on(tc->midi_channel, note, vel);
            midi_buf_note_off(tc->midi_channel, active_notes[track]);
        } else {
            midi_buf_note_on(tc->midi_channel, note, vel);
        }
    }

    // Stage CV/Gate
    if ((tc->output_mode == OUTPUT_CV_GATE || tc->output_mode == OUTPUT_BOTH)
        && tc->cv_channel < NUM_CV_CHANNELS) {
        cv_set_note(tc->cv_channel, note);
        pending_gate_on |= (1u << tc->cv_channel);

        uint8_t gate_pulses = (tc->gate_length_pct * PULSES_PER_STEP) / 100;
        if (gate_pulses < 1) gate_pulses = 1;
        gate_off_countdown[tc->cv_channel] = gate_pulses;
    }

    active_notes[track] = note;
    note_is_on[track] = true;
}

static void commit_step(void) {
    // 1. All gates change simultaneously (single GPIO register writes)
    gate_commit(pending_gate_on, pending_gate_off);
    pending_gate_on = 0;
    pending_gate_off = 0;

    // 2. Flush all buffered MIDI messages
    midi_buf_flush();
}

// ─── Song mode advancement ──────────────────────────────────────

static void advance_song_position(void) {
    transport_t *t = (transport_t *)&g_seq.transport;
    const song_t *s = &g_seq.song;

    if (t->song_repeat_count > 1) {
        t->song_repeat_count--;
        return;
    }

    // Move to next song entry
    t->song_position++;

    if (t->song_position >= s->length) {
        // Song finished
        if (s->end_mode == SONG_END_STOP) {
            t->state = TRANSPORT_STOPPED;
            for (int i = 0; i < NUM_TRACKS; i++) {
                if (note_is_on[i]) {
                    const track_config_t *tc = &get_playing_pattern()->tracks[i];
                    if (tc->output_mode == OUTPUT_MIDI_ONLY || tc->output_mode == OUTPUT_BOTH)
                        midi_send_note_off(tc->midi_channel, active_notes[i]);
                    note_is_on[i] = false;
                }
            }
            cv_gate_all_off();
            midi_send_stop();
            return;
        }
        // SONG_END_LOOP
        t->song_position = 0;
    }

    // Skip entries with 0 repeats
    uint16_t start = t->song_position;
    while (s->entries[t->song_position].repeats == 0) {
        t->song_position++;
        if (t->song_position >= s->length) t->song_position = 0;
        if (t->song_position == start) {
            // All entries are 0-repeat — stop to avoid infinite loop
            t->state = TRANSPORT_STOPPED;
            midi_send_stop();
            return;
        }
    }

    t->current_pattern = s->entries[t->song_position].pattern_index;
    t->current_variation = s->entries[t->song_position].variation;
    t->song_repeat_count = s->entries[t->song_position].repeats;
}

// ─── Pattern queue: switch at end of current pattern ────────────

static void check_pattern_queue(void) {
    transport_t *t = (transport_t *)&g_seq.transport;
    if (t->queued_pattern == 0xFF) return;

    t->current_pattern = t->queued_pattern;
    t->current_variation = t->queued_variation;
    t->queued_pattern = 0xFF;
    t->queued_variation = 0;
}

// ─── Timer ISR — called every pulse (96 PPQN) ──────────────────

static void __isr __time_critical_func(sequencer_timer_isr)(void) {
    // Clear the alarm IRQ
    hw_clear_bits(&timer_hw->intr, 1u << timer_alarm_num);

    // Schedule next alarm
    uint64_t target = timer_hw->alarm[timer_alarm_num] + pulse_interval_us;
    timer_hw->alarm[timer_alarm_num] = (uint32_t)target;

    transport_t *t = (transport_t *)&g_seq.transport;
    if (t->state != TRANSPORT_PLAYING) return;

    // Send MIDI clock on every pulse
    midi_send_clock();

    // Decrement gate-off countdowns
    for (int ch = 0; ch < NUM_CV_CHANNELS; ch++) {
        if (gate_off_countdown[ch] > 0) {
            gate_off_countdown[ch]--;
            if (gate_off_countdown[ch] == 0) {
                gate_off(ch);
            }
        }
    }

    // Check if we're at a step boundary
    uint32_t pulse_in_pattern = t->pulse_count % (PULSES_PER_STEP * STEPS_PER_PATTERN);
    uint8_t new_step = pulse_in_pattern / PULSES_PER_STEP;

    // Apply swing: delay even-numbered steps
    // Swing 50 = straight, swing > 50 = shuffle
    // For simplicity, shift even steps by (swing-50)% of a step duration
    bool is_step_boundary = (pulse_in_pattern % PULSES_PER_STEP) == 0;

    if (is_step_boundary && new_step != t->current_step) {
        t->current_step = new_step;
        t->step_triggered = true;

        const pattern_t *pat = get_playing_pattern();

        // ── Phase 1: Collect ── prepare all outputs without sending
        midi_buf_clear();
        pending_gate_on = 0;
        pending_gate_off = 0;

        for (int tr = 0; tr < NUM_TRACKS; tr++) {
            const step_t *step = &pat->steps[tr][new_step];
            if (!(step->flags & STEP_FLAG_ACTIVE) || (g_seq.track_mutes & (1u << tr))) {
                prepare_note_off(tr);
            } else {
                prepare_trigger(tr, step);
            }
        }

        // ── Phase 2: Commit ── all outputs fire together
        commit_step();

        // End of pattern?
        if (new_step == 0 && t->pulse_count > 0) {
            if (t->mode == PLAY_MODE_SONG) {
                advance_song_position();
            } else {
                // Pattern mode: check for queued pattern switch
                check_pattern_queue();
            }
        }
    }

    t->pulse_count++;
}

// ─── Core 1 Entry ───────────────────────────────────────────────

void sequencer_core1_entry(void) {
    // Initialize timer alarm for sequencer
    timer_alarm_num = 0;  // Use alarm 0

    pulse_interval_us = bpm_to_pulse_interval_us(g_seq.transport.bpm);

    // Clear active note state
    for (int i = 0; i < NUM_TRACKS; i++) {
        active_notes[i] = 0;
        note_is_on[i] = false;
    }
    for (int i = 0; i < NUM_CV_CHANNELS; i++) {
        gate_off_countdown[i] = 0;
    }

    // Set up the hardware alarm
    hw_set_bits(&timer_hw->inte, 1u << timer_alarm_num);
    irq_set_exclusive_handler(TIMER_IRQ_0, sequencer_timer_isr);
    irq_set_enabled(TIMER_IRQ_0, true);

    // Arm the first alarm
    uint64_t now = timer_hw->timerawl;
    timer_hw->alarm[timer_alarm_num] = (uint32_t)(now + pulse_interval_us);

    // Core 1 idle loop — the real work happens in the ISR
    while (true) {
        __wfi();  // Wait for interrupt — saves power
    }
}

// ─── Transport Controls (called from Core 0) ───────────────────

void transport_start(void) {
    transport_t *t = (transport_t *)&g_seq.transport;

    // Reset position
    t->pulse_count = 0;
    t->current_step = 0;
    t->queued_pattern = 0xFF;
    t->queued_variation = 0;
    t->current_variation = 0;

    if (t->mode == PLAY_MODE_SONG && g_seq.song.length > 0) {
        t->song_position = 0;
        t->current_pattern = g_seq.song.entries[0].pattern_index;
        t->current_variation = g_seq.song.entries[0].variation;
        t->song_repeat_count = g_seq.song.entries[0].repeats;
    }

    midi_send_start();
    t->state = TRANSPORT_PLAYING;
}

void transport_stop(void) {
    transport_t *t = (transport_t *)&g_seq.transport;
    t->state = TRANSPORT_STOPPED;

    // All notes off
    for (int i = 0; i < NUM_TRACKS; i++) {
        send_note_off_if_active(i);
    }
    cv_gate_all_off();
    midi_send_stop();
}

void transport_continue(void) {
    transport_t *t = (transport_t *)&g_seq.transport;
    midi_send_continue();
    t->state = TRANSPORT_PLAYING;
}

void transport_set_bpm(float bpm) {
    if (bpm < MIN_BPM) bpm = MIN_BPM;
    if (bpm > MAX_BPM) bpm = MAX_BPM;
    g_seq.transport.bpm = bpm;
    pulse_interval_us = bpm_to_pulse_interval_us(bpm);
}
