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

// Two fake expanders: 0x20 (buttons) and 0x21 (LEDs), 22 registers each.
static uint8_t regs[2][0x16];
static uint8_t read_pointer[2];
static bool fail_writes, fail_reads;
static unsigned timeouts_seen_bad;       // calls with a timeout other than 2000 us
static unsigned led_writes;              // 3-byte OLAT writes to 0x21
static unsigned last_write_len;
static unsigned last_read_len;
static unsigned i2c_calls;               // every transfer, failed or not

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
    i2c_calls++;
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
    i2c_calls++;
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
    timeouts_seen_bad = led_writes = i2c_calls = 0;
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

static void test_late_expander_is_configured_before_decoding(void) {
    reset();
    fail_writes = true;
    mcp23017_init();                          // chip absent at power-up
    step_buttons_t *b = mcp23017_get_buttons();
    press(1u << 5);
    sleep_ms(30);
    mcp23017_poll();
    CHECK(b->held == 0 && b->pressed == 0);  // nothing decoded while unconfigured
    fail_writes = false;
    sleep_ms(100);                            // a failed attempt waits 100 ms
    mcp23017_poll();                          // configures, then decodes
    CHECK(regs[0][0x00] == 0xFF && regs[0][0x01] == 0xFF);
    CHECK(regs[0][0x0C] == 0xFF && regs[0][0x0D] == 0xFF);
    CHECK(regs[0][0x02] == 0xFF && regs[0][0x03] == 0xFF);
    CHECK(b->held == (1u << 5) && b->pressed == (1u << 5));
    CHECK(timeouts_seen_bad == 0);
}

static void test_i2c_pins_have_no_internal_pulls(void) {
    reset();
    mcp23017_init();
    CHECK(fake_gpio_pulls_disabled[8] && fake_gpio_pulls_disabled[9]);
    CHECK(!fake_gpio_pulled_up[8] && !fake_gpio_pulled_up[9]);
}

static void poll_buttons_for(unsigned ms) {
    for (unsigned t = 0; t < ms; t += 10) {
        sleep_ms(10);
        mcp23017_poll();
    }
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
    press(1u << 5);                           // during a flash stall: 3 up, 5 down
    sleep_ms(1000);
    mcp23017_resync();
    poll_buttons_for(1200);                   // 5 stays held well past a long press
    CHECK(b->pressed == 0 && b->released == 0 && b->long_pressed == 0);
    CHECK(b->held == (1u << 5));
    press(0);
    poll_buttons_for(50);
    CHECK(b->released == (1u << 5));
    CHECK(b->pressed == 0 && b->long_pressed == 0);
}

static void test_failed_read_reconfigures_the_expander(void) {
    reset();
    mcp23017_init();
    regs[0][0x0C] = regs[0][0x0D] = 0;        // pull-ups lost (chip reset)
    fail_reads = true;
    sleep_ms(30);
    mcp23017_poll();
    fail_reads = false;
    sleep_ms(30);
    mcp23017_poll();
    CHECK(regs[0][0x0C] == 0xFF && regs[0][0x0D] == 0xFF);
}

static void test_reset_expander_is_reconfigured(void) {
    reset();
    mcp23017_init();
    step_leds_init();
    step_leds_set_mask(0x0005);
    regs[0][0x02] = regs[0][0x03] = 0;        // buttons: IPOL and pull-ups at defaults
    regs[0][0x0C] = regs[0][0x0D] = 0;
    regs[1][0x00] = regs[1][0x01] = 0xFF;     // LEDs: back to inputs, latches cleared
    regs[1][0x14] = regs[1][0x15] = 0;
    for (unsigned t = 0; t < 1200; t += 10) {
        sleep_ms(10);
        mcp23017_poll();
        step_leds_set_mask(0x0005);
    }
    CHECK(regs[0][0x02] == 0xFF && regs[0][0x03] == 0xFF);
    CHECK(regs[0][0x0C] == 0xFF && regs[0][0x0D] == 0xFF);
    CHECK(regs[1][0x00] == 0x00 && regs[1][0x01] == 0x00);
    CHECK(regs[1][0x14] == 0x05 && regs[1][0x15] == 0x00);
}

static void test_failed_led_write_rewrites_the_direction(void) {
    reset();
    mcp23017_init();
    step_leds_init();
    regs[1][0x00] = regs[1][0x01] = 0xFF;     // the write failed because the chip reset
    fail_writes = true;
    step_leds_set_mask(0x0006);
    fail_writes = false;
    step_leds_set_mask(0x0006);
    CHECK(regs[1][0x00] == 0x00 && regs[1][0x01] == 0x00);
    CHECK(regs[1][0x14] == 0x06);
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
    CHECK(i2c_calls <= 26);                   // about one attempt per chip per 100 ms
    fail_writes = fail_reads = false;         // the bus comes back
    sleep_ms(100);
    press(1u << 2);
    poll_buttons_for(50);
    CHECK(mcp23017_get_buttons()->held == (1u << 2));
}

// A chip that reset but still answers has IPOL and the pull-ups at their
// defaults, so its idle lines read as pressed. That sample is checked against
// the configuration and becomes the new baseline, with no press.
static void test_reset_expander_idle_lines_are_not_presses(void) {
    reset();
    mcp23017_init();
    step_buttons_t *b = mcp23017_get_buttons();
    poll_buttons_for(50);
    CHECK(b->held == 0 && b->pressed == 0);
    regs[0][0x02] = regs[0][0x03] = 0;        // reset: IPOL and pull-ups at defaults
    regs[0][0x0C] = regs[0][0x0D] = 0;
    press(0xFFFF);                            // idle lines, no longer inverted
    sleep_ms(30);
    mcp23017_poll();
    CHECK(b->pressed == 0 && b->held == 0 && b->long_pressed == 0);
    press(0);                                 // reconfigured: idle reads 0 again
    poll_buttons_for(50);
    CHECK(regs[0][0x02] == 0xFF && regs[0][0x03] == 0xFF);
    CHECK(regs[0][0x0C] == 0xFF && regs[0][0x0D] == 0xFF);
    CHECK(b->pressed == 0 && b->held == 0 && b->released == 0);
    press(1u << 4);                           // and a real press still works
    poll_buttons_for(30);
    CHECK(b->pressed == (1u << 4) && b->held == (1u << 4));
}

// A real chord on a configured chip passes the readback.
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
