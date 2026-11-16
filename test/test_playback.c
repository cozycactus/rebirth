#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
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

// Real-time bytes only: channel messages may go out with running status.
static unsigned count_byte(uint8_t byte) {
    unsigned count = 0;
    for (unsigned i = 0; i < fake_midi_length; i++) count += fake_midi[i] == byte;
    return count;
}

static fake_midi_msg_t midi_msgs[FAKE_MIDI_CAPACITY];

// Decodes all MIDI sent so far into midi_msgs; returns the message count.
static unsigned decode_midi(void) {
    return fake_midi_decode(midi_msgs, FAKE_MIDI_CAPACITY);
}

static bool msg_is(fake_midi_msg_t msg, uint8_t status, uint8_t data1, uint8_t data2) {
    return msg.status == status && msg.data1 == data1 && msg.data2 == data2;
}

// The decoded message `back` places before the last one.
static fake_midi_msg_t last_msg(unsigned back) {
    unsigned count = decode_midi();
    CHECK(count > back);
    return midi_msgs[count - 1 - back];
}

static unsigned count_note_on(uint8_t channel, uint8_t note) {
    unsigned count = 0;
    unsigned decoded = decode_midi();
    for (unsigned i = 0; i < decoded; i++) {
        count += midi_msgs[i].status == (MIDI_NOTE_ON | channel) && midi_msgs[i].data1 == note;
    }
    return count;
}

// True when byte `index` of fake_midi does not fall inside a channel message.
static bool message_boundary(unsigned index) {
    uint8_t running = 0;
    unsigned left = 0; // data bytes still due in the current message
    for (unsigned i = 0; i < index; i++) {
        uint8_t byte = fake_midi[i];
        if (byte >= 0xF8) continue;
        if (byte & 0x80) {
            running = byte;
            left = ((byte & 0xE0) == 0xC0) ? 1 : 2;
            continue;
        }
        if (left == 0) left = ((running & 0xE0) == 0xC0) ? 1 : 2; // running status
        left--;
    }
    return left == 0;
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
    transport_set_launch_quant(LAUNCH_QUANT_TRACK_LOOP);
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
    CHECK(decode_midi() >= 3);
    CHECK(midi_msgs[0].status == MIDI_START);
    CHECK(midi_msgs[1].status == MIDI_CLOCK);
    CHECK(msg_is(midi_msgs[2], MIDI_NOTE_ON, 60, 100));
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
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 1);
}

static void test_restart_releases_previous_note(void) {
    sound_note();
    transport_start();
    CHECK(fake_gates == 0);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 1);
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
    unsigned count = decode_midi();
    CHECK(count >= 32);
    for (uint8_t channel = 0; channel < 16; channel++) {
        const fake_midi_msg_t *pair = &midi_msgs[count - 32 + channel * 2];
        CHECK(msg_is(pair[0], MIDI_CC | channel, MIDI_CC_ALL_SOUND_OFF, 0));
        CHECK(msg_is(pair[1], MIDI_CC | channel, MIDI_CC_ALL_NOTES_OFF, 0));
    }
}

static void test_slide_route_change_releases_old_destination(void) {
    set_note(0, 0, 0, 60, STEP_FLAG_SLIDE);
    transport_start();
    tick();
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_CV_GATE;
    set_note(0, 0, 1, 65, 0);
    next_step();
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 1);
    CHECK(fake_gates == 1);
}

static void test_same_note_slide_does_not_end_with_note_off(void) {
    set_note(0, 0, 0, 60, STEP_FLAG_SLIDE);
    set_note(0, 0, 1, 60, 0);
    transport_start();
    tick();
    next_step();
    CHECK(fake_midi_count(MIDI_NOTE_ON) == 1);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 0);
    CHECK(note_is_on[0]);
}

static void test_tie_does_not_keep_old_midi_channel(void) {
    set_note(0, 0, 0, 60, STEP_FLAG_TIE);
    transport_start();
    tick();
    g_seq.patterns[0].tracks[0].midi_channel = 3;
    set_note(0, 0, 1, 60, 0);
    next_step();
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 1);
    CHECK(fake_midi_count(MIDI_NOTE_ON | 3) == 1);
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

    CHECK(fake_midi_count(MIDI_NOTE_ON) == 1);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 0);
    CHECK(fake_gates == 0);
    pulses(PULSES_PER_STEP / 2);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 1);
    CHECK(msg_is(last_msg(0), MIDI_NOTE_OFF, 60, 0));
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
    CHECK(fake_midi_count(MIDI_NOTE_ON | 3) == 0);
    pulses(PULSES_PER_STEP / 2);
    CHECK(fake_gates == 0);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 0);
    CHECK(fake_midi_count(MIDI_NOTE_OFF | 3) == 0);
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
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 1);
    CHECK(fake_midi_count(MIDI_NOTE_ON) == 2);
    pulses(PULSES_PER_STEP / 2);
    CHECK(fake_gates == 0);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 2);
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

    CHECK(msg_is(last_msg(1), MIDI_NOTE_ON, 65, 100));
    CHECK(msg_is(last_msg(0), MIDI_NOTE_OFF, 60, 0));
    CHECK(fake_gates == 0);
    pulses(PULSES_PER_STEP / 2);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 2);
    CHECK(msg_is(last_msg(0), MIDI_NOTE_OFF, 65, 0));
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
    CHECK(fake_midi_count(MIDI_NOTE_ON) == 1);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 0);
    CHECK(fake_gates == 0);
    pulses(PULSES_PER_STEP / 2);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 1);
}

static void test_midi_and_gate_share_note_duration(void) {
    sound_note();
    pulses(PULSES_PER_STEP / 2 - 1);
    CHECK(fake_gates == 1);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 0);
    tick();
    CHECK(fake_gates == 0);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 1);
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
    CHECK(fake_midi_count(MIDI_NOTE_ON) == 1);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 0);
    pulses(PULSES_PER_STEP / 2);
    CHECK(fake_gates == 0);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 1);
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
    CHECK(msg_is(last_msg(1), MIDI_NOTE_ON, 65, 100));
    CHECK(msg_is(last_msg(0), MIDI_NOTE_OFF, 60, 0));
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
    CHECK(decode_midi() == 85);
    for (unsigned note = 0; note < 84; note++) {
        CHECK(msg_is(midi_msgs[note], MIDI_NOTE_ON, note, 100));
    }
    CHECK(midi_msgs[84].status == MIDI_CLOCK);
    CHECK(message_boundary(fake_midi_length));
    CHECK(!fake_uart_tx_irq);
}

static void test_full_queue_drains_free_uart_before_dropping(void) {
    fake_uart_writable = false;
    for (unsigned note = 0; note < 85; note++) midi_send_note_on(0, note, 100);
    CHECK(!midi_output_overflowed()); // 255 bytes: the queue is full.
    fake_uart_writable = true;        // The UART can take bytes before its IRQ runs.
    midi_send_note_on(1, 42, 100);
    CHECK(!midi_output_overflowed());
    fake_irq_handlers[UART0_IRQ]();
    CHECK(decode_midi() == 86);
    CHECK(msg_is(midi_msgs[85], MIDI_NOTE_ON | 1, 42, 100));
}

static void play_song_entries(unsigned entries) {
    for (unsigned entry = 0; entry < entries; entry++) {
        for (unsigned step = 0; step < STEPS_PER_PATTERN; step++) next_step();
    }
}

static void start_three_entry_song_at_second_entry(void) {
    CHECK(song_append(0, 1));
    CHECK(song_append(1, 1));
    CHECK(song_append(2, 1));
    transport_set_mode(PLAY_MODE_SONG);
    transport_start();
    tick();
    play_song_entries(1);
    CHECK(g_seq.transport.song_position == 1);
    CHECK(g_seq.transport.current_pattern[0] == 1);
}

static void test_removing_earlier_song_entry_keeps_next_entry(void) {
    start_three_entry_song_at_second_entry();
    CHECK(song_remove(0)); // [1, 2]: entry 1 still plays, 2 follows.
    play_song_entries(1);
    CHECK(g_seq.transport.current_pattern[0] == 2);
}

static void test_inserting_earlier_song_entry_keeps_next_entry(void) {
    start_three_entry_song_at_second_entry();
    CHECK(song_insert(0, 3, 1)); // [3, 0, 1, 2]: entry 1 still plays, 2 follows.
    play_song_entries(1);
    CHECK(g_seq.transport.current_pattern[0] == 2);
}

static void test_removing_playing_song_entry_moves_to_following_entry(void) {
    start_three_entry_song_at_second_entry();
    CHECK(song_remove(1)); // [0, 2]: the removed entry finishes, then 2 plays.
    play_song_entries(1);
    CHECK(g_seq.transport.current_pattern[0] == 2);
}

static void test_removing_playing_first_song_entry_moves_to_following_entry(void) {
    CHECK(song_append(0, 2));
    CHECK(song_append(1, 1));
    CHECK(song_append(2, 1));
    transport_set_mode(PLAY_MODE_SONG);
    transport_start();
    tick();
    CHECK(song_remove(0)); // [1, 2]: its remaining repeat is dropped.
    play_song_entries(1);
    CHECK(g_seq.transport.song_position == 0);
    CHECK(g_seq.transport.current_pattern[0] == 1);
}

static void test_moving_song_entry_keeps_playing_entry_anchor(void) {
    start_three_entry_song_at_second_entry();
    CHECK(song_move(2, 0)); // [2, 0, 1]: entry 1 is last, so the loop returns to 2.
    play_song_entries(1);
    CHECK(g_seq.transport.current_pattern[0] == 2);
}

static void test_staging_overflow_never_sends_partial_message(void) {
    for (unsigned note = 0; note < 54; note++) midi_buf_note_on(0, note, 100);
    midi_buf_flush();
    CHECK(fake_midi_count(MIDI_NOTE_ON) == 53);
    CHECK(message_boundary(fake_midi_length));
    CHECK(midi_output_overflowed());
}

static bool midi_bytes_are(const uint8_t *expected, unsigned length) {
    return fake_midi_length == length && memcmp(fake_midi, expected, length) == 0;
}

static void test_running_status_within_burst(void) {
    fake_uart_writable = false;
    midi_send_note_on(0, 36, 100);
    midi_send_note_on(0, 38, 100);
    midi_send_note_on(1, 40, 100);
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    const uint8_t expected[] = {0x90, 36, 100, 38, 100, 0x91, 40, 100};
    CHECK(midi_bytes_are(expected, sizeof(expected)));
    CHECK(!fake_uart_tx_irq);
}

static void test_status_repeated_after_queue_ran_empty(void) {
    midi_send_note_on(0, 36, 100);
    midi_send_note_on(0, 38, 100);
    fake_uart_budget = 3;          // runs out on the last byte of the next message
    midi_send_note_on(0, 40, 100);
    midi_send_note_on(0, 41, 100); // queued after the queue ran empty
    CHECK(fake_uart_tx_irq);
    fake_uart_budget = UINT_MAX;
    fake_irq_handlers[UART0_IRQ]();
    const uint8_t expected[] = {
        0x90, 36, 100, 0x90, 38, 100, 0x90, 40, 100, 0x90, 41, 100
    };
    CHECK(midi_bytes_are(expected, sizeof(expected)));
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
    CHECK(msg_is(last_msg(0), MIDI_CC | 15, MIDI_CC_ALL_NOTES_OFF, 0));
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
    CHECK(decode_midi() == 1000);
    CHECK(message_boundary(fake_midi_length));
    unsigned notes[2] = {0};
    for (unsigned i = 0; i < 1000; i++) {
        uint8_t channel = midi_msgs[i].status & 0x0F;
        CHECK(channel < 2);
        uint8_t note = notes[channel]++ & 0x7F;
        CHECK(msg_is(midi_msgs[i], MIDI_NOTE_ON | channel, note, 100 + channel));
    }
    CHECK(notes[0] == 500 && notes[1] == 500);
    CHECK(!midi_output_overflowed());
}
#endif

#define RUN(test) do { reset(); test(); tests++; printf("PASS %s\n", #test); } while (0)

static void run_through_step(uint32_t step) {
    while (g_seq.transport.pulse_count <= step * PULSES_PER_STEP) tick();
}

static void start_quant_test(launch_quant_t q) {
    set_note(0, 0, 0, 60, 0);
    set_note(1, 0, 0, 62, 0);
    transport_set_launch_quant(q);
    transport_start();
}

static void test_bar_launch_waits_for_bar_line(void) {
    start_quant_test(LAUNCH_QUANT_BAR);
    run_through_step(5);
    CHECK(transport_queue_track_pattern(0, 1));
    run_through_step(15);
    CHECK(g_seq.transport.current_pattern[0] == 0);
    CHECK(g_seq.transport.queued_pattern[0] == 1);
    run_through_step(16);
    CHECK(g_seq.transport.current_pattern[0] == 1);
    CHECK(g_seq.transport.queued_pattern[0] == NO_QUEUED_PATTERN);
    CHECK(g_seq.transport.current_track_step[0] == 0);
}

static void test_bar_launch_on_short_loop_waits_for_bar(void) {
    set_length(0, 0, 12);
    set_length(1, 0, 12);
    start_quant_test(LAUNCH_QUANT_BAR);
    run_through_step(3);
    CHECK(transport_queue_track_pattern(0, 1));
    run_through_step(12);
    CHECK(g_seq.transport.current_pattern[0] == 0);
    run_through_step(16);
    CHECK(g_seq.transport.current_pattern[0] == 1);
    CHECK(g_seq.transport.current_track_step[0] == 0);
}

static void test_beat_launch_joins_in_phase(void) {
    set_note(1, 0, 8, 64, 0);
    start_quant_test(LAUNCH_QUANT_BEAT);
    run_through_step(5);
    CHECK(transport_queue_track_pattern(0, 1));
    run_through_step(7);
    CHECK(g_seq.transport.current_pattern[0] == 0);
    CHECK(count_note_on(0, 64) == 0);
    run_through_step(8);
    CHECK(g_seq.transport.current_pattern[0] == 1);
    CHECK(g_seq.transport.current_track_step[0] == 8);
    CHECK(count_note_on(0, 64) == 1);
}

static void test_launch_phase_matrix(void) {
    static const launch_quant_t quants[] = {
        LAUNCH_QUANT_BAR, LAUNCH_QUANT_BEAT, LAUNCH_QUANT_STEP, LAUNCH_QUANT_TRACK_LOOP
    };
    static const uint8_t due[] = {16, 8, 6, 16};
    static const uint8_t lengths[] = {5, 12, 16};
    static const uint8_t expected[4][3] = {
        {0, 0, 0}, {3, 8, 8}, {1, 6, 6}, {0, 0, 0}
    };
    for (unsigned q = 0; q < 4; q++) {
        for (unsigned l = 0; l < 3; l++) {
            reset();
            set_length(1, 0, lengths[l]);
            start_quant_test(quants[q]);
            run_through_step(5);
            CHECK(transport_queue_track_pattern(0, 1));
            run_through_step(due[q] - 1u);
            CHECK(g_seq.transport.current_pattern[0] == 0);
            run_through_step(due[q]);
            CHECK(g_seq.transport.current_pattern[0] == 1);
            CHECK(g_seq.transport.current_track_step[0] == expected[q][l]);
        }
    }
}

static void test_requeue_of_playing_location_keeps_phase(void) {
    set_length(0, 0, 12);
    start_quant_test(LAUNCH_QUANT_BAR);
    run_through_step(13);
    CHECK(transport_queue_track_pattern(0, 0));
    run_through_step(16);
    CHECK(g_seq.transport.queued_pattern[0] == NO_QUEUED_PATTERN);
    CHECK(g_seq.transport.current_track_step[0] == 4);
}

static void test_quant_change_retargets_pending_queue(void) {
    start_quant_test(LAUNCH_QUANT_BAR);
    run_through_step(2);
    CHECK(transport_queue_track_pattern(0, 1));
    transport_set_launch_quant(LAUNCH_QUANT_STEP);
    run_through_step(3);
    CHECK(g_seq.transport.current_pattern[0] == 1);
}

static void test_shortening_under_bar_does_not_launch_early(void) {
    start_quant_test(LAUNCH_QUANT_BAR);
    run_through_step(9);
    CHECK(transport_queue_track_pattern(0, 1));
    set_length(0, 0, 8);
    run_through_step(12);
    CHECK(g_seq.transport.current_pattern[0] == 0);
    run_through_step(16);
    CHECK(g_seq.transport.current_pattern[0] == 1);
}

static void test_queue_right_after_bar_waits_a_whole_bar(void) {
    start_quant_test(LAUNCH_QUANT_BAR);
    run_through_step(16);
    CHECK(transport_queue_track_pattern(0, 1));
    run_through_step(31);
    CHECK(g_seq.transport.current_pattern[0] == 0);
    run_through_step(32);
    CHECK(g_seq.transport.current_pattern[0] == 1);
}

static void test_launch_keeps_track_routing(void) {
    track_config_t cfg = g_seq.patterns[0].tracks[0];
    cfg.midi_channel = 5;
    cfg.output_mode = OUTPUT_MIDI_ONLY;
    seq_set_track_config(0, &cfg);
    set_note(3, 0, 0, 60, 0);
    transport_start();
    CHECK(transport_queue_track_pattern(0, 3));
    run_through_step(16);
    CHECK(g_seq.transport.current_pattern[0] == 3);
    CHECK(count_note_on(5, 60) > 0);
}

static void test_launch_quant_setter_ignores_invalid(void) {
    transport_set_launch_quant(LAUNCH_QUANT_BEAT);
    transport_set_launch_quant(LAUNCH_QUANT_COUNT);
    CHECK(transport_get_launch_quant() == LAUNCH_QUANT_BEAT);
}

static unsigned count_note_ons(void) {
    unsigned count = 0;
    unsigned decoded = decode_midi();
    for (unsigned i = 0; i < decoded; i++) count += (midi_msgs[i].status & 0xF0) == MIDI_NOTE_ON;
    return count;
}

static void test_clip_mask_tracks_edits(void) {
    CHECK(sequencer_clip_mask(1) == 0);
    set_note(9, 2, 3, 60, 0);
    CHECK(sequencer_clip_mask(1) == (1ull << (2 * 8 + 1)));
    set_length(9, 2, 3);
    CHECK(sequencer_clip_mask(1) == 0);
}

static void test_clip_mask_ignores_stored_mute(void) {
    g_seq.patterns[9].tracks[2].muted = true;
    set_note(9, 2, 0, 60, 0);
    CHECK(sequencer_clip_mask(1) == (1ull << (2 * 8 + 1)));
}

static void test_scene_queue_skips_tracks_already_there(void) {
    transport_start();
    CHECK(transport_queue_track_pattern(1, 9));
    run_through_step(16);
    CHECK(g_seq.transport.current_pattern[1] == 9);
    CHECK(transport_queue_track_pattern(1, 4));
    CHECK(transport_queue_track_pattern(3, 2));
    CHECK(transport_launch(UINT8_MAX, 9));
    CHECK(g_seq.transport.queued_pattern[1] == NO_QUEUED_PATTERN);
    CHECK(g_seq.transport.queued_pattern[3] == 9);
    CHECK(g_seq.transport.queued_pattern[0] == 9);
}

static void test_empty_scene_silences_all_tracks(void) {
    for (uint8_t track = 0; track < 4; track++)
        for (uint8_t step = 0; step < 16; step++) set_note(0, track, step, 60, 0);
    transport_set_launch_quant(LAUNCH_QUANT_BAR);
    transport_start();
    run_through_step(3);
    CHECK(transport_launch(UINT8_MAX, 5));
    run_through_step(16);
    for (uint8_t track = 0; track < NUM_TRACKS; track++)
        CHECK(g_seq.transport.current_pattern[track] == 5);
    unsigned before = count_note_ons();
    run_through_step(24);
    CHECK(count_note_ons() == before);
}

static void test_scene_selects_at_once_when_stopped(void) {
    CHECK(transport_launch(UINT8_MAX, 4));
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        CHECK(g_seq.transport.current_pattern[track] == 4);
        CHECK(g_seq.transport.queued_pattern[track] == NO_QUEUED_PATTERN);
    }
}

static void test_scene_rejected_in_song_mode(void) {
    transport_set_mode(PLAY_MODE_SONG);
    CHECK(!transport_launch(UINT8_MAX, 4));
}

static void test_snapshot_reports_queues_and_mutes(void) {
    transport_start();
    CHECK(transport_queue_track_pattern(2, 7));
    seq_set_track_mute(1, true);
    g_seq.patterns[0].tracks[3].muted = true;
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    CHECK(snap.queued[2] == 7);
    CHECK(snap.live_mutes == 0x02);
    CHECK(snap.stored_mutes & 0x08);
    CHECK(snap.state == TRANSPORT_PLAYING);
}

static unsigned muted_locations(uint8_t track) {
    unsigned count = 0;
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) count += g_seq.patterns[p].tracks[track].muted;
    return count;
}

static void test_track_config_keeps_each_stored_mute(void) {
    g_seq.patterns[4].tracks[2].muted = true;
    track_config_t cfg = g_seq.patterns[0].tracks[2];      // edited at an audible location
    cfg.midi_channel = 6;
    seq_set_track_config(2, &cfg);
    CHECK(muted_locations(2) == 1 && g_seq.patterns[4].tracks[2].muted);
    cfg = g_seq.patterns[4].tracks[2];                      // edited at the muted location
    cfg.midi_channel = 7;
    seq_set_track_config(2, &cfg);
    CHECK(muted_locations(2) == 1 && g_seq.patterns[4].tracks[2].muted);
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) CHECK(g_seq.patterns[p].tracks[2].midi_channel == 7);
}

static bool track_note_on(uint8_t channel, uint8_t note) {
    return count_note_on(channel, note) > 0;
}

static void test_mute_button_undoes_a_stored_mute(void) {
    set_note(0, 2, 0, 61, 0);
    set_note(9, 2, 0, 61, 0);
    g_seq.patterns[0].tracks[2].muted = true;
    g_seq.patterns[9].tracks[2].muted = true;
    sequencer_refresh_track_playback(2, 0);
    sequencer_refresh_track_playback(2, 9);
    transport_start();
    run_through_step(0);
    CHECK(!track_note_on(2, 61));                           // stored mute: silent
    seq_toggle_track_mute(2);                               // muted -> audible
    CHECK(g_seq.track_mutes == 0);
    CHECK(muted_locations(2) == 0);
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    CHECK(snap.stored_mutes == 0);
    run_through_step(16);
    CHECK(track_note_on(2, 61));                            // heard on its next step
    seq_toggle_track_mute(2);                               // audible -> live mute
    CHECK(g_seq.track_mutes == 1u << 2);
    seq_toggle_track_mute(2);
    CHECK(g_seq.track_mutes == 0);
}

static void test_sequencer_init_sets_kit_defaults(void) {
    kit_config_t expect;
    kit_config_defaults(&expect);
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        CHECK(memcmp(&g_seq.kit_config[t], &expect, sizeof(expect)) == 0);
    }
}

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
    RUN(test_full_queue_drains_free_uart_before_dropping);
    RUN(test_removing_earlier_song_entry_keeps_next_entry);
    RUN(test_inserting_earlier_song_entry_keeps_next_entry);
    RUN(test_removing_playing_song_entry_moves_to_following_entry);
    RUN(test_removing_playing_first_song_entry_moves_to_following_entry);
    RUN(test_moving_song_entry_keeps_playing_entry_anchor);
    RUN(test_staging_overflow_never_sends_partial_message);
    RUN(test_running_status_within_burst);
    RUN(test_status_repeated_after_queue_ran_empty);
    RUN(test_output_overflow_stops_transport);
    RUN(test_bar_launch_waits_for_bar_line);
    RUN(test_bar_launch_on_short_loop_waits_for_bar);
    RUN(test_beat_launch_joins_in_phase);
    RUN(test_launch_phase_matrix);
    RUN(test_requeue_of_playing_location_keeps_phase);
    RUN(test_quant_change_retargets_pending_queue);
    RUN(test_shortening_under_bar_does_not_launch_early);
    RUN(test_queue_right_after_bar_waits_a_whole_bar);
    RUN(test_launch_keeps_track_routing);
    RUN(test_launch_quant_setter_ignores_invalid);
    RUN(test_clip_mask_tracks_edits);
    RUN(test_clip_mask_ignores_stored_mute);
    RUN(test_scene_queue_skips_tracks_already_there);
    RUN(test_empty_scene_silences_all_tracks);
    RUN(test_scene_selects_at_once_when_stopped);
    RUN(test_scene_rejected_in_song_mode);
    RUN(test_snapshot_reports_queues_and_mutes);
    RUN(test_track_config_keeps_each_stored_mute);
    RUN(test_mute_button_undoes_a_stored_mute);
    RUN(test_sequencer_init_sets_kit_defaults);
#ifdef REBIRTH_THREADED_TEST
    RUN(test_stop_during_pending_gate_commit);
    RUN(test_panic_during_pending_gate_commit);
    RUN(test_concurrent_midi_producers_and_uart_irq);
#endif
    printf("%u playback tests passed\n", tests);
    return 0;
}
