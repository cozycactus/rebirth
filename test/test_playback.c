#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fakes/fake_hardware.h"
#include "../src/sequencer.c"

sequencer_state_t g_seq;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void tick(void) {
    timer_hw->timerawl = timer_hw->alarm[0];
    sequencer_timer_isr();
}

static void pulses(unsigned count) {
    while (count--) tick();
}

static void next_step(void) { pulses(PULSES_PER_STEP); }

static unsigned count_byte(uint8_t byte) {
    unsigned count = 0;
    for (unsigned i = 0; i < fake_midi_length; i++) count += fake_midi[i] == byte;
    return count;
}

static void reset(void) {
    uint32_t shared_lock = g_seq.spinlock_num;
    memset(&g_seq, 0, sizeof(g_seq));
    g_seq.spinlock_num = shared_lock;
    g_seq.spinlock_ready = true;
    fake_hardware_reset();
    midi_init();
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) pattern_init(&g_seq.patterns[p], p);
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        g_seq.transport.queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    g_seq.transport.bpm = DEFAULT_BPM;
    sequencer_init();
    timer_hw->alarm[0] = pulse_interval_us;
}

static void set_note(uint8_t pattern, uint8_t track, uint8_t step, uint8_t note, uint8_t flags) {
    pattern_set_step(&g_seq.patterns[pattern], track, step, note, 100, STEP_FLAG_ACTIVE | flags);
    sequencer_refresh_track_playback(track, pattern);
}

static void set_length(uint8_t pattern, uint8_t track, uint8_t length) {
    pattern_set_track_length(&g_seq.patterns[pattern], track, length);
    sequencer_refresh_track_playback(track, pattern);
}

static void test_clock_rate_and_start_order(void) {
    set_note(0, 0, 0, 60, 0);
    transport_start();
    pulses(PPQN);
    CHECK(count_byte(MIDI_CLOCK) == 24);
    CHECK(fake_midi[0] == MIDI_START);
    CHECK(fake_midi[1] == MIDI_CLOCK);
    CHECK(fake_midi[2] == MIDI_NOTE_ON);
}

static void test_stopped_selection_survives_start(void) {
    transport_start();
    CHECK(transport_queue_track_pattern(3, 2));
    transport_stop();
    CHECK(g_seq.transport.queued_pattern[3] == NO_QUEUED_PATTERN);
    CHECK(transport_queue_track_pattern(3, 5));
    CHECK(g_seq.transport.current_pattern[3] == 5);
    CHECK(g_seq.transport.queued_pattern[3] == NO_QUEUED_PATTERN);
    transport_start();
    tick();
    CHECK(g_seq.transport.current_pattern[3] == 5);
}

static void test_independent_track_queues(void) {
    set_length(0, 0, 3);
    set_length(0, 1, 5);
    set_length(1, 0, 4);
    set_length(1, 1, 2);
    transport_start();
    tick();
    CHECK(transport_queue_pattern(1));
    next_step();
    next_step();
    CHECK(g_seq.transport.current_pattern[0] == 0);
    next_step();
    CHECK(g_seq.transport.current_pattern[0] == 1);
    CHECK(g_seq.transport.current_track_step[0] == 0);
    CHECK(g_seq.transport.current_pattern[1] == 0);
    CHECK(g_seq.transport.current_track_step[1] == 3);
    next_step();
    next_step();
    CHECK(g_seq.transport.current_pattern[1] == 1);
    CHECK(g_seq.transport.current_track_step[1] == 0);
    CHECK(g_seq.transport.current_track_step[0] == 2);
}

static void test_shorten_wraps_without_skipping_start(void) {
    transport_start();
    tick();
    for (unsigned step = 0; step < 6; step++) next_step();
    CHECK(g_seq.transport.current_track_step[0] == 6);
    set_length(0, 0, 4);
    next_step();
    CHECK(g_seq.transport.current_track_step[0] == 0);
}

static void test_lengthen_keeps_playhead(void) {
    set_length(0, 0, 3);
    transport_start();
    tick();
    next_step();
    next_step();
    set_length(0, 0, 6);
    next_step();
    CHECK(g_seq.transport.current_track_step[0] == 3);
}

static void test_queued_edit_is_heard_at_switch(void) {
    set_length(0, 0, 2);
    set_note(1, 0, 0, 60, 0);
    transport_start();
    tick();
    CHECK(transport_queue_track_pattern(0, 1));
    set_note(1, 0, 0, 72, 0);
    next_step();
    next_step();
    CHECK(active_notes[0] == 72);
}

static void test_song_preserves_unchanged_polymeter(void) {
    set_length(0, 0, 5);
    CHECK(song_append(0, 1));
    uint8_t patterns[NUM_TRACKS] = {0};
    patterns[1] = 2;
    CHECK(song_append_patterns(patterns, 1));
    transport_set_mode(PLAY_MODE_SONG);
    transport_start();
    CHECK(!transport_queue_track_pattern(0, 3));
    // Update a future Song location while the first entry is playing.
    set_note(2, 1, 0, 76, 0);
    tick();
    for (unsigned step = 0; step < 16; step++) next_step();
    CHECK(g_seq.transport.song_position == 1);
    CHECK(g_seq.transport.current_track_step[0] == 1);
    CHECK(g_seq.transport.current_track_step[1] == 0);
    CHECK(active_notes[1] == 76);
}

static void test_empty_song_does_not_start(void) {
    transport_set_mode(PLAY_MODE_SONG);
    transport_start();
    transport_continue();
    tick();
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    CHECK(count_byte(MIDI_START) == 0);
    CHECK(count_byte(MIDI_CLOCK) == 0);
}

static void test_song_skips_initial_and_trailing_zero_repeats(void) {
    CHECK(song_append(0, 1));
    CHECK(song_append(1, 1));
    CHECK(song_append(2, 1));
    g_seq.song.entries[0].repeats = 0;
    g_seq.song.entries[2].repeats = 0;
    song_set_end_mode(SONG_END_STOP);
    transport_set_mode(PLAY_MODE_SONG);
    transport_start();
    tick();
    CHECK(g_seq.transport.song_position == 1);
    for (unsigned step = 0; step < 16; step++) next_step();
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
}

static void sound_note(void) {
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_BOTH;
    set_note(0, 0, 0, 60, 0);
    transport_start();
    tick();
    CHECK(fake_gates == 1);
}

static void test_mute_cuts_sounding_note_on_next_pulse(void) {
    sound_note();
    seq_set_track_mute(0, true);
    tick();
    CHECK(fake_gates == 0);
    CHECK(!note_is_on[0]);
    CHECK(count_byte(MIDI_NOTE_OFF) == 1);
}

static void test_restart_releases_previous_note(void) {
    sound_note();
    transport_start();
    CHECK(fake_gates == 0);
    CHECK(count_byte(MIDI_NOTE_OFF) == 1);
    tick();
    CHECK(fake_gates == 1);
}

static void test_panic_clears_all_outputs_and_queues(void) {
    sound_note();
    CHECK(transport_queue_pattern(1));
    sequencer_panic();
    CHECK(fake_gates == 0);
    CHECK(pending_gate_on == 0);
    CHECK(pending_gate_off == 0);
    unsigned length = fake_midi_length;
    pulses(PPQN * 4);
    CHECK(fake_midi_length == length);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        CHECK(g_seq.transport.queued_pattern[track] == NO_QUEUED_PATTERN);
        CHECK(!note_is_on[track]);
    }
    for (uint8_t channel = 0; channel < 16; channel++) {
        unsigned offset = length - 96 + channel * 6;
        CHECK(fake_midi[offset] == (MIDI_CC | channel));
        CHECK(fake_midi[offset + 1] == MIDI_CC_ALL_SOUND_OFF);
        CHECK(fake_midi[offset + 4] == MIDI_CC_ALL_NOTES_OFF);
    }
}

static void test_slide_route_change_releases_old_destination(void) {
    set_note(0, 0, 0, 60, STEP_FLAG_SLIDE);
    transport_start();
    tick();
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_CV_GATE;
    set_note(0, 0, 1, 65, 0);
    next_step();
    CHECK(count_byte(MIDI_NOTE_OFF) == 1);
    CHECK(fake_gates == 1);
}

static void test_same_note_slide_does_not_end_with_note_off(void) {
    set_note(0, 0, 0, 60, STEP_FLAG_SLIDE);
    set_note(0, 0, 1, 60, 0);
    transport_start();
    tick();
    next_step();
    CHECK(count_byte(MIDI_NOTE_ON) == 1);
    CHECK(count_byte(MIDI_NOTE_OFF) == 0);
    CHECK(note_is_on[0]);
}

static void test_tie_does_not_keep_old_midi_channel(void) {
    set_note(0, 0, 0, 60, STEP_FLAG_TIE);
    transport_start();
    tick();
    g_seq.patterns[0].tracks[0].midi_channel = 3;
    set_note(0, 0, 1, 60, 0);
    next_step();
    CHECK(count_byte(MIDI_NOTE_OFF) == 1);
    CHECK(count_byte(MIDI_NOTE_ON | 3) == 1);
}

static void test_midi_only_tie_ignores_unused_cv_channel(void) {
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_MIDI_ONLY;
    set_note(0, 0, 0, 60, STEP_FLAG_TIE);
    set_note(0, 0, 1, 60, 0);
    transport_start();
    tick();

    g_seq.patterns[0].tracks[0].cv_channel = 2;
    sequencer_refresh_track_playback(0, 0);
    next_step();

    CHECK(count_byte(MIDI_NOTE_ON) == 1);
    CHECK(count_byte(MIDI_NOTE_OFF) == 0);
    CHECK(fake_gates == 0);
    pulses(PULSES_PER_STEP / 2);
    CHECK(count_byte(MIDI_NOTE_OFF) == 1);
    CHECK(fake_midi[fake_midi_length - 2] == 60);
}

static void test_cv_only_tie_ignores_unused_midi_channel(void) {
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_CV_GATE;
    set_note(0, 0, 0, 60, STEP_FLAG_TIE);
    set_note(0, 0, 1, 60, 0);
    transport_start();
    tick();
    CHECK(fake_gates == 1);
    unsigned rises = fake_gate_rises;

    g_seq.patterns[0].tracks[0].midi_channel = 3;
    sequencer_refresh_track_playback(0, 0);
    next_step();

    CHECK(fake_gates == 1);
    CHECK(fake_gate_rises == rises);
    CHECK(count_byte(MIDI_NOTE_ON | 3) == 0);
    pulses(PULSES_PER_STEP / 2);
    CHECK(fake_gates == 0);
    CHECK(count_byte(MIDI_NOTE_OFF) == 0);
    CHECK(count_byte(MIDI_NOTE_OFF | 3) == 0);
}

static void test_tie_does_not_keep_old_cv_channel(void) {
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_BOTH;
    set_note(0, 0, 0, 60, STEP_FLAG_TIE);
    set_note(0, 0, 1, 60, 0);
    transport_start();
    tick();
    CHECK(fake_gates == 1);

    g_seq.patterns[0].tracks[0].cv_channel = 2;
    sequencer_refresh_track_playback(0, 0);
    next_step();

    CHECK(fake_gates == (1u << 2));
    CHECK(count_byte(MIDI_NOTE_OFF) == 1);
    CHECK(count_byte(MIDI_NOTE_ON) == 2);
    pulses(PULSES_PER_STEP / 2);
    CHECK(fake_gates == 0);
    CHECK(count_byte(MIDI_NOTE_OFF) == 2);
}

static void test_midi_only_slide_ignores_unused_cv_channel(void) {
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_MIDI_ONLY;
    set_note(0, 0, 0, 60, STEP_FLAG_SLIDE);
    set_note(0, 0, 1, 65, 0);
    transport_start();
    tick();

    g_seq.patterns[0].tracks[0].cv_channel = 2;
    sequencer_refresh_track_playback(0, 0);
    next_step();

    const uint8_t expected[] = {MIDI_NOTE_ON, 65, 100, MIDI_NOTE_OFF, 60, 0};
    CHECK(fake_midi_length >= sizeof(expected));
    CHECK(memcmp(fake_midi + fake_midi_length - sizeof(expected),
                 expected, sizeof(expected)) == 0);
    CHECK(fake_gates == 0);
    pulses(PULSES_PER_STEP / 2);
    CHECK(count_byte(MIDI_NOTE_OFF) == 2);
    CHECK(fake_midi[fake_midi_length - 2] == 65);
}

static void test_queued_pattern_tie_ignores_unused_cv_channel(void) {
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_MIDI_ONLY;
    g_seq.patterns[1].tracks[0].output_mode = OUTPUT_MIDI_ONLY;
    g_seq.patterns[1].tracks[0].cv_channel = 2;
    set_length(0, 0, 1);
    set_note(0, 0, 0, 60, STEP_FLAG_TIE);
    set_note(1, 0, 0, 60, 0);
    transport_start();
    tick();
    CHECK(transport_queue_track_pattern(0, 1));
    next_step();

    CHECK(g_seq.transport.current_pattern[0] == 1);
    CHECK(count_byte(MIDI_NOTE_ON) == 1);
    CHECK(count_byte(MIDI_NOTE_OFF) == 0);
    CHECK(fake_gates == 0);
    pulses(PULSES_PER_STEP / 2);
    CHECK(count_byte(MIDI_NOTE_OFF) == 1);
}

static void test_midi_and_gate_share_note_duration(void) {
    sound_note();
    pulses(PULSES_PER_STEP / 2 - 1);
    CHECK(fake_gates == 1);
    CHECK(count_byte(MIDI_NOTE_OFF) == 0);
    tick();
    CHECK(fake_gates == 0);
    CHECK(count_byte(MIDI_NOTE_OFF) == 1);
}

static void test_outgoing_tie_keeps_gate_and_midi_connected(void) {
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_BOTH;
    set_note(0, 0, 0, 60, STEP_FLAG_TIE);
    set_note(0, 0, 1, 60, 0);
    transport_start();
    tick();
    unsigned rises = fake_gate_rises;
    for (unsigned pulse = 0; pulse < PULSES_PER_STEP; pulse++) {
        tick();
        CHECK(fake_gates == 1);
    }
    CHECK(fake_gate_rises == rises);
    CHECK(count_byte(MIDI_NOTE_ON) == 1);
    CHECK(count_byte(MIDI_NOTE_OFF) == 0);
    pulses(PULSES_PER_STEP / 2);
    CHECK(fake_gates == 0);
    CHECK(count_byte(MIDI_NOTE_OFF) == 1);
}

static void test_outgoing_slide_keeps_gate_and_changes_note(void) {
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_BOTH;
    set_note(0, 0, 0, 60, STEP_FLAG_SLIDE);
    set_note(0, 0, 1, 65, 0);
    transport_start();
    tick();
    next_step();
    CHECK(fake_gates == 1);
    CHECK(active_notes[0] == 65);
    CHECK(fake_midi[fake_midi_length - 6] == MIDI_NOTE_ON);
    CHECK(fake_midi[fake_midi_length - 5] == 65);
    CHECK(fake_midi[fake_midi_length - 3] == MIDI_NOTE_OFF);
    CHECK(fake_midi[fake_midi_length - 2] == 60);
}

static void test_tempo_change_does_not_fake_jitter(void) {
    pulses(4);
    transport_set_bpm(200.0f);
    tick();
    CHECK(jitter_stats.last_period_error_us == 0);
    tick();
    CHECK(jitter_stats.last_period_error_us == 0);
    CHECK(pulse_interval_us == 3125);
    transport_set_bpm(NAN);
    CHECK(g_seq.transport.bpm == 200.0f);
}

static void test_missed_alarm_rearms_in_future(void) {
    transport_start();
    timer_hw->timerawl = timer_hw->alarm[0] + 3 * pulse_interval_us;
    sequencer_timer_isr();
    CHECK((int32_t)(timer_hw->alarm[0] - timer_hw->timerawl) > 0);
    CHECK(count_byte(MIDI_CLOCK) == 1);
    CHECK(jitter_stats.overruns == 1);
    tick();
    CHECK(jitter_stats.samples == 2);
}

static void test_timer_wrap_is_normal(void) {
    timer_hw->alarm[0] = UINT32_MAX - 100;
    tick();
    CHECK(timer_hw->alarm[0] < pulse_interval_us);
    tick();
    CHECK(jitter_stats.last_period_error_us == 0);
    CHECK(jitter_stats.overruns == 0);
}

static void test_uart_overflow_never_sends_partial_message(void) {
    fake_uart_writable = false;
    for (unsigned note = 0; note < 84; note++) midi_send_note_on(0, note, 100);
    midi_send_clock(); // 253 bytes: only two queue slots remain.
    midi_send_note_on(1, 100, 100);
    CHECK(midi_output_overflowed());
    CHECK(fake_midi_length == 0);
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    CHECK(fake_midi_length == 253);
    for (unsigned note = 0; note < 84; note++) {
        CHECK(fake_midi[note * 3] == MIDI_NOTE_ON);
        CHECK(fake_midi[note * 3 + 1] == note);
    }
    CHECK(fake_midi[252] == MIDI_CLOCK);
    CHECK(!fake_uart_tx_irq);
}

static void test_staging_overflow_never_sends_partial_message(void) {
    for (unsigned note = 0; note < 22; note++) midi_buf_note_on(0, note, 100);
    midi_buf_flush();
    CHECK(fake_midi_length == 63);
    CHECK(midi_output_overflowed());
}

static void test_output_overflow_stops_transport(void) {
    sound_note();
    fake_uart_writable = false;
    for (unsigned note = 0; note < 100; note++) midi_send_note_on(0, note, 100);
    tick();
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    CHECK(fake_gates == 0);
    CHECK(!midi_output_overflowed());
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    CHECK(fake_midi[fake_midi_length - 2] == MIDI_CC_ALL_NOTES_OFF);
}

#ifdef REBIRTH_THREADED_TEST
static pthread_mutex_t interleave_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t interleave_ready = PTHREAD_COND_INITIALIZER;
static bool gate_waiting;
static bool stop_waiting;
static _Thread_local bool stopping_thread;

static void wait_at_gate_commit(void) {
    CHECK(pthread_mutex_lock(&interleave_lock) == 0);
    gate_waiting = true;
    CHECK(pthread_cond_broadcast(&interleave_ready) == 0);
    while (!stop_waiting) CHECK(pthread_cond_wait(&interleave_ready, &interleave_lock) == 0);
    CHECK(pthread_mutex_unlock(&interleave_lock) == 0);
}

static void observe_stop_lock(spin_lock_t *lock) {
    if (!stopping_thread || lock != spin_lock_instance(playback_spinlock_num)) return;
    CHECK(pthread_mutex_lock(&interleave_lock) == 0);
    stop_waiting = true;
    CHECK(pthread_cond_broadcast(&interleave_ready) == 0);
    CHECK(pthread_mutex_unlock(&interleave_lock) == 0);
}

static void *play_one_tick(void *unused) { (void)unused; tick(); return NULL; }
static void *stop_from_other_core(void *panic) {
    stopping_thread = true;
    if (panic) sequencer_panic(); else transport_stop();
    stopping_thread = false;
    return NULL;
}

static void concurrent_stop(bool panic) {
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_BOTH;
    set_note(0, 0, 0, 60, 0);
    transport_start();
    gate_waiting = false;
    stop_waiting = false;
    fake_before_gate_commit = wait_at_gate_commit;
    fake_before_lock = observe_stop_lock;
    pthread_t playing, stopping;
    CHECK(pthread_create(&playing, NULL, play_one_tick, NULL) == 0);
    CHECK(pthread_mutex_lock(&interleave_lock) == 0);
    while (!gate_waiting) CHECK(pthread_cond_wait(&interleave_ready, &interleave_lock) == 0);
    CHECK(pthread_mutex_unlock(&interleave_lock) == 0);
    CHECK(pthread_create(&stopping, NULL, stop_from_other_core, panic ? &gate_waiting : NULL) == 0);
    CHECK(pthread_join(playing, NULL) == 0);
    CHECK(pthread_join(stopping, NULL) == 0);
    fake_before_gate_commit = NULL;
    fake_before_lock = NULL;
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    CHECK(fake_gates == 0);
    CHECK(!note_is_on[0]);
    CHECK(pending_gate_on == 0);
    unsigned length = fake_midi_length;
    pulses(PPQN);
    CHECK(fake_midi_length == length);
}

static void test_stop_during_pending_gate_commit(void) { concurrent_stop(false); }
static void test_panic_during_pending_gate_commit(void) { concurrent_stop(true); }

static void *midi_producer(void *channel_pointer) {
    uint8_t channel = *(uint8_t *)channel_pointer;
    for (unsigned note = 0; note < 500; note++) {
        midi_send_note_on(channel, note & 0x7F, 100 + channel);
    }
    return NULL;
}

static void *midi_consumer(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 1000; i++) fake_irq_handlers[UART0_IRQ]();
    return NULL;
}

static void test_concurrent_midi_producers_and_uart_irq(void) {
    pthread_t producers[2], consumer;
    uint8_t channels[] = {0, 1};
    CHECK(pthread_create(&consumer, NULL, midi_consumer, NULL) == 0);
    for (unsigned i = 0; i < 2; i++) {
        CHECK(pthread_create(&producers[i], NULL, midi_producer, &channels[i]) == 0);
    }
    for (unsigned i = 0; i < 2; i++) CHECK(pthread_join(producers[i], NULL) == 0);
    CHECK(pthread_join(consumer, NULL) == 0);
    CHECK(fake_midi_length == 3000);
    unsigned notes[2] = {0};
    for (unsigned i = 0; i < fake_midi_length; i += 3) {
        uint8_t channel = fake_midi[i] & 0x0F;
        CHECK(channel < 2);
        CHECK(fake_midi[i] == (MIDI_NOTE_ON | channel));
        CHECK(fake_midi[i + 1] == (notes[channel]++ & 0x7F));
        CHECK(fake_midi[i + 2] == 100 + channel);
    }
    CHECK(notes[0] == 500 && notes[1] == 500);
    CHECK(!midi_output_overflowed());
}
#endif

#define RUN(test) do { reset(); test(); tests++; printf("PASS %s\n", #test); } while (0)

int main(void) {
    unsigned tests = 0;
    seq_lock_init();
    RUN(test_clock_rate_and_start_order);
    RUN(test_stopped_selection_survives_start);
    RUN(test_independent_track_queues);
    RUN(test_shorten_wraps_without_skipping_start);
    RUN(test_lengthen_keeps_playhead);
    RUN(test_queued_edit_is_heard_at_switch);
    RUN(test_song_preserves_unchanged_polymeter);
    RUN(test_empty_song_does_not_start);
    RUN(test_song_skips_initial_and_trailing_zero_repeats);
    RUN(test_mute_cuts_sounding_note_on_next_pulse);
    RUN(test_restart_releases_previous_note);
    RUN(test_panic_clears_all_outputs_and_queues);
    RUN(test_slide_route_change_releases_old_destination);
    RUN(test_same_note_slide_does_not_end_with_note_off);
    RUN(test_tie_does_not_keep_old_midi_channel);
    RUN(test_midi_only_tie_ignores_unused_cv_channel);
    RUN(test_cv_only_tie_ignores_unused_midi_channel);
    RUN(test_tie_does_not_keep_old_cv_channel);
    RUN(test_midi_only_slide_ignores_unused_cv_channel);
    RUN(test_queued_pattern_tie_ignores_unused_cv_channel);
    RUN(test_midi_and_gate_share_note_duration);
    RUN(test_outgoing_tie_keeps_gate_and_midi_connected);
    RUN(test_outgoing_slide_keeps_gate_and_changes_note);
    RUN(test_tempo_change_does_not_fake_jitter);
    RUN(test_missed_alarm_rearms_in_future);
    RUN(test_timer_wrap_is_normal);
    RUN(test_uart_overflow_never_sends_partial_message);
    RUN(test_staging_overflow_never_sends_partial_message);
    RUN(test_output_overflow_stops_transport);
#ifdef REBIRTH_THREADED_TEST
    RUN(test_stop_during_pending_gate_commit);
    RUN(test_panic_during_pending_gate_commit);
    RUN(test_concurrent_midi_producers_and_uart_irq);
#endif
    printf("%u playback tests passed\n", tests);
    return 0;
}
