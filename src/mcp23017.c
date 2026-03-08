#ifndef MOCK_HARDWARE
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#else
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
static uint32_t _mock_ms = 0;
static uint8_t _mock_regs[256] = {0};
int i2c_write_blocking(void *i2c, uint8_t addr, const uint8_t *buf, int len, bool nostop) {
    (void)i2c; (void)addr; (void)nostop;
    if (len == 2) _mock_regs[buf[0]] = buf[1];
    return len;
}
int i2c_read_blocking(void *i2c, uint8_t addr, uint8_t *buf, int len, bool nostop) {
    (void)i2c; (void)addr; (void)nostop; (void)len;
    buf[0] = 0xFF;  // All high (no buttons pressed)
    return len;
}
void i2c_init(void *i2c, uint32_t freq) { (void)i2c; (void)freq; }
void gpio_set_function(uint8_t pin, int func) { (void)pin; (void)func; }
void gpio_pull_up(uint8_t pin) { (void)pin; }
uint32_t to_ms_since_boot(uint64_t t) { (void)t; return _mock_ms; }
uint64_t get_absolute_time(void) { return 0; }
#define i2c0 ((void *)0)
#define GPIO_FUNC_I2C 0
#endif

#include "mcp23017.h"

// ─── I2C instance ──────────────────────────────────────────────
#define MCP_I2C     i2c0

// ─── MCP23017 Register Addresses (BANK=0, sequential) ──────────
#define REG_IODIRA      0x00    // I/O direction port A (1=input)
#define REG_IODIRB      0x01    // I/O direction port B
#define REG_IPOLA       0x02    // Input polarity port A
#define REG_IPOLB       0x03    // Input polarity port B
#define REG_GPPUA       0x0C    // Pull-up resistors port A
#define REG_GPPUB       0x0D    // Pull-up resistors port B
#define REG_GPIOA       0x12    // GPIO port A
#define REG_GPIOB       0x13    // GPIO port B

// ─── State ─────────────────────────────────────────────────────

static step_buttons_t buttons;

// ─── I2C Helpers ───────────────────────────────────────────────

static void mcp_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    i2c_write_blocking(MCP_I2C, MCP23017_ADDR, buf, 2, false);
}

static uint8_t mcp_read_reg(uint8_t reg) {
    uint8_t val;
    i2c_write_blocking(MCP_I2C, MCP23017_ADDR, &reg, 1, true);
    i2c_read_blocking(MCP_I2C, MCP23017_ADDR, &val, 1, false);
    return val;
}

// ─── Initialization ────────────────────────────────────────────

void mcp23017_init(void) {
    // Initialize I2C0
    i2c_init(MCP_I2C, MCP23017_I2C_FREQ);
    gpio_set_function(MCP23017_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MCP23017_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(MCP23017_SDA_PIN);
    gpio_pull_up(MCP23017_SCL_PIN);

    // Configure all 16 pins as inputs
    mcp_write_reg(REG_IODIRA, 0xFF);
    mcp_write_reg(REG_IODIRB, 0xFF);

    // Enable internal pull-ups on all pins
    mcp_write_reg(REG_GPPUA, 0xFF);
    mcp_write_reg(REG_GPPUB, 0xFF);

    // Enable input polarity inversion (buttons pull low → read as 1)
    mcp_write_reg(REG_IPOLA, 0xFF);
    mcp_write_reg(REG_IPOLB, 0xFF);

    // Zero internal state
    memset(&buttons, 0, sizeof(buttons));
}

// ─── Polling ───────────────────────────────────────────────────

void mcp23017_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Read both ports (already inverted via IPOL: pressed = 1)
    uint8_t port_a = mcp_read_reg(REG_GPIOA);
    uint8_t port_b = mcp_read_reg(REG_GPIOB);
    uint16_t raw = ((uint16_t)port_b << 8) | port_a;

    // Debounce: only update if enough time has passed
    if (now - buttons._debounce_ms >= MCP_DEBOUNCE_MS) {
        buttons._debounce_ms = now;
        buttons._raw = raw;
    }

    // Edge detection
    uint16_t just_pressed = buttons._raw & ~buttons._prev;
    uint16_t just_released = ~buttons._raw & buttons._prev;

    buttons.pressed |= just_pressed;
    buttons.released |= just_released;
    buttons.held = buttons._raw;

    // Per-button press timing for long press
    for (int i = 0; i < NUM_STEP_BUTTONS; i++) {
        uint16_t mask = 1u << i;

        // Record press timestamp
        if (just_pressed & mask) {
            buttons._down_time[i] = now;
            buttons._long_fired &= ~mask;  // Reset long-press flag
        }

        // Check for long press
        if ((buttons._raw & mask) &&
            !(buttons._long_fired & mask) &&
            (now - buttons._down_time[i] >= MCP_LONG_PRESS_MS)) {
            buttons.long_pressed |= mask;
            buttons._long_fired |= mask;
        }

        // Clear long-fired on release
        if (just_released & mask) {
            buttons._long_fired &= ~mask;
        }
    }

    buttons._prev = buttons._raw;
}

// ─── Accessors ─────────────────────────────────────────────────

step_buttons_t *mcp23017_get_buttons(void) {
    return &buttons;
}

uint16_t mcp23017_read_raw(void) {
    uint8_t port_a = mcp_read_reg(REG_GPIOA);
    uint8_t port_b = mcp_read_reg(REG_GPIOB);
    return ((uint16_t)port_b << 8) | port_a;
}
