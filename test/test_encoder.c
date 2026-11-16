#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "fake_hardware.h"
#include "encoder.h"
#include "sequencer.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

sequencer_state_t g_seq;   // referenced by the shared fake hardware

#define PIN_A 18
#define PIN_B 19

static void setup(void) {
    fake_hardware_reset();
    encoder_init();
}

// One edge: set (A,B), let `ms` pass, poll.
static void edge(bool a, bool b, uint32_t ms) {
    fake_gpio_level[PIN_A] = a;
    fake_gpio_level[PIN_B] = b;
    sleep_ms(ms);
    encoder_poll();
}

static void cw_first_half(uint32_t ms)  { edge(0, 1, ms); edge(0, 0, ms); }
static void cw_second_half(uint32_t ms) { edge(1, 0, ms); edge(1, 1, ms); }
static void ccw_cycle(uint32_t ms) {
    edge(1, 0, ms); edge(0, 0, ms); edge(0, 1, ms); edge(1, 1, ms);
}
static void cw_cycle(uint32_t ms) { cw_first_half(ms); cw_second_half(ms); }

static int8_t detents(void) { return encoder_get_turn(0).detents; }

static void test_one_detent_per_four_edges(void) {
    setup();
    cw_first_half(100); cw_second_half(100);
    CHECK(detents() == 1);
    CHECK(detents() == 0);
    ccw_cycle(100);
    CHECK(detents() == -1);
    printf("PASS test_one_detent_per_four_edges\n");
}

static void test_half_detent_is_kept(void) {
    setup();
    cw_first_half(100);
    CHECK(detents() == 0);
    cw_second_half(100);
    CHECK(detents() == 1);
    printf("PASS test_half_detent_is_kept\n");
}

static void test_fast_turn_counts_every_detent(void) {
    setup();
    for (int i = 0; i < 8; i++) { cw_first_half(2); cw_second_half(2); }
    CHECK(detents() == 8);
    printf("PASS test_fast_turn_counts_every_detent\n");
}

// A click is one detent; its edges come close together, so only the time
// between detents counts. A slow turn is worth one per detent.
static void test_slow_detents_are_worth_one(void) {
    setup();
    for (int i = 0; i < 10; i++) {
        sleep_ms(ENC_ACCEL_FAST_MS);
        cw_cycle(1);
        encoder_turn_t t = encoder_get_turn(0);
        CHECK(t.detents == 1 && t.amount == 1);
    }
    printf("PASS test_slow_detents_are_worth_one\n");
}

// Detents under ENC_ACCEL_FAST_MS apart: x1 for the first four, x4 for the
// next three, then x8.
static void test_fast_detents_accelerate(void) {
    static const int8_t worth[10] = {1, 1, 1, 1, 4, 4, 4, 8, 8, 8};
    setup();
    for (int i = 0; i < 10; i++) {
        cw_cycle(ENC_ACCEL_FAST_MS / 4 - 1);    // a detent every 56 ms
        encoder_turn_t t = encoder_get_turn(0);
        CHECK(t.detents == 1 && t.amount == worth[i]);
    }
    // Read once for several detents: each keeps its own worth.
    for (int i = 0; i < 3; i++) cw_cycle(2);
    encoder_turn_t t = encoder_get_turn(0);
    CHECK(t.detents == 3 && t.amount == 24);
    printf("PASS test_fast_detents_accelerate\n");
}

static void test_pause_or_turn_back_starts_over(void) {
    setup();
    for (int i = 0; i < 8; i++) cw_cycle(2);
    CHECK(encoder_get_turn(0).amount == 24);
    sleep_ms(ENC_ACCEL_FAST_MS);
    cw_cycle(0);                                // after a pause: x1
    CHECK(encoder_get_turn(0).amount == 1);
    for (int i = 0; i < 7; i++) cw_cycle(2);    // fast again: 1+1+1+4+4+4+8
    CHECK(encoder_get_turn(0).amount == 23);
    ccw_cycle(2);                               // a turn back: x1
    encoder_turn_t t = encoder_get_turn(0);
    CHECK(t.detents == -1 && t.amount == -1);
    printf("PASS test_pause_or_turn_back_starts_over\n");
}

// Half a detent and back is no detent and leaves the timing alone.
static void test_half_detent_and_back_is_nothing(void) {
    setup();
    sleep_ms(ENC_ACCEL_FAST_MS);
    cw_first_half(1);
    edge(0, 1, 1); edge(1, 1, 1);               // back to rest
    encoder_turn_t t = encoder_get_turn(0);
    CHECK(t.detents == 0 && t.amount == 0);
    cw_cycle(1);
    t = encoder_get_turn(0);
    CHECK(t.detents == 1 && t.amount == 1);
    printf("PASS test_half_detent_and_back_is_nothing\n");
}

int main(void) {
    test_one_detent_per_four_edges();
    test_half_detent_is_kept();
    test_fast_turn_counts_every_detent();
    test_slow_detents_are_worth_one();
    test_fast_detents_accelerate();
    test_pause_or_turn_back_starts_over();
    test_half_detent_and_back_is_nothing();
    return 0;
}
