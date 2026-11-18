#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/timer.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "sequencer.h"
#include "kit.h"
#include "midi_song.h"
#include <string.h>
#include <math.h>

// Host builds check playback invariants; on the target the check compiles away (no
// flash call from the ISR path that runs from RAM).
#if defined(__arm__)
#define PLAYBACK_INVARIANT(condition) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#define PLAYBACK_INVARIANT(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: invariant %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)
#endif

// ─── Internal state ─────────────────────────────────────────────

static uint32_t timer_alarm_num;
static uint32_t pulse_interval_us;
static uint32_t playback_spinlock_num;

// Lock order: playback -> shared state / MIDI. UI edits only take shared state.
// In RAM, like sequencer_snapshot, when the compiler keeps them out of line.
static uint32_t __not_in_flash_func(playback_lock)(void) {
    return spin_lock_blocking(spin_lock_instance(playback_spinlock_num));
}

static void __not_in_flash_func(playback_unlock)(uint32_t irq_state) {
    spin_unlock(spin_lock_instance(playback_spinlock_num), irq_state);
}

_Static_assert(PPQN % MIDI_PPQN == 0, "MIDI clock must divide the internal clock");

typedef struct {
    uint8_t flags;
    uint8_t note;
    uint8_t velocity;
    uint8_t midi_channel;
    uint8_t output_mode;
    uint8_t cv_channel;
    uint8_t gate_pulses;
} prepared_track_event_t;

enum { PREPARED_EVENTS = 0, PREPARED_KIT = 1 };

typedef struct {
    uint8_t kind;                    // PREPARED_EVENTS or PREPARED_KIT
    uint8_t length;                  // shared, outside the union
    union {
        prepared_track_event_t step[STEPS_PER_PATTERN];
        struct {
            uint32_t lanes[STEPS_PER_PATTERN]; // zero at and beyond the length, and muted
            uint8_t note[KIT_VOICES];
            uint8_t vel[3];          // normal, accent, max
            uint8_t channel, gate_pulses, flam_pulses;
        } kit;
    };
} prepared_track_pattern_t;

typedef struct {
    prepared_track_pattern_t track[NUM_TRACKS];
} prepared_playback_t;

typedef struct {
    uint8_t note;
    uint8_t flags;
    uint8_t midi_channel;
    uint8_t output_mode;
    uint8_t cv_channel;
} active_note_state_t;

// Track which notes are currently sounding (for note-off before re-trigger)
static uint8_t active_notes[NUM_TRACKS];
static bool    note_is_on[NUM_TRACKS];
static active_note_state_t active_note_state[NUM_TRACKS];

// Note durations share one countdown for MIDI and CV/Gate on each track.
static uint8_t note_off_countdown[NUM_TRACKS];

// What a track playing a kit row has sounding. One release channel per kit track:
// every voice is released at the latest at the next step boundary, before that step's
// hits, so all sounding voices come from one step and share the channel stored at that
// step (play_kit_step checks it before it stores a channel). Each voice keeps the note
// it was sent on. A flam's second hits come before that release and use the same notes
// and channel.
typedef struct {
    uint16_t sounding;               // bit v: voice v
    uint16_t flam;                   // voices whose second hit is due; within sounding
    uint8_t  note[KIT_VOICES];
    uint8_t  channel;                // of every sounding voice: the step's channel
    uint8_t  countdown;              // pulses to the release; 0 = none due
    uint8_t  flam_countdown;         // pulses to the second hits; 0 = none due
    uint8_t  flam_velocity;          // one level above the step's note level
} kit_runtime_t;

static kit_runtime_t kit_runtime[NUM_TRACKS];

static prepared_playback_t prepared_playback;
static prepared_track_pattern_t prepared_patterns[MAX_PATTERNS][NUM_TRACKS];
static uint32_t prepared_revision[MAX_PATTERNS][NUM_TRACKS];
static uint64_t clip_mask[PATTERN_BANKS];
static uint32_t playback_revision[NUM_TRACKS];
static uint8_t playback_pattern[NUM_TRACKS];

static volatile sequencer_jitter_stats_t jitter_stats;
static uint32_t jitter_last_irq_us;
static bool jitter_have_last_irq;
static volatile bool jitter_reset_pending;
static uint32_t jitter_expected_period_us;

// ─── Helpers ────────────────────────────────────────────────────

static uint32_t bpm_to_pulse_interval_us(float bpm) {
    // microseconds per pulse = 60,000,000 / (BPM * PPQN)
    return (uint32_t)(60000000.0f / (bpm * (float)PPQN));
}

static uint32_t abs_i32(int32_t value) {
    return (value < 0) ? 0u - (uint32_t)value : (uint32_t)value;
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
    jitter_stats.flam_drops = 0;
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

    if (latency_us >= (int32_t)jitter_expected_period_us) {
        jitter_stats.overruns++;
    }

    if (jitter_have_last_irq) {
        int32_t period_us = (int32_t)(now_us - jitter_last_irq_us);
        int32_t period_error_us = period_us - (int32_t)jitter_expected_period_us;
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

static bool fill_prepared_track(prepared_track_pattern_t *dst,
                                uint8_t track,
                                uint8_t pattern_index) {
    step_t steps[STEPS_PER_PATTERN];
    uint32_t words[STEPS_PER_PATTERN];
    kit_config_t kit;
    uint32_t lock_state = seq_lock();
    const pattern_t *pat = &g_seq.patterns[pattern_index];
    dst->length = pattern_get_track_length(pat, track);
    track_config_t config = pat->tracks[track];
    // Only what the cell's own type plays.
    bool is_kit = kit_is_cell(pat, track);
    if (is_kit) {
        memcpy(words, pat->kit_steps[track], sizeof(words));
        kit = g_seq.kit_config[track];
    } else {
        memcpy(steps, pat->steps[track], sizeof(steps));
    }
    seq_unlock(lock_state);

    uint8_t gate_pulses = (config.gate_length_pct * PULSES_PER_STEP) / 100;
    if (gate_pulses < 1) gate_pulses = 1;

    if (is_kit) {
        dst->kind = PREPARED_KIT;
        for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
            dst->kit.lanes[step] = (config.muted || step >= dst->length) ? 0 : words[step];
        }
        // Masked here so the ISR builds its release bytes from them as they are.
        for (uint8_t v = 0; v < KIT_VOICES; v++) dst->kit.note[v] = kit.note[v] & 0x7F;
        dst->kit.vel[0] = kit.vel_normal;
        dst->kit.vel[1] = kit.vel_accent;
        dst->kit.vel[2] = kit.vel_max;
        dst->kit.channel = config.midi_channel & 0x0F;
        dst->kit.gate_pulses = gate_pulses;
        dst->kit.flam_pulses = kit.flam_pulses;
        // From the words before the blanking: a stored mute does not hide the clip.
        return kit_cell_has_clip(words, dst->length);
    }

    dst->kind = PREPARED_EVENTS;
    bool has_clip = false;
    for (uint8_t step = 0; step < dst->length; step++) {
        if (steps[step].flags & STEP_FLAG_ACTIVE) has_clip = true;
    }

    for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
        const step_t *src_step = &steps[step];
        const track_config_t *tc = &config;
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
        dst_event->gate_pulses = gate_pulses;
    }
    return has_clip;
}

// Immediate note-off: live mutes, note-off countdowns and a kind change.
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
    note_off_countdown[track] = 0;
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
    note_off_countdown[track] = 0;
}

static void prepare_trigger(uint8_t track, const prepared_track_event_t *event,
                            uint8_t track_mutes) {
    // Check global mute bitmask
    if (track_mutes & (1u << track)) return;
    if (!(event->flags & STEP_FLAG_ACTIVE)) return;

    uint8_t note = event->note;
    uint8_t vel = event->velocity;
    const active_note_state_t *previous = &active_note_state[track];
    bool same_route = note_is_on[track] &&
        previous->output_mode == event->output_mode &&
        (event->output_mode == OUTPUT_CV_GATE ||
         previous->midi_channel == event->midi_channel) &&
        (event->output_mode == OUTPUT_MIDI_ONLY ||
         previous->cv_channel == event->cv_channel);

    // Slide/tie belong to the outgoing step, matching the pattern flags and
    // the connection drawn to its right in the editor.
    bool connected = same_route && (previous->flags & (STEP_FLAG_SLIDE | STEP_FLAG_TIE));
    if (connected && active_notes[track] == note) {
        active_note_state[track].flags = event->flags;
        note_off_countdown[track] = (event->flags & (STEP_FLAG_SLIDE | STEP_FLAG_TIE))
                                  ? 0 : event->gate_pulses;
        return;
    }

    // Legato is valid only on the same destination. A same-pitch on/off pair
    // in that order would silence the newly triggered MIDI note.
    bool slide = same_route && (previous->flags & STEP_FLAG_SLIDE);
    if (!slide) {
        prepare_note_off(track);
    }

    // Queue MIDI messages
    if (event->output_mode == OUTPUT_MIDI_ONLY || event->output_mode == OUTPUT_BOTH) {
        if (slide) {
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
    }

    active_notes[track] = note;
    active_note_state[track].note = note;
    active_note_state[track].flags = event->flags;
    active_note_state[track].midi_channel = event->midi_channel;
    active_note_state[track].output_mode = event->output_mode;
    active_note_state[track].cv_channel = event->cv_channel;
    note_is_on[track] = true;
    note_off_countdown[track] = (event->flags & (STEP_FLAG_SLIDE | STEP_FLAG_TIE))
                              ? 0 : event->gate_pulses;
}

static void commit_step(void) {
    // 1. All gates change simultaneously (single GPIO register writes)
    gate_commit(pending_gate_on, pending_gate_off);
    pending_gate_on = 0;
    pending_gate_off = 0;

    // 2. Flush all buffered MIDI messages
    midi_buf_flush();
}

// ─── Kit tracks ─────────────────────────────────────────────────
// MIDI only: a kit never touches CV or gates. Its Note Off is Note On velocity 0.

// Writes a release for every voice the kit on `track` has sounding into `out`, each on
// the note it was sent on and all on the one channel stored at their step; the voices
// stop sounding and a pending flam is cancelled. Returns the byte count. In RAM: the
// timer ISR calls it on every release.
static uint8_t __not_in_flash_func(kit_take_release)(uint8_t track, uint8_t *out) {
    kit_runtime_t *k = &kit_runtime[track];
    uint8_t length = 0;
    if (k->sounding) {
        uint8_t status = (uint8_t)(MIDI_NOTE_ON | k->channel);
        for (uint8_t i = 0; i < KIT_VOICES; i++) {
            uint8_t v = kit_voice_order[i];
            if (!(k->sounding & (1u << v))) continue;
            out[length++] = status;
            out[length++] = k->note[v];
            out[length++] = 0;
        }
        k->sounding = 0;
    }
    k->countdown = 0;
    k->flam = 0;
    k->flam_countdown = 0;
    return length;
}

// Live mutes, note-off countdowns and a step's leftovers: a send of its own, never
// through midi_buf. In RAM, as the rest of the kit path the timer ISR runs.
static void __not_in_flash_func(kit_release)(uint8_t track) {
    uint8_t message[KIT_VOICES * 3];
    uint8_t length = kit_take_release(track, message);
    if (length) midi_send_bytes(message, length);
}

// Stop, Panic, Start while playing and a play-mode change: every kit voice in one send,
// queued before the Stop byte. The Stop, and a restart's Start and Clock, are real-time
// bytes and may overtake it on the wire; it still goes before the next run's notes.
// Its buffer is guarded by the playback lock all callers hold.
static void kit_release_all(void) {
    static uint8_t message[NUM_TRACKS * KIT_VOICES * 3];
    uint16_t length = 0;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        length += kit_take_release(track, &message[length]);
    }
    if (length) midi_send_bytes(message, length);
}

// A kit step on an audible track: leftovers released first, then the step's Note Ons
// into midi_buf in kit_voice_order. A flam plays its first hit at the note level and
// arms its second hit, one level up, `flam_pulses` later; the step's note-off then
// waits for the second hit's gate, up to the step boundary. In RAM.
static void __not_in_flash_func(play_kit_step)(uint8_t track, const prepared_track_pattern_t *row,
                                               uint8_t step) {
    kit_release(track);              // leftovers only: the countdown ends by now
    uint32_t word = row->kit.lanes[step];
    if (!(word & 0x00FFFFFCu)) return;  // no voice lane set
    kit_runtime_t *k = &kit_runtime[track];
    // One release channel: no voice of an earlier step may still sound on another.
    PLAYBACK_INVARIANT(k->sounding == 0 || k->channel == row->kit.channel);
    k->channel = row->kit.channel;
    uint8_t accent = (word & 3u) != 0;  // lane 0: AC
    uint16_t flam = 0;
    for (uint8_t i = 0; i < KIT_VOICES; i++) {
        uint8_t v = kit_voice_order[i];
        uint8_t value = (uint8_t)((word >> (2u * (v + 1u))) & 3u);
        if (value == KIT_OFF) continue;
        // note: AC ? accent : normal; accent: AC ? max : accent; flam: as a note.
        uint8_t velocity = row->kit.vel[(value == KIT_ACCENT) + accent];
        midi_buf_note_on(row->kit.channel, row->kit.note[v], velocity);
        k->note[v] = row->kit.note[v];
        k->sounding |= (uint16_t)(1u << v);
        if (value == KIT_FLAM) flam |= (uint16_t)(1u << v);
    }
    k->countdown = row->kit.gate_pulses;
    if (flam) {
        unsigned off = (unsigned)row->kit.gate_pulses + row->kit.flam_pulses;
        k->countdown = (uint8_t)(off < PULSES_PER_STEP ? off : PULSES_PER_STEP);
        k->flam = flam;
        k->flam_countdown = row->kit.flam_pulses;
        k->flam_velocity = row->kit.vel[1 + accent];  // AC ? max : accent
    }
}

// The most bytes the TX queue may hold once a flam's second hits are in it: 319, the
// fill the first design allowed in a 512-byte queue (192 bytes left free). The cap
// limits flams only; mandatory messages may fill the whole queue, whose headroom above
// the cap covers scheduled playback (168 bytes in one pulse) and the releases and first
// hits of an immediate restart (Start while playing, or Stop then Start).
#define KIT_FLAM_MAX_QUEUED 319u
_Static_assert(KIT_FLAM_MAX_QUEUED < MIDI_TX_QUEUE_SIZE - 1u, "the flam cap leaves headroom");

// Pulse order step 4: the second hits of a kit's flam, per voice Note On velocity 0
// then Note On at the flam velocity, in one send of their own, never through midi_buf.
// The only kit message that may be dropped: skipped when the TX queue would then hold
// more than KIT_FLAM_MAX_QUEUED bytes; the voices then sound on until their note-off.
// In RAM.
static void __not_in_flash_func(kit_play_flam)(uint8_t track) {
    kit_runtime_t *k = &kit_runtime[track];
    uint8_t message[KIT_VOICES * 6];
    uint8_t length = 0;
    uint8_t status = (uint8_t)(MIDI_NOTE_ON | k->channel);
    for (uint8_t i = 0; i < KIT_VOICES; i++) {
        uint8_t v = kit_voice_order[i];
        if (!(k->flam & (1u << v))) continue;
        message[length++] = status;
        message[length++] = k->note[v];
        message[length++] = 0;
        message[length++] = status;
        message[length++] = k->note[v];
        message[length++] = k->flam_velocity;
    }
    k->flam = 0;
    if (length && !midi_try_send_bytes_capped(message, length, KIT_FLAM_MAX_QUEUED)) {
        jitter_stats.flam_drops++;
    }
}

// ─── Installing prepared rows ───────────────────────────────────

// Installs the row of the track's playing location when it changed. A row of the other
// kind first releases what the old kind left sounding, in sends of their own before the
// new row plays: the event note on its stored route (a tie or slide note has no
// countdown), or every kit voice with a pending flam cancelled. Called with the playback
// lock held, or by transport_start() stopped with that state cleared: nothing to release.
static void load_prepared_track(uint8_t track) {
    uint8_t old_kind = prepared_playback.track[track].kind;
    uint32_t lock_state = seq_lock();
    uint8_t pattern = g_seq.transport.current_pattern[track];
    if (pattern >= MAX_PATTERNS) pattern = 0;
    if (playback_pattern[track] != pattern ||
        playback_revision[track] != prepared_revision[pattern][track]) {
        prepared_playback.track[track] = prepared_patterns[pattern][track];
        playback_revision[track] = prepared_revision[pattern][track];
        playback_pattern[track] = pattern;
    }
    seq_unlock(lock_state);
    if (prepared_playback.track[track].kind == old_kind) return;
    if (old_kind == PREPARED_KIT) {
        kit_release(track);
    } else {
        send_note_off_if_active(track);
    }
}

// Applies the queued pattern; returns true when the playing location changed.
static bool apply_track_queue_if_ready(uint8_t track) {
    transport_t *t = (transport_t *)&g_seq.transport;
    if (t->queued_pattern[track] == NO_QUEUED_PATTERN) return false;

    bool changed = false;
    uint32_t lock_state = seq_lock();
    uint8_t queued = t->queued_pattern[track];
    if (queued != NO_QUEUED_PATTERN) {
        changed = t->current_pattern[track] != queued;
        t->current_pattern[track] = queued;
        t->queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    seq_unlock(lock_state);
    load_prepared_track(track);
    return changed;
}

static void clear_runtime_note_state(void) {
    pending_gate_on = 0;
    pending_gate_off = 0;
    for (int i = 0; i < NUM_TRACKS; i++) {
        active_notes[i] = 0;
        note_is_on[i] = false;
        active_note_state[i].note = 0;
        active_note_state[i].flags = 0;
        active_note_state[i].midi_channel = 0;
        active_note_state[i].output_mode = OUTPUT_MIDI_ONLY;
        active_note_state[i].cv_channel = 0;
    }
    for (int i = 0; i < NUM_TRACKS; i++) {
        note_off_countdown[i] = 0;
    }
    memset(kit_runtime, 0, sizeof(kit_runtime));
}

static void stop_playback_outputs(void) {
    midi_song_end();
    kit_release_all();
    // All note-offs in one MIDI send: one short TX lock section, not one per track.
    midi_buf_clear();
    for (uint8_t i = 0; i < NUM_TRACKS; i++) {
        prepare_note_off(i);
    }
    midi_buf_flush();
    cv_gate_all_off();
    clear_runtime_note_state();
    midi_send_stop();
}

static void panic_playback(void) {
    g_seq.transport.state = TRANSPORT_STOPPED;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        g_seq.transport.queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    midi_clear_output_queue();
    stop_playback_outputs();
    midi_send_all_notes_off();
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
        // A list edit removed the playing first entry (song.c): entry 0 is next.
        t->song_position = 0;
    } else {
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
    }

    // Skip entries with 0 repeats
    uint16_t start = t->song_position;
    while (s->entries[t->song_position].repeats == 0) {
        t->song_position++;
        if (t->song_position >= s->length) {
            if (s->end_mode == SONG_END_STOP) {
                t->state = TRANSPORT_STOPPED;
                should_stop = true;
                goto done;
            }
            t->song_position = 0;
        }
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
    gpio_put(JITTER_PROBE_PIN, 1);
    uint32_t playback_irq_state = playback_lock();
    uint32_t scheduled_us = timer_hw->alarm[timer_alarm_num];
    jitter_record_entry(isr_start_us, scheduled_us);

    // Clear the alarm IRQ
    hw_clear_bits(&timer_hw->intr, 1u << timer_alarm_num);

    transport_t *t = (transport_t *)&g_seq.transport;
    uint32_t midi_deadline = 0;
    bool has_midi_deadline = false;
    if (t->state != TRANSPORT_PLAYING) goto isr_done;
    if (t->mode == PLAY_MODE_MIDI_SONG) {
        has_midi_deadline = midi_song_dispatch(isr_start_us, &midi_deadline);
        if (!has_midi_deadline) {
            t->state = TRANSPORT_STOPPED;
            stop_playback_outputs();
        }
        goto isr_done;
    }

    if ((t->pulse_count % (PPQN / MIDI_PPQN)) == 0) midi_send_clock();

    // A mute cuts an already sounding note on the next internal pulse. A kit with
    // nothing sounding and no flam pending has nothing to release.
    uint8_t track_mutes = g_seq.track_mutes;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if (track_mutes & (1u << track)) {
            send_note_off_if_active(track);
            if (kit_runtime[track].sounding || kit_runtime[track].flam_countdown) {
                kit_release(track);
            }
        }
    }

    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if (note_off_countdown[track] > 0) {
            note_off_countdown[track]--;
            if (note_off_countdown[track] == 0) {
                send_note_off_if_active(track);
            }
        }
        if (kit_runtime[track].countdown > 0 && --kit_runtime[track].countdown == 0) {
            kit_release(track);
        }
    }

    // Flam countdowns, after every note-off of this pulse.
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if (kit_runtime[track].flam_countdown > 0 && --kit_runtime[track].flam_countdown == 0) {
            kit_play_flam(track);
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
            t->pulse_count = 0;
            global_step_count = 0;
        }

        t->current_step = (uint8_t)(global_step_count % STEPS_PER_PATTERN);
        t->step_triggered = true;

        // ── Phase 1: Collect ── prepare all outputs without sending
        midi_buf_clear();
        pending_gate_on = 0;
        pending_gate_off = 0;

        for (int tr = 0; tr < NUM_TRACKS; tr++) {
            load_prepared_track((uint8_t)tr);
            prepared = &prepared_playback;

            uint8_t track_len = clamp_pattern_steps(prepared->track[tr].length);
            if (t->mode == PLAY_MODE_PATTERN &&
                t->queued_pattern[tr] != NO_QUEUED_PATTERN) {
                uint8_t quant = g_seq.launch_quant;
                bool due;
                switch (quant) {
                    case LAUNCH_QUANT_BAR:  due = (global_step_count % 16u) == 0; break;
                    case LAUNCH_QUANT_BEAT: due = (global_step_count % 4u) == 0; break;
                    case LAUNCH_QUANT_STEP: due = true; break;
                    default:                due = t->track_step_count[tr] >= track_len; break;
                }
                if (due) {
                    if (apply_track_queue_if_ready((uint8_t)tr)) {
                        prepared = &prepared_playback;
                        track_len = clamp_pattern_steps(prepared->track[tr].length);
                        t->track_step_count[tr] = (quant == LAUNCH_QUANT_TRACK_LOOP)
                            ? 0u
                            : (global_step_count % STEPS_PER_PATTERN) % track_len;
                    }
                }
            }
            if (t->track_step_count[tr] >= track_len) {
                t->track_step_count[tr] = 0;
            }

            uint8_t track_step = (uint8_t)t->track_step_count[tr];
            t->current_track_step[tr] = track_step;
            const prepared_track_pattern_t *row = &prepared->track[tr];
            if (row->kind == PREPARED_KIT) {
                // A live mute released the voices in this pulse already.
                if (!(track_mutes & (1u << tr))) play_kit_step((uint8_t)tr, row, track_step);
            } else {
                const prepared_track_event_t *event = &row->step[track_step];
                if (!(event->flags & STEP_FLAG_ACTIVE) || (track_mutes & (1u << tr))) {
                    prepare_note_off(tr);
                } else {
                    prepare_trigger(tr, event, track_mutes);
                }
            }
            t->track_step_count[tr]++;
        }

        // ── Phase 2: Commit ── all outputs fire together
        commit_step();

    }

    t->pulse_count++;

isr_done:
    if (midi_output_overflowed()) {
        panic_playback();
        has_midi_deadline = false;
    }

    // Re-arm after processing. Writing an alarm in the past would stall for
    // the 32-bit timer wrap (about 72 minutes); recover without a clock burst.
    uint32_t target = has_midi_deadline ? midi_deadline : scheduled_us + pulse_interval_us;
    uint32_t now_us = timer_hw->timerawl;
    if ((int32_t)(target - now_us) <= 0) {
        if (!has_midi_deadline && (int32_t)(isr_start_us - scheduled_us) < (int32_t)jitter_expected_period_us) {
            jitter_stats.overruns++;
        }
        // Bound the work in one MIDI Song IRQ while retaining the song's
        // absolute timeline when several events are already due.
        target = now_us + (has_midi_deadline ? 10u : pulse_interval_us);
    }
    timer_hw->alarm[timer_alarm_num] = target;
    if ((int32_t)(target - timer_hw->timerawl) <= 0) {
        hw_clear_bits(&timer_hw->intr, 1u << timer_alarm_num);
        irq_clear(TIMER_IRQ_0);
        timer_hw->alarm[timer_alarm_num] = timer_hw->timerawl + (has_midi_deadline ? 10u : pulse_interval_us);
    }
    jitter_expected_period_us = pulse_interval_us;
    jitter_record_exit(isr_start_us);
    gpio_put(JITTER_PROBE_PIN, 0);
    playback_unlock(playback_irq_state);
}

// ─── Core 1 Entry ───────────────────────────────────────────────

void sequencer_init(void) {
    midi_song_reset();
    static bool lock_ready;
    if (!lock_ready) {
        playback_spinlock_num = spin_lock_claim_unused(true);
        lock_ready = true;
    }
    timer_alarm_num = 0;
    pulse_interval_us = bpm_to_pulse_interval_us(g_seq.transport.bpm);
    jitter_expected_period_us = pulse_interval_us;
    jitter_reset_now();
    clear_runtime_note_state();
    memset(playback_pattern, NO_QUEUED_PATTERN, sizeof(playback_pattern));
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        kit_config_defaults(&g_seq.kit_config[track]);
    }
    for (uint8_t pattern = 0; pattern < MAX_PATTERNS; pattern++) {
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            sequencer_refresh_track_playback(track, pattern);
        }
    }
}

void sequencer_core1_entry(void) {
    gpio_init(JITTER_PROBE_PIN);
    gpio_set_dir(JITTER_PROBE_PIN, GPIO_OUT);
    gpio_put(JITTER_PROBE_PIN, 0);

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
    uint32_t playback_irq_state = playback_lock();
    if (g_seq.transport.state == TRANSPORT_PLAYING) stop_playback_outputs();
    clear_runtime_note_state();
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    t->state = TRANSPORT_STOPPED;

    if (t->mode == PLAY_MODE_MIDI_SONG) {
        if (midi_song_loaded()) {
            const uint32_t start = timer_hw->timerawl + 1000u;
            midi_song_begin(start);
            t->pulse_count = 0;
            t->state = TRANSPORT_PLAYING;
            timer_hw->alarm[timer_alarm_num] = start;
            midi_send_start();
        }
        seq_unlock(lock_state);
        playback_unlock(playback_irq_state);
        return;
    }

    // Reset position
    t->pulse_count = 0;
    t->current_step = 0;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        t->track_step_count[track] = 0;
        t->current_track_step[track] = 0;
        t->queued_pattern[track] = NO_QUEUED_PATTERN;
    }

    if (t->mode == PLAY_MODE_SONG) {
        t->song_position = 0;
        while (t->song_position < g_seq.song.length &&
               g_seq.song.entries[t->song_position].repeats == 0) {
            t->song_position++;
        }
        if (t->song_position >= g_seq.song.length) {
            seq_unlock(lock_state);
            playback_unlock(playback_irq_state);
            return;
        }
        memcpy(t->current_pattern,
               g_seq.song.entries[t->song_position].pattern_index,
               sizeof(t->current_pattern));
        t->song_repeat_count = g_seq.song.entries[t->song_position].repeats;
    }

    seq_unlock(lock_state);
    playback_unlock(playback_irq_state);

    // Stopped, the timer ISR leaves the prepared rows alone: load them with
    // interrupts on between tracks, not inside one long playback-lock section.
    for (uint8_t track = 0; track < NUM_TRACKS; track++) load_prepared_track(track);

    playback_irq_state = playback_lock();
    lock_state = seq_lock();
    t = (transport_t *)&g_seq.transport;
    t->state = TRANSPORT_PLAYING;
    seq_unlock(lock_state);
    midi_send_start();
    playback_unlock(playback_irq_state);
}

void transport_stop(void) {
    uint32_t playback_irq_state = playback_lock();
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    t->state = TRANSPORT_STOPPED;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        t->queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    seq_unlock(lock_state);

    // All notes off
    stop_playback_outputs();
    playback_unlock(playback_irq_state);
}

void transport_continue(void) {
    uint32_t playback_irq_state = playback_lock();
    if (g_seq.transport.state == TRANSPORT_PLAYING || g_seq.transport.mode == PLAY_MODE_MIDI_SONG ||
        (g_seq.transport.mode == PLAY_MODE_SONG &&
         g_seq.transport.song_position >= g_seq.song.length)) {
        playback_unlock(playback_irq_state);
        return;
    }
    for (uint8_t track = 0; track < NUM_TRACKS; track++) load_prepared_track(track);
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    t->state = TRANSPORT_PLAYING;
    seq_unlock(lock_state);
    midi_send_continue();
    playback_unlock(playback_irq_state);
}

void transport_set_bpm(float bpm) {
    if (!isfinite(bpm)) return;
    if (bpm < MIN_BPM) bpm = MIN_BPM;
    if (bpm > MAX_BPM) bpm = MAX_BPM;
    uint32_t interval_us = bpm_to_pulse_interval_us(bpm);
    uint32_t playback_irq_state = playback_lock();
    uint32_t lock_state = seq_lock();
    g_seq.transport.bpm = bpm;
    pulse_interval_us = interval_us;
    seq_unlock(lock_state);
    playback_unlock(playback_irq_state);
}

void transport_set_mode(play_mode_t mode) {
    if (mode != PLAY_MODE_PATTERN && mode != PLAY_MODE_SONG &&
        (mode != PLAY_MODE_MIDI_SONG || !midi_song_loaded())) return;
    uint32_t playback_irq_state = playback_lock();
    if (g_seq.transport.mode != mode) {
        g_seq.transport.state = TRANSPORT_STOPPED;
        stop_playback_outputs();
        g_seq.transport.mode = mode;
        for (uint8_t track = 0; track < NUM_TRACKS; track++) {
            g_seq.transport.queued_pattern[track] = NO_QUEUED_PATTERN;
        }
    }
    playback_unlock(playback_irq_state);
}

bool transport_open_midi_song(void) {
    if (g_seq.transport.state != TRANSPORT_STOPPED || !midi_song_mount()) return false;
    transport_set_mode(PLAY_MODE_MIDI_SONG);
    return true;
}

void transport_midi_song_toggle(uint8_t track, bool solo) {
    const uint32_t irq = playback_lock();
    midi_song_toggle(track, solo);
    playback_unlock(irq);
}

void transport_midi_song_view(midi_song_view_t *view) {
    const uint32_t irq = playback_lock();
    midi_song_view(timer_hw->timerawl, view);
    playback_unlock(irq);
}

void transport_set_launch_quant(launch_quant_t q) {
    if ((unsigned)q >= LAUNCH_QUANT_COUNT) return;
    uint32_t playback_irq_state = playback_lock();
    uint32_t lock_state = seq_lock();
    g_seq.launch_quant = (uint8_t)q;
    seq_unlock(lock_state);
    playback_unlock(playback_irq_state);
}

launch_quant_t transport_get_launch_quant(void) {
    return (launch_quant_t)g_seq.launch_quant;
}

static bool queue_patterns(uint8_t track_mask, uint8_t pattern_index) {
    if (pattern_index >= MAX_PATTERNS) return false;
    uint32_t playback_irq_state = playback_lock();
    if (g_seq.transport.mode != PLAY_MODE_PATTERN) {
        playback_unlock(playback_irq_state);
        return false;
    }
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if (!(track_mask & (1u << track))) continue;
        if (t->state == TRANSPORT_PLAYING) {
            t->queued_pattern[track] = pattern_index;
        } else {
            t->current_pattern[track] = pattern_index;
            t->queued_pattern[track] = NO_QUEUED_PATTERN;
            t->track_step_count[track] = 0;
            t->current_track_step[track] = 0;
        }
    }
    seq_unlock(lock_state);
    playback_unlock(playback_irq_state);
    return true;
}

bool transport_queue_pattern(uint8_t pattern_index) {
    return queue_patterns(UINT8_MAX, pattern_index);
}

bool transport_queue_track_pattern(uint8_t track, uint8_t pattern_index) {
    if (track >= NUM_TRACKS) return false;
    return queue_patterns((uint8_t)(1u << track), pattern_index);
}

bool transport_launch(uint8_t track_mask, uint8_t pattern_index) {
    if (pattern_index >= MAX_PATTERNS) return false;
    // The decision reads current and queued under the same locks as the write,
    // so the timer cannot apply a queue between the two.
    uint32_t playback_irq_state = playback_lock();
    if (g_seq.transport.mode != PLAY_MODE_PATTERN) {
        playback_unlock(playback_irq_state);
        return false;
    }
    uint32_t lock_state = seq_lock();
    transport_t *t = (transport_t *)&g_seq.transport;
    bool cancel = false;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if ((track_mask & (1u << track)) && t->queued_pattern[track] == pattern_index) {
            cancel = true;
        }
    }
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        if (!(track_mask & (1u << track))) continue;
        if (cancel || t->current_pattern[track] == pattern_index) {
            t->queued_pattern[track] = NO_QUEUED_PATTERN;
        } else if (t->state == TRANSPORT_PLAYING) {
            t->queued_pattern[track] = pattern_index;
        } else {
            t->current_pattern[track] = pattern_index;
            t->queued_pattern[track] = NO_QUEUED_PATTERN;
            t->track_step_count[track] = 0;
            t->current_track_step[track] = 0;
        }
    }
    seq_unlock(lock_state);
    playback_unlock(playback_irq_state);
    return true;
}

// Runs from RAM, as do the lock helpers it calls: it holds the playback lock
// about 30 times a second, and a cache-cold run from XIP flash could stretch
// that hold and delay the timer (MIDI Clock, gate edges).
void __not_in_flash_func(sequencer_snapshot)(transport_snapshot_t *out) {
    if (!out) return;
    uint32_t playback_irq_state = playback_lock();
    uint32_t lock_state = seq_lock();
    const transport_t *t = (const transport_t *)&g_seq.transport;
    out->state = t->state;
    out->mode = t->mode;
    out->pulse_count = t->pulse_count;
    out->live_mutes = g_seq.track_mutes;
    out->stored_mutes = 0;
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        out->current[track] = t->current_pattern[track];
        out->queued[track] = t->queued_pattern[track];
        out->track_step[track] = t->current_track_step[track];
        uint8_t pattern = t->current_pattern[track];
        if (pattern < MAX_PATTERNS && g_seq.patterns[pattern].tracks[track].muted) {
            out->stored_mutes |= (uint8_t)(1u << track);
        }
    }
    seq_unlock(lock_state);
    playback_unlock(playback_irq_state);
}

uint64_t sequencer_clip_mask(uint8_t bank) {
    if (bank >= PATTERN_BANKS) return 0;
    uint32_t lock_state = seq_lock();
    uint64_t mask = clip_mask[bank];
    seq_unlock(lock_state);
    return mask;
}

void transport_clear_queue(void) {
    uint32_t playback_irq_state = playback_lock();
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        g_seq.transport.queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    playback_unlock(playback_irq_state);
}

void transport_clear_track_queue(uint8_t track) {
    if (track >= NUM_TRACKS) return;
    uint32_t playback_irq_state = playback_lock();
    g_seq.transport.queued_pattern[track] = NO_QUEUED_PATTERN;
    playback_unlock(playback_irq_state);
}

void sequencer_panic(void) {
    uint32_t playback_irq_state = playback_lock();
    panic_playback();
    playback_unlock(playback_irq_state);
}

void sequencer_refresh_track_playback(uint8_t track, uint8_t pattern_index) {
    if (track >= NUM_TRACKS) return;
    if (pattern_index >= MAX_PATTERNS) return;

    prepared_track_pattern_t updated;
    bool has_clip = fill_prepared_track(&updated, track, pattern_index);

    uint32_t lock_state = seq_lock();
    uint64_t bit = 1ull << (track * 8 + pattern_index % PATTERNS_PER_BANK);
    uint8_t bank = pattern_index / PATTERNS_PER_BANK;
    clip_mask[bank] = has_clip ? (clip_mask[bank] | bit) : (clip_mask[bank] & ~bit);
    prepared_patterns[pattern_index][track] = updated;
    prepared_revision[pattern_index][track]++;
    seq_unlock(lock_state);
}

void sequencer_get_jitter_stats(sequencer_jitter_stats_t *out) {
    if (!out) return;
    uint32_t playback_irq_state = playback_lock();

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
    out->flam_drops = jitter_stats.flam_drops;
    playback_unlock(playback_irq_state);
}

void sequencer_reset_jitter_stats(void) {
    uint32_t playback_irq_state = playback_lock();
    jitter_reset_pending = true;
    playback_unlock(playback_irq_state);
}
