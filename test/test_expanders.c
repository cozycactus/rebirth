#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/timer.h"
#include "fake_hardware.h"
#include "mcp23017.h"
#include "step_leds.h"
#include "sequencer.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

i2c_inst_t fake_i2c0_inst;
sequencer_state_t g_seq;   // referenced by the shared fake hardware

// Two fake expanders: 0x20 (buttons) and 0x21 (LEDs), 22 registers each.
static uint8_t regs[2][0x16];
static uint8_t read_pointer[2];
static bool fail_writes, fail_reads;
static unsigned timeouts_seen_bad;       // calls with a timeout other than 2000 us
static unsigned led_writes;              // 3-byte OLAT writes to 0x21
static unsigned last_write_len;
static unsigned last_read_len;

static int chip_of(uint8_t addr) {
    CHECK(addr == MCP23017_ADDR || addr == STEP_LEDS_ADDR);
    return addr == STEP_LEDS_ADDR;
}

unsigned i2c_init(i2c_inst_t *i2c, unsigned baudrate) {
    (void)i2c; return baudrate;
}

int i2c_write_timeout_us(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len,
                         bool nostop, unsigned timeout_us) {
    (void)i2c; (void)nostop;
    last_write_len = (unsigned)len;
    if (timeout_us != 2000) timeouts_seen_bad++;
    if (fail_writes) return PICO_ERROR_TIMEOUT;
    int chip = chip_of(addr);
    CHECK(len >= 1);
    read_pointer[chip] = src[0];
    for (size_t i = 1; i < len; i++) {
        CHECK(src[0] + i - 1 < sizeof(regs[chip]));
        regs[chip][src[0] + i - 1] = src[i];
    }
    if (chip == 1 && len == 3 && src[0] == 0x14) led_writes++;
    return (int)len;
}

int i2c_read_timeout_us(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len,
                        bool nostop, unsigned timeout_us) {
    (void)i2c; (void)nostop;
    last_read_len = (unsigned)len;
    if (timeout_us != 2000) timeouts_seen_bad++;
    if (fail_reads) return PICO_ERROR_TIMEOUT;
    int chip = chip_of(addr);
    for (size_t i = 0; i < len; i++) dst[i] = regs[chip][read_pointer[chip] + i];
    return (int)len;
}

static void reset(void) {
    fake_hardware_reset();
    memset(regs, 0, sizeof(regs));
    memset(read_pointer, 0, sizeof(read_pointer));
    fail_writes = fail_reads = false;
    timeouts_seen_bad = led_writes = 0;
    last_write_len = last_read_len = 0;
}

static void press(uint16_t mask) {          // inverted by IPOL: pressed = 1
    regs[0][0x12] = (uint8_t)(mask & 0xFF);
    regs[0][0x13] = (uint8_t)(mask >> 8);
}

static void test_failed_port_read_keeps_buttons(void) {
    reset();
    mcp23017_init();
    step_buttons_t *b = mcp23017_get_buttons();
    press(1u << 3);
    sleep_ms(30);
    mcp23017_poll();
    CHECK(b->held & (1u << 3));
    CHECK(b->pressed & (1u << 3));
    CHECK(last_read_len == 2);               // both ports in one transfer
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
    CHECK(timeouts_seen_bad == 0);
}

static void test_failed_led_write_is_retried(void) {
    reset();
    mcp23017_init();
    step_leds_init();
    CHECK(timeouts_seen_bad == 0);
    led_writes = 0;
    fail_writes = true;
    step_leds_set_mask(0x0005);
    fail_writes = false;
    CHECK(led_writes == 0);
    step_leds_set_mask(0x0005);               // same mask is written again
    CHECK(led_writes == 1);
    CHECK(last_write_len == 3);
    CHECK(regs[1][0x14] == 0x05 && regs[1][0x15] == 0x00);
    step_leds_set_mask(0x0005);               // accepted: no new traffic
    CHECK(led_writes == 1);
    step_leds_set_mask(0x0100);
    CHECK(led_writes == 2 && regs[1][0x14] == 0x00 && regs[1][0x15] == 0x01);
    CHECK(timeouts_seen_bad == 0);
}

static void test_i2c_pins_have_no_internal_pulls(void) {
    reset();
    mcp23017_init();
    CHECK(fake_gpio_pulls_disabled[8] && fake_gpio_pulls_disabled[9]);
    CHECK(!fake_gpio_pulled_up[8] && !fake_gpio_pulled_up[9]);
}

#define RUN(t) do { t(); printf("PASS %s\n", #t); } while (0)
int main(void) {
    RUN(test_failed_port_read_keeps_buttons);
    RUN(test_failed_led_write_is_retried);
    RUN(test_i2c_pins_have_no_internal_pulls);
    return 0;
}
