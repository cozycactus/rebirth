#ifndef MCP23017_H
#define MCP23017_H

#include <stdint.h>
#include <stdbool.h>

// ─── Configuration ─────────────────────────────────────────────

#define MCP23017_SDA_PIN    8
#define MCP23017_SCL_PIN    9
#define MCP23017_ADDR       0x20    // U701: steps 1-7 (GPA0-6), 9-15 (GPB0-6)
#define MCP23017_STEP8_16_ADDR 0x24 // U703: steps 8 (GPA0) and 16 (GPA1)
#define MCP23017_I2C_FREQ   400000  // 400 kHz (fast mode)

#define NUM_STEP_BUTTONS    16

// Timing
#define MCP_DEBOUNCE_MS     20      // Button debounce window
#define MCP_LONG_PRESS_MS   500     // Long press threshold

// ─── Types ─────────────────────────────────────────────────────

typedef struct {
    // Public — edge-detected events (consumed by caller, cleared each cycle)
    uint16_t    pressed;            // Bits that just became pressed
    uint16_t    released;           // Bits that just became released
    uint16_t    held;               // Bits currently held down
    uint16_t    long_pressed;       // Bits held >= 500 ms at a good sample (once per press)

    // Internal
    uint16_t    _raw;               // Current debounced state
    uint16_t    _prev;              // Previous debounced state
    uint32_t    _debounce_ms;       // Time of the last sample (or baseline)
    uint32_t    _down_time[NUM_STEP_BUTTONS];   // Per-button press timestamp
    uint16_t    _long_fired;        // Per-button long press already emitted
} step_buttons_t;

// ─── API ───────────────────────────────────────────────────────

// Initialize I2C0 and configure both step chips: the key pins as inputs with
// pull-ups, inverted by IPOL (pressed = 1); every other pin, GPA7/GPB7
// included (output-only), an output driving low. A chip that does not answer
// is configured by a later poll.
void mcp23017_init(void);

// Poll button state — call from ui_update() on every pass. The chips are read
// only when a sample is due (every MCP_DEBOUNCE_MS), or at once for a chip
// whose rebaseline is pending; between samples the state stays as the last
// one left it. A long press fires only in a pass that just took a good sample
// of the key's chip, the first at or after MCP_LONG_PRESS_MS (up to one sample
// late), so a step released before that sample never long-presses. Each
// chip's ports count only if IPOL, read back after them, still holds. A chip
// that reset (IPOL lost) is configured again and its next good read becomes
// its baseline, with no events. A chip
// with an I2C error or a reset keeps its keys as they were, with no long
// press, until its next good sample; its configuration is written again
// before its next read, and an attempt after a failed one (a configuration
// not yet proved by a good sample counts) waits 100 ms. The other chip
// carries on, so a missing U703 costs only steps 8 and 16.
void mcp23017_poll(void);

// After a flash stall or a panic: the ports read now (or at each chip's next
// good read) become the held state. Nothing that changed meanwhile produces a
// press, release or long press, and keys held now never long-press.
void mcp23017_resync(void);

// Get pointer to button state struct
// Caller should process pressed/released/long_pressed then clear them
step_buttons_t *mcp23017_get_buttons(void);

// Read both chips now (diagnostics). A chip that is unconfigured, fails or
// has reset gives its bits from the last accepted state.
uint16_t mcp23017_read_raw(void);

#endif // MCP23017_H
