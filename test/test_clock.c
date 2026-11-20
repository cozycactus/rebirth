// Following an external MIDI clock: the production sequencer, UART interrupt and
// clock path with the fakes. Device time moves only in run_until(): alarm 0 fires
// the timer ISR at its time, a forced TIMER_IRQ_1 runs the doorbell at once, and a
// byte on the DIN input runs the UART interrupt at the moment it arrives.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fakes/fake_hardware.h"
#include "kit.h"
#include "../src/sequencer.c"

sequencer_state_t g_seq;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define T145 17241u     // a Clock interval at 145 BPM, us
#define T120 20833u
#define T300 8333u
#define T20  125000u

static uint32_t now(void) { return timer_hw->timerawl; }

// With the wire's time on (fake_uart_run_until), the UART sends a byte per 320 us
// while device time moves; otherwise every byte leaves at once.
static bool wire_time;

static void advance_to(uint32_t t) {
    if (wire_time) {
        fake_uart_run_until(t);
    } else {
        timer_hw->timerawl = t;
    }
}

static void run_until(uint32_t until) {
    for (unsigned guard = 0; ; guard++) {
        CHECK(guard < 10000000u);
        if (timer_hw->intf & (1u << CLOCK_IN_ALARM)) {
            sequencer_clock_in_isr();
            CHECK(!(timer_hw->intf & (1u << CLOCK_IN_ALARM)));
            continue;
        }
        uint32_t alarm = timer_hw->alarm[0];
        // An alarm behind the clock would never fire on the RP2040.
        CHECK((int32_t)(alarm - now()) >= 0);
        if ((int32_t)(alarm - until) > 0) break;
        advance_to(alarm);
        sequencer_timer_isr();
    }
    advance_to(until);
}

// One byte whose stop bit ends at `at`: the UART interrupt reads it then, and the
// doorbell it rings runs at once on core 1.
static void wire(uint32_t at, uint8_t byte) {
    run_until(at);
    fake_uart_rx_push(&byte, 1);
    fake_irq_handlers[UART0_IRQ]();
    run_until(at);
}

// `count` Clocks `interval` apart, the first at `first`; returns when the last came.
static uint32_t clocks(uint32_t first, uint32_t interval, unsigned count) {
    uint32_t t = first;
    for (unsigned i = 0; i < count; i++) {
        wire(t, MIDI_CLOCK);
        if (i + 1 < count) t += interval;
    }
    return t;
}

static unsigned count_byte(uint8_t byte) {
    unsigned count = 0;
    for (unsigned i = 0; i < fake_midi_length; i++) count += fake_midi[i] == byte;
    return count;
}

static fake_midi_msg_t msgs[FAKE_MIDI_CAPACITY];

static unsigned note_ons(uint8_t channel, uint8_t note) {
    unsigned n = fake_midi_decode(msgs, FAKE_MIDI_CAPACITY), count = 0;
    for (unsigned i = 0; i < n; i++) {
        count += msgs[i].status == (MIDI_NOTE_ON | channel) && msgs[i].data1 == note &&
                 msgs[i].data2 > 0;
    }
    return count;
}

// Every Note On with a velocity, in order, as channel << 8 | note.
static unsigned played(uint16_t *out, unsigned max) {
    unsigned n = fake_midi_decode(msgs, FAKE_MIDI_CAPACITY), count = 0;
    for (unsigned i = 0; i < n && count < max; i++) {
        if ((msgs[i].status & 0xF0) == MIDI_NOTE_ON && msgs[i].data2 > 0) {
            out[count++] = (uint16_t)((msgs[i].status & 0x0F) << 8 | msgs[i].data1);
        }
    }
    return count;
}

static void reset(clock_source_t source) {
    uint32_t shared_lock = g_seq.spinlock_num;
    memset(&g_seq, 0, sizeof(g_seq));
    g_seq.spinlock_num = shared_lock;
    g_seq.spinlock_ready = true;
    fake_hardware_reset();
    wire_time = false;
    timer_hw->timerawl = 1000;
    midi_init();
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) pattern_init(&g_seq.patterns[p], p);
    for (uint8_t track = 0; track < NUM_TRACKS; track++) {
        g_seq.transport.queued_pattern[track] = NO_QUEUED_PATTERN;
    }
    g_seq.transport.bpm = DEFAULT_BPM;
    sequencer_init();
    transport_set_launch_quant(LAUNCH_QUANT_BAR);
    timer_hw->inte = 1u | (1u << CLOCK_IN_ALARM);
    timer_hw->alarm[0] = now() + pulse_interval_us;
    transport_set_clock_source(source);
}

static void set_note(uint8_t pattern, uint8_t track, uint8_t step, uint8_t note, uint8_t flags) {
    pattern_set_step(&g_seq.patterns[pattern], track, step, note, 100, STEP_FLAG_ACTIVE | flags);
    sequencer_refresh_track_playback(track, pattern);
}

static void set_length(uint8_t pattern, uint8_t track, uint8_t length) {
    pattern_set_track_length(&g_seq.patterns[pattern], track, length);
    sequencer_refresh_track_playback(track, pattern);
}

// A note on every step of track 0 (channel 0, note 40 + step).
static void every_step(void) {
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) set_note(0, 0, s, (uint8_t)(40 + s), 0);
}

static bool following(void) { return ext_following; }

// ─── The estimate ───────────────────────────────────────────────

static void test_estimate_steady_clock(void) {
    clock_est_t e;
    clock_est_reset(&e);
    CHECK(clock_est_timeout_us(&e) == CLOCK_EST_LOSS_UNKNOWN_US);
    uint32_t t = 5000;
    CHECK(clock_est_tick(&e, t) == 0);
    for (unsigned i = 0; i < 200; i++) {
        t += T145 + (i % 2);                 // 17241.5 on average
        CHECK(clock_est_tick(&e, t) >= T145);
    }
    CHECK(e.interval_us == T145 || e.interval_us == T145 + 1);
    float bpm = 40000000.0f / (float)e.shown_x16;
    CHECK(fabsf(bpm - 145.0f) < 0.01f);
    CHECK(e.plausible == 200);
    CHECK(clock_est_timeout_us(&e) == 3u * e.interval_us);
    CHECK(clock_est_last(&e) == t);
}

static void test_estimate_jittered_clock_shows_a_steady_tempo(void) {
    // +-1 ms on every Clock, as from a USB-MIDI interface: the shown tenth holds.
    clock_est_t e;
    clock_est_reset(&e);
    uint32_t t = 5000, nominal = 5000;
    unsigned seed = 1;
    float lo = 1000.0f, hi = 0.0f;
    for (unsigned i = 0; i < 2000; i++) {
        seed = seed * 1103515245u + 12345u;
        int32_t jitter = (int32_t)((seed >> 16) % 2001u) - 1000;
        nominal += T145;
        t = nominal + (uint32_t)jitter;
        clock_est_tick(&e, t);
        if (i > 300) {
            float bpm = 40000000.0f / (float)e.shown_x16;
            if (bpm < lo) lo = bpm;
            if (bpm > hi) hi = bpm;
        }
    }
    CHECK(lo > 144.85f && hi < 145.15f);
}

static void test_estimate_follows_a_tempo_change_within_a_beat(void) {
    clock_est_t e;
    clock_est_reset(&e);
    uint32_t t = 5000;
    for (unsigned i = 0; i < 200; i++) clock_est_tick(&e, t += T120);
    for (unsigned i = 0; i < 24; i++) clock_est_tick(&e, t += T145);
    float bpm = 40000000.0f / (float)e.shown_x16;
    CHECK(fabsf(bpm - 145.0f) < 0.2f);
    CHECK(e.interval_us == T145);
}

static void test_estimate_restarts_after_a_gap(void) {
    clock_est_t e;
    clock_est_reset(&e);
    uint32_t t = 5000;
    for (unsigned i = 0; i < 50; i++) clock_est_tick(&e, t += T145);
    CHECK(e.count == 50);
    t += 400000;                             // a flash save: no Clock for 400 ms
    CHECK(clock_est_tick(&e, t) == 0);
    CHECK(e.count == 1 && e.interval_us == 0 && e.shown_x16 == 0);
    clock_est_tick(&e, t += T145);
    CHECK(e.interval_us == T145);
    // A burst is implausible: the run of plausible intervals ends.
    for (unsigned i = 0; i < 5; i++) clock_est_tick(&e, t += T145);
    CHECK(e.plausible >= 5);
    clock_est_tick(&e, t += 320);
    CHECK(e.plausible == 0);
}

static void test_timeout_has_a_floor(void) {
    clock_est_t e;
    clock_est_reset(&e);
    uint32_t t = 5000;
    for (unsigned i = 0; i < 10; i++) clock_est_tick(&e, t += T300);
    CHECK(clock_est_timeout_us(&e) == CLOCK_EST_LOSS_MIN_US);
    clock_est_reset(&e);
    for (unsigned i = 0; i < 10; i++) clock_est_tick(&e, t += T20);
    CHECK(clock_est_timeout_us(&e) == 3u * T20);
}

// ─── The receiving side ─────────────────────────────────────────

static void test_spp_with_real_time_bytes_inside(void) {
    reset(CLOCK_SOURCE_EXT);
    // F2, a Clock, LSB 5, Active Sensing, MSB 2: SPP 5 + 2 * 128 = 261.
    const uint8_t bytes[] = {0xF2, 0xF8, 5, 0xFE, 2};
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        CHECK(clock_in_rx_byte(bytes[i], 100 + i) == (bytes[i] == 0xF8));
    }
    clock_in_event_t ev;
    CHECK(clock_in_pop(&ev) && ev.kind == CLOCK_IN_TICK && ev.t_us == 101);
    CHECK(clock_in_pop(&ev) && ev.kind == CLOCK_IN_SPP && ev.value == 261 && ev.t_us == 104);
    CHECK(!clock_in_pop(&ev));
    // A status byte inside it abandons it; data bytes after it make nothing.
    const uint8_t cut[] = {0xF2, 7, 0x90, 1, 60, 100};
    for (unsigned i = 0; i < sizeof(cut); i++) CHECK(!clock_in_rx_byte(cut[i], 200));
    CHECK(!clock_in_pop(&ev));
    CHECK(clock_in_take_pushed());
    CHECK(!clock_in_take_pushed());
}

static void test_ring_full_drops_and_counts(void) {
    reset(CLOCK_SOURCE_EXT);
    for (unsigned i = 0; i < CLOCK_IN_RING_SIZE + 3; i++) clock_in_rx_byte(MIDI_CLOCK, i);
    CHECK(clock_in_dropped() == 3);
    clock_in_event_t ev;
    unsigned n = 0;
    while (clock_in_pop(&ev)) CHECK(ev.t_us == n++);
    CHECK(n == CLOCK_IN_RING_SIZE);
}

static void test_uart_interrupt_rings_the_doorbell(void) {
    reset(CLOCK_SOURCE_EXT);
    uint8_t note_on[] = {0x90, 60, 100};
    fake_uart_rx_push(note_on, sizeof(note_on));
    fake_irq_handlers[UART0_IRQ]();
    CHECK(!(timer_hw->intf & (1u << CLOCK_IN_ALARM)));      // notes do not
    uint8_t clock = MIDI_CLOCK;
    timer_hw->timerawl += 320;
    fake_uart_rx_push(&clock, 1);
    fake_irq_handlers[UART0_IRQ]();
    CHECK(timer_hw->intf & (1u << CLOCK_IN_ALARM));
    CHECK(ext_est.count == 0);              // core 1 has not run yet
    run_until(now());
    CHECK(ext_est.count == 1 && jitter_stats.ext_clocks == 1);
}

// ─── Following ──────────────────────────────────────────────────

static void test_start_then_clock_one_byte_later_plays_one_step(void) {
    reset(CLOCK_SOURCE_EXT);
    set_note(0, 0, 0, 60, 0);
    set_note(0, 0, 1, 62, 0);
    uint32_t t = now() + 5000;
    wire(t, MIDI_START);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED && transport_armed());
    CHECK(count_byte(MIDI_START) == 0);      // not before the first Clock
    wire(t + 320, MIDI_CLOCK);
    run_until(t + 320);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING && !transport_armed());
    CHECK(g_seq.transport.pulse_count == 1);
    CHECK(note_ons(0, 60) == 1 && note_ons(0, 62) == 0);
    unsigned n = fake_midi_decode(msgs, FAKE_MIDI_CAPACITY);
    CHECK(n >= 3 && msgs[0].status == MIDI_START && msgs[1].status == MIDI_CLOCK);
    // The interval is not known yet: the three later pulses are spaced by the internal
    // pulse period (120 BPM).
    run_until(t + 320 + pulse_interval_us - 10);
    CHECK(g_seq.transport.pulse_count == 1);
    run_until(t + 320 + pulse_interval_us + 10);
    CHECK(g_seq.transport.pulse_count == 2);
    run_until(t + 320 + 3 * pulse_interval_us + 10);
    CHECK(g_seq.transport.pulse_count == 4);
    run_until(t + 320 + T145 - 10);
    CHECK(g_seq.transport.pulse_count == 4);  // nothing runs ahead of the Clock
}

static void check_rate(uint32_t interval) {
    reset(CLOCK_SOURCE_EXT);
    every_step();
    uint32_t t = now() + 1000;
    clocks(t, interval, 30);                 // the tempo is known before the start
    t += 30 * interval;
    wire(t, MIDI_START);
    uint32_t last = clocks(t + interval, interval, 96);
    run_until(last + interval - interval / 8);
    CHECK(g_seq.transport.pulse_count == 4u * 96u);
    CHECK(count_byte(MIDI_CLOCK) == 96);
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) CHECK(note_ons(0, (uint8_t)(40 + s)) == 1);
    CHECK(jitter_stats.ext_catchups == 0 && jitter_stats.ext_dropped == 0);
}

static void test_four_pulses_per_clock_at_20_145_300_bpm(void) {
    check_rate(T20);
    check_rate(T145);
    check_rate(T300);
}

static void test_steps_start_on_their_clock(void) {
    reset(CLOCK_SOURCE_EXT);
    every_step();
    uint32_t t = now() + 1000;
    clocks(t, T145, 10);
    t += 10 * T145;
    wire(t, MIDI_START);
    for (unsigned c = 0; c < 48; c++) {
        uint32_t at = t + T145 + c * T145;
        wire(at, MIDI_CLOCK);
        run_until(at);
        // The Clock's first pulse runs as it is handled: on a step's Clock, the step.
        CHECK(g_seq.transport.pulse_count == 4u * c + 1u);
        if (c % 6 == 0) CHECK(note_ons(0, (uint8_t)(40 + c / 6)) == 1);
    }
}

static void test_early_and_late_clocks_lose_and_double_nothing(void) {
    reset(CLOCK_SOURCE_EXT);
    every_step();
    uint32_t t = now() + 1000;
    clocks(t, T145, 8);
    t += 8 * T145;
    wire(t, MIDI_START);
    unsigned seed = 7;
    uint32_t nominal = t, last = t;
    for (unsigned c = 0; c < 96; c++) {
        seed = seed * 1103515245u + 12345u;
        int32_t jitter = (int32_t)((seed >> 16) % 7001u) - 3500;   // +-3.5 ms
        nominal += T145;
        last = nominal + (uint32_t)jitter;
        wire(last, MIDI_CLOCK);
    }
    run_until(last + T145 - 100);           // the last Clock's three later pulses ran
    CHECK(g_seq.transport.pulse_count == 4u * 96u);
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) CHECK(note_ons(0, (uint8_t)(40 + s)) == 1);
    CHECK(count_byte(MIDI_CLOCK) == 96);
    CHECK(jitter_stats.ext_max_dev_us > 3000 && jitter_stats.ext_dropped == 0);
}

static void test_a_burst_is_caught_up_not_played_at_once(void) {
    reset(CLOCK_SOURCE_EXT);
    every_step();
    uint32_t t = now() + 1000;
    clocks(t, T145, 8);
    t += 8 * T145;
    wire(t, MIDI_START);
    uint32_t last = clocks(t + T145, T145, 12);
    // A stall of two Clock times, then they come in a bunch, then on time again.
    uint32_t b = last + 2 * T145 + 4000;
    clocks(b, 320, 6);
    CHECK(g_seq.transport.pulse_count < 4u * 12u + 4u + 12u);   // not run at once
    CHECK(jitter_stats.ext_catchups == 1);
    // On time again: everything is caught up within a few Clocks.
    uint32_t again = clocks(last + 3 * T145, T145, 10);
    run_until(again + T145 - 100);
    CHECK(g_seq.transport.pulse_count == 4u * (12u + 6u + 10u));
    CHECK(count_byte(MIDI_CLOCK) == 12 + 6 + 10);
    for (uint8_t s = 0; s < 4; s++) CHECK(note_ons(0, (uint8_t)(40 + s)) == 1);
}

static void test_the_backlog_is_capped(void) {
    reset(CLOCK_SOURCE_EXT);
    uint32_t t = now() + 1000;
    clocks(t, T145, 8);
    t += 8 * T145;
    wire(t, MIDI_START);
    uint32_t last = clocks(t + T145, T145, 4);
    clocks(last + 3000, 320, 30);            // 30 Clocks in 10 ms
    CHECK(ext_owed <= EXT_OWED_MAX);
    CHECK(jitter_stats.ext_dropped > 0);
    run_until(now() + 2 * T145);
    CHECK(g_seq.transport.pulse_count == 4u * (4u + 30u - jitter_stats.ext_dropped));
    CHECK(!midi_output_overflowed());
}

static void make_kit(uint8_t track, uint8_t channel) {
    for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
        g_seq.patterns[p].tracks[track].type = TRACK_TYPE_KIT;
        g_seq.patterns[p].tracks[track].midi_channel = channel;
        sequencer_refresh_track_playback(track, p);
    }
}

static void test_two_kits_flamming_through_bursts_never_overflow(void) {
    reset(CLOCK_SOURCE_EXT);
    make_kit(4, 9);
    make_kit(5, 10);
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        uint32_t word = 0;
        for (uint8_t lane = 1; lane < KIT_LANES; lane++) word = kit_set(word, lane, KIT_FLAM);
        word = kit_set(word, KIT_LANE_AC, KIT_NOTE);
        g_seq.patterns[0].kit_steps[4][s] = word;
        g_seq.patterns[0].kit_steps[5][s] = word;
    }
    sequencer_refresh_track_playback(4, 0);
    sequencer_refresh_track_playback(5, 0);
    wire_time = true;                       // the wire takes 320 us a byte from here
    fake_uart_run_until(now());
    uint32_t t = now() + 1000;
    for (unsigned i = 0; i < 8; i++) {
        wire(t, MIDI_CLOCK);
        t += T300;
    }
    wire(t, MIDI_START);
    // Real time on average: in every round two Clocks come late, bunched with the
    // one that is on time (a USB stall), so the device catches up 12 times.
    for (unsigned round = 0; round < 12; round++) {
        for (unsigned i = 0; i < 6; i++) {
            t += T300;
            wire(t, MIDI_CLOCK);
        }
        t += 3 * T300;
        for (unsigned i = 0; i < 3; i++) wire(t + 320 * i, MIDI_CLOCK);
    }
    for (unsigned i = 0; i < 8; i++) {
        t += T300;
        wire(t, MIDI_CLOCK);
    }
    run_until(t + T300 - 100);
    CHECK(!midi_output_overflowed());
    CHECK(fake_midi_count(0xB0) == 0);      // no panic (All Notes Off)
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    CHECK(g_seq.transport.pulse_count == 4u * (12u * 9u + 8u));
    CHECK(jitter_stats.ext_catchups > 0);
}

// Following, a Clock read from the DIN input runs its pulse at once, the pulse queues the
// Clock out before its notes, and the drain sends it at the next message boundary. So
// each Clock out starts on the wire within four byte times (1,280 us) of the Clock it
// follows being read: the output adds at most that to the input's own jitter. Here the
// input is +-3.5 ms off its tempo and the wire is busy at every Clock: two kits with
// every voice on every step (one flammed, its second hits on the pulses between the
// Clocks) and six SYNTH tracks with a note on every step. The model runs the doorbell
// as the byte is read; the device's interrupt latency is what jitter_stats measures.
static void test_clock_out_leaves_within_four_bytes_of_clock_in(void) {
    reset(CLOCK_SOURCE_EXT);
    make_kit(4, 9);
    make_kit(5, 10);
    g_seq.kit_config[4].flam_pulses = 3;
    uint32_t flammed = 0, plain = 0;
    for (uint8_t lane = 1; lane < KIT_LANES; lane++) {
        flammed = kit_set(flammed, lane, KIT_FLAM);
        plain = kit_set(plain, lane, KIT_NOTE);
    }
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        g_seq.patterns[0].kit_steps[4][s] = flammed;
        g_seq.patterns[0].kit_steps[5][s] = plain;
    }
    sequencer_refresh_track_playback(4, 0);
    sequencer_refresh_track_playback(5, 0);
    for (uint8_t track = 0; track < PANEL_TRACKS; track++) {
        if (track == 4 || track == 5) continue;
        g_seq.patterns[0].tracks[track].type = TRACK_TYPE_SYNTH;
        for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) set_note(0, track, s, (uint8_t)(60 + s), 0);
    }
    wire_time = true;                       // the wire takes 320 us a byte from here
    fake_uart_run_until(now());
    uint32_t t = now() + 1000;
    clocks(t, T145, 8);                     // the tempo is known before the start
    t += 8 * T145;
    wire(t, MIDI_START);
    enum { CLOCKS_IN = 96 };
    static uint32_t read_us[CLOCKS_IN];
    unsigned seed = 11;
    uint32_t nominal = t;
    for (unsigned c = 0; c < CLOCKS_IN; c++) {
        seed = seed * 1103515245u + 12345u;
        int32_t jitter = (int32_t)((seed >> 16) % 7001u) - 3500;   // +-3.5 ms
        nominal += T145;
        read_us[c] = nominal + (uint32_t)jitter;
        wire(read_us[c], MIDI_CLOCK);
    }
    run_until(read_us[CLOCKS_IN - 1] + T145 - 100);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    CHECK(!midi_output_overflowed());
    // Every Clock was taken on its own: none was owed to a catch-up, none dropped.
    CHECK(jitter_stats.ext_catchups == 0 && jitter_stats.ext_dropped == 0);
    unsigned clock = 0;
    uint32_t longest = 0;
    for (unsigned i = 0; i < fake_midi_length; i++) {
        if (fake_midi[i] != MIDI_CLOCK) continue;
        CHECK(clock < CLOCKS_IN);
        uint32_t wait = fake_midi_time_us[i] - read_us[clock++];
        CHECK(wait <= 4u * 320u);
        if (wait > longest) longest = wait;
    }
    CHECK(clock == CLOCKS_IN);
    // The load reaches the Clocks: some waited past the two bytes already in the UART,
    // for the rest of a message.
    CHECK(longest > 2u * 320u);
}

// ─── Start, Stop, Continue, SPP ────────────────────────────────

static void test_stop_and_continue_between_sub_pulses(void) {
    reset(CLOCK_SOURCE_EXT);
    every_step();
    set_note(0, 1, 7, 70, STEP_FLAG_TIE);   // a tie holds its gate to the Stop
    uint32_t t = now() + 1000;
    clocks(t, T145, 8);
    t += 8 * T145;
    wire(t, MIDI_START);
    uint32_t last = clocks(t + T145, T145, 44);
    run_until(last + T145 / 2);             // two sub-pulses of Clock 44 have run
    CHECK(g_seq.transport.pulse_count == 4u * 43u + 3u);
    CHECK(fake_gates & 2u);
    wire(last + T145 / 2 + 100, MIDI_STOP);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED && !following());
    CHECK(fake_gates == 0);
    CHECK(fake_midi[fake_midi_length - 1] == MIDI_STOP || count_byte(MIDI_STOP) == 1);
    // Clocks while stopped play nothing.
    unsigned sent = fake_midi_length;
    clocks(last + T145, T145, 5);
    CHECK(fake_midi_length == sent);
    // Continue: the next Clock is pulse 176, the boundary after 175.
    uint32_t c = last + 6 * T145;
    wire(c - 2000, MIDI_CONTINUE);
    CHECK(transport_armed() && g_seq.transport.pulse_count == 176);
    wire(c, MIDI_CLOCK);
    run_until(c);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING && g_seq.transport.pulse_count == 177);
    CHECK(count_byte(MIDI_CONTINUE) == 1);
    CHECK(fake_midi[sent] == MIDI_CONTINUE && fake_midi[sent + 1] == MIDI_CLOCK);
}

// A MUTE change made while following waits for the bar, which comes with its Clock;
// a Stop from the wire drops a waiting one.
static void test_a_waiting_mute_lands_on_the_bar_of_the_clock(void) {
    reset(CLOCK_SOURCE_EXT);
    every_step();
    uint32_t t = now() + 1000;
    wire(t, MIDI_START);
    uint32_t last = clocks(t + T145, T145, 2);  // pulses 0-7: the bar's launch point is past
    transport_toggle_mutes(TRACK_BIT(0));
    CHECK(g_seq.mute_pending == TRACK_BIT(0) && g_seq.track_mutes == 0);
    last = clocks(last + T145, T145, 94);       // Clocks 2-95: up to pulse 383
    run_until(last + T145 - 10);
    CHECK(g_seq.transport.pulse_count == 384);
    CHECK(g_seq.track_mutes == 0 && g_seq.mute_pending == TRACK_BIT(0));
    CHECK(note_ons(0, 40) == 1 && note_ons(0, 55) == 1);
    wire(last + T145, MIDI_CLOCK);              // Clock 96: the second bar
    CHECK(g_seq.track_mutes == TRACK_BIT(0) && g_seq.mute_pending == 0);
    CHECK(note_ons(0, 40) == 1);                // its downbeat is muted
    // A change back to audible waits, and the wire's Stop drops it.
    transport_toggle_mutes(TRACK_BIT(0));
    CHECK(g_seq.mute_pending == TRACK_BIT(0));
    wire(last + T145 + 5000, MIDI_STOP);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED && g_seq.mute_pending == 0);
    CHECK(g_seq.track_mutes == TRACK_BIT(0));
}

static void test_wire_transport_is_ignored_with_int(void) {
    reset(CLOCK_SOURCE_INT);
    every_step();
    uint32_t t = now() + 1000;
    wire(t, MIDI_START);
    clocks(t + T145, T145, 10);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED && !transport_armed());
    CHECK(ext_est.count == 10);              // the tempo is still measured
    transport_start();
    run_until(now() + 10 * pulse_interval_us + 1);
    unsigned pulses = g_seq.transport.pulse_count;
    wire(now() + 100, MIDI_STOP);
    clocks(now() + 200, 320, 6);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    run_until(now() + pulse_interval_us);
    CHECK(g_seq.transport.pulse_count == pulses + 1);   // still on the internal timer
}

static void test_play_arms_in_ext_and_cancels(void) {
    reset(CLOCK_SOURCE_EXT);
    set_note(0, 0, 0, 60, 0);
    transport_start();
    CHECK(transport_armed() && g_seq.transport.state == TRANSPORT_STOPPED);
    run_until(now() + 100000);               // no Clock: it waits
    CHECK(transport_armed() && count_byte(MIDI_START) == 0);
    CHECK(!sequencer_storage_begin());       // armed counts as busy
    transport_stop();                        // PLAY again
    CHECK(!transport_armed());
    clocks(now() + 1000, T145, 3);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
    transport_start();
    wire(now() + T145, MIDI_CLOCK);
    run_until(now());
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING && note_ons(0, 60) == 1);
    CHECK(count_byte(MIDI_START) == 1);
}

static void test_auto_starts_on_its_own_timer_without_a_clock(void) {
    reset(CLOCK_SOURCE_AUTO);
    set_note(0, 0, 0, 60, 0);
    transport_start();
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING && !transport_armed());
    CHECK(transport_tempo_is_internal());
    transport_stop();
    clocks(now() + 1000, T145, 4);           // a master sends Clocks while stopped
    CHECK(!transport_tempo_is_internal());
    transport_start();
    CHECK(transport_armed());
}

// INT playback from the start for `steps` steps, then the notes of the next `more`.
static unsigned reference_notes(play_mode_t mode, unsigned steps, unsigned more, uint16_t *out,
                                unsigned max);
static void build_arrangement(play_mode_t mode);

static unsigned reference_notes(play_mode_t mode, unsigned steps, unsigned more, uint16_t *out,
                                unsigned max) {
    reset(CLOCK_SOURCE_INT);
    build_arrangement(mode);
    transport_start();
    for (unsigned p = 0; p < steps * PULSES_PER_STEP; p++) {
        run_until(timer_hw->alarm[0]);
    }
    fake_midi_length = 0;
    for (unsigned p = 0; p < more * PULSES_PER_STEP; p++) run_until(timer_hw->alarm[0]);
    return played(out, max);
}

// Three locations with distinct notes on four SYNTH tracks of different lengths; in
// Song mode entries with repeats, a skipped one and tracks that keep their location.
static void build_arrangement(play_mode_t mode) {
    static const uint8_t lengths[3][4] = {{16, 12, 5, 7}, {16, 9, 5, 3}, {8, 12, 6, 7}};
    for (uint8_t loc = 0; loc < 3; loc++) {
        for (uint8_t tr = 0; tr < 4; tr++) {
            set_length(loc, tr, lengths[loc][tr]);
            for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
                set_note(loc, tr, s, (uint8_t)(20 + loc * 16 + s), 0);
            }
        }
    }
    transport_set_mode(mode);
    if (mode != PLAY_MODE_SONG) return;
    song_clear();
    uint8_t a[NUM_TRACKS] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t b[NUM_TRACKS] = {1, 0, 1, 0, 0, 0, 0, 0};   // tracks 1 and 3 keep location 0
    uint8_t c[NUM_TRACKS] = {2, 2, 1, 0, 0, 0, 0, 0};
    CHECK(song_append_patterns(a, 2));
    CHECK(song_append_patterns(c, 1));
    CHECK(song_append_patterns(b, 3));
    CHECK(song_append_patterns(c, 1));
    g_seq.song.entries[3].repeats = 0;       // skipped
    CHECK(song_append_patterns(a, 1));
    song_set_end_mode(SONG_END_LOOP);
}

static void check_spp_plays_as_from_the_start(play_mode_t mode, unsigned spp) {
    static uint16_t want[2048], got[2048];
    const unsigned more = 40;
    unsigned n_want = reference_notes(mode, spp, more, want, 2048);
    CHECK(n_want > 0);

    reset(CLOCK_SOURCE_EXT);
    build_arrangement(mode);
    uint32_t t = now() + 1000;
    uint32_t last = clocks(t, T145, 8);
    wire(last + 3000, 0xF2);
    wire(last + 3320, (uint8_t)(spp & 0x7F));
    wire(last + 3640, (uint8_t)(spp >> 7));
    wire(last + 4000, MIDI_CONTINUE);
    fake_midi_length = 0;
    uint32_t end = clocks(last + T145, T145, more * 6);
    run_until(end + T145 - 50);
    unsigned n_got = played(got, 2048);
    CHECK(n_got == n_want);
    for (unsigned i = 0; i < n_got; i++) CHECK(got[i] == want[i]);
}

static void test_spp_in_pattern_mode(void) {
    const unsigned positions[] = {0, 1, 4, 13, 16, 37, 100, 1000};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
        check_spp_plays_as_from_the_start(PLAY_MODE_PATTERN, positions[i]);
    }
}

static void test_spp_in_song_mode(void) {
    // Bars 0-1 entry A, 2 C, 3-5 B, (D skipped), 6 A, then the loop.
    const unsigned positions[] = {0, 5, 16, 31, 32, 40, 48, 70, 96, 99, 112, 120, 200};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
        check_spp_plays_as_from_the_start(PLAY_MODE_SONG, positions[i]);
    }
}

static void test_spp_past_the_end_of_a_song_that_stops(void) {
    reset(CLOCK_SOURCE_EXT);
    build_arrangement(PLAY_MODE_SONG);
    song_set_end_mode(SONG_END_STOP);
    uint32_t t = now() + 1000;
    uint32_t last = clocks(t, T145, 8);
    const uint16_t spp = 7 * 16 + 3;         // the song has 7 bars
    wire(last + 1000, 0xF2);
    wire(last + 1320, spp & 0x7F);
    wire(last + 1640, spp >> 7);
    CHECK(g_seq.transport.song_position >= g_seq.song.length);
    wire(last + 2000, MIDI_CONTINUE);
    CHECK(!transport_armed());
    clocks(last + T145, T145, 10);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
}

static void test_a_song_that_ends_is_not_restarted_by_clocks(void) {
    reset(CLOCK_SOURCE_EXT);
    set_note(0, 0, 0, 60, 0);
    song_clear();
    CHECK(song_append(0, 1));
    song_set_end_mode(SONG_END_STOP);
    transport_set_mode(PLAY_MODE_SONG);
    uint32_t t = now() + 1000;
    wire(t, MIDI_START);
    clocks(t + T145, T145, 6 * 16 + 30);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED && !following() && !transport_armed());
    CHECK(note_ons(0, 60) == 1);
}

static void test_spp_while_playing_is_ignored(void) {
    reset(CLOCK_SOURCE_EXT);
    uint32_t t = now() + 1000;
    wire(t, MIDI_START);
    uint32_t last = clocks(t + T145, T145, 10);
    wire(last + 1000, 0xF2);
    wire(last + 1320, 64);
    wire(last + 1640, 0);
    run_until(last + T145 - 100);
    CHECK(g_seq.transport.pulse_count == 40);
}

// ─── Lost clock ─────────────────────────────────────────────────

static void test_ext_stops_on_a_lost_clock(void) {
    reset(CLOCK_SOURCE_EXT);
    every_step();
    set_note(0, 1, 2, 70, STEP_FLAG_TIE);
    set_note(0, 1, 3, 70, STEP_FLAG_TIE);
    uint32_t t = now() + 1000;
    wire(t, MIDI_START);
    uint32_t last = clocks(t + T145, T145, 20);  // step 4 began: the tie sounds on
    CHECK(fake_gates & 2u);
    CHECK(note_ons(1, 70) == 1);
    run_until(last + 3 * T145 - 200);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);     // not yet
    run_until(last + 3 * T145 + 200);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED && !following());
    CHECK(fake_gates == 0);
    CHECK(count_byte(MIDI_STOP) == 1);
    CHECK(jitter_stats.ext_losses == 1);
    unsigned n = fake_midi_decode(msgs, FAKE_MIDI_CAPACITY);
    bool released = false;
    for (unsigned i = 0; i < n; i++) released |= msgs[i].status == (MIDI_NOTE_OFF | 1) && msgs[i].data1 == 70;
    CHECK(released);
    // Clocks that come back do not start it again.
    clocks(now() + 1000, T145, 20);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
}

static void test_auto_plays_on_and_follows_again(void) {
    reset(CLOCK_SOURCE_AUTO);
    every_step();
    uint32_t t = now() + 1000;
    uint32_t last = clocks(t, T145, 100);
    transport_start();                       // PLAY: the Clocks are there, so it arms
    CHECK(transport_armed());
    last = clocks(last + T145, T145, 30);
    CHECK(g_seq.transport.pulse_count == 4u * 29u + 1u);
    // The clock goes away: after the timeout the internal timer takes over at 145.
    run_until(last + 3 * T145 + 100);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING && !following());
    CHECK(fabsf(g_seq.transport.bpm - 145.0f) < 0.05f);
    CHECK(jitter_stats.ext_losses == 1);
    CHECK(count_byte(MIDI_STOP) == 0);
    uint32_t p = g_seq.transport.pulse_count;
    run_until(now() + 96 * pulse_interval_us);
    CHECK(g_seq.transport.pulse_count == p + 96);   // a beat on the internal timer
    // The clock returns: after three plausible intervals the device follows it again.
    uint32_t back = clocks(now() + 1234, T145, 3);
    CHECK(!following());
    wire(back + T145, MIDI_CLOCK);
    run_until(back + T145);
    CHECK(following());
    CHECK(g_seq.transport.pulse_count % 4u == 1u);  // this Clock's first pulse has run
    uint32_t q = g_seq.transport.pulse_count;
    clocks(back + 2 * T145, T145, 12);
    run_until(back + 13 * T145 + T145 - 100);
    CHECK(g_seq.transport.pulse_count == q - 1 + 4u * 13u);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
}

static void test_source_changes_while_playing(void) {
    // Following, to INT: the internal timer takes over at the measured tempo.
    reset(CLOCK_SOURCE_EXT);
    uint32_t t = now() + 1000;
    clocks(t, T145, 30);
    wire(t + 30 * T145, MIDI_START);
    uint32_t last = clocks(t + 31 * T145, T145, 10);
    transport_set_clock_source(CLOCK_SOURCE_INT);
    CHECK(!following() && g_seq.transport.state == TRANSPORT_PLAYING);
    CHECK(fabsf(g_seq.transport.bpm - 145.0f) < 0.05f);
    uint32_t p = g_seq.transport.pulse_count;
    clocks(last + T145, T145, 6);            // ignored now
    run_until(last + 7 * T145);
    CHECK(g_seq.transport.pulse_count > p + 20);
    // On the internal timer, to EXT with Clocks arriving: follows them.
    transport_set_clock_source(CLOCK_SOURCE_EXT);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    last = clocks(last + 7 * T145, T145, 2);
    CHECK(following());
    // To EXT with no Clock: stops.
    transport_set_clock_source(CLOCK_SOURCE_INT);
    run_until(last + 10 * T145);
    transport_set_clock_source(CLOCK_SOURCE_EXT);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
}

// ─── Storage ────────────────────────────────────────────────────

static void test_storage_keeps_the_wire_from_starting(void) {
    reset(CLOCK_SOURCE_EXT);
    set_note(0, 0, 0, 60, 0);
    CHECK(sequencer_storage_begin());
    uint32_t t = now() + 1000;
    wire(t, MIDI_START);
    clocks(t + T145, T145, 6);
    wire(now() + 100, MIDI_CONTINUE);
    clocks(now() + T145, T145, 3);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED && !transport_armed());
    sequencer_storage_end();
    wire(now() + 100, MIDI_START);
    wire(now() + T145, MIDI_CLOCK);
    run_until(now());
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING && note_ons(0, 60) == 1);
    CHECK(!sequencer_storage_begin());       // playing
}

static void test_transport_start_keeps_the_wire_out_while_rows_load(void) {
    reset(CLOCK_SOURCE_EXT);
    // ext_inhibit is set between transport_start()'s two lock sections; a Start from
    // the wire then is dropped and PLAY's own arm stands.
    ext_inhibit = true;
    clock_in_event_t start = {now(), 0, CLOCK_IN_START};
    ext_on_event(&start);
    CHECK(!transport_armed());
    ext_inhibit = false;
    transport_start();
    CHECK(transport_armed() && !ext_inhibit);
}

static void test_panic_lets_go(void) {
    reset(CLOCK_SOURCE_EXT);
    uint32_t t = now() + 1000;
    wire(t, MIDI_START);
    clocks(t + T145, T145, 4);
    sequencer_panic();
    CHECK(!following() && !transport_armed());
    clocks(now() + T145, T145, 6);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED);
}

// ─── Found in review ────────────────────────────────────────────

static void test_a_stall_is_not_a_tempo_change(void) {
    // Two Clocks held back by a stall come bunched with the third: the shown tempo and
    // the tempo AUTO plays on at, once the master is gone, stay at 145.
    reset(CLOCK_SOURCE_AUTO);
    every_step();
    uint32_t last = clocks(now() + 1000, T145, 100);
    transport_start();
    last = clocks(last + T145, T145, 20);
    // The next Clock is held back a Clock time and comes bunched with the one after,
    // then they are on time again.
    const uint32_t at[] = {last + 2 * T145, last + 2 * T145 + 320, last + 3 * T145,
                           last + 4 * T145};
    for (unsigned i = 0; i < 4; i++) {
        wire(at[i], MIDI_CLOCK);
        CHECK(following());
        float shown = 40000000.0f / (float)ext_est.shown_x16;
        CHECK(fabsf(shown - 145.0f) < 1.0f);
        CHECK(clock_est_timeout_us(&ext_est) >= 3u * T145 - 100u);
    }
    run_until(at[3] + 200000);              // and then the master is gone
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING && !following());
    CHECK(fabsf(g_seq.transport.bpm - 145.0f) < 0.5f);
    // Extra Clocks in a bunch (more than real time) move it a little, not to 300.
    reset(CLOCK_SOURCE_AUTO);
    last = clocks(now() + 1000, T145, 100);
    transport_start();
    last = clocks(last + T145, T145, 20);
    clocks(last + T145, 320, 6);
    run_until(now() + 120000);
    CHECK(fabsf(g_seq.transport.bpm - 145.0f) < 2.0f);
}

static void test_ext_on_its_own_timer_stops_when_the_clock_goes(void) {
    // Switched to EXT while Clocks were arriving, before it follows them: when they
    // stop, EXT stops too.
    reset(CLOCK_SOURCE_INT);
    every_step();
    transport_start();
    uint32_t last = clocks(now() + 1000, T145, 2);
    transport_set_clock_source(CLOCK_SOURCE_EXT);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING && !following());
    run_until(last + 3 * T145 - 100);
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING);
    run_until(last + 3 * T145 + pulse_interval_us + 100);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED && fake_gates == 0);
    CHECK(count_byte(MIDI_STOP) == 1);
}

static void test_spp_past_the_end_after_a_start_stays_stopped(void) {
    reset(CLOCK_SOURCE_EXT);
    set_note(0, 0, 0, 60, 0);
    song_clear();
    CHECK(song_append(0, 1));
    song_set_end_mode(SONG_END_STOP);
    transport_set_mode(PLAY_MODE_SONG);
    uint32_t t = now() + 1000;
    wire(t, MIDI_START);
    CHECK(transport_armed());
    wire(t + 320, 0xF2);
    wire(t + 640, 17);
    wire(t + 960, 0);
    CHECK(g_seq.transport.song_position >= g_seq.song.length);
    CHECK(!transport_armed() && !following());
    clocks(t + T145, T145, 30);
    CHECK(g_seq.transport.state == TRANSPORT_STOPPED && note_ons(0, 60) == 0);
}

static void test_the_alarm_drains_the_ring_when_both_are_due(void) {
    // On the RP2040 TIMER_IRQ_0 runs first when both core 1 interrupts are pending: the
    // Clock is taken by the timer ISR, and the doorbell after it finds the ring empty.
    reset(CLOCK_SOURCE_EXT);
    every_step();
    uint32_t t = now() + 1000;
    wire(t, MIDI_START);
    uint32_t last = clocks(t + T145, T145, 6);
    run_until(last + T145 - 10);
    CHECK(g_seq.transport.pulse_count == 24);
    // The next Clock arrives as alarm 0 falls due.
    timer_hw->timerawl = timer_hw->alarm[0];
    uint8_t clock = MIDI_CLOCK;
    fake_uart_rx_push(&clock, 1);
    fake_irq_handlers[UART0_IRQ]();
    CHECK(timer_hw->intf & (1u << CLOCK_IN_ALARM));
    sequencer_timer_isr();
    CHECK(g_seq.transport.pulse_count == 25 && note_ons(0, 41) == 1);
    sequencer_clock_in_isr();
    CHECK(!(timer_hw->intf & (1u << CLOCK_IN_ALARM)));
    CHECK(g_seq.transport.pulse_count == 25 && note_ons(0, 41) == 1);
    CHECK(count_byte(MIDI_CLOCK) == 7);
}

static void test_a_stale_clock_is_forgotten(void) {
    // Once the Clocks are older than the timeout the estimate is dropped, so the
    // 32-bit difference to the last one can never wrap round into "present".
    reset(CLOCK_SOURCE_AUTO);
    uint32_t last = clocks(now() + 1000, T145, 10);
    CHECK(ext_present(now()) && !transport_tempo_is_internal());
    run_until(last + 3 * T145 + pulse_interval_us + 10);
    CHECK(ext_est.count == 0 && !ext_present(now()) && transport_tempo_is_internal());
    timer_hw->timerawl = last + 100;        // the counter come round: still not present
    CHECK(!ext_present(now()));
    transport_start();                      // AUTO starts on its own timer
    CHECK(g_seq.transport.state == TRANSPORT_PLAYING && !transport_armed());
}

static void test_transport_running_is_playing_or_armed(void) {
    reset(CLOCK_SOURCE_EXT);
    CHECK(!transport_running());
    transport_start();
    CHECK(transport_running() && transport_armed());
    wire(now() + 1000, MIDI_CLOCK);
    CHECK(transport_running() && !transport_armed());
    transport_stop();
    CHECK(!transport_running());
}

#define RUN(test) do { test(); printf("PASS %s\n", #test); tests++; } while (0)

int main(void) {
    unsigned tests = 0;
    seq_lock_init();
    RUN(test_estimate_steady_clock);
    RUN(test_estimate_jittered_clock_shows_a_steady_tempo);
    RUN(test_estimate_follows_a_tempo_change_within_a_beat);
    RUN(test_estimate_restarts_after_a_gap);
    RUN(test_timeout_has_a_floor);
    RUN(test_spp_with_real_time_bytes_inside);
    RUN(test_ring_full_drops_and_counts);
    RUN(test_uart_interrupt_rings_the_doorbell);
    RUN(test_start_then_clock_one_byte_later_plays_one_step);
    RUN(test_four_pulses_per_clock_at_20_145_300_bpm);
    RUN(test_steps_start_on_their_clock);
    RUN(test_early_and_late_clocks_lose_and_double_nothing);
    RUN(test_a_burst_is_caught_up_not_played_at_once);
    RUN(test_the_backlog_is_capped);
    RUN(test_two_kits_flamming_through_bursts_never_overflow);
    RUN(test_clock_out_leaves_within_four_bytes_of_clock_in);
    RUN(test_stop_and_continue_between_sub_pulses);
    RUN(test_a_waiting_mute_lands_on_the_bar_of_the_clock);
    RUN(test_wire_transport_is_ignored_with_int);
    RUN(test_play_arms_in_ext_and_cancels);
    RUN(test_auto_starts_on_its_own_timer_without_a_clock);
    RUN(test_spp_in_pattern_mode);
    RUN(test_spp_in_song_mode);
    RUN(test_spp_past_the_end_of_a_song_that_stops);
    RUN(test_a_song_that_ends_is_not_restarted_by_clocks);
    RUN(test_spp_while_playing_is_ignored);
    RUN(test_ext_stops_on_a_lost_clock);
    RUN(test_auto_plays_on_and_follows_again);
    RUN(test_source_changes_while_playing);
    RUN(test_storage_keeps_the_wire_from_starting);
    RUN(test_transport_start_keeps_the_wire_out_while_rows_load);
    RUN(test_panic_lets_go);
    RUN(test_a_stall_is_not_a_tempo_change);
    RUN(test_ext_on_its_own_timer_stops_when_the_clock_goes);
    RUN(test_spp_past_the_end_after_a_start_stays_stopped);
    RUN(test_the_alarm_drains_the_ring_when_both_are_due);
    RUN(test_a_stale_clock_is_forgotten);
    RUN(test_transport_running_is_playing_or_armed);
    printf("%u clock tests passed\n", tests);
    return 0;
}
