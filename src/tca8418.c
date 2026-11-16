#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "tca8418.h"

#define TCA_I2C i2c0
#define TCA_TIMEOUT_US 2000

#define REG_CFG 0x01
#define REG_INT_STAT 0x02
#define REG_KEY_LCK_EC 0x03
#define REG_KEY_EVENT_A 0x04
#define REG_KP_GPIO1 0x1D
#define REG_DEBOUNCE_DIS1 0x29

#define CFG_AI 0x80
#define CFG_OVR_FLOW_M 0x20
#define CFG_OVR_FLOW_IEN 0x08
#define CFG_KE_IEN 0x01
#define INT_OVR_FLOW 0x08
#define INT_ALL 0xFF
#define EC_COUNT_MASK 0x0F

#define FLUSH_MAX_PASSES 8

static bool write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_write_timeout_us(TCA_I2C, TCA8418_ADDR, buf, 2, false, TCA_TIMEOUT_US) == 2;
}

static bool read_reg(uint8_t reg, uint8_t *value) {
    if (i2c_write_timeout_us(TCA_I2C, TCA8418_ADDR, &reg, 1, true, TCA_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(TCA_I2C, TCA8418_ADDR, value, 1, false, TCA_TIMEOUT_US) == 1;
}

#define CFG_VALUE (CFG_AI | CFG_OVR_FLOW_M | CFG_OVR_FLOW_IEN | CFG_KE_IEN)

bool tca8418_init(void) {
    static const uint8_t keypad_mask[3] = { 0xFF, 0xFF, 0x03 };
    for (uint8_t i = 0; i < 3; i++) {
        if (!write_reg(REG_KP_GPIO1 + i, keypad_mask[i])) return false;
        if (!write_reg(REG_DEBOUNCE_DIS1 + i, keypad_mask[i])) return false;
    }
    return write_reg(REG_CFG, CFG_VALUE);
}

bool tca8418_read_config(bool *configured) {
    uint8_t cfg;
    if (!read_reg(REG_CFG, &cfg)) return false;
    *configured = cfg == CFG_VALUE;
    return true;
}

int tca8418_read_events(uint8_t *events, int max, bool *overflow) {
    uint8_t count;
    uint8_t status;
    if (overflow) *overflow = false;
    if (!read_reg(REG_KEY_LCK_EC, &count)) return TCA8418_READ_FAILED;
    count &= EC_COUNT_MASK;

    // From here on a failed read may already have popped an event, and a
    // failed INT_STAT read hides a possible overflow: the batch is uncertain.
    int n = 0;
    while (n < count && n < max) {
        uint8_t event;
        if (!read_reg(REG_KEY_EVENT_A, &event)) return TCA8418_READ_UNCERTAIN;
        if (event == 0) break;
        events[n++] = event;
    }
    // The flags are read last and never cleared here: tca8418_flush clears them
    // once the caller has acted, so a failed read cannot lose an overflow.
    if (!read_reg(REG_INT_STAT, &status)) return TCA8418_READ_UNCERTAIN;
    if (overflow) *overflow = (status & INT_OVR_FLOW) != 0;
    return n;
}

bool tca8418_flush(void) {
    for (int pass = 0; pass < FLUSH_MAX_PASSES; pass++) {
        uint8_t count;
        if (!write_reg(REG_INT_STAT, INT_ALL)) return false;
        if (!read_reg(REG_KEY_LCK_EC, &count)) return false;
        count &= EC_COUNT_MASK;
        if (count == 0) return true;
        for (uint8_t i = 0; i < count; i++) {
            uint8_t event;
            if (!read_reg(REG_KEY_EVENT_A, &event)) return false;
        }
    }
    return false;
}
