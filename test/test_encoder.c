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

static void test_one_detent_per_four_edges(void) {
    setup();
    cw_first_half(100); cw_second_half(100);
    CHECK(encoder_get_detents(0) == 1);
    CHECK(encoder_get_detents(0) == 0);
    ccw_cycle(100);
    CHECK(encoder_get_detents(0) == -1);
    printf("PASS test_one_detent_per_four_edges\n");
}

static void test_half_detent_is_kept(void) {
    setup();
    cw_first_half(100);
    CHECK(encoder_get_detents(0) == 0);
    cw_second_half(100);
    CHECK(encoder_get_detents(0) == 1);
    printf("PASS test_half_detent_is_kept\n");
}

static void test_fast_turn_counts_every_detent(void) {
    setup();
    for (int i = 0; i < 8; i++) { cw_first_half(2); cw_second_half(2); }
    CHECK(encoder_get_detents(0) == 8);
    printf("PASS test_fast_turn_counts_every_detent\n");
}

int main(void) {
    test_one_detent_per_four_edges();
    test_half_detent_is_kept();
    test_fast_turn_counts_every_detent();
    return 0;
}
