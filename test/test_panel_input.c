#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fakes/fake_hardware.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include "panel_input.h"
#include "sequencer.h"
#include "tca8418.h"

sequencer_state_t g_seq;
i2c_inst_t fake_i2c0_inst;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define RUN(name) do { name(); printf("PASS %s\n", #name); } while (0)

// ---- Fake TCA8418 -------------------------------------------------------
#define FIFO_DEPTH 10
static uint8_t chip_regs[256];
static uint8_t chip_fifo[FIFO_DEPTH];
static int chip_fifo_count;
static uint8_t chip_pointer;
static int chip_fail_count;  // next N transactions fail with a timeout
static int chip_txn;         // transactions since chip_reset
static int chip_fail_txn;    // 1-based transaction number that fails once (0 = none)

static void chip_reset(void) {
    memset(chip_regs, 0, sizeof(chip_regs));
    chip_fifo_count = 0;
    chip_pointer = 0;
    chip_fail_count = 0;
    chip_txn = 0;
    chip_fail_txn = 0;
}

static bool chip_should_fail(void) {
    chip_txn++;
    if (chip_fail_txn != 0 && chip_txn == chip_fail_txn) return true;
    if (chip_fail_count > 0) { chip_fail_count--; return true; }
    return false;
}

static uint8_t chip_pop(void);

static void chip_push(uint8_t event) {
    if (chip_fifo_count == FIFO_DEPTH) {
        // OVR_FLOW_M = 1: the oldest event is pushed out
        chip_regs[0x02] |= 0x08;
        chip_pop();
    }
    chip_fifo[chip_fifo_count++] = event;
    chip_regs[0x02] |= 0x01;  // K_INT
}

static uint8_t chip_pop(void) {
    if (chip_fifo_count == 0) return 0;
    uint8_t event = chip_fifo[0];
    memmove(chip_fifo, chip_fifo + 1, --chip_fifo_count);
    return event;
}

int i2c_write_timeout_us(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len,
                         bool nostop, unsigned timeout_us) {
    (void)i2c; (void)nostop;
    CHECK(addr == TCA8418_ADDR);
    CHECK(timeout_us > 0);
    if (chip_should_fail()) return PICO_ERROR_TIMEOUT;
    CHECK(len == 1 || len == 2);
    chip_pointer = src[0];
    if (len == 2) {
        if (chip_pointer == 0x02) chip_regs[0x02] &= (uint8_t)~src[1];  // write 1 to clear
        else chip_regs[chip_pointer] = src[1];
    }
    return (int)len;
}

int i2c_read_timeout_us(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len,
                        bool nostop, unsigned timeout_us) {
    (void)i2c; (void)nostop;
    CHECK(addr == TCA8418_ADDR);
    CHECK(timeout_us > 0);
    if (chip_should_fail()) return PICO_ERROR_TIMEOUT;
    CHECK(len == 1);
    if (chip_pointer == 0x04) dst[0] = chip_pop();
    else if (chip_pointer == 0x03) dst[0] = (uint8_t)chip_fifo_count;
    else dst[0] = chip_regs[chip_pointer];
    return 1;
}

// ---- Helpers --------------------------------------------------------------
static uint8_t key_of(int row, int col) { return (uint8_t)(row * 10 + col + 1); }
static void key_press(int row, int col) { chip_push(0x80 | key_of(row, col)); }
static void key_release(int row, int col) { chip_push(key_of(row, col)); }

static void reset(void) {
    fake_hardware_reset();
    chip_reset();
    panel_input_init();
}

static void poll_to(uint32_t end) {
    for (uint32_t t = 0; t <= end; t++) panel_input_poll(t);
}

static int drain(panel_event_t *out, int max) {
    int n = 0;
    while (n < max && panel_input_next(&out[n])) n++;
    return n;
}

static int count_kind(const panel_event_t *e, int n, panel_kind_t kind, bool down) {
    int c = 0;
    for (int i = 0; i < n; i++) if (e[i].kind == kind && e[i].down == down) c++;
    return c;
}

// ---- Tests ----------------------------------------------------------------
static void test_init_configures_keypad(void) {
    chip_reset();
    CHECK(tca8418_init());
    CHECK(chip_regs[0x01] == 0xA9);
    CHECK(chip_regs[0x1D] == 0xFF && chip_regs[0x1E] == 0xFF && chip_regs[0x1F] == 0x03);
    CHECK(chip_regs[0x29] == 0xFF && chip_regs[0x2A] == 0xFF && chip_regs[0x2B] == 0x03);
    chip_fail_count = 1;
    CHECK(!tca8418_init());
}

static void test_key_codes_map_to_controls(void) {
    reset();
    key_press(2, 5);
    key_press(4, 8);
    key_press(6, 9);
    poll_to(8);
    panel_event_t e[8];
    CHECK(drain(e, 8) == 3);
    CHECK(e[0].kind == PANEL_PAD && e[0].track == 5 && e[0].row == 2 && e[0].down);
    CHECK(e[1].kind == PANEL_SCENE && e[1].row == 4 && e[1].down);
    CHECK(e[2].kind == PANEL_MUTE && e[2].track == 6 && e[2].down);
}

static void test_bounce_yields_one_press(void) {
    reset();
    panel_event_t e[8];
    // press at t=0, release at t=1, press at t=2
    key_press(0, 0); panel_input_poll(0);
    key_release(0, 0); panel_input_poll(1);
    key_press(0, 0); panel_input_poll(2);
    for (uint32_t t = 3; t < 10; t++) {
        panel_input_poll(t);
        CHECK(drain(e, 8) == 0);
    }
    panel_input_poll(10);
    CHECK(drain(e, 8) == 1);
    CHECK(e[0].kind == PANEL_PAD && e[0].down);
    panel_input_poll(11);
    CHECK(drain(e, 8) == 0);
}

static void test_release_is_reported(void) {
    reset();
    panel_event_t e[8];
    key_press(1, 1);
    for (uint32_t t = 0; t < 20; t++) panel_input_poll(t);
    CHECK(drain(e, 8) == 1 && e[0].down);
    key_release(1, 1);
    for (uint32_t t = 20; t < 28; t++) {
        panel_input_poll(t);
        CHECK(!panel_input_next(&e[0]));
    }
    panel_input_poll(28);
    CHECK(panel_input_next(&e[0]));
    CHECK(e[0].kind == PANEL_PAD && e[0].track == 1 && e[0].row == 1 && !e[0].down);
}

static void test_ten_key_chord_all_reported(void) {
    reset();
    for (int i = 0; i < 10; i++) key_press(i % 8, i);
    poll_to(8);
    panel_event_t e[16];
    int n = drain(e, 16);
    CHECK(n == 10);
    CHECK(count_kind(e, n, PANEL_PAD, true) == 8);
    CHECK(count_kind(e, n, PANEL_SCENE, true) == 1);
    CHECK(count_kind(e, n, PANEL_MUTE, true) == 1);
}

static void test_overflow_resyncs_without_stuck_keys(void) {
    reset();
    panel_event_t e[16];
    for (int i = 0; i < 12; i++) key_press(i % 8, i % 10);
    CHECK(chip_regs[0x02] & 0x08);
    poll_to(30);
    CHECK(drain(e, 16) == 0);
    CHECK(chip_fifo_count == 0);
    CHECK(!(chip_regs[0x02] & 0x09));
    key_press(3, 3);
    poll_to(60);
    CHECK(drain(e, 16) == 1);
    CHECK(e[0].kind == PANEL_PAD && e[0].track == 3 && e[0].row == 3 && e[0].down);
}

static void test_i2c_error_does_not_block(void) {
    reset();
    panel_event_t e[8];
    key_press(0, 1);
    chip_fail_count = 1;
    panel_input_poll(0);
    CHECK(drain(e, 8) == 0);
    CHECK(chip_fifo_count == 1);
    for (uint32_t t = 1; t <= 9; t++) panel_input_poll(t);
    CHECK(chip_fifo_count == 0);
    CHECK(drain(e, 8) == 1);
    CHECK(e[0].kind == PANEL_PAD && e[0].track == 1 && e[0].down);
}

static void test_play_button_debounce(void) {
    reset();
    panel_event_t e[8];
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    for (uint32_t t = 0; t < 20; t++) {
        panel_input_poll(t);
        CHECK(drain(e, 8) == 0);
    }
    panel_input_poll(20);
    CHECK(drain(e, 8) == 1 && e[0].kind == PANEL_PLAY && e[0].down);
    for (uint32_t t = 21; t < 50; t++) panel_input_poll(t);
    fake_gpio_level[PLAY_BUTTON_PIN] = true;
    for (uint32_t t = 50; t < 70; t++) {
        panel_input_poll(t);
        CHECK(drain(e, 8) == 0);
    }
    panel_input_poll(70);
    CHECK(drain(e, 8) == 1 && e[0].kind == PANEL_PLAY && !e[0].down);
}

static void test_resync_drops_pending_events(void) {
    reset();
    panel_event_t e;
    key_press(0, 0);
    poll_to(8);
    panel_input_resync();
    CHECK(!panel_input_next(&e));
}

static void test_held_play_survives_resync_silently(void) {
    reset();
    panel_event_t e[8];
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    poll_to(30);
    CHECK(drain(e, 8) == 1);
    panel_input_resync();
    for (uint32_t t = 31; t <= 200; t++) panel_input_poll(t);
    CHECK(drain(e, 8) == 0);
    fake_gpio_level[PLAY_BUTTON_PIN] = true;
    for (uint32_t t = 201; t <= 260; t++) panel_input_poll(t);
    CHECK(drain(e, 8) == 0);
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    for (uint32_t t = 261; t <= 300; t++) panel_input_poll(t);
    int n = drain(e, 8);
    CHECK(n == 1 && e[0].kind == PANEL_PLAY && e[0].down);
}

static void test_held_pad_survives_resync_silently(void) {
    reset();
    panel_event_t e[8];
    key_press(5, 2);
    poll_to(20);
    CHECK(drain(e, 8) == 1);
    panel_input_resync();
    key_release(5, 2);
    for (uint32_t t = 21; t <= 60; t++) panel_input_poll(t);
    CHECK(drain(e, 8) == 0);
    key_press(5, 2);
    for (uint32_t t = 61; t <= 100; t++) panel_input_poll(t);
    int n = drain(e, 8);
    CHECK(n == 1 && e[0].kind == PANEL_PAD && e[0].track == 2 && e[0].row == 5 && e[0].down);
}

static void test_failed_flush_stays_in_recovery(void) {
    reset();
    panel_event_t e[8];
    key_press(0, 0);
    key_press(1, 1);
    chip_fail_count = 1000;  // the chip does not answer during resync
    panel_input_resync();
    for (uint32_t t = 0; t < 40; t++) {
        panel_input_poll(t);
        CHECK(drain(e, 8) == 0);
    }
    CHECK(chip_fifo_count == 2);
    chip_fail_count = 0;
    for (uint32_t t = 40; t < 80; t++) {
        panel_input_poll(t);
        CHECK(drain(e, 8) == 0);
    }
    CHECK(chip_fifo_count == 0);
    key_press(2, 2);
    for (uint32_t t = 80; t < 100; t++) panel_input_poll(t);
    int n = drain(e, 8);
    CHECK(n == 1 && e[0].track == 2 && e[0].row == 2 && e[0].down);
}

static void test_overflow_survives_a_failed_transaction(void) {
    // Fail each transaction of the overflowing poll in turn; the overflow must
    // still be acted on, leaving no stuck key and no stale event.
    for (int k = 1; k <= 30; k++) {
        reset();
        panel_event_t e[16];
        for (int i = 0; i < 12; i++) key_press(i % 8, i % 10);
        chip_fail_txn = chip_txn + k;
        for (uint32_t t = 0; t <= 40; t++) panel_input_poll(t);
        CHECK(drain(e, 16) == 0);
        CHECK(chip_fifo_count == 0);
        CHECK(!(chip_regs[0x02] & 0x08));
        key_press(3, 3);
        for (uint32_t t = 41; t <= 70; t++) panel_input_poll(t);
        CHECK(drain(e, 16) == 1);
        CHECK(e[0].kind == PANEL_PAD && e[0].track == 3 && e[0].row == 3 && e[0].down);
    }
}

static void test_play_works_while_chip_recovers(void) {
    reset();
    panel_event_t e[8];
    chip_fail_count = 1000000;
    panel_input_resync();
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    for (uint32_t t = 0; t < 20; t++) panel_input_poll(t);
    CHECK(drain(e, 8) == 0);
    panel_input_poll(20);
    CHECK(drain(e, 8) == 1 && e[0].kind == PANEL_PLAY && e[0].down);
}

static void test_queued_play_survives_overflow(void) {
    reset();
    panel_event_t e[16];
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    for (uint32_t t = 0; t <= 20; t++) panel_input_poll(t);
    for (int i = 0; i < 12; i++) key_press(i % 8, i % 10);
    for (uint32_t t = 21; t <= 40; t++) panel_input_poll(t);
    int n = drain(e, 16);
    CHECK(n == 1 && e[0].kind == PANEL_PLAY && e[0].down);
}

static void test_init_failure_at_boot_recovers(void) {
    fake_hardware_reset();
    chip_reset();
    chip_fail_count = 1;
    panel_input_init();
    CHECK(chip_regs[0x01] == 0);
    panel_event_t e[8];
    panel_input_poll(0);
    CHECK(chip_regs[0x01] == 0xA9);
    key_press(4, 4);
    for (uint32_t t = 1; t <= 12; t++) panel_input_poll(t);
    CHECK(drain(e, 8) == 1);
    CHECK(e[0].kind == PANEL_PAD && e[0].track == 4 && e[0].row == 4 && e[0].down);
}

static void test_full_ring_keeps_changes_pending(void) {
    reset();
    panel_event_t e[64];
    int pressed = 0;
    for (uint32_t t = 0; t < 4; t++) {
        for (int i = 0; i < 10; i++, pressed++) key_press(pressed / 10, pressed % 10);
        panel_input_poll(t);
    }
    for (uint32_t t = 4; t <= 30; t++) panel_input_poll(t);
    CHECK(drain(e, 64) == 32);
    for (uint32_t t = 31; t <= 40; t++) panel_input_poll(t);
    CHECK(drain(e + 32, 32) == 8);
}

static void test_overflow_keeps_play_events_in_a_wrapped_ring(void) {
    reset();
    panel_event_t e[40];
    uint32_t t = 0;
    for (int batch = 0; batch < 3; batch++) {
        for (int i = 0; i < 10; i++) key_press(batch, i);
        panel_input_poll(t++);
    }
    for (int i = 0; i < 30; i++) panel_input_poll(t++);
    CHECK(drain(e, 40) == 30);  // the ring head now sits at slot 30
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    for (int i = 0; i < 20; i++) panel_input_poll(t++);   // PLAY down: slot 30
    key_press(5, 0);
    for (int i = 0; i < 10; i++) panel_input_poll(t++);   // key: slot 31
    fake_gpio_level[PLAY_BUTTON_PIN] = true;
    key_press(5, 1);
    for (int i = 0; i < 30; i++) panel_input_poll(t++);   // key: slot 0, PLAY up: slot 1
    key_press(5, 2);
    for (int i = 0; i < 12; i++) panel_input_poll(t++);   // key: slot 2
    for (int i = 0; i < 12; i++) key_press(i % 8, 3 + i % 7);
    for (int i = 0; i < 30; i++) panel_input_poll(t++);   // overflow
    int n = drain(e, 40);
    CHECK(n == 2);
    CHECK(e[0].kind == PANEL_PLAY && e[0].down);
    CHECK(e[1].kind == PANEL_PLAY && !e[1].down);
}

int main(void) {
    RUN(test_init_configures_keypad);
    RUN(test_key_codes_map_to_controls);
    RUN(test_bounce_yields_one_press);
    RUN(test_release_is_reported);
    RUN(test_ten_key_chord_all_reported);
    RUN(test_overflow_resyncs_without_stuck_keys);
    RUN(test_i2c_error_does_not_block);
    RUN(test_play_button_debounce);
    RUN(test_resync_drops_pending_events);
    RUN(test_held_play_survives_resync_silently);
    RUN(test_held_pad_survives_resync_silently);
    RUN(test_failed_flush_stays_in_recovery);
    RUN(test_overflow_survives_a_failed_transaction);
    RUN(test_play_works_while_chip_recovers);
    RUN(test_queued_play_survives_overflow);
    RUN(test_init_failure_at_boot_recovers);
    RUN(test_full_ring_keeps_changes_pending);
    RUN(test_overflow_keeps_play_events_in_a_wrapped_ring);
    return 0;
}
