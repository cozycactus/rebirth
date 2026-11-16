#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include <string.h>
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

#define MCP_I2C_TIMEOUT_US 2000

// Every transfer is bounded and must move exactly the requested bytes.
static bool mcp_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_write_timeout_us(MCP_I2C, MCP23017_ADDR, buf, 2, false,
                                MCP_I2C_TIMEOUT_US) == 2;
}

// Reads a register pair in one transfer (address pointer auto-increments).
static bool mcp_read_pair(uint8_t reg, uint16_t *value) {
    uint8_t val[2];
    if (i2c_write_timeout_us(MCP_I2C, MCP23017_ADDR, &reg, 1, true,
                             MCP_I2C_TIMEOUT_US) != 1) return false;
    if (i2c_read_timeout_us(MCP_I2C, MCP23017_ADDR, val, 2, false,
                            MCP_I2C_TIMEOUT_US) != 2) return false;
    *value = ((uint16_t)val[1] << 8) | val[0];
    return true;
}

// Reads GPIOA and GPIOB in one transfer.
static bool mcp_read_ports(uint16_t *raw) {
    return mcp_read_pair(REG_GPIOA, raw);
}

// ─── Initialization ────────────────────────────────────────────

#define MCP_RETRY_MS    100     // between failed configuration attempts
#define MCP_CHECK_MS    1000    // IPOL readback, to notice a chip that reset
#define MCP_RESET_SUSPECT_KEYS 3  // keys going down in one sample that trigger a readback

static bool configured;         // config accepted, and no I2C error since
static bool retry_armed;        // the last configuration attempt failed
static uint32_t retry_from_ms;
static uint32_t check_ms;       // last IPOL readback
static bool rebaseline;         // the next good read is the state, with no events

static bool mcp_configure(void) {
    return mcp_write_reg(REG_IODIRA, 0xFF) && mcp_write_reg(REG_IODIRB, 0xFF)   // inputs
        && mcp_write_reg(REG_GPPUA, 0xFF) && mcp_write_reg(REG_GPPUB, 0xFF)     // pull-ups
        && mcp_write_reg(REG_IPOLA, 0xFF) && mcp_write_reg(REG_IPOLB, 0xFF);    // pressed = 1
}

// Writes the configuration again when it may be lost. An attempt after a
// failed one waits MCP_RETRY_MS, so a dead bus costs one timeout per 100 ms.
static bool ensure_configured(uint32_t now) {
    if (configured) return true;
    if (retry_armed && (uint32_t)(now - retry_from_ms) < MCP_RETRY_MS) return false;
    configured = mcp_configure();
    retry_armed = !configured;
    retry_from_ms = now;
    return configured;
}

// A chip that reset (brown-out, ESD) answers with IPOL and the pull-ups back
// at their defaults. Reads IPOL back: on a mismatch the chip is reconfigured
// and the ports that follow become the new state, with no events; an I2C
// error rewrites the configuration before the next read. True only when the
// configuration is intact.
static bool config_intact(void) {
    uint16_t ipol;
    if (!mcp_read_pair(REG_IPOLA, &ipol)) {
        configured = false;
        return false;
    }
    if (ipol != 0xFFFF) {
        configured = false;
        rebaseline = true;
        return false;
    }
    return true;
}

// About once a second, to notice a reset chip whatever its ports show.
static void check_config(uint32_t now) {
    if (!configured || (uint32_t)(now - check_ms) < MCP_CHECK_MS) return;
    check_ms = now;
    config_intact();
}

// The ports as the held state, with no press, release or long press from the
// change; keys held now never long-press.
static void set_baseline(uint16_t raw, uint32_t now) {
    buttons._raw = buttons._prev = buttons.held = raw;
    buttons._long_fired = raw;
    buttons._debounce_ms = now;
    rebaseline = false;
}

void mcp23017_init(void) {
    // Initialize I2C0
    i2c_init(MCP_I2C, MCP23017_I2C_FREQ);
    gpio_set_function(MCP23017_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MCP23017_SCL_PIN, GPIO_FUNC_I2C);
    // The bus has external pull-ups; the weak internal ones stay off.
    gpio_disable_pulls(MCP23017_SDA_PIN);
    gpio_disable_pulls(MCP23017_SCL_PIN);

    // Zero internal state
    memset(&buttons, 0, sizeof(buttons));
    rebaseline = false;

    // If the chip does not answer now, poll retries the configuration.
    uint32_t now = to_ms_since_boot(get_absolute_time());
    check_ms = now;
    configured = mcp_configure();
    retry_armed = !configured;
    retry_from_ms = now;
}

void mcp23017_resync(void) {
    buttons.pressed = 0;
    buttons.released = 0;
    buttons.long_pressed = 0;
    rebaseline = true;
    mcp23017_poll();    // takes the baseline now if the chip answers
}

// ─── Polling ───────────────────────────────────────────────────

void mcp23017_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Never decode the ports before the chip is configured (IPOL, pull-ups).
    check_config(now);
    if (!ensure_configured(now)) return;

    // Read both ports (already inverted via IPOL: pressed = 1). A failed
    // read keeps the last accepted state: no phantom releases or presses. The
    // chip may have reset, so the configuration is written again first.
    uint16_t raw;
    if (!mcp_read_ports(&raw)) {
        configured = false;
        return;
    }
    if (rebaseline) {
        set_baseline(raw, now);
        return;
    }

    // Debounce: only update if enough time has passed
    if (now - buttons._debounce_ms >= MCP_DEBOUNCE_MS) {
        // Idle lines of a chip that reset read as pressed: several keys going
        // down in one sample are accepted only once the configuration reads
        // back intact.
        uint16_t going_down = raw & (uint16_t)~buttons._raw;
        if (__builtin_popcount(going_down) >= MCP_RESET_SUSPECT_KEYS && !config_intact()) {
            return;
        }
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
    uint16_t raw;
    return mcp_read_ports(&raw) ? raw : buttons._raw;
}
