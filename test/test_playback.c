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
    for (unsigned note = 0; note < 170; note++) midi_send_note_on(0, note & 0x7F, 100);
    midi_send_clock(); // 510 bytes; the Clock takes none: one queue byte remains.
    midi_send_note_on(1, 100, 100);
    CHECK(midi_output_overflowed());
    CHECK(fake_midi_length == 0);
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    CHECK(decode_midi() == 171);
    CHECK(midi_msgs[0].status == MIDI_CLOCK);
    for (unsigned note = 0; note < 170; note++) {
        CHECK(msg_is(midi_msgs[1 + note], MIDI_NOTE_ON, note & 0x7F, 100));
    }
    CHECK(message_boundary(fake_midi_length));
    CHECK(!fake_uart_tx_irq);
}

static void test_full_queue_drains_free_uart_before_dropping(void) {
    fake_uart_writable = false;
    for (unsigned note = 0; note < 170; note++) midi_send_note_on(0, note & 0x7F, 100);
    midi_send_clock();
    CHECK(!midi_output_overflowed()); // 510 of 511 bytes: no room for another message.
    fake_uart_writable = true;        // The UART can take bytes before its IRQ runs.
    midi_send_note_on(1, 42, 100);
    CHECK(!midi_output_overflowed());
    fake_irq_handlers[UART0_IRQ]();
    CHECK(decode_midi() == 172);
    CHECK(midi_msgs[0].status == MIDI_CLOCK);
    CHECK(msg_is(midi_msgs[171], MIDI_NOTE_ON | 1, 42, 100));
    CHECK(message_boundary(fake_midi_length));
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

// Lets the UART take one byte per TX interrupt until nothing is pending.
static void drain_byte_by_byte(void) {
    while (fake_uart_tx_irq) {
        unsigned written = fake_midi_length;
        fake_uart_budget = 1;
        fake_irq_handlers[UART0_IRQ]();
        CHECK(fake_midi_length == written + 1);
    }
    fake_uart_budget = UINT_MAX;
}

static void test_clock_with_empty_queue_goes_at_once(void) {
    midi_send_clock();
    const uint8_t expected[] = {MIDI_CLOCK};
    CHECK(midi_bytes_are(expected, sizeof(expected)));
    CHECK(!fake_uart_tx_irq);
}

static void test_clock_at_message_boundary(void) {
    fake_uart_writable = false;
    midi_send_note_on(0, 36, 100);
    midi_send_note_on(0, 38, 100);
    fake_uart_budget = 1;
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ](); // writes the first status byte
    midi_send_clock();
    drain_byte_by_byte();
    const uint8_t expected[] = {0x90, 36, 100, MIDI_CLOCK, 38, 100};
    CHECK(midi_bytes_are(expected, sizeof(expected)));
}

static void test_start_and_clock_overtake_old_notes(void) {
    fake_uart_writable = false;
    midi_send_note_on(0, 36, 100);
    midi_send_stop();
    midi_send_start();
    midi_send_clock();
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    const uint8_t expected[] = {MIDI_STOP, MIDI_START, MIDI_CLOCK, 0x90, 36, 100};
    CHECK(midi_bytes_are(expected, sizeof(expected)));
    CHECK(!fake_uart_tx_irq);
}

// Review focus: Stop then Start within one step while note bytes are still queued.
static void test_quick_restart_does_not_wait_for_old_notes(void) {
    set_note(0, 0, 0, 60, 0);
    transport_start();
    tick();                       // Start, Clock and the step's Note On go out
    unsigned sent = fake_midi_length;
    fake_uart_writable = false;
    pulses(4);                    // the next Clock waits on the blocked UART
    transport_stop();
    transport_start();
    tick();
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    const uint8_t expected[] = {
        MIDI_STOP, MIDI_START, MIDI_CLOCK, MIDI_NOTE_OFF, 60, 0, MIDI_NOTE_ON, 60, 100
    };
    CHECK(fake_midi_length == sent + sizeof(expected));
    CHECK(memcmp(fake_midi + sent, expected, sizeof(expected)) == 0);
}

static void test_continue_before_clock(void) {
    fake_uart_writable = false;
    midi_send_note_on(0, 36, 100);
    midi_send_stop();
    midi_send_continue();
    midi_send_clock();
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    const uint8_t expected[] = {MIDI_STOP, MIDI_CONTINUE, MIDI_CLOCK, 0x90, 36, 100};
    CHECK(midi_bytes_are(expected, sizeof(expected)));
}

static void test_stop_cancels_pending_clock(void) {
    fake_uart_writable = false;
    midi_send_note_on(0, 36, 100);
    midi_send_clock();
    midi_send_stop();
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    const uint8_t expected[] = {MIDI_STOP, 0x90, 36, 100};
    CHECK(midi_bytes_are(expected, sizeof(expected)));
}

static void test_panic_cancels_pending_clock(void) {
    transport_start();
    pulses(4);
    unsigned sent = fake_midi_length;
    fake_uart_writable = false;
    tick();                       // this pulse's Clock waits on the blocked UART
    sequencer_panic();
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    CHECK(fake_midi_length > sent);
    CHECK(fake_midi[sent] == MIDI_STOP);
    for (unsigned i = sent; i < fake_midi_length; i++) CHECK(fake_midi[i] != MIDI_CLOCK);
    CHECK(msg_is(last_msg(0), MIDI_CC | 15, MIDI_CC_ALL_NOTES_OFF, 0));
}

static void test_clear_queue_cancels_clock(void) {
    fake_uart_writable = false;
    midi_send_clock();
    midi_clear_output_queue();
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    CHECK(fake_midi_length == 0);
    CHECK(!fake_uart_tx_irq);
}

static void test_wait_idle_sends_pending_clock(void) {
    fake_uart_writable = false;
    midi_send_clock();
    fake_uart_writable = true;
    midi_wait_output_idle();
    const uint8_t expected[] = {MIDI_CLOCK};
    CHECK(midi_bytes_are(expected, sizeof(expected)));
}

static void test_clock_spacing_on_empty_pattern(void) {
    fake_uart_run_until(0);       // wire-time mode
    transport_start();
    for (unsigned pulse = 0; pulse < 96; pulse++) {
        fake_uart_run_until(timer_hw->alarm[0]);
        tick();
    }
    fake_uart_run_until(timer_hw->timerawl + 10000);
    uint32_t clock_us[24];
    unsigned clocks = 0;
    for (unsigned i = 0; i < fake_midi_length; i++) {
        if (fake_midi[i] != MIDI_CLOCK) continue;
        CHECK(clocks < 24);
        clock_us[clocks++] = fake_midi_time_us[i];
    }
    CHECK(clocks == 24);
    uint32_t expected_us = 4 * pulse_interval_us;
    for (unsigned c = 1; c < clocks; c++) {
        uint32_t gap_us = clock_us[c] - clock_us[c - 1];
        CHECK(gap_us + 320 >= expected_us && gap_us <= expected_us + 320);
    }
}

static void test_clock_waits_one_message_in_long_burst(void) {
    fake_uart_run_until(0);       // wire-time mode, at 0 us
    // Alternate channels: 3 bytes per message on the wire, 102 bytes in all.
    for (unsigned note = 0; note < 34; note++) midi_send_note_on(note & 1, 36 + note, 100);
    fake_uart_run_until(5000);
    midi_send_clock();
    fake_uart_run_until(100000);
    unsigned clock = 0;
    while (clock < fake_midi_length && fake_midi[clock] != MIDI_CLOCK) clock++;
    CHECK(clock < fake_midi_length);
    CHECK(fake_midi_time_us[clock] <= 5960);
    CHECK(message_boundary(clock));
    CHECK(fake_midi_count(MIDI_NOTE_ON) + fake_midi_count(MIDI_NOTE_ON | 1) == 34);
}

// Queues `bytes` bytes of whole messages: Program Changes, then Note Ons.
static void queue_bytes(unsigned bytes) {
    static const uint8_t program_change[] = {0xC0, 1};
    for (; bytes % 3; bytes -= 2) midi_send_bytes(program_change, sizeof(program_change));
    for (; bytes; bytes -= 3) midi_send_note_on(0, 60, 100);
}

static void test_try_send_keeps_reserve(void) {
    uint8_t burst[30];
    for (unsigned i = 0; i < 10; i++) {
        burst[3 * i] = MIDI_NOTE_ON | 9;
        burst[3 * i + 1] = (uint8_t)(36 + i);
        burst[3 * i + 2] = 100;
    }
    fake_uart_writable = false;
    queue_bytes(300);             // 211 bytes free
    CHECK(!midi_try_send_bytes_reserve(burst, sizeof(burst), 192));
    CHECK(!midi_output_overflowed());
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    CHECK(fake_midi_count(MIDI_NOTE_ON | 9) == 0);
    fake_uart_writable = false;
    queue_bytes(289);             // 222 bytes free: just enough
    CHECK(midi_try_send_bytes_reserve(burst, sizeof(burst), 192));
    midi_clear_output_queue();
    queue_bytes(100);
    CHECK(midi_try_send_bytes_reserve(burst, sizeof(burst), 192));
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    CHECK(fake_midi_count(MIDI_NOTE_ON | 9) == 10);
    CHECK(!midi_output_overflowed());
}

static void test_fifo_disabled(void) {
    midi_init();
    CHECK(!fake_uart_fifo_enabled);
}

static void test_output_overflow_stops_transport(void) {
    sound_note();
    fake_uart_writable = false;
    for (unsigned note = 0; note < 200; note++) midi_send_note_on(0, note & 0x7F, 100);
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

// ─── Kit tracks ─────────────────────────────────────────────────

#define KIT_CHANNEL 9
#define KIT_STATUS  (MIDI_NOTE_ON | KIT_CHANNEL)
#define KIT_LANE_SD 2

static fake_midi_msg_t msg[FAKE_MIDI_CAPACITY];

// Decodes all MIDI sent so far into msg[], channel messages only (real-time bytes left
// out); returns the count.
static unsigned msgs(void) {
    unsigned decoded = decode_midi();
    unsigned count = 0;
    for (unsigned i = 0; i < decoded; i++) {
        if (midi_msgs[i].status < 0xF8) msg[count++] = midi_msgs[i];
    }
    return count;
}

// Kit Note Ons on the kit channel with velocity `velocity` (0: releases) for `note`.
static unsigned count_kit(uint8_t note, uint8_t velocity) {
    unsigned count = 0;
    unsigned total = msgs();
    for (unsigned i = 0; i < total; i++) count += msg_is(msg[i], KIT_STATUS, note, velocity);
    return count;
}

// Kit Note Ons with a velocity (hits), any note.
static unsigned count_kit_hits(void) {
    unsigned count = 0;
    unsigned total = msgs();
    for (unsigned i = 0; i < total; i++) count += msg[i].status == KIT_STATUS && msg[i].data2 != 0;
    return count;
}

// Type KIT and `channel` in all 32 locations; the track stays OUTPUT_BOTH on its CV channel.
static void make_kit(uint8_t track, uint8_t channel) {
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        g_seq.patterns[p].tracks[track].type = TRACK_TYPE_KIT;
        g_seq.patterns[p].tracks[track].midi_channel = channel;
        g_seq.patterns[p].tracks[track].output_mode = OUTPUT_BOTH;
        sequencer_refresh_track_playback(track, p);
    }
}

static void set_kit(uint8_t pattern, uint8_t track, uint8_t step, uint8_t lane, uint8_t value) {
    uint32_t *word = &g_seq.patterns[pattern].kit_steps[track][step];
    *word = kit_set(*word, lane, value);
    sequencer_refresh_track_playback(track, pattern);
}

static void set_gate(uint8_t track, uint8_t pct) {
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        g_seq.patterns[p].tracks[track].gate_length_pct = pct;
        sequencer_refresh_track_playback(track, p);
    }
}

static void test_kit_all_voices_order_and_levels(void) {
    static const uint8_t notes[KIT_VOICES] = {36, 38, 39, 42, 46, 37, 45, 47, 50, 49, 51};
    make_kit(1, KIT_CHANNEL);
    for (uint8_t lane = 1; lane < KIT_LANES; lane++) set_kit(0, 1, 0, lane, KIT_NOTE);
    transport_start();
    tick();
    CHECK(msgs() == KIT_VOICES);
    for (unsigned i = 0; i < KIT_VOICES; i++) CHECK(msg_is(msg[i], KIT_STATUS, notes[i], 80));
    CHECK(fake_gates == 0);
    for (unsigned pulse = 0; pulse < 2 * PPQN; pulse++) {
        tick();
        CHECK(fake_gates == 0);
    }
    CHECK(fake_gate_rises == 0);
    CHECK(count_kit_hits() == KIT_VOICES);
}

static void test_kit_levels(void) {
    make_kit(1, KIT_CHANNEL);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_NOTE);
    set_kit(0, 1, 0, KIT_LANE_SD, KIT_ACCENT);
    set_kit(0, 1, 1, KIT_LANE_AC, 1);
    set_kit(0, 1, 1, KIT_LANE_BD, KIT_NOTE);
    set_kit(0, 1, 1, KIT_LANE_SD, KIT_ACCENT);
    transport_start();
    tick();
    CHECK(msgs() == 2);
    CHECK(msg_is(msg[0], KIT_STATUS, 36, 80));
    CHECK(msg_is(msg[1], KIT_STATUS, 38, 110));
    next_step();
    unsigned count = msgs();
    CHECK(count == 6);                      // two releases at the gate between them
    CHECK(msg_is(msg[4], KIT_STATUS, 36, 110));
    CHECK(msg_is(msg[5], KIT_STATUS, 38, 127));
}

static void test_kit_note_off_at_gate_is_velocity_zero(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 50);                        // 12 pulses
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_NOTE);
    set_kit(0, 1, 0, KIT_LANE_CH, KIT_NOTE);
    transport_start();
    tick();
    CHECK(msgs() == 2);
    for (unsigned pulse = 1; pulse < 12; pulse++) {
        tick();
        CHECK(msgs() == 2);
    }
    tick();                                 // pulse 12
    CHECK(msgs() == 4);
    CHECK(msg_is(msg[2], KIT_STATUS, 36, 0));
    CHECK(msg_is(msg[3], KIT_STATUS, 42, 0));
    CHECK(fake_midi_count(MIDI_NOTE_OFF | KIT_CHANNEL) == 0);
}

static void test_kit_gate_100_releases_before_new_hits(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 100);
    for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) set_kit(0, 1, step, KIT_LANE_BD, KIT_NOTE);
    transport_start();
    tick();
    CHECK(msgs() == 1);
    for (unsigned step = 1; step < 2 * STEPS_PER_PATTERN; step++) {
        unsigned before = msgs();
        pulses(PULSES_PER_STEP - 1);
        CHECK(msgs() == before);            // nothing within the step
        tick();
        CHECK(msgs() == before + 2);        // one release, then the hit, in the boundary pulse
        CHECK(msg_is(msg[before], KIT_STATUS, 36, 0));
        CHECK(msg_is(msg[before + 1], KIT_STATUS, 36, 80));
    }
}

static void test_kit_length_wraps(void) {
    static const uint8_t expected_step[] = {0, 1, 2, 3, 0, 1, 2, 3, 0};
    make_kit(1, KIT_CHANNEL);
    set_length(0, 1, 4);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_NOTE);
    transport_start();
    tick();
    for (unsigned i = 0; i < sizeof(expected_step); i++) {
        if (i) next_step();
        CHECK(g_seq.transport.current_track_step[1] == expected_step[i]);
        CHECK(count_kit(36, 80) == i / 4 + 1);
    }

    reset();
    make_kit(1, KIT_CHANNEL);
    set_length(0, 1, 6);
    set_kit(0, 1, 7, KIT_LANE_BD, KIT_NOTE);
    transport_start();
    tick();
    for (unsigned step = 0; step < 2 * STEPS_PER_PATTERN; step++) next_step();
    CHECK(count_kit_hits() == 0);
    CHECK(!(sequencer_clip_mask(0) & (1ull << (1 * 8 + 0))));
}

// The first hit of a flam is at the note level (its second hit is not checked here).
static void test_kit_flam_first_hit_at_note_level(void) {
    make_kit(1, KIT_CHANNEL);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_FLAM);
    set_kit(0, 1, 1, KIT_LANE_AC, 1);
    set_kit(0, 1, 1, KIT_LANE_BD, KIT_FLAM);
    transport_start();
    tick();
    CHECK(msgs() == 1 && msg_is(msg[0], KIT_STATUS, 36, 80));
    pulses(PULSES_PER_STEP - 1);
    unsigned before = msgs();
    tick();
    unsigned count = msgs();
    CHECK(count > before && msg_is(msg[count - 1], KIT_STATUS, 36, 110));
}

static void test_kit_stored_mute_and_clip_mask(void) {
    make_kit(1, KIT_CHANNEL);
    for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) set_kit(0, 1, step, KIT_LANE_BD, KIT_NOTE);
    transport_start();
    tick();
    CHECK(count_kit(36, 80) == 1);
    g_seq.patterns[0].tracks[1].muted = true;
    sequencer_refresh_track_playback(1, 0);
    next_step();
    next_step();
    CHECK(count_kit(36, 80) == 1);          // the next steps play nothing
    CHECK(count_kit(36, 0) == 1);           // the sounding voice ended at its countdown
    CHECK(sequencer_clip_mask(0) & (1ull << (1 * 8 + 0)));

    set_kit(9, 1, 2, KIT_LANE_AC, 1);       // bank 1, slot 1: AC only
    CHECK(!(sequencer_clip_mask(1) & (1ull << (1 * 8 + 1))));
    set_kit(9, 1, 2, KIT_LANE_BD, KIT_NOTE);
    CHECK(sequencer_clip_mask(1) & (1ull << (1 * 8 + 1)));
}

static void test_kit_live_mute_releases_next_pulse(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 100);
    for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) set_kit(0, 1, step, KIT_LANE_BD, KIT_NOTE);
    transport_start();
    tick();
    seq_toggle_track_mute(1);
    CHECK(g_seq.track_mutes == 1u << 1);
    unsigned before = msgs();
    tick();
    CHECK(msgs() == before + 1);
    CHECK(msg_is(msg[before], KIT_STATUS, 36, 0));
    pulses(PPQN);
    CHECK(msgs() == before + 1);            // muted: no hits, no second release
}

// Gate 100: BD and SD sounding on track 1.
static void sound_kit(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 100);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_NOTE);
    set_kit(0, 1, 0, KIT_LANE_SD, KIT_NOTE);
    transport_start();
    tick();
    CHECK(count_kit_hits() == 2);
}

// Stop queues the kit releases before the Stop byte. On a busy wire the Stop, a
// real-time byte, overtakes them at the next message boundary; every voice is still
// released before the next run's hits.
static void test_kit_stop_queues_releases_before_stop_byte(void) {
    sound_kit();
    unsigned sent = fake_midi_length;
    fake_uart_writable = false;             // the wire is busy
    transport_stop();
    CHECK(fake_midi_length == sent);
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    const uint8_t expected[] = {MIDI_STOP, KIT_STATUS, 36, 0, 38, 0};
    CHECK(fake_midi_length == sent + sizeof(expected));
    CHECK(memcmp(fake_midi + sent, expected, sizeof(expected)) == 0);
    transport_start();
    tick();                                 // the new run's first hits
    CHECK(msg_is(last_msg(1), KIT_STATUS, 36, 80));
    CHECK(msg_is(last_msg(0), KIT_STATUS, 38, 80));
    CHECK(count_kit(36, 0) == 1 && count_kit(38, 0) == 1);
    CHECK(fake_midi_count(MIDI_NOTE_OFF | KIT_CHANNEL) == 0);
}

static void test_kit_panic_releases(void) {
    sound_kit();
    sequencer_panic();
    unsigned count = decode_midi();
    CHECK(count >= 35);
    CHECK(msg_is(midi_msgs[count - 35], KIT_STATUS, 36, 0));
    CHECK(msg_is(midi_msgs[count - 34], KIT_STATUS, 38, 0));
    CHECK(msg_is(midi_msgs[count - 33], MIDI_STOP, 0, 0));
    CHECK(msg_is(midi_msgs[count - 32], MIDI_CC, MIDI_CC_ALL_SOUND_OFF, 0));
    CHECK(count_kit(36, 0) == 1 && count_kit(38, 0) == 1);
}

// A panic in the middle of a message cuts it: the drain starts again at a message
// boundary, so the bytes after the cut decode on their own, the Stop goes first and a
// Start queued behind the cut message is dropped. The cut message has the status of the
// kit's releases, so a running status kept across the cut would show.
static void test_panic_mid_message_resets_the_drain(void) {
    sound_kit();
    fake_uart_writable = false;
    midi_send_note_on(KIT_CHANNEL, 40, 100);
    unsigned cut = fake_midi_length + 1;
    fake_uart_budget = 1;
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();         // its status byte only
    CHECK(fake_midi_length == cut && fake_midi[cut - 1] == KIT_STATUS);
    midi_send_start();                      // waits for the message boundary
    sequencer_panic();
    CHECK(fake_midi_length == cut);
    fake_uart_budget = UINT_MAX;
    fake_irq_handlers[UART0_IRQ]();
    CHECK(!fake_uart_tx_irq);
    CHECK(fake_midi_length > cut && fake_midi[cut] == MIDI_STOP);
    for (unsigned i = cut; i < fake_midi_length; i++) CHECK(fake_midi[i] != MIDI_START);
    // Decode what follows the cut on its own: a data byte without a status would abort.
    fake_midi_length -= cut;
    memmove(fake_midi, fake_midi + cut, fake_midi_length);
    CHECK(message_boundary(fake_midi_length));
    unsigned count = decode_midi();
    CHECK(count == 1 + 2 + 32);             // the Stop, the releases, the 32 CCs
    CHECK(msg_is(midi_msgs[0], MIDI_STOP, 0, 0));
    CHECK(msg_is(midi_msgs[1], KIT_STATUS, 36, 0));
    CHECK(msg_is(midi_msgs[2], KIT_STATUS, 38, 0));
    CHECK(msg_is(midi_msgs[3], MIDI_CC, MIDI_CC_ALL_SOUND_OFF, 0));
    CHECK(msg_is(midi_msgs[count - 1], MIDI_CC | 15, MIDI_CC_ALL_NOTES_OFF, 0));
}

static void test_kit_restart_releases(void) {
    sound_kit();
    transport_start();
    CHECK(msg_is(last_msg(3), KIT_STATUS, 36, 0));
    CHECK(msg_is(last_msg(2), KIT_STATUS, 38, 0));
    CHECK(msg_is(last_msg(1), MIDI_STOP, 0, 0));
    CHECK(msg_is(last_msg(0), MIDI_START, 0, 0));
    tick();                                 // the new run's first hits
    CHECK(msg_is(last_msg(1), KIT_STATUS, 36, 80));
    CHECK(msg_is(last_msg(0), KIT_STATUS, 38, 80));
    CHECK(count_kit(36, 0) == 1 && count_kit(38, 0) == 1);
}

static void test_kit_mode_change_releases(void) {
    sound_kit();
    transport_set_mode(PLAY_MODE_SONG);
    CHECK(msg_is(last_msg(2), KIT_STATUS, 36, 0));
    CHECK(msg_is(last_msg(1), KIT_STATUS, 38, 0));
    CHECK(msg_is(last_msg(0), MIDI_STOP, 0, 0));
}

static void test_kit_settings_edit_keeps_sounding_note(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 100);
    set_kit(0, 1, 0, KIT_LANE_CH, KIT_NOTE);
    set_kit(0, 1, 1, KIT_LANE_CH, KIT_NOTE);
    transport_start();
    tick();
    CHECK(msgs() == 1 && msg_is(msg[0], KIT_STATUS, 42, 80));
    g_seq.kit_config[1].note[7] = 44;
    sequencer_refresh_track_playback(1, 0);
    next_step();
    CHECK(msgs() == 3);
    CHECK(msg_is(msg[1], KIT_STATUS, 42, 0));
    CHECK(msg_is(msg[2], KIT_STATUS, 44, 80));
}

// The release goes on the channel the voice was sent on.
static void test_kit_channel_edit_releases_on_old_channel(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 100);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_NOTE);
    set_kit(0, 1, 1, KIT_LANE_BD, KIT_NOTE);
    transport_start();
    tick();
    make_kit(1, 3);
    next_step();
    CHECK(msgs() == 3);
    CHECK(msg_is(msg[1], KIT_STATUS, 36, 0));
    CHECK(msg_is(msg[2], MIDI_NOTE_ON | 3, 36, 80));
}

// Voices still sounding at a step boundary with no release due (countdown 0) are
// released first, before the step's hits. No path leaves voices so today (a kind change
// releases them at the load); the test sets the countdown to 0 itself.
static void test_kit_step_releases_leftover_voices_first(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 100);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_NOTE);
    set_kit(0, 1, 0, KIT_LANE_SD, KIT_NOTE);
    set_kit(0, 1, 1, KIT_LANE_BD, KIT_NOTE);
    transport_start();
    tick();
    kit_runtime[1].countdown = 0;           // no release due
    pulses(PULSES_PER_STEP - 1);
    CHECK(msgs() == 2);
    tick();
    CHECK(msgs() == 5);
    CHECK(msg_is(msg[2], KIT_STATUS, 36, 0));
    CHECK(msg_is(msg[3], KIT_STATUS, 38, 0));
    CHECK(msg_is(msg[4], KIT_STATUS, 36, 80));
}

// ─── Kit flams ──────────────────────────────────────────────────

static void set_flam(uint8_t track, uint8_t flam_pulses) {
    g_seq.kit_config[track].flam_pulses = flam_pulses;
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) sequencer_refresh_track_playback(track, p);
}

static void test_flam_second_hit(void) {
    make_kit(1, KIT_CHANNEL);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_FLAM);
    set_kit(0, 1, 1, KIT_LANE_AC, 1);
    set_kit(0, 1, 1, KIT_LANE_BD, KIT_FLAM);
    transport_start();
    tick();                                 // pulse 0
    CHECK(msgs() == 1 && msg_is(msg[0], KIT_STATUS, 0x24, 0x50));
    pulses(2);
    CHECK(msgs() == 1);
    tick();                                 // pulse 3: flam 3
    CHECK(msgs() == 3);
    CHECK(msg_is(msg[1], KIT_STATUS, 0x24, 0x00));
    CHECK(msg_is(msg[2], KIT_STATUS, 0x24, 0x6E));
    pulses(11);                             // pulses 4-14
    CHECK(msgs() == 3);
    tick();                                 // gate 50 % + flam 3: the release at pulse 15
    CHECK(msgs() == 4 && msg_is(msg[3], KIT_STATUS, 0x24, 0x00));
    pulses(PULSES_PER_STEP - 16);           // pulses 16-23
    CHECK(msgs() == 4);
    tick();                                 // pulse 24: step 1, AC
    CHECK(msgs() == 5 && msg_is(msg[4], KIT_STATUS, 0x24, 0x6E));
    pulses(3);
    CHECK(msgs() == 7);
    CHECK(msg_is(msg[5], KIT_STATUS, 0x24, 0x00));
    CHECK(msg_is(msg[6], KIT_STATUS, 0x24, 0x7F));
}

static void test_flam_gate_extends_to_second_hit(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 10);                        // 2 pulses
    set_flam(1, 12);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_FLAM);
    transport_start();
    tick();                                 // pulse 0
    pulses(11);
    CHECK(msgs() == 1);
    tick();                                 // pulse 12: the second hit
    CHECK(msgs() == 3 && msg_is(msg[2], KIT_STATUS, 36, 110));
    tick();
    CHECK(msgs() == 3);
    tick();                                 // pulse 14 = gate 2 + flam 12
    CHECK(msgs() == 4 && msg_is(msg[3], KIT_STATUS, 36, 0));
    pulses(PPQN);
    CHECK(msgs() == 4);
}

// Review focus: gate 100 and flam 12 on BD, SD and CH on every step for one bar's first
// four steps, then Stop.
static void test_flam_gate_100_every_step(void) {
    static const uint8_t notes[] = {36, 38, 42};
    static unsigned sent_at[256];           // pulse each kit message came in; 96: Stop
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 100);
    set_flam(1, 12);
    for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
        set_kit(0, 1, step, KIT_LANE_BD, KIT_FLAM);
        set_kit(0, 1, step, KIT_LANE_SD, KIT_FLAM);
        set_kit(0, 1, step, KIT_LANE_CH, KIT_FLAM);
    }
    transport_start();
    unsigned seen = 0;
    for (unsigned pulse = 0; pulse <= 96; pulse++) {
        if (pulse < 96) tick(); else transport_stop();
        unsigned total = msgs();
        CHECK(total <= 256);
        for (; seen < total; seen++) sent_at[seen] = pulse;
    }
    for (unsigned n = 0; n < sizeof(notes); n++) {
        // Per note: the first hit, then (release, hit) at every flam and step, the
        // release at Stop last.
        unsigned expect_pulse[16];
        uint8_t expect_velocity[16];
        unsigned expected = 0;
        for (unsigned step = 0; step < 4; step++) {
            if (step) {
                expect_pulse[expected] = 24 * step;
                expect_velocity[expected++] = 0;
            }
            expect_pulse[expected] = 24 * step;
            expect_velocity[expected++] = 80;
            expect_pulse[expected] = 24 * step + 12;
            expect_velocity[expected++] = 0;
            expect_pulse[expected] = 24 * step + 12;
            expect_velocity[expected++] = 110;
        }
        expect_pulse[expected] = 96;
        expect_velocity[expected++] = 0;
        CHECK(expected == 16);
        unsigned found = 0;
        for (unsigned i = 0; i < seen; i++) {
            if (msg[i].status != KIT_STATUS || msg[i].data1 != notes[n]) continue;
            CHECK(found < expected);
            CHECK(sent_at[i] == expect_pulse[found]);
            CHECK(msg[i].data2 == expect_velocity[found]);
            found++;
        }
        CHECK(found == expected);           // eight hits, eight releases
    }
    CHECK(msg_is(last_msg(3), KIT_STATUS, 36, 0));
    CHECK(msg_is(last_msg(2), KIT_STATUS, 38, 0));
    CHECK(msg_is(last_msg(1), KIT_STATUS, 42, 0));
    CHECK(msg_is(last_msg(0), MIDI_STOP, 0, 0));
}

static void test_live_mute_cancels_pending_flam(void) {
    make_kit(1, KIT_CHANNEL);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_FLAM);
    set_kit(0, 1, 1, KIT_LANE_BD, KIT_FLAM);
    transport_start();
    tick();                                 // pulse 0: the first hit
    seq_toggle_track_mute(1);
    tick();                                 // pulse 1: released, the flam cancelled
    CHECK(msgs() == 2 && msg_is(msg[1], KIT_STATUS, 36, 0));
    pulses(PULSES_PER_STEP - 2);            // through pulse 23
    CHECK(msgs() == 2);
    seq_toggle_track_mute(1);
    pulses(4);                              // step 1 and its flam play
    CHECK(count_kit(36, 80) == 2);
    CHECK(count_kit(36, 110) == 1);
}

static void test_edit_during_flam_keeps_second_hit(void) {
    make_kit(1, KIT_CHANNEL);
    set_kit(0, 1, 0, KIT_LANE_CH, KIT_FLAM);
    transport_start();
    tick();                                 // pulse 0
    tick();
    g_seq.kit_config[1].note[7] = 44;       // CH
    sequencer_refresh_track_playback(1, 0);
    pulses(PULSES_PER_STEP - 2);            // through pulse 23
    CHECK(msgs() == 4);
    CHECK(msg_is(msg[0], KIT_STATUS, 42, 80));
    CHECK(msg_is(msg[1], KIT_STATUS, 42, 0));
    CHECK(msg_is(msg[2], KIT_STATUS, 42, 110));
    CHECK(msg_is(msg[3], KIT_STATUS, 42, 0));
}

static uint32_t flam_drops(void) {
    sequencer_jitter_stats_t stats;
    sequencer_get_jitter_stats(&stats);
    return stats.flam_drops;
}

static void test_flam_overload_drops_second_hits(void) {
    make_kit(1, KIT_CHANNEL);
    for (uint8_t lane = 1; lane < KIT_LANES; lane++) set_kit(0, 1, 0, lane, KIT_FLAM);
    transport_start();
    tick();                                 // pulse 0: the first hits go out at once
    CHECK(count_kit_hits() == KIT_VOICES);
    fake_uart_writable = false;
    // 255 bytes queued, 256 free: under KIT_FLAM_RESERVE (288) + 66, the 11-voice flam
    // message as queued.
    for (unsigned i = 0; i < 85; i++) midi_send_note_on(0, 60, 100);
    pulses(3);                              // pulse 3: flam 3
    CHECK(flam_drops() == 1);
    CHECK(!midi_output_overflowed());
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    fake_uart_writable = true;
    fake_irq_handlers[UART0_IRQ]();
    pulses(PULSES_PER_STEP - 4);            // gate 50 % + flam 3: the releases at pulse 15
    CHECK(count_kit_hits() == KIT_VOICES);  // no second hits
    for (uint8_t v = 0; v < KIT_VOICES; v++) {
        CHECK(count_kit(g_seq.kit_config[1].note[v], 0) == 1);
    }
    CHECK(!midi_output_overflowed());
    sequencer_reset_jitter_stats();
    tick();
    CHECK(flam_drops() == 0);
}

// Review focus: a restart under dense kit load, in wire time. Kits on tracks 0 and 4
// (channels 10 and 9), ten voices each (no OH) flammed on every step, six SYNTH tracks
// with a note on every step, 300 BPM. At pulse 768 the transport restarts: Start while
// playing, or (`stop_first`) Stop then an immediate Start, as from the PLAY button. The
// old run's releases join the TX backlog; the new run's first step must still fit.
static void dense_kit_restart(bool stop_first) {
    static const uint8_t gates[NUM_TRACKS] = {65, 27, 51, 87, 20, 49, 71, 21};
    fake_uart_run_until(0);                 // wire-time mode
    transport_set_bpm(300);
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        bool kit = t == 0 || t == 4;
        if (kit) make_kit(t, t == 0 ? 10 : 9);
        else g_seq.patterns[0].tracks[t].type = TRACK_TYPE_SYNTH;
        g_seq.patterns[0].tracks[t].gate_length_pct = gates[t];
        if (kit) g_seq.kit_config[t].flam_pulses = t == 0 ? 7 : 12;
        for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
            if (kit) {
                uint32_t word = 0;
                for (uint8_t l = 1; l < KIT_LANES; l++)
                    if (l != KIT_LANE_OH) word = kit_edit(word, l, KIT_FLAM);
                g_seq.patterns[0].kit_steps[t][s] = word;
            } else {
                pattern_set_step(&g_seq.patterns[0], t, s, 60 + s, 100, STEP_FLAG_ACTIVE);
            }
        }
        sequencer_refresh_track_playback(t, 0);
    }
    transport_start();
    for (unsigned p = 0; p < 1000; p++) {
        fake_uart_run_until(timer_hw->alarm[0]);
        tick();
        if (p == 768) {
            if (stop_first) transport_stop();
            transport_start();
        }
        CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
        CHECK(!midi_output_overflowed());
    }
}

static void test_dense_kit_restart_does_not_overflow(void) { dense_kit_restart(false); }
static void test_dense_kit_stop_start_does_not_overflow(void) { dense_kit_restart(true); }

typedef struct {
    float bpm;
    uint8_t kit_gate_pct;                   // the SYNTH tracks play at gate 100 %
    uint8_t flam_pulses;
    uint8_t flam_kits;                      // kits with every voice flammed: 0, 1 or 2
    uint8_t bars;
} load_case_t;

// Kits on tracks 0 and 1 (channels 9 and 10) with every voice on every step, six SYNTH
// tracks with a note every step, played in wire time; checks the run and returns the
// flam drops.
static uint32_t play_load(const load_case_t *c) {
    reset();
    fake_uart_run_until(0);                 // wire-time mode
    transport_set_bpm(c->bpm);
    for (uint8_t kit = 0; kit < 2; kit++) {
        make_kit(kit, (uint8_t)(9 + kit));
        set_flam(kit, c->flam_pulses);
        uint32_t word = 0;
        uint8_t value = kit < c->flam_kits ? KIT_FLAM : KIT_NOTE;
        for (uint8_t lane = 1; lane < KIT_LANES; lane++) word = kit_set(word, lane, value);
        for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
            g_seq.patterns[0].kit_steps[kit][step] = word;
        }
    }
    for (uint8_t track = 2; track < NUM_TRACKS; track++) {
        g_seq.patterns[0].tracks[track].type = TRACK_TYPE_SYNTH;
        for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
            pattern_set_step(&g_seq.patterns[0], track, step, 60, 100, STEP_FLAG_ACTIVE);
        }
    }
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        set_gate(track, track < 2 ? c->kit_gate_pct : 100);
    }
    transport_start();
    unsigned steps = c->bars * 16u;
    // The pulse time of each Clock (every PPQN / MIDI_PPQN pulses from the first).
    static uint32_t clock_due[16 * 16 * PULSES_PER_STEP / (PPQN / MIDI_PPQN)];
    unsigned clocks = 0;
    CHECK(steps * PULSES_PER_STEP / (PPQN / MIDI_PPQN) <= sizeof(clock_due) / sizeof(clock_due[0]));
    for (unsigned pulse = 0; pulse < steps * PULSES_PER_STEP; pulse++) {
        fake_uart_run_until(timer_hw->alarm[0]);
        if (pulse % (PPQN / MIDI_PPQN) == 0) clock_due[clocks++] = timer_hw->alarm[0];
        tick();
    }
    // A TX overflow would have stopped the transport (the ISR panics and clears the flag).
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    CHECK(!midi_output_overflowed());
    fake_uart_run_until(timer_hw->timerawl + 1000000); // the queue drains
    CHECK(!fake_uart_tx_irq);
    // Each Clock starts on the wire within 4 byte times (1,280 us) of its pulse: at most
    // the byte being shifted out and a whole channel message whose status is already in
    // the holding register.
    unsigned clock = 0;
    for (unsigned i = 0; i < fake_midi_length; i++) {
        if (fake_midi[i] != MIDI_CLOCK) continue;
        CHECK(clock < clocks);
        CHECK(fake_midi_time_us[i] - clock_due[clock] <= 4u * 320u);
        clock++;
    }
    CHECK(clock == clocks);
    // Stopped, every note still sounding is released (after the Clock check: the Stop
    // cancels a pending Clock).
    transport_stop();
    fake_uart_run_until(timer_hw->timerawl + 1000000);
    CHECK(!fake_uart_tx_irq);
    static unsigned ons[16][128], offs[16][128];
    memset(ons, 0, sizeof(ons));
    memset(offs, 0, sizeof(offs));
    unsigned total = msgs();
    unsigned first_hits[2] = {0, 0};
    unsigned second_hits = 0;
    for (unsigned i = 0; i < total; i++) {
        uint8_t kind = msg[i].status & 0xF0, channel = msg[i].status & 0x0F;
        if (kind == MIDI_NOTE_ON && msg[i].data2) ons[channel][msg[i].data1]++;
        else if (kind == MIDI_NOTE_ON || kind == MIDI_NOTE_OFF) offs[channel][msg[i].data1]++;
        for (uint8_t kit = 0; kit < 2; kit++) {
            if (msg[i].status != (MIDI_NOTE_ON | (9 + kit))) continue;
            first_hits[kit] += msg[i].data2 == 80;
            second_hits += msg[i].data2 == 110 || msg[i].data2 == 127;
        }
    }
    // Every hit has its release, per channel and note.
    for (unsigned channel = 0; channel < 16; channel++) {
        for (unsigned note = 0; note < 128; note++) CHECK(ons[channel][note] == offs[channel][note]);
    }
    CHECK(first_hits[0] == KIT_VOICES * steps);
    CHECK(first_hits[1] == KIT_VOICES * steps);
    // Every flam step has its second hits, but for those dropped: each drop is one kit's
    // flam pulse, all its voices.
    uint32_t drops = flam_drops();
    CHECK(second_hits == KIT_VOICES * (c->flam_kits * steps - drops));
    return drops;
}

static void test_kits_at_wire_rate(void) {
    static const float tempos[] = {150.0f, 200.0f, 300.0f};
    for (unsigned i = 0; i < sizeof(tempos) / sizeof(tempos[0]); i++) {
        load_case_t c = {tempos[i], 100, 3, 0, 16};
        CHECK(play_load(&c) == 0);
    }
    static const uint8_t gates[] = {10, 50, 100};
    static const uint8_t flams[] = {1, 12};
    for (unsigned g = 0; g < sizeof(gates); g++) {
        for (unsigned f = 0; f < sizeof(flams); f++) {
            load_case_t c = {300.0f, gates[g], flams[f], 2, 8};
            CHECK(play_load(&c) > 0);
        }
    }
    load_case_t one_kit = {150.0f, 100, 3, 1, 16};
    CHECK(play_load(&one_kit) == 0);
}

// ─── Kit launches, Song mode and type changes ───────────────────

// Under every quantization: A1 plays BD on every step at gate 100, A2 (SD on every step) is
// queued at step 5 and joins in the phase test_launch_phase_matrix expects for length 16.
static void test_kit_launch_every_quant(void) {
    static const launch_quant_t quants[] = {
        LAUNCH_QUANT_TRACK_LOOP, LAUNCH_QUANT_BAR, LAUNCH_QUANT_BEAT, LAUNCH_QUANT_STEP
    };
    static const uint8_t due[] = {16, 16, 8, 6};
    static const uint8_t phase[] = {0, 0, 8, 6};
    for (unsigned q = 0; q < 4; q++) {
        reset();
        make_kit(1, KIT_CHANNEL);
        set_gate(1, 100);
        for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
            set_kit(0, 1, step, KIT_LANE_BD, KIT_NOTE);
            set_kit(1, 1, step, KIT_LANE_SD, KIT_NOTE);
        }
        transport_set_launch_quant(quants[q]);
        transport_start();
        run_through_step(5);
        CHECK(transport_queue_track_pattern(1, 1));
        run_through_step(due[q] - 1u);
        CHECK(g_seq.transport.current_pattern[1] == 0);
        CHECK(count_kit(38, 80) == 0);
        unsigned before = msgs();
        run_through_step(due[q]);           // the launch point
        CHECK(g_seq.transport.current_pattern[1] == 1);
        CHECK(g_seq.transport.current_track_step[1] == phase[q]);
        CHECK(msgs() == before + 2);
        CHECK(msg_is(msg[before], KIT_STATUS, 0x24, 0x00));
        CHECK(msg_is(msg[before + 1], KIT_STATUS, 0x26, 80));
        CHECK(count_kit(36, 80) == due[q]);
        CHECK(count_kit(36, 0) == due[q]);
    }
}

// Song entries: the kit plays location 0 (BD), then location 1 (SD), at gate 100; the song
// stops at its end.
static void test_kit_song_mode(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 100);
    for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) {
        set_kit(0, 1, step, KIT_LANE_BD, KIT_NOTE);
        set_kit(1, 1, step, KIT_LANE_SD, KIT_NOTE);
    }
    uint8_t entry[NUM_TRACKS] = {0};
    CHECK(song_append_patterns(entry, 1));
    entry[1] = 1;
    CHECK(song_append_patterns(entry, 1));
    song_set_end_mode(SONG_END_STOP);
    transport_set_mode(PLAY_MODE_SONG);
    transport_start();
    run_through_step(15);                   // entry 0, its last step
    CHECK(count_kit(36, 80) == 16 && count_kit(38, 80) == 0);
    unsigned before = msgs();
    pulses(PULSES_PER_STEP);                // entry 1, its first step
    CHECK(g_seq.transport.song_position == 1);
    CHECK(g_seq.transport.current_pattern[1] == 1);
    CHECK(msgs() == before + 2);
    CHECK(msg_is(msg[before], KIT_STATUS, 36, 0));
    CHECK(msg_is(msg[before + 1], KIT_STATUS, 38, 80));
    pulses(15 * PULSES_PER_STEP);           // entry 1, its last step
    CHECK(count_kit(36, 80) == 16 && count_kit(38, 80) == 16);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    pulses(PULSES_PER_STEP);                // the end of the song
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    CHECK(msg_is(last_msg(1), KIT_STATUS, 38, 0));
    CHECK(msg_is(last_msg(0), MIDI_STOP, 0, 0));
    CHECK(count_kit(36, 0) == 16 && count_kit(38, 0) == 16);
    CHECK(kit_runtime[1].sounding == 0);
}

// Kit messages: each hit (velocity > 0) of a note has its release; all of them come
// before message `limit`. Returns the hits.
static unsigned kit_paired_before(unsigned limit) {
    unsigned total = msgs();
    unsigned hits[128] = {0};
    unsigned releases[128] = {0};
    unsigned count = 0;
    for (unsigned i = 0; i < total; i++) {
        if (msg[i].status != KIT_STATUS) continue;
        CHECK(i < limit);
        if (msg[i].data2) hits[msg[i].data1]++; else releases[msg[i].data1]++;
        count += msg[i].data2 != 0;
    }
    for (unsigned note = 0; note < 128; note++) CHECK(hits[note] == releases[note]);
    return count;
}

// Index of the first message with `status`; the message count when there is none.
static unsigned first_msg(uint8_t status) {
    unsigned total = msgs();
    for (unsigned i = 0; i < total; i++) {
        if (msg[i].status == status) return i;
    }
    return total;
}

// Review focus: track 1 switched from a kit to SYNTH (channel 2) while BD's flam is pending.
static void test_kit_to_synth_during_flam(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 100);
    set_flam(1, 12);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_FLAM);
    set_note(0, 1, 1, 60, 0);               // the step_t data the SYNTH row plays
    transport_start();
    tick();                                 // pulse 0: the first hit, the flam armed
    CHECK(kit_runtime[1].flam_countdown == 12);
    track_config_t synth = g_seq.patterns[0].tracks[1];
    synth.type = TRACK_TYPE_SYNTH;
    synth.midi_channel = 2;
    CHECK(seq_set_track_config(1, &synth));
    pulses(3 * PULSES_PER_STEP);            // through step 3
    unsigned first_synth = first_msg(MIDI_NOTE_ON | 2);
    CHECK(first_synth < msgs());
    CHECK(msg_is(msg[first_synth], MIDI_NOTE_ON | 2, 60, 100));
    CHECK(kit_paired_before(first_synth) == 2);   // the first hit and the second
    CHECK(kit_runtime[1].sounding == 0);
    CHECK(count_kit(36, 110) == 1);
}

// Review focus: a SYNTH note with a tie or slide (no countdown) on MIDI channel 0 and CV 0;
// the track switched to a kit on channel 9 releases it at the next step boundary on its
// stored route, before the kit's first hit.
static void synth_connected_note_to_kit(uint8_t flag) {
    g_seq.patterns[0].tracks[0].output_mode = OUTPUT_BOTH;
    g_seq.patterns[0].tracks[0].cv_channel = 0;
    set_note(0, 0, 0, 60, flag);
    set_kit(0, 0, 1, KIT_LANE_BD, KIT_NOTE);
    transport_start();
    tick();                                 // pulse 0: the synth note, gate high
    CHECK(note_is_on[0] && note_off_countdown[0] == 0);
    CHECK(fake_gates == 1);
    track_config_t kit = g_seq.patterns[0].tracks[0];
    kit.type = TRACK_TYPE_KIT;
    kit.midi_channel = KIT_CHANNEL;
    CHECK(seq_set_track_config(0, &kit));
    pulses(PULSES_PER_STEP - 1);            // through pulse 23: the note holds
    CHECK(fake_gates == 1);
    CHECK(msgs() == 1);
    tick();                                 // pulse 24: step 1
    CHECK(msgs() == 3);
    CHECK(msg_is(msg[1], MIDI_NOTE_OFF, 60, 0));
    CHECK(msg_is(msg[2], KIT_STATUS, 36, 80));
    CHECK(fake_gates == 0);
    CHECK(!note_is_on[0]);
    pulses(2 * PPQN);
    CHECK(fake_gates == 0);
    CHECK(fake_midi_count(MIDI_NOTE_OFF) == 1);
    CHECK(count_note_on(0, 60) == 1);
}

static void test_synth_tie_to_kit_releases_note_and_gate(void) {
    synth_connected_note_to_kit(STEP_FLAG_TIE);
}

static void test_synth_slide_to_kit_releases_note_and_gate(void) {
    synth_connected_note_to_kit(STEP_FLAG_SLIDE);
}

// Track 1, a kit sounding BD and SD at gate 100, switched to DRUM (note 40 on channel 2).
static void test_kit_to_drum_releases_voices(void) {
    make_kit(1, KIT_CHANNEL);
    set_gate(1, 100);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_NOTE);
    set_kit(0, 1, 0, KIT_LANE_SD, KIT_NOTE);
    set_note(0, 1, 1, 60, 0);
    transport_start();
    tick();
    CHECK(msgs() == 2);
    track_config_t drum = g_seq.patterns[0].tracks[1];
    drum.type = TRACK_TYPE_DRUM;
    drum.drum_note = 40;
    drum.midi_channel = 2;
    drum.output_mode = OUTPUT_MIDI_ONLY;
    CHECK(seq_set_track_config(1, &drum));
    pulses(PULSES_PER_STEP - 1);
    CHECK(msgs() == 2);
    tick();                                 // pulse 24: step 1
    CHECK(msgs() == 5);
    CHECK(msg_is(msg[2], KIT_STATUS, 36, 0));
    CHECK(msg_is(msg[3], KIT_STATUS, 38, 0));
    CHECK(msg_is(msg[4], MIDI_NOTE_ON | 2, 40, 100));
    pulses(2 * PPQN);
    CHECK(kit_paired_before(first_msg(MIDI_NOTE_ON | 2)) == 2);
    CHECK(kit_runtime[1].sounding == 0);
}

// Kit voices still sounding when an event row is installed are released before the row's
// first note. The test clears their countdown, which otherwise ends by the boundary.
static void test_kind_change_releases_kit_voices_left_sounding(void) {
    make_kit(1, KIT_CHANNEL);
    set_kit(0, 1, 0, KIT_LANE_BD, KIT_NOTE);
    set_kit(0, 1, 0, KIT_LANE_CH, KIT_NOTE);
    set_note(0, 1, 1, 60, 0);
    transport_start();
    tick();
    kit_runtime[1].countdown = 0;           // no release due
    track_config_t synth = g_seq.patterns[0].tracks[1];
    synth.type = TRACK_TYPE_SYNTH;
    synth.midi_channel = 2;
    CHECK(seq_set_track_config(1, &synth));
    pulses(PULSES_PER_STEP - 1);
    CHECK(msgs() == 2);
    tick();                                 // pulse 24: step 1
    CHECK(msgs() == 5);
    CHECK(msg_is(msg[2], KIT_STATUS, 36, 0));
    CHECK(msg_is(msg[3], KIT_STATUS, 42, 0));
    CHECK(msg_is(msg[4], MIDI_NOTE_ON | 2, 60, 100));
    CHECK(kit_runtime[1].sounding == 0);
}

// Track 1, a DRUM note (40 on channel 1, CV 1) at gate 100, switched to a kit on channel 9.
static void test_drum_to_kit_releases_note(void) {
    track_config_t drum = g_seq.patterns[0].tracks[1];
    drum.type = TRACK_TYPE_DRUM;
    drum.drum_note = 40;
    drum.midi_channel = 1;
    drum.output_mode = OUTPUT_BOTH;
    drum.cv_channel = 1;
    drum.gate_length_pct = 100;
    CHECK(seq_set_track_config(1, &drum));
    set_note(0, 1, 0, 60, 0);
    set_kit(0, 1, 1, KIT_LANE_BD, KIT_NOTE);
    transport_start();
    tick();                                 // pulse 0: the drum note, gate high
    CHECK(msgs() == 1 && msg_is(msg[0], MIDI_NOTE_ON | 1, 40, 100));
    CHECK(fake_gates == 1u << 1);
    track_config_t kit = drum;
    kit.type = TRACK_TYPE_KIT;
    kit.midi_channel = KIT_CHANNEL;
    CHECK(seq_set_track_config(1, &kit));
    pulses(PULSES_PER_STEP - 1);
    CHECK(fake_gates == 1u << 1);
    tick();                                 // pulse 24: step 1
    CHECK(msgs() == 3);
    CHECK(msg_is(msg[1], MIDI_NOTE_OFF | 1, 40, 0));
    CHECK(msg_is(msg[2], KIT_STATUS, 36, 80));
    CHECK(fake_gates == 0);
    CHECK(!note_is_on[1]);
    pulses(2 * PPQN);
    CHECK(fake_gates == 0);
    CHECK(fake_midi_count(MIDI_NOTE_OFF | 1) == 1);
}

static void test_type_change_keeps_both_step_arrays(void) {
    static const track_type_t types[] = {TRACK_TYPE_KIT, TRACK_TYPE_SYNTH, TRACK_TYPE_KIT};
    pattern_t *p = &g_seq.patterns[0];
    pattern_set_step(p, 1, 3, 64, 90, STEP_FLAG_ACTIVE | STEP_FLAG_ACCENT);
    p->kit_steps[1][5] = kit_set(0, KIT_LANE_SD, KIT_FLAM);
    step_t steps[STEPS_PER_PATTERN];
    uint32_t words[STEPS_PER_PATTERN];
    memcpy(steps, p->steps[1], sizeof(steps));
    memcpy(words, p->kit_steps[1], sizeof(words));
    track_config_t cfg = p->tracks[1];
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        cfg.type = types[i];
        CHECK(seq_set_track_config(1, &cfg));
        CHECK(p->tracks[1].type == types[i]);
    }
    CHECK(memcmp(steps, p->steps[1], sizeof(steps)) == 0);
    CHECK(memcmp(words, p->kit_steps[1], sizeof(words)) == 0);
}

static void test_kit_config_clamps_and_refreshes(void) {
    make_kit(1, KIT_CHANNEL);
    for (uint8_t step = 0; step < STEPS_PER_PATTERN; step++) set_kit(0, 1, step, KIT_LANE_BD, KIT_NOTE);
    transport_start();
    tick();                                 // pulse 0: BD on note 36
    kit_config_t cfg = g_seq.kit_config[1];
    cfg.note[0] = 200;
    cfg.flam_pulses = 0;
    cfg.vel_normal = 0;
    cfg.vel_accent = 0;
    cfg.vel_max = 0;
    seq_set_kit_config(1, &cfg);
    CHECK(g_seq.kit_config[1].note[0] == 127);
    CHECK(g_seq.kit_config[1].flam_pulses == 1);
    CHECK(g_seq.kit_config[1].vel_normal == 1);
    CHECK(g_seq.kit_config[1].vel_accent == 1);
    CHECK(g_seq.kit_config[1].vel_max == 1);
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        CHECK(prepared_patterns[p][1].kit.note[0] == 127);
        CHECK(prepared_patterns[p][1].kit.flam_pulses == 1);
    }
    next_step();                            // pulse 24: the next hit
    CHECK(msg_is(last_msg(0), KIT_STATUS, 127, 1));
    CHECK(count_kit(36, 0) == 1);           // the sounding voice ended on its own note
    cfg.flam_pulses = 13;
    seq_set_kit_config(1, &cfg);
    CHECK(g_seq.kit_config[1].flam_pulses == 12);
    CHECK(prepared_patterns[5][1].kit.flam_pulses == 12);
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
    RUN(test_clock_with_empty_queue_goes_at_once);
    RUN(test_clock_at_message_boundary);
    RUN(test_start_and_clock_overtake_old_notes);
    RUN(test_quick_restart_does_not_wait_for_old_notes);
    RUN(test_continue_before_clock);
    RUN(test_stop_cancels_pending_clock);
    RUN(test_panic_cancels_pending_clock);
    RUN(test_clear_queue_cancels_clock);
    RUN(test_wait_idle_sends_pending_clock);
    RUN(test_clock_spacing_on_empty_pattern);
    RUN(test_clock_waits_one_message_in_long_burst);
    RUN(test_try_send_keeps_reserve);
    RUN(test_fifo_disabled);
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
    RUN(test_kit_all_voices_order_and_levels);
    RUN(test_kit_levels);
    RUN(test_kit_note_off_at_gate_is_velocity_zero);
    RUN(test_kit_gate_100_releases_before_new_hits);
    RUN(test_kit_length_wraps);
    RUN(test_kit_flam_first_hit_at_note_level);
    RUN(test_kit_stored_mute_and_clip_mask);
    RUN(test_kit_live_mute_releases_next_pulse);
    RUN(test_kit_stop_queues_releases_before_stop_byte);
    RUN(test_kit_panic_releases);
    RUN(test_panic_mid_message_resets_the_drain);
    RUN(test_kit_restart_releases);
    RUN(test_kit_mode_change_releases);
    RUN(test_kit_settings_edit_keeps_sounding_note);
    RUN(test_kit_channel_edit_releases_on_old_channel);
    RUN(test_kit_step_releases_leftover_voices_first);
    RUN(test_flam_second_hit);
    RUN(test_flam_gate_extends_to_second_hit);
    RUN(test_flam_gate_100_every_step);
    RUN(test_live_mute_cancels_pending_flam);
    RUN(test_edit_during_flam_keeps_second_hit);
    RUN(test_flam_overload_drops_second_hits);
    RUN(test_dense_kit_restart_does_not_overflow);
    RUN(test_dense_kit_stop_start_does_not_overflow);
    RUN(test_kits_at_wire_rate);
    RUN(test_kit_launch_every_quant);
    RUN(test_kit_song_mode);
    RUN(test_kit_to_synth_during_flam);
    RUN(test_synth_tie_to_kit_releases_note_and_gate);
    RUN(test_synth_slide_to_kit_releases_note_and_gate);
    RUN(test_kit_to_drum_releases_voices);
    RUN(test_kind_change_releases_kit_voices_left_sounding);
    RUN(test_drum_to_kit_releases_note);
    RUN(test_type_change_keeps_both_step_arrays);
    RUN(test_kit_config_clamps_and_refreshes);
#ifdef REBIRTH_THREADED_TEST
    RUN(test_stop_during_pending_gate_commit);
    RUN(test_panic_during_pending_gate_commit);
    RUN(test_concurrent_midi_producers_and_uart_irq);
#endif
    printf("%u playback tests passed\n", tests);
    return 0;
}
