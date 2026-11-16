#include "step_leds.h"
#include "mcp23017.h"

#if STEP_LEDS_ENABLED
#include "pico/stdlib.h"
#include "hardware/i2c.h"
// MCP23017 registers (BANK=0).
#define REG_IODIRA      0x00
#define REG_OLATA       0x14

#define LED_I2C_TIMEOUT_US 2000

static uint16_t g_led_mask;     // last mask the chip accepted
static bool g_mask_known;       // false until a write succeeded
static bool g_dir_ok;           // both ports are configured as outputs
static bool g_leds_ready;

// One 3-byte transfer: register, then the value(s) for it and its neighbour.
static bool led_write2(uint8_t reg, uint8_t a, uint8_t b) {
    uint8_t buf[3] = { reg, a, b };
    return i2c_write_timeout_us(i2c0, STEP_LEDS_ADDR, buf, 3, false,
                                LED_I2C_TIMEOUT_US) == 3;
}

void step_leds_init(void) {
    g_led_mask = 0;
    g_mask_known = false;
    g_leds_ready = true;

    // I2C pins and peripheral are already configured by mcp23017_init().
    // Set both ports as outputs, then clear all LEDs. A failure here is
    // retried by the next step_leds_set_mask call.
    g_dir_ok = led_write2(REG_IODIRA, 0x00, 0x00);
    g_mask_known = g_dir_ok && led_write2(REG_OLATA, 0x00, 0x00);
}

void step_leds_set_mask(uint16_t mask) {
    if (!g_leds_ready) return;
    if (g_mask_known && mask == g_led_mask) return;
    if (!g_dir_ok) {
        g_dir_ok = led_write2(REG_IODIRA, 0x00, 0x00);
        if (!g_dir_ok) return;
    }
    // OLATA and OLATB in one transfer; cache only what the chip accepted.
    if (led_write2(REG_OLATA, (uint8_t)(mask & 0xFFu), (uint8_t)(mask >> 8))) {
        g_led_mask = mask;
        g_mask_known = true;
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
