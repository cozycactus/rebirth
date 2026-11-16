#include "step_leds.h"
#include "mcp23017.h"

#if STEP_LEDS_ENABLED
#include "pico/stdlib.h"
#include "hardware/i2c.h"
// MCP23017 registers (BANK=0).
#define REG_IODIRA      0x00
#define REG_OLATA       0x14

#define LED_I2C_TIMEOUT_US 2000
#define LED_RETRY_MS    100     // between failed direction writes
#define LED_CHECK_MS    1000    // IODIR readback, to notice a chip that reset

static uint16_t g_led_mask;     // last mask the chip accepted
static bool g_mask_known;       // false until a write succeeded
static bool g_dir_ok;           // both ports are configured as outputs
static bool g_leds_ready;
static bool g_retry_armed;      // the last direction write failed
static uint32_t g_retry_from_ms;
static uint32_t g_check_ms;     // last IODIR readback

// One 3-byte transfer: register, then the value(s) for it and its neighbour.
static bool led_write2(uint8_t reg, uint8_t a, uint8_t b) {
    uint8_t buf[3] = { reg, a, b };
    return i2c_write_timeout_us(i2c0, STEP_LEDS_ADDR, buf, 3, false,
                                LED_I2C_TIMEOUT_US) == 3;
}

// Reads a register and its neighbour in one transfer.
static bool led_read2(uint8_t reg, uint8_t val[2]) {
    if (i2c_write_timeout_us(i2c0, STEP_LEDS_ADDR, &reg, 1, true,
                             LED_I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(i2c0, STEP_LEDS_ADDR, val, 2, false,
                               LED_I2C_TIMEOUT_US) == 2;
}

// After any I2C error, or a chip found reset, the direction and the mask are
// written again.
static void mark_unconfigured(void) {
    g_dir_ok = false;
    g_mask_known = false;
}

void step_leds_init(void) {
    g_led_mask = 0;
    g_leds_ready = true;
    g_retry_armed = false;
    g_check_ms = to_ms_since_boot(get_absolute_time());
    mark_unconfigured();

    // I2C pins and peripheral are already configured by mcp23017_init().
    // Set both ports as outputs, then clear all LEDs. A failure here is
    // retried by a later step_leds_set_mask call.
    step_leds_set_mask(0);
}

void step_leds_set_mask(uint16_t mask) {
    if (!g_leds_ready) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    // About once a second: a chip that reset has its ports back as inputs.
    if (g_dir_ok && (uint32_t)(now - g_check_ms) >= LED_CHECK_MS) {
        g_check_ms = now;
        uint8_t dir[2];
        if (!led_read2(REG_IODIRA, dir) || dir[0] != 0x00 || dir[1] != 0x00) {
            mark_unconfigured();
        }
    }
    if (g_mask_known && mask == g_led_mask) return;
    if (!g_dir_ok) {
        // An attempt after a failed one waits LED_RETRY_MS.
        if (g_retry_armed && (uint32_t)(now - g_retry_from_ms) < LED_RETRY_MS) return;
        g_dir_ok = led_write2(REG_IODIRA, 0x00, 0x00);
        g_retry_armed = !g_dir_ok;
        g_retry_from_ms = now;
        if (!g_dir_ok) return;
    }
    // OLATA and OLATB in one transfer; cache only what the chip accepted.
    if (led_write2(REG_OLATA, (uint8_t)(mask & 0xFFu), (uint8_t)(mask >> 8))) {
        g_led_mask = mask;
        g_mask_known = true;
    } else {
        mark_unconfigured();
    }
}

void step_leds_clear(void) {
    step_leds_set_mask(0);
}

bool step_leds_available(void) {
    return true;
}

#else

void step_leds_init(void) {}
void step_leds_set_mask(uint16_t mask) { (void)mask; }
void step_leds_clear(void) {}
bool step_leds_available(void) { return false; }

#endif
