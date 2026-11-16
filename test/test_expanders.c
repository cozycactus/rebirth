#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/timer.h"
#include "fake_hardware.h"
#include "mcp23017.h"
#include "step_leds.h"
#include "encoder.h"
#include "sequencer.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

i2c_inst_t fake_i2c0_inst;
sequencer_state_t g_seq;   // referenced by the shared fake hardware

// Three fake MCP23017 on I2C0, at their power-on values after reset():
// 0x20 (U701, steps 1-7 and 9-15), 0x21 (U702, step LEDs) and 0x24 (U703,
// steps 8 and 16), 22 registers each (BANK = 0). A GPIO read comes from the
// keys alone: a pin reads low while its key is closed and high otherwise,
// then IPOL inverts it. High is the harsher case for every other pin: a
// floating input after a reset, GPA7/GPB7 (whose read-back the datasheet
// leaves open) and the unused outputs of U703. So a chip that reset reads its
// idle lines as pressed.
enum { U701, U702, U703, NUM_CHIPS };
static const uint8_t chip_address[NUM_CHIPS] = { 0x20, 0x21, 0x24 };
static uint8_t regs[NUM_CHIPS][0x16];
static uint8_t read_pointer[NUM_CHIPS];
static int pointer_pending = -1;            // chip whose pointer write awaits its read
static uint8_t closed[NUM_CHIPS][2];        // closed keys, per port
static bool fail_writes, fail_reads;        // every write / every read fails
static bool absent[NUM_CHIPS];              // every transfer to the chip fails (not fitted)
static int reset_in_read = -1;              // chip that resets after the first byte of its next GPIO read
static unsigned i2c_calls;                  // every transfer, failed or not
static unsigned calls[NUM_CHIPS];           // the same, per chip
static unsigned ipol_writes[NUM_CHIPS];     // bytes written to IPOLA/IPOLB
static unsigned led_writes;                 // 3-byte OLAT writes to 0x21
static unsigned last_write_len;

// Register writes and reads since the log was cleared (pointer writes are
// not logged): chip, first register, data bytes.
typedef struct { int chip; uint8_t reg; unsigned len; bool read; } txn_t;
static txn_t txns[32];
static unsigned txns_logged;

static void log_txn(int chip, uint8_t reg, unsigned len, bool read) {
    if (txns_logged < sizeof(txns) / sizeof(txns[0])) {
        txns[txns_logged++] = (txn_t){ chip, reg, len, read };
    }
}

static int chip_of(uint8_t addr) {
    int c = 0;
    while (c < NUM_CHIPS && chip_address[c] != addr) c++;
    CHECK(c < NUM_CHIPS);                     // only the three expanders are on this bus
    return c;
}

// Power-on values, and a brown-out: IODIR = FF/FF, every other register 0.
static void chip_reset(int c) {
    memset(regs[c], 0, sizeof(regs[c]));
    regs[c][0x00] = regs[c][0x01] = 0xFF;
}

static uint8_t port_value(int c, int port) {
    return (uint8_t)(~closed[c][port] ^ regs[c][0x02 + port]);
}

static uint8_t read_reg(int c, unsigned reg) {
    CHECK(reg < sizeof(regs[c]));
    if (reg == 0x12 || reg == 0x13) return port_value(c, (int)reg - 0x12);
    return regs[c][reg];
}

unsigned i2c_init(i2c_inst_t *i2c, unsigned baudrate) {
    (void)i2c; return baudrate;
}

int i2c_write_timeout_us(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len,
                         bool nostop, unsigned timeout_us) {
    (void)i2c;
    int c = chip_of(addr);
    i2c_calls++;
    calls[c]++;
    last_write_len = (unsigned)len;
    CHECK(timeout_us == 2000);
    CHECK(len >= 1);
    CHECK(pointer_pending < 0);               // a pointer write is followed by its read
    CHECK(nostop == (len == 1));              // only a pointer write keeps the bus for its read
    for (size_t i = 1; i < len; i++) {
        size_t reg = src[0] + i - 1;
        CHECK(reg < sizeof(regs[c]));
        // GPA7/GPB7 of the step chips are output-only: never made inputs.
        if (c != U702 && reg <= 0x01) CHECK(!(src[i] & 0x80));
        if (reg == 0x02 || reg == 0x03) ipol_writes[c]++;
    }
    if (len > 1) log_txn(c, src[0], (unsigned)len - 1, false);
    if (fail_writes || absent[c]) return PICO_ERROR_TIMEOUT;
    read_pointer[c] = src[0];
    if (len == 1) pointer_pending = c;
    for (size_t i = 1; i < len; i++) regs[c][src[0] + i - 1] = src[i];
    if (c == U702 && len == 3 && src[0] == 0x14) led_writes++;
    return (int)len;
}

int i2c_read_timeout_us(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len,
                        bool nostop, unsigned timeout_us) {
    (void)i2c;
    int c = chip_of(addr);
    i2c_calls++;
    calls[c]++;
    CHECK(timeout_us == 2000);
    CHECK(pointer_pending == c);              // the read follows its pointer write
    CHECK(!nostop);
    pointer_pending = -1;
    log_txn(c, read_pointer[c], (unsigned)len, true);
    if (fail_reads || absent[c]) return PICO_ERROR_TIMEOUT;
    // A chip that resets inside its GPIO read restarts its I2C interface: the
    // rest of the read comes back as 0xFF, with a full byte count.
    bool cut = c == reset_in_read && read_pointer[c] == 0x12;
    for (size_t i = 0; i < len; i++) {
        if (cut && i == 1) chip_reset(c);
        dst[i] = (cut && i >= 1) ? 0xFF : read_reg(c, read_pointer[c] + (unsigned)i);
    }
    if (cut) reset_in_read = -1;
    return (int)len;
}

static void reset(void) {
    fake_hardware_reset();
    for (int c = 0; c < NUM_CHIPS; c++) chip_reset(c);
    memset(read_pointer, 0, sizeof(read_pointer));
    memset(closed, 0, sizeof(closed));
    memset(absent, 0, sizeof(absent));
    memset(calls, 0, sizeof(calls));
    memset(ipol_writes, 0, sizeof(ipol_writes));
    pointer_pending = reset_in_read = -1;
    fail_writes = fail_reads = false;
    i2c_calls = led_writes = last_write_len = txns_logged = 0;
}

// The board's pin map (PR #10), kept here apart from the driver: bit n of the
// step word is step n + 1. U701 GPA0-6 = steps 1-7, GPB0-6 = steps 9-15;
// U703 GPA0 = step 8, GPA1 = step 16.
static void press(uint16_t steps) {
    memset(closed, 0, sizeof(closed));
    closed[U701][0] = (uint8_t)(steps & 0x7F);
    closed[U701][1] = (uint8_t)((steps >> 8) & 0x7F);
    closed[U703][0] = (uint8_t)(((steps >> 7) & 0x01) | ((steps >> 14) & 0x02));
}

// The step-row configuration for key pins a/b: IPOL, IODIR and GPPU on the
// keys, every output latch low.
static bool configured_as(int c, uint8_t a, uint8_t b) {
    const uint8_t *r = regs[c];
    return r[0x00] == a && r[0x01] == b && r[0x02] == a && r[0x03] == b
        && r[0x0C] == a && r[0x0D] == b && r[0x14] == 0x00 && r[0x15] == 0x00;
}

// Since the log was cleared, chip c was read exactly twice, 2 bytes each,
// starting at these registers in this order.
static bool reads_were(int c, uint8_t first, uint8_t second) {
    uint8_t seen[2];
    unsigned n = 0;
    for (unsigned i = 0; i < txns_logged; i++) {
        if (txns[i].chip != c || !txns[i].read) continue;
        if (n == 2 || txns[i].len != 2) return false;
        seen[n++] = txns[i].reg;
    }
    return n == 2 && seen[0] == first && seen[1] == second;
}

static void poll_buttons_for(unsigned ms) {
    for (unsigned t = 0; t < ms; t += 10) {
        sleep_ms(10);
        mcp23017_poll();
    }
}

static void poll_each_ms_for(unsigned ms) {
    for (unsigned t = 0; t < ms; t++) {
        sleep_ms(1);
        mcp23017_poll();
    }
}

static void test_step_row_configures_both_chips(void) {
    reset();
    regs[U701][0x14] = regs[U701][0x15] = 0xFF;   // latches the configuration must clear
    regs[U703][0x14] = regs[U703][0x15] = 0xFF;
    mcp23017_init();
    CHECK(configured_as(U701, 0x7F, 0x7F));
    CHECK(configured_as(U703, 0x03, 0x00));
    // Per chip: IPOL (the canary) first, then OLAT, IODIR, GPPU, each a
    // 3-byte transfer; nothing is read at init.
    static const uint8_t order[4] = { 0x02, 0x14, 0x00, 0x0C };
    static const int step_chips[2] = { U701, U703 };
    for (unsigned s = 0; s < 2; s++) {
        int c = step_chips[s];
        unsigned k = 0;
        for (unsigned i = 0; i < txns_logged; i++) {
            if (txns[i].chip != c) continue;
            CHECK(k < 4 && !txns[i].read && txns[i].len == 2 && txns[i].reg == order[k]);
            k++;
        }
        CHECK(k == 4);
    }
    CHECK(txns_logged == 8);
}

static void test_steps_8_and_16_come_from_0x24(void) {
    reset();
    mcp23017_init();
    step_buttons_t *b = mcp23017_get_buttons();
    poll_buttons_for(50);
    // GPA7/GPB7 of 0x20 and the unused pins of 0x24 read high, and are ignored.
    CHECK((port_value(U701, 0) & 0x80) && (port_value(U701, 1) & 0x80));
    CHECK(port_value(U703, 0) == 0xFC && port_value(U703, 1) == 0xFF);
    CHECK(b->held == 0 && b->pressed == 0);
    press(1u << 7);                           // 0x24 GPA0
    poll_buttons_for(30);
    CHECK(b->pressed == (1u << 7) && b->held == (1u << 7));
    b->pressed = 0;
    press(1u << 15);                          // 0x24 GPA1
    poll_buttons_for(30);
    CHECK(b->pressed == (1u << 15) && b->held == (1u << 15));
    CHECK(b->released == (1u << 7));
    press((1u << 3) | (1u << 7));             // read_raw reads both chips now,
    CHECK(mcp23017_read_raw() == ((1u << 3) | (1u << 7)));
    CHECK(b->held == (1u << 15));             // without touching the state
    // With U703 silent, U701 is sampled alone: its GPA7/GPB7 still never count.
    absent[U703] = true;
    press(0);
    poll_buttons_for(30);
    CHECK(b->held == (1u << 15));             // step 16 kept as last accepted
}

// A U703 that resets before a read reads its idle steps 8 and 16 as pressed;
// the IPOL read after the ports catches it.
static void test_reset_0x24_never_presses_8_or_16(void) {
    reset();
    mcp23017_init();
    step_buttons_t *b = mcp23017_get_buttons();
    poll_buttons_for(50);
    chip_reset(U703);                         // brown-out: power-on registers
    CHECK((port_value(U703, 0) & 0x03) == 0x03);
    for (unsigned ms = 0; ms < 100; ms++) {
        sleep_ms(1);
        mcp23017_poll();
        CHECK(!(b->pressed & 0x8080) && !(b->held & 0x8080) && !(b->long_pressed & 0x8080));
    }
    CHECK(configured_as(U703, 0x03, 0x00));
    press(1u << 15);                          // a real press of step 16 still works
    poll_buttons_for(30);
    CHECK(b->pressed == (1u << 15) && b->held == (1u << 15));
}

// A chip that resets inside its port read gives its first byte, then 0xFF with
// a full count, and IPOL at 0 after it: the sample is dropped, the chip is
// configured again and its next good read is the baseline. The key going down
// across the reset is then held, never pressed, and never long-presses.
static void test_reset_inside_the_read_never_presses(void) {
    static const struct { int chip; uint16_t key; uint8_t a, b; } cases[] = {
        { U703, 1u << 7, 0x03, 0x00 },        // step 8
        { U701, 1u << 2, 0x7F, 0x7F },        // step 3
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        reset();
        mcp23017_init();
        step_buttons_t *b = mcp23017_get_buttons();
        poll_buttons_for(50);
        press(cases[k].key);
        reset_in_read = cases[k].chip;
        for (unsigned ms = 0; ms < 100; ms++) {
            sleep_ms(1);
            mcp23017_poll();
            CHECK(b->pressed == 0);
        }
        CHECK(reset_in_read < 0);             // the cut read happened
        CHECK(configured_as(cases[k].chip, cases[k].a, cases[k].b));
        CHECK(b->held == cases[k].key);
        poll_buttons_for(600);
        CHECK(b->pressed == 0 && b->long_pressed == 0);
        press(0);
        poll_buttons_for(30);
        CHECK(b->released == cases[k].key && b->held == 0);
    }
}

// An old board without U703: the other 14 steps work, U703 costs one NACKed
// write per 100 ms, and a resync (which leaves U703's rebaseline pending for
// good) neither stalls U701 nor makes it read on every pass.
static void test_missing_0x24_keeps_other_steps(void) {
    reset();
    absent[U703] = true;
    mcp23017_init();                          // tries U703 once, at t = 0
    step_buttons_t *b = mcp23017_get_buttons();
    uint32_t last_try = 0;
    unsigned tries = calls[U703], samples = 0;
    for (uint32_t ms = 1; ms <= 1000; ms++) {
        if (ms == 100 || ms == 600) press(0x7F7F);    // steps 1-7, 9-15
        if (ms == 300 || ms == 800) press(0);
        sleep_ms(1);
        unsigned before = calls[U701];
        if (ms == 500) mcp23017_resync(); else mcp23017_poll();
        unsigned reads = calls[U701] - before;
        CHECK(reads == 0 || reads == 4);      // whole samples only
        samples += reads == 4;
        if (calls[U703] != tries) {           // one attempt: one NACKed write
            CHECK(calls[U703] == tries + 1 && ms - last_try >= 100);
            tries = calls[U703];
            last_try = ms;
        }
        if (ms == 299 || ms == 799) CHECK(b->pressed == 0x7F7F && b->held == 0x7F7F);
        if (ms == 499 || ms == 999) CHECK(b->released == 0x7F7F && b->held == 0);
    }
    CHECK(samples <= 1000 / MCP_DEBOUNCE_MS + 1);
    CHECK(b->long_pressed == 0);
}

// Step 3 is released while every transfer to U701 fails: its last accepted
// state (held) stays, but it must not long-press on it.
static void test_no_long_press_from_a_failing_chip(void) {
    reset();
    mcp23017_init();
    step_buttons_t *b = mcp23017_get_buttons();
    poll_buttons_for(50);
    press(1u << 2);
    for (unsigned ms = 0; ms < 30 && !(b->pressed & (1u << 2)); ms++) {
        sleep_ms(1);
        mcp23017_poll();
    }
    CHECK(b->pressed & (1u << 2));            // held from this sample on
    poll_each_ms_for(450);
    CHECK(b->long_pressed == 0 && (b->held & (1u << 2)));
    absent[U701] = true;
    press(0);
    for (unsigned ms = 0; ms < 200; ms++) {
        sleep_ms(1);
        mcp23017_poll();
        CHECK(b->long_pressed == 0 && (b->held & (1u << 2)));
    }
    absent[U701] = false;                     // the bus is back
    for (unsigned ms = 0; ms < 150; ms++) {
        sleep_ms(1);
        mcp23017_poll();
        CHECK(b->long_pressed == 0);
    }
    CHECK((b->released & (1u << 2)) && !(b->held & (1u << 2)));
}

static void test_reads_only_when_a_sample_is_due(void) {
    reset();
    mcp23017_init();
    unsigned ipol_before = ipol_writes[U701] + ipol_writes[U703];
    unsigned total = 0;
    for (unsigned ms = 0; ms < 1000; ms++) {
        unsigned before = calls[U701] + calls[U703];
        sleep_ms(1);
        mcp23017_poll();
        unsigned n = calls[U701] + calls[U703] - before;
        CHECK(n == 0 || n == 8);              // a sample: per chip the ports, then IPOL
        total += n;
    }
    CHECK(total >= 2 * 4 * 49 && total <= 2 * 4 * 51);
    CHECK(ipol_writes[U701] + ipol_writes[U703] == ipol_before);   // the canary stays
}

// A chip that takes its configuration but fails the sample after it counts as
// a failed attempt: with a rebaseline pending (a resync) it is retried every
// 100 ms, not on every pass.
static void test_failed_first_sample_is_retried_every_100_ms(void) {
    reset();
    mcp23017_init();
    poll_buttons_for(50);
    fail_reads = true;                        // writes land, every read fails
    unsigned before = calls[U701];
    mcp23017_resync();
    poll_each_ms_for(999);
    // The failed read at the resync (pointer write, read), then one attempt
    // per 100 ms from the next pass: 4 writes, a pointer write, a read.
    CHECK(calls[U701] - before <= 2 + 6 * 10);
    fail_reads = false;
    press(1u << 4);
    poll_buttons_for(150);
    CHECK(mcp23017_get_buttons()->held == (1u << 4));
    CHECK(mcp23017_get_buttons()->pressed == 0);   // the baseline after the resync
}

static void test_failed_port_read_keeps_buttons(void) {
    reset();
    mcp23017_init();
    step_buttons_t *b = mcp23017_get_buttons();
    press(1u << 3);
    sleep_ms(30);
    txns_logged = 0;
    mcp23017_poll();
    CHECK(b->held & (1u << 3));
    CHECK(b->pressed & (1u << 3));
    CHECK(reads_were(U701, 0x12, 0x02));     // both ports in one 2-byte read, then IPOL
    CHECK(reads_were(U703, 0x12, 0x02));
    b->pressed = 0;
    fail_reads = true;
    press(0);
    sleep_ms(30);
    mcp23017_poll();
    CHECK(b->held & (1u << 3));
    CHECK(!(b->released & (1u << 3)));
    CHECK(mcp23017_read_raw() & (1u << 3));
    fail_reads = false;
    sleep_ms(30);
    mcp23017_poll();                          // the chip recovered: now released
    CHECK(!(b->held & (1u << 3)));
    CHECK(b->released & (1u << 3));
}

static void test_failed_led_write_is_retried(void) {
    reset();
    mcp23017_init();
    step_leds_init();
    led_writes = 0;
    fail_writes = true;
    step_leds_set_mask(0x0005);
    fail_writes = false;
    CHECK(led_writes == 0);
    step_leds_set_mask(0x0005);               // same mask is written again
    CHECK(led_writes == 1);
    CHECK(last_write_len == 3);
    CHECK(regs[U702][0x14] == 0x05 && regs[U702][0x15] == 0x00);
    step_leds_set_mask(0x0005);               // accepted: no new traffic
    CHECK(led_writes == 1);
    step_leds_set_mask(0x0100);
    CHECK(led_writes == 2 && regs[U702][0x14] == 0x00 && regs[U702][0x15] == 0x01);
}

static void test_late_expander_is_configured_before_decoding(void) {
    reset();
    fail_writes = true;
    mcp23017_init();                          // chips absent at power-up
    step_buttons_t *b = mcp23017_get_buttons();
    press((1u << 5) | (1u << 15));
    sleep_ms(30);
    mcp23017_poll();
    CHECK(b->held == 0 && b->pressed == 0);  // nothing decoded while unconfigured
    fail_writes = false;
    sleep_ms(100);                            // a failed attempt waits 100 ms
    mcp23017_poll();                          // configures, then decodes
    CHECK(configured_as(U701, 0x7F, 0x7F));
    CHECK(configured_as(U703, 0x03, 0x00));
    CHECK(b->held == ((1u << 5) | (1u << 15)) && b->pressed == ((1u << 5) | (1u << 15)));
}

static void test_i2c_pins_have_no_internal_pulls(void) {
    reset();
    mcp23017_init();
    CHECK(fake_gpio_pulls_disabled[8] && fake_gpio_pulls_disabled[9]);
    CHECK(!fake_gpio_pulled_up[8] && !fake_gpio_pulled_up[9]);
}

static void test_resync_takes_the_ports_as_baseline(void) {
    reset();
    mcp23017_init();
    step_buttons_t *b = mcp23017_get_buttons();
    press(1u << 3);
    sleep_ms(30);
    mcp23017_poll();
    CHECK(b->pressed == (1u << 3));
    b->pressed = 0;
    press((1u << 5) | (1u << 15));            // during a flash stall: 3 up, 5 and 16 down
    sleep_ms(1000);
    mcp23017_resync();
    poll_buttons_for(1200);                   // they stay held well past a long press
    CHECK(b->pressed == 0 && b->released == 0 && b->long_pressed == 0);
    CHECK(b->held == ((1u << 5) | (1u << 15)));
    press(0);
    poll_buttons_for(50);
    CHECK(b->released == ((1u << 5) | (1u << 15)));
    CHECK(b->pressed == 0 && b->long_pressed == 0);
}

static void test_failed_read_reconfigures_the_expander(void) {
    reset();
    mcp23017_init();
    poll_buttons_for(30);                     // a good sample proves the configuration
    regs[U701][0x0C] = regs[U701][0x0D] = 0;  // pull-ups lost (chip reset)
    fail_reads = true;
    sleep_ms(30);
    mcp23017_poll();
    fail_reads = false;
    sleep_ms(30);
    mcp23017_poll();
    CHECK(configured_as(U701, 0x7F, 0x7F));
}

static void test_reset_expander_is_reconfigured(void) {
    reset();
    mcp23017_init();
    step_leds_init();
    step_leds_set_mask(0x0005);
    chip_reset(U701);                         // buttons: back to inputs, IPOL and pull-ups lost
    chip_reset(U703);
    chip_reset(U702);                         // LEDs: back to inputs, latches cleared
    for (unsigned t = 0; t < 1200; t += 10) {
        sleep_ms(10);
        mcp23017_poll();
        step_leds_set_mask(0x0005);
    }
    CHECK(configured_as(U701, 0x7F, 0x7F));
    CHECK(configured_as(U703, 0x03, 0x00));
    CHECK(regs[U702][0x00] == 0x00 && regs[U702][0x01] == 0x00);
    CHECK(regs[U702][0x14] == 0x05 && regs[U702][0x15] == 0x00);
}

static void test_failed_led_write_rewrites_the_direction(void) {
    reset();
    mcp23017_init();
    step_leds_init();
    regs[U702][0x00] = regs[U702][0x01] = 0xFF;   // the write failed because the chip reset
    fail_writes = true;
    step_leds_set_mask(0x0006);
    fail_writes = false;
    step_leds_set_mask(0x0006);
    CHECK(regs[U702][0x00] == 0x00 && regs[U702][0x01] == 0x00);
    CHECK(regs[U702][0x14] == 0x06);
}

static void test_dead_bus_is_retried_at_most_every_100_ms(void) {
    reset();
    mcp23017_init();
    step_leds_init();
    fail_writes = fail_reads = true;
    i2c_calls = 0;
    for (unsigned ms = 0; ms < 1000; ms++) {
        sleep_ms(1);
        mcp23017_poll();
        step_leds_set_mask((uint16_t)ms);
    }
    // Each step chip: its first sample fails (t = 20, one pointer write) and
    // arms the retry, then one configuration attempt (one failed write) per
    // 100 ms, t = 120 ... 920: 10. The LEDs: the failed mask write, the
    // failed direction write, then one per 100 ms: 11.
    CHECK(i2c_calls <= 2 * 10 + 11);
    fail_writes = fail_reads = false;         // the bus comes back
    sleep_ms(100);
    press(1u << 2);
    poll_buttons_for(50);
    CHECK(mcp23017_get_buttons()->held == (1u << 2));
}

// A chip that reset but still answers has IPOL and the pull-ups at their
// defaults, so its idle lines read as pressed. IPOL, read back after the
// ports, shows it: the sample is dropped and the next good read becomes the
// baseline, with no press.
static void test_reset_expander_idle_lines_are_not_presses(void) {
    reset();
    mcp23017_init();
    step_buttons_t *b = mcp23017_get_buttons();
    poll_buttons_for(50);
    CHECK(b->held == 0 && b->pressed == 0);
    chip_reset(U701);
    CHECK(port_value(U701, 0) == 0xFF && port_value(U701, 1) == 0xFF);
    sleep_ms(30);
    mcp23017_poll();
    CHECK(b->pressed == 0 && b->held == 0 && b->long_pressed == 0);
    poll_buttons_for(50);                     // reconfigured: idle reads 0 again
    CHECK(configured_as(U701, 0x7F, 0x7F));
    CHECK(b->pressed == 0 && b->held == 0 && b->released == 0);
    press(1u << 4);                           // and a real press still works
    poll_buttons_for(30);
    CHECK(b->pressed == (1u << 4) && b->held == (1u << 4));
}

// Several keys going down in one sample are an ordinary chord.
static void test_three_key_chord_is_pressed(void) {
    reset();
    mcp23017_init();
    step_buttons_t *b = mcp23017_get_buttons();
    poll_buttons_for(50);
    press(0x0007);
    sleep_ms(30);
    mcp23017_poll();
    CHECK(b->pressed == 0x0007 && b->held == 0x0007);
}

static button_event_t poll_encoder(uint8_t enc) {
    sleep_ms(25);
    encoder_poll();
    return encoder_get_button(enc);
}

static void test_encoder_button_released_during_stall_is_silent(void) {
    reset();
    encoder_init();
    fake_gpio_level[ENC1_SW_PIN] = false;     // pressed before the stall
    for (int i = 0; i < 3; i++) CHECK(poll_encoder(0) == BTN_NONE);
    CHECK(encoder_button_held(0));
    fake_gpio_level[ENC1_SW_PIN] = true;      // released during it
    sleep_ms(1000);
    encoder_resync();
    for (int i = 0; i < 3; i++) CHECK(poll_encoder(0) == BTN_NONE);
    CHECK(!encoder_button_held(0));
}

static void test_encoder_button_held_through_stall_never_fires(void) {
    reset();
    encoder_init();
    fake_gpio_level[ENC2_SW_PIN] = false;     // pressed during the stall
    sleep_ms(1000);
    encoder_resync();
    CHECK(encoder_button_held(1));
    for (int i = 0; i < 40; i++) {            // a second: past a long press
        button_event_t ev = poll_encoder(1);
        CHECK(ev != BTN_PRESS && ev != BTN_LONG_PRESS);
    }
    fake_gpio_level[ENC2_SW_PIN] = true;
    for (int i = 0; i < 3; i++) {
        button_event_t ev = poll_encoder(1);
        CHECK(ev != BTN_PRESS && ev != BTN_LONG_PRESS);
    }
    fake_gpio_level[ENC2_SW_PIN] = false;     // the next press works
    for (int i = 0; i < 3; i++) poll_encoder(1);
    fake_gpio_level[ENC2_SW_PIN] = true;
    button_event_t ev = BTN_NONE;
    for (int i = 0; i < 3 && ev == BTN_NONE; i++) ev = poll_encoder(1);
    CHECK(ev == BTN_PRESS);
}

#define RUN(t) do { t(); printf("PASS %s\n", #t); } while (0)
int main(void) {
    RUN(test_step_row_configures_both_chips);
    RUN(test_steps_8_and_16_come_from_0x24);
    RUN(test_reset_0x24_never_presses_8_or_16);
    RUN(test_reset_inside_the_read_never_presses);
    RUN(test_missing_0x24_keeps_other_steps);
    RUN(test_no_long_press_from_a_failing_chip);
    RUN(test_reads_only_when_a_sample_is_due);
    RUN(test_failed_first_sample_is_retried_every_100_ms);
    RUN(test_failed_port_read_keeps_buttons);
    RUN(test_failed_led_write_is_retried);
    RUN(test_late_expander_is_configured_before_decoding);
    RUN(test_i2c_pins_have_no_internal_pulls);
    RUN(test_resync_takes_the_ports_as_baseline);
    RUN(test_failed_read_reconfigures_the_expander);
    RUN(test_reset_expander_is_reconfigured);
    RUN(test_failed_led_write_rewrites_the_direction);
    RUN(test_dead_bus_is_retried_at_most_every_100_ms);
    RUN(test_reset_expander_idle_lines_are_not_presses);
    RUN(test_three_key_chord_is_pressed);
    RUN(test_encoder_button_released_during_stall_is_silent);
    RUN(test_encoder_button_held_through_stall_never_fires);
    return 0;
}
