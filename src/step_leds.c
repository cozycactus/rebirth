#include "step_leds.h"
#include "mcp23017.h"

#if STEP_LEDS_ENABLED && !defined(MOCK_HARDWARE)
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#endif

#if STEP_LEDS_ENABLED
// MCP23017 registers (BANK=0).
#define REG_IODIRA      0x00
#define REG_IODIRB      0x01
#define REG_OLATA       0x14
#define REG_OLATB       0x15

static uint16_t g_led_mask;
static bool g_leds_ready;

#if !defined(MOCK_HARDWARE)
static void led_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    (void)i2c_write_blocking(i2c0, STEP_LEDS_ADDR, buf, 2, false);
}
#endif

void step_leds_init(void) {
    g_led_mask = 0;
    g_leds_ready = true;

#if !defined(MOCK_HARDWARE)
    // I2C pins and peripheral are already configured by mcp23017_init().
    // Set both MCP ports as outputs, then clear all LEDs.
    led_write_reg(REG_IODIRA, 0x00);
    led_write_reg(REG_IODIRB, 0x00);
    led_write_reg(REG_OLATA, 0x00);
    led_write_reg(REG_OLATB, 0x00);
#endif
}

void step_leds_set_mask(uint16_t mask) {
    if (!g_leds_ready || mask == g_led_mask) return;
    g_led_mask = mask;

#if !defined(MOCK_HARDWARE)
    led_write_reg(REG_OLATA, (uint8_t)(mask & 0xFFu));
    led_write_reg(REG_OLATB, (uint8_t)(mask >> 8));
#endif
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
