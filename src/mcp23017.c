#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include <string.h>
#include "mcp23017.h"

// ─── I2C instance ──────────────────────────────────────────────
#define MCP_I2C     i2c0

// ─── MCP23017 Register Addresses (BANK=0, sequential) ──────────
// Each B register follows its A register; a pair is one transfer.
#define REG_IODIRA      0x00    // I/O direction port A (1=input)
#define REG_IPOLA       0x02    // Input polarity port A
#define REG_GPPUA       0x0C    // Pull-up resistors port A
#define REG_GPIOA       0x12    // GPIO port A
#define REG_OLATA       0x14    // Output latch port A

// ─── State ─────────────────────────────────────────────────────

static step_buttons_t buttons;

// Two chips make the step word (bit n = step n + 1), each with its own health,
// so a missing or failing one costs only its own keys.
typedef struct {
    uint8_t addr;
    uint8_t keys_a, keys_b;     // key pins: inputs, inverted, pulled up
    uint16_t steps;             // the step bits this chip supplies
    bool configured;            // config accepted, and no I2C error since
    bool retry_armed;           // the last configuration attempt failed
    uint32_t retry_from_ms;
    bool rebaseline;            // the next good read is the state, with no events
    bool trusted;               // the last sample was good: the configuration holds
} mcp_chip_t;

#define MCP_CHIPS 2
static mcp_chip_t chips[MCP_CHIPS] = {
    { .addr = MCP23017_ADDR, .keys_a = 0x7F, .keys_b = 0x7F, .steps = 0x7F7F },
    { .addr = MCP23017_STEP8_16_ADDR, .keys_a = 0x03, .keys_b = 0x00, .steps = 0x8080 },
};

// The step bits in a chip's GPIOA/GPIOB (the board's pin map). U701 GPA7/GPB7
// are outputs and never count.
static uint16_t step_bits(const mcp_chip_t *c, const uint8_t gpio[2]) {
    if (c->addr == MCP23017_ADDR) {
        return (uint16_t)((gpio[0] & 0x7F) | (gpio[1] & 0x7F) << 8);
    }
    return (uint16_t)((gpio[0] & 0x01) << 7 | (gpio[0] & 0x02) << 14);
}

// ─── I2C Helpers ───────────────────────────────────────────────

#define MCP_I2C_TIMEOUT_US 2000

// Every transfer is bounded and must move exactly the requested bytes.
// Writes a register and its neighbour in one transfer.
static bool write_pair(uint8_t addr, uint8_t reg, uint8_t a, uint8_t b) {
    uint8_t buf[3] = { reg, a, b };
    return i2c_write_timeout_us(MCP_I2C, addr, buf, 3, false,
                                MCP_I2C_TIMEOUT_US) == 3;
}

// Reads a register pair in one transfer (address pointer auto-increments).
static bool read_pair(uint8_t addr, uint8_t reg, uint8_t val[2]) {
    if (i2c_write_timeout_us(MCP_I2C, addr, &reg, 1, true,
                             MCP_I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(MCP_I2C, addr, val, 2, false,
                               MCP_I2C_TIMEOUT_US) == 2;
}

// ─── Initialization ────────────────────────────────────────────

#define MCP_RETRY_MS    100     // between failed configuration attempts

// IPOL is the canary: it differs from its power-on value, is written first
// and never again outside this sequence, so a reset at any moment after it
// clears it, whatever was written later; a reset before it is repaired by
// the rest. OLAT goes before IODIR so the pins that become outputs (GPA7/GPB7
// and the unused ones) drive low from the start.
static bool configure(const mcp_chip_t *c) {
    return write_pair(c->addr, REG_IPOLA, c->keys_a, c->keys_b)      // pressed = 1
        && write_pair(c->addr, REG_OLATA, 0x00, 0x00)
        && write_pair(c->addr, REG_IODIRA, c->keys_a, c->keys_b)     // keys in, the rest out
        && write_pair(c->addr, REG_GPPUA, c->keys_a, c->keys_b);     // pull-ups on the keys
}

// Writes the configuration again when it may be lost. An attempt after a
// failed one waits MCP_RETRY_MS, so a dead bus or a missing chip costs one
// transfer per 100 ms.
static bool ensure_configured(mcp_chip_t *c, uint32_t now) {
    if (c->configured) return true;
    if (c->retry_armed && (uint32_t)(now - c->retry_from_ms) < MCP_RETRY_MS) return false;
    c->configured = configure(c);
    c->retry_armed = !c->configured;
    c->retry_from_ms = now;
    return c->configured;
}

typedef enum { READ_OK, READ_FAILED, READ_RESET } read_result_t;

// Reads GPIOA/GPIOB, then IPOL back: the ports count only if the canary still
// holds after them, so a reset before or during the port read is caught (the
// IPOL read then finds the power-on 0). A reset during the IPOL read ends it
// in 0xFF bytes, which the configured IPOL never holds.
static read_result_t read_chip(const mcp_chip_t *c, uint16_t *steps) {
    uint8_t gpio[2], ipol[2];
    if (!read_pair(c->addr, REG_GPIOA, gpio) || !read_pair(c->addr, REG_IPOLA, ipol)) {
        return READ_FAILED;
    }
    if (ipol[0] != c->keys_a || ipol[1] != c->keys_b) return READ_RESET;
    *steps = step_bits(c, gpio);
    return READ_OK;
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

    // A chip that does not answer now is configured by a later poll.
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (int i = 0; i < MCP_CHIPS; i++) {
        mcp_chip_t *c = &chips[i];
        c->configured = c->retry_armed = c->rebaseline = c->trusted = false;
        ensure_configured(c, now);
    }
}

void mcp23017_resync(void) {
    buttons.pressed = 0;
    buttons.released = 0;
    buttons.long_pressed = 0;
    for (int i = 0; i < MCP_CHIPS; i++) chips[i].rebaseline = true;
    mcp23017_poll();    // takes the baselines now if the chips answer
}

// ─── Polling ───────────────────────────────────────────────────

typedef enum { SAMPLE_NONE, SAMPLE_GOOD, SAMPLE_BASELINE } sample_result_t;

// One sample of one chip. A failed transfer, or a canary that no longer
// holds, keeps the chip's keys as they were until its next good sample, and
// its configuration is written again before its next read. SAMPLE_NONE when
// there was no good sample; SAMPLE_BASELINE when the good one became the
// chip's baseline.
static sample_result_t sample_chip(mcp_chip_t *c, uint32_t now) {
    if (!ensure_configured(c, now)) return SAMPLE_NONE;
    uint16_t steps;
    read_result_t result = read_chip(c, &steps);
    if (result != READ_OK) {
        // Until a good sample proves the configuration, a failure counts as a
        // failed attempt: a chip that takes its configuration but cannot be
        // read, or keeps losing it, is retried every MCP_RETRY_MS, not on
        // every pass while its rebaseline is pending.
        if (!c->trusted) {
            c->retry_armed = true;
            c->retry_from_ms = now;
        }
        c->configured = false;
        c->trusted = false;
        if (result == READ_RESET) c->rebaseline = true;
        return SAMPLE_NONE;
    }
    c->trusted = true;
    buttons._raw = (uint16_t)((buttons._raw & ~c->steps) | steps);
    if (!c->rebaseline) return SAMPLE_GOOD;
    // The keys as held, with no press, release or long press from the change;
    // keys held now never long-press.
    buttons._prev = (uint16_t)((buttons._prev & ~c->steps) | steps);
    buttons._long_fired = (uint16_t)((buttons._long_fired & ~c->steps) | steps);
    c->rebaseline = false;
    return SAMPLE_BASELINE;
}

void mcp23017_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // The chips are read only when a sample is due, or at once for a chip
    // whose rebaseline is pending. A due sample is taken whether or not the
    // reads succeed, so a failing chip never makes the other read more often;
    // a baseline restarts the sample clock.
    bool due = (uint32_t)(now - buttons._debounce_ms) >= MCP_DEBOUNCE_MS;
    bool baseline = false;
    uint16_t sampled = 0;       // the steps of the chips read good in this pass
    for (int i = 0; i < MCP_CHIPS; i++) {
        mcp_chip_t *c = &chips[i];
        if (!due && !c->rebaseline) continue;
        sample_result_t result = sample_chip(c, now);
        if (result != SAMPLE_NONE) sampled |= c->steps;
        if (result == SAMPLE_BASELINE) baseline = true;
    }
    if (due || baseline) buttons._debounce_ms = now;

    // Edge detection
    uint16_t just_pressed = buttons._raw & ~buttons._prev;
    uint16_t just_released = ~buttons._raw & buttons._prev;

    buttons.pressed |= just_pressed;
    buttons.released |= just_released;
    buttons.held = buttons._raw;

    // A long press fires only in a pass that just read the key's chip good.
    // Between samples the last one may be up to MCP_DEBOUNCE_MS old, and the
    // sample grid drifts against the press time with the pass length, so a
    // timer running on it could long-press a key released since. The keys of
    // a chip whose sample failed keep their last accepted state, maybe stale:
    // no long press from them either. A long press is up to one sample late.

    // Per-button press timing for long press
    for (int i = 0; i < NUM_STEP_BUTTONS; i++) {
        uint16_t mask = 1u << i;

        // Record press timestamp
        if (just_pressed & mask) {
            buttons._down_time[i] = now;
            buttons._long_fired &= ~mask;  // Reset long-press flag
        }

        // Check for long press
        if ((buttons._raw & sampled & mask) &&
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
    uint16_t word = buttons._raw;
    for (int i = 0; i < MCP_CHIPS; i++) {
        const mcp_chip_t *c = &chips[i];
        uint16_t steps;
        if (c->configured && read_chip(c, &steps) == READ_OK) {
            word = (uint16_t)((word & ~c->steps) | steps);
        }
    }
    return word;
}
