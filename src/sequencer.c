#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/timer.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "sequencer.h"
#include <string.h>

// ─── Internal state ─────────────────────────────────────────────

static uint32_t timer_alarm_num;
static uint32_t pulse_interval_us;

typedef struct {
    uint8_t flags;
    uint8_t note;
    uint8_t velocity;
    uint8_t midi_channel;
    uint8_t output_mode;
    uint8_t cv_channel;
    uint8_t gate_pulses;
} prepared_track_event_t;

typedef struct {
    uint8_t length;
    prepared_track_event_t step[STEPS_PER_PATTERN];
} prepared_track_pattern_t;

typedef struct {
    prepared_track_pattern_t track[NUM_TRACKS];
} prepared_playback_t;

typedef struct {
    uint8_t note;
    uint8_t midi_channel;
    uint8_t output_mode;
    uint8_t cv_channel;
} active_note_state_t;

// Track which notes are currently sounding (for note-off before re-trigger)
static uint8_t active_notes[NUM_TRACKS];
static bool    note_is_on[NUM_TRACKS];
static active_note_state_t active_note_state[NUM_TRACKS];

// Gate timing: count down pulses until gate off
static uint8_t gate_off_countdown[NUM_CV_CHANNELS];

static prepared_playback_t prepared_playback;
static prepared_track_pattern_t prepared_queued_tracks[NUM_TRACKS];
static prepared_track_pattern_t prepared_refresh_tracks[NUM_TRACKS];
static volatile uint8_t prepared_refresh_mask;

static volatile sequencer_jitter_stats_t jitter_stats;
static uint32_t jitter_last_irq_us;
static bool jitter_have_last_irq;
static volatile bool jitter_reset_pending;

// ─── Helpers ────────────────────────────────────────────────────

static uint32_t bpm_to_pulse_interval_us(float bpm) {
    // microseconds per pulse = 60,000,000 / (BPM * PPQN)
    return (uint32_t)(60000000.0f / (bpm * (float)PPQN));
}

static uint32_t abs_i32(int32_t value) {
    return (value < 0) ? (uint32_t)(-value) : (uint32_t)value;
}

static void jitter_reset_now(void) {
    jitter_stats.samples = 0;
    jitter_stats.last_irq_latency_us = 0;
    jitter_stats.min_irq_latency_us = 0;
    jitter_stats.max_irq_latency_us = 0;
    jitter_stats.last_period_error_us = 0;
    jitter_stats.min_period_error_us = 0;
    jitter_stats.max_period_error_us = 0;
    jitter_stats.max_abs_period_error_us = 0;
    jitter_stats.last_isr_duration_us = 0;
    jitter_stats.max_isr_duration_us = 0;
    jitter_stats.overruns = 0;
    jitter_last_irq_us = 0;
    jitter_have_last_irq = false;
    jitter_reset_pending = false;
}

static void jitter_record_entry(uint32_t now_us, uint32_t scheduled_us) {
    if (jitter_reset_pending) {
        jitter_reset_now();
    }

    uint32_t samples = jitter_stats.samples;
    int32_t latency_us = (int32_t)(now_us - scheduled_us);

    jitter_stats.last_irq_latency_us = latency_us;
    if (samples == 0) {
        jitter_stats.min_irq_latency_us = latency_us;
        jitter_stats.max_irq_latency_us = latency_us;
    } else {
        if (latency_us < jitter_stats.min_irq_latency_us) {
            jitter_stats.min_irq_latency_us = latency_us;
        }
        if (latency_us > jitter_stats.max_irq_latency_us) {
            jitter_stats.max_irq_latency_us = latency_us;
        }
    }

    if (latency_us > (int32_t)pulse_interval_us) {
        jitter_stats.overruns++;
    }

    if (jitter_have_last_irq) {
        int32_t period_us = (int32_t)(now_us - jitter_last_irq_us);
        int32_t period_error_us = period_us - (int32_t)pulse_interval_us;
        uint32_t abs_period_error_us = abs_i32(period_error_us);

        jitter_stats.last_period_error_us = period_error_us;
        if (samples <= 1) {
            jitter_stats.min_period_error_us = period_error_us;
            jitter_stats.max_period_error_us = period_error_us;
        } else {
            if (period_error_us < jitter_stats.min_period_error_us) {
                jitter_stats.min_period_error_us = period_error_us;
            }
            if (period_error_us > jitter_stats.max_period_error_us) {
                jitter_stats.max_period_error_us = period_error_us;
            }
        }
        if (abs_period_error_us > jitter_stats.max_abs_period_error_us) {
            jitter_stats.max_abs_period_error_us = abs_period_error_us;
        }
    }

    jitter_last_irq_us = now_us;
    jitter_have_last_irq = true;
    jitter_stats.samples = samples + 1;
}

static void jitter_record_exit(uint32_t start_us) {
    uint32_t duration_us = (uint32_t)(timer_hw->timerawl - start_us);
    jitter_stats.last_isr_duration_us = duration_us;
    if (duration_us > jitter_stats.max_isr_duration_us) {
        jitter_stats.max_isr_duration_us = duration_us;
    }
}

static inline uint8_t clamp_pattern_steps(uint8_t steps) {
    if (steps < 1) return 1;
    if (steps > STEPS_PER_PATTERN) return STEPS_PER_PATTERN;
    return steps;
}

static void fill_prepared_track(prepared_track_pattern_t *dst,
                                uint8_t track,
                                uint8_t pattern_index) {
    uint32_t lock_state = seq_lock();
    const pattern_t *pat = &g_seq.patterns[pattern_index];
    dst->length = pattern_get_track_length(pat, track);

    for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
        const step_t *src_step = &pat->steps[track][step];
        const track_config_t *tc = &pat->tracks[track];
        prepared_track_event_t *dst_event = &dst->step[step];

        dst_event->flags = src_step->flags;
        if (tc->muted || step >= dst->length) {
            dst_event->flags &= (uint8_t)~STEP_FLAG_ACTIVE;
        }

        dst_event->note = (tc->type == TRACK_TYPE_DRUM)
                          ? tc->drum_note
                          : src_step->note;
        dst_event->velocity = (src_step->flags & STEP_FLAG_ACCENT)
                              ? 127
                              : src_step->velocity;
        dst_event->midi_channel = tc->midi_channel;
        dst_event->output_mode = (uint8_t)tc->output_mode;
        dst_event->cv_channel = tc->cv_channel;

        uint8_t gate_pulses = (tc->gate_length_pct * PULSES_PER_STEP) / 100;
        if (gate_pulses < 1) gate_pulses = 1;
        dst_event->gate_pulses = gate_pulses;
    }
    seq_unlock(lock_state);
}

static void reset_prepared_buffers_for_current(void) {
    uint8_t current_patterns[NUM_TRACKS];
    uint32_t lock_state = seq_lock();
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        current_patterns[track] = g_seq.transport.current_pattern[track];
    }
    seq_unlock(lock_state);

    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        fill_prepared_track(&prepared_playback.track[track], track, current_patterns[track]);
    }

    lock_state = seq_lock();
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        g_seq.transport.queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    prepared_refresh_mask = 0;
    seq_unlock(lock_state);
}

static void refresh_prepared_tracks_for_current(uint8_t track_mask) {
    uint8_t current_patterns[NUM_TRACKS];
    uint32_t lock_state = seq_lock();
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        current_patterns[track] = g_seq.transport.current_pattern[track];
    }
    seq_unlock(lock_state);

    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        uint8_t mask = (uint8_t)(1u << track);
        if (track_mask & mask) {
            fill_prepared_track(&prepared_playback.track[track],
                                track,
                                current_patterns[track]);
        }
    }

    lock_state = seq_lock();
    prepared_refresh_mask &= (uint8_t)~track_mask;
    seq_unlock(lock_state);
}

static void apply_track_refresh_if_ready(uint8_t track) {
    uint8_t mask = (uint8_t)(1u << track);
    if (!(prepared_refresh_mask & mask)) return;

    uint32_t lock_state = seq_lock();
    if (prepared_refresh_mask & mask) {
        prepared_playback.track[track] = prepared_refresh_tracks[track];
        prepared_refresh_mask &= (uint8_t)~mask;
    }
    seq_unlock(lock_state);
}

static void apply_track_queue_if_ready(uint8_t track) {
    transport_t *t = (transport_t *)&g_seq.transport;
    if (t->queued_pattern[track] == NO_QUEUED_PATTERN) return;

    uint32_t lock_state = seq_lock();
    uint8_t queued = t->queued_pattern[track];
    if (queued != NO_QUEUED_PATTERN) {
        prepared_playback.track[track] = prepared_queued_tracks[track];
        prepared_refresh_mask &= (uint8_t)~(1u << track);
        t->current_pattern[track] = queued;
        t->queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    seq_unlock(lock_state);
}

// Immediate note-off (used by transport_stop)
static void send_note_off_if_active(uint8_t track) {
    if (!note_is_on[track]) return;

    const active_note_state_t *state = &active_note_state[track];

    if (state->output_mode == OUTPUT_MIDI_ONLY || state->output_mode == OUTPUT_BOTH) {
        midi_send_note_off(state->midi_channel, active_notes[track]);
    }
    if ((state->output_mode == OUTPUT_CV_GATE || state->output_mode == OUTPUT_BOTH)
        && state->cv_channel < NUM_CV_CHANNELS) {
        gate_off(state->cv_channel);
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

    const active_note_state_t *state = &active_note_state[track];

    if (state->output_mode == OUTPUT_MIDI_ONLY || state->output_mode == OUTPUT_BOTH) {
        midi_buf_note_off(state->midi_channel, active_notes[track]);
    }
    if ((state->output_mode == OUTPUT_CV_GATE || state->output_mode == OUTPUT_BOTH)
        && state->cv_channel < NUM_CV_CHANNELS) {
        pending_gate_off |= (1u << state->cv_channel);
    }
    note_is_on[track] = false;
}

static void prepare_trigger(uint8_t track, const prepared_track_event_t *event,
                            uint8_t track_mutes) {
    // Check global mute bitmask
    if (track_mutes & (1u << track)) return;
    if (!(event->flags & STEP_FLAG_ACTIVE)) return;

    uint8_t note = event->note;
    uint8_t vel = event->velocity;

    // Tie — keep the note ringing, no retrigger
    if (event->flags & STEP_FLAG_TIE && note_is_on[track] && active_notes[track] == note) {
        return;
    }

    // Turn off previous note (unless slide)
    if (!(event->flags & STEP_FLAG_SLIDE)) {
        prepare_note_off(track);
    }

    // Queue MIDI messages
    if (event->output_mode == OUTPUT_MIDI_ONLY || event->output_mode == OUTPUT_BOTH) {
        if (event->flags & STEP_FLAG_SLIDE && note_is_on[track]) {
            midi_buf_note_on(event->midi_channel, note, vel);
            midi_buf_note_off(active_note_state[track].midi_channel, active_notes[track]);
        } else {
            midi_buf_note_on(event->midi_channel, note, vel);
        }
    }

    // Stage CV/Gate
    if ((event->output_mode == OUTPUT_CV_GATE || event->output_mode == OUTPUT_BOTH)
        && event->cv_channel < NUM_CV_CHANNELS) {
        cv_set_note(event->cv_channel, note);
        pending_gate_on |= (1u << event->cv_channel);
        gate_off_countdown[event->cv_channel] = event->gate_pulses;
    }

    active_notes[track] = note;
    active_note_state[track].note = note;
    active_note_state[track].midi_channel = event->midi_channel;
    active_note_state[track].output_mode = event->output_mode;
    active_note_state[track].cv_channel = event->cv_channel;
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

static void clear_runtime_note_state(void) {
    for (int i = 0; i < NUM_TRACKS; i++) {
        active_notes[i] = 0;
        note_is_on[i] = false;
        active_note_state[i].note = 0;
        active_note_state[i].midi_channel = 0;
        active_note_state[i].output_mode = OUTPUT_MIDI_ONLY;
        active_note_state[i].cv_channel = 0;
    }
    for (int i = 0; i < NUM_CV_CHANNELS; i++) {
        gate_off_countdown[i] = 0;
    }
}

static void stop_playback_outputs(void) {
    midi_buf_clear();
    for (int i = 0; i < NUM_TRACKS; i++) {
        send_note_off_if_active(i);
    }
    cv_gate_all_off();
    clear_runtime_note_state();
    midi_send_stop();
}

// ─── Song mode advancement ──────────────────────────────────────

static uint8_t advance_song_position(void) {
    bool should_stop = false;
    uint8_t changed_mask = 0;
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    const song_t *s = &g_seq.song;

    if (s->length == 0) {
        t->state = TRANSPORT_STOPPED;
        seq_unlock(lock_state);
        stop_playback_outputs();
        return 0;
    }

    if (t->song_position >= s->length) {
        t->song_position = 0;
    }

    if (t->song_repeat_count > 1) {
        t->song_repeat_count--;
        seq_unlock(lock_state);
        return 0;
    }

    // Move to next song entry
    t->song_position++;

    if (t->song_position >= s->length) {
        // Song finished
        if (s->end_mode == SONG_END_STOP) {
            t->state = TRANSPORT_STOPPED;
            should_stop = true;
            goto done;
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
            should_stop = true;
            goto done;
        }
    }

    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        uint8_t next_pattern = s->entries[t->song_position].pattern_index[track];
        if (t->current_pattern[track] != next_pattern) {
            changed_mask |= (uint8_t)(1u << track);
            t->current_pattern[track] = next_pattern;
        }
    }
    t->song_repeat_count = s->entries[t->song_position].repeats;

done:
    seq_unlock(lock_state);
    if (should_stop) {
        stop_playback_outputs();
    }
    return changed_mask;
}

// ─── Timer ISR — called every pulse (96 PPQN) ──────────────────

static void __isr __time_critical_func(sequencer_timer_isr)(void) {
    uint32_t isr_start_us = timer_hw->timerawl;
    uint32_t scheduled_us = timer_hw->alarm[timer_alarm_num];
    gpio_put(JITTER_PROBE_PIN, 1);
    jitter_record_entry(isr_start_us, scheduled_us);

    // Clear the alarm IRQ
    hw_clear_bits(&timer_hw->intr, 1u << timer_alarm_num);

    // Schedule next alarm
    uint64_t target = timer_hw->alarm[timer_alarm_num] + pulse_interval_us;
    timer_hw->alarm[timer_alarm_num] = (uint32_t)target;

    transport_t *t = (transport_t *)&g_seq.transport;
    if (t->state != TRANSPORT_PLAYING) goto isr_done;

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

    prepared_playback_t *prepared = &prepared_playback;
    uint32_t pulse_in_step = t->pulse_count % PULSES_PER_STEP;
    uint32_t global_step_count = t->pulse_count / PULSES_PER_STEP;

    // Swing is stored per pattern but playback timing is still straight.
    bool is_step_boundary = pulse_in_step == 0;

    if (is_step_boundary) {
        if (t->mode == PLAY_MODE_SONG &&
            t->pulse_count > 0 &&
            (global_step_count % STEPS_PER_PATTERN) == 0) {
            uint8_t changed_mask = advance_song_position();
            if (t->state != TRANSPORT_PLAYING) {
                goto isr_done;
            }
            for (uint8_t tr = 0; tr < NUM_TRACKS; tr++) {
                if (changed_mask & (1u << tr)) {
                    t->track_step_count[tr] = 0;
                }
            }
            if (changed_mask) {
                refresh_prepared_tracks_for_current(changed_mask);
                prepared = &prepared_playback;
            }
            t->pulse_count = 0;
            global_step_count = 0;
        }

        t->current_step = (uint8_t)(global_step_count % STEPS_PER_PATTERN);
        t->step_triggered = true;

        uint8_t track_mutes = g_seq.track_mutes;

        // ── Phase 1: Collect ── prepare all outputs without sending
        midi_buf_clear();
        pending_gate_on = 0;
        pending_gate_off = 0;

        for (int tr = 0; tr < NUM_TRACKS; tr++) {
            apply_track_refresh_if_ready((uint8_t)tr);
            prepared = &prepared_playback;

            uint8_t track_len = clamp_pattern_steps(prepared->track[tr].length);
            if (t->mode == PLAY_MODE_PATTERN &&
                t->track_step_count[tr] > 0 &&
                (t->track_step_count[tr] % track_len) == 0) {
                apply_track_queue_if_ready((uint8_t)tr);
                prepared = &prepared_playback;
                t->track_step_count[tr] = 0;
                track_len = clamp_pattern_steps(prepared->track[tr].length);
            }

            uint8_t track_step = (uint8_t)(t->track_step_count[tr] % track_len);
            t->current_track_step[tr] = track_step;
            const prepared_track_event_t *event = &prepared->track[tr].step[track_step];
            if (!(event->flags & STEP_FLAG_ACTIVE) || (track_mutes & (1u << tr))) {
                prepare_note_off(tr);
            } else {
                prepare_trigger(tr, event, track_mutes);
            }
            t->track_step_count[tr]++;
        }

        // ── Phase 2: Commit ── all outputs fire together
        commit_step();

    }

    t->pulse_count++;

isr_done:
    jitter_record_exit(isr_start_us);
    gpio_put(JITTER_PROBE_PIN, 0);
}

// ─── Core 1 Entry ───────────────────────────────────────────────

void sequencer_core1_entry(void) {
    // Initialize timer alarm for sequencer
    timer_alarm_num = 0;  // Use alarm 0

    pulse_interval_us = bpm_to_pulse_interval_us(g_seq.transport.bpm);
    jitter_reset_now();

    gpio_init(JITTER_PROBE_PIN);
    gpio_set_dir(JITTER_PROBE_PIN, GPIO_OUT);
    gpio_put(JITTER_PROBE_PIN, 0);

    clear_runtime_note_state();

    // Set up the hardware alarm
    hw_set_bits(&timer_hw->inte, 1u << timer_alarm_num);
    irq_set_exclusive_handler(TIMER_IRQ_0, sequencer_timer_isr);
    irq_set_enabled(TIMER_IRQ_0, true);

    // Arm the first alarm
    uint64_t now = timer_hw->timerawl;
    timer_hw->alarm[timer_alarm_num] = (uint32_t)(now + pulse_interval_us);

    // Allow Core 0 to pause us during flash writes
    multicore_lockout_victim_init();

    // Core 1 idle loop — the real work happens in the ISR
    while (true) {
        __wfi();  // Wait for interrupt — saves power
    }
}

// ─── Transport Controls (called from Core 0) ───────────────────

void transport_start(void) {
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;

    // Reset position
    t->pulse_count = 0;
    t->current_step = 0;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        t->track_step_count[track] = 0;
        t->current_track_step[track] = 0;
        t->queued_pattern[track] = NO_QUEUED_PATTERN;
    }

    if (t->mode == PLAY_MODE_SONG && g_seq.song.length > 0) {
        t->song_position = 0;
        memcpy(t->current_pattern,
               g_seq.song.entries[0].pattern_index,
               sizeof(t->current_pattern));
        t->song_repeat_count = g_seq.song.entries[0].repeats;
    }

    seq_unlock(lock_state);

    reset_prepared_buffers_for_current();

    lock_state = seq_lock();
    t = (transport_t *)&g_seq.transport;
    t->state = TRANSPORT_PLAYING;
    seq_unlock(lock_state);
    midi_send_start();
}

void transport_stop(void) {
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    t->state = TRANSPORT_STOPPED;
    seq_unlock(lock_state);

    // All notes off
    stop_playback_outputs();
}

void transport_continue(void) {
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    t->state = TRANSPORT_PLAYING;
    seq_unlock(lock_state);
    midi_send_continue();
}

void transport_set_bpm(float bpm) {
    if (bpm < MIN_BPM) bpm = MIN_BPM;
    if (bpm > MAX_BPM) bpm = MAX_BPM;
    uint32_t lock_state = seq_lock();
    g_seq.transport.bpm = bpm;
    pulse_interval_us = bpm_to_pulse_interval_us(bpm);
    seq_unlock(lock_state);
}

void sequencer_panic(void) {
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    t->state = TRANSPORT_STOPPED;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        t->queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    prepared_refresh_mask = 0;
    seq_unlock(lock_state);

    midi_clear_output_queue();
    stop_playback_outputs();
    midi_send_all_notes_off();
}

void sequencer_prepare_queued_pattern(uint8_t track, uint8_t pattern_index) {
    if (track >= NUM_TRACKS) return;
    if (pattern_index >= MAX_PATTERNS) return;

    fill_prepared_track(&prepared_queued_tracks[track], track, pattern_index);
}

void sequencer_refresh_track_playback(uint8_t track, uint8_t pattern_index) {
    if (track >= NUM_TRACKS) return;
    if (pattern_index >= MAX_PATTERNS) return;

    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    bool is_current = t->current_pattern[track] == pattern_index;
    bool is_queued = t->queued_pattern[track] == pattern_index;
    seq_unlock(lock_state);

    if (!is_current && !is_queued) return;

    prepared_track_pattern_t updated;
    fill_prepared_track(&updated, track, pattern_index);

    lock_state = seq_lock();
    t = (transport_t *)&g_seq.transport;
    if (t->current_pattern[track] == pattern_index) {
        prepared_refresh_tracks[track] = updated;
        prepared_refresh_mask |= (uint8_t)(1u << track);
    }
    if (t->queued_pattern[track] == pattern_index) {
        prepared_queued_tracks[track] = updated;
    }
    seq_unlock(lock_state);
}

void sequencer_get_jitter_stats(sequencer_jitter_stats_t *out) {
    if (!out) return;

    out->samples = jitter_stats.samples;
    out->last_irq_latency_us = jitter_stats.last_irq_latency_us;
    out->min_irq_latency_us = jitter_stats.min_irq_latency_us;
    out->max_irq_latency_us = jitter_stats.max_irq_latency_us;
    out->last_period_error_us = jitter_stats.last_period_error_us;
    out->min_period_error_us = jitter_stats.min_period_error_us;
    out->max_period_error_us = jitter_stats.max_period_error_us;
    out->max_abs_period_error_us = jitter_stats.max_abs_period_error_us;
    out->last_isr_duration_us = jitter_stats.last_isr_duration_us;
    out->max_isr_duration_us = jitter_stats.max_isr_duration_us;
    out->overruns = jitter_stats.overruns;
}

void sequencer_reset_jitter_stats(void) {
    jitter_reset_pending = true;
}
