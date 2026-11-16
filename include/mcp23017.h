#ifndef MCP23017_H
#define MCP23017_H

#include <stdint.h>
#include <stdbool.h>

// ─── Configuration ─────────────────────────────────────────────

#define MCP23017_SDA_PIN    8
#define MCP23017_SCL_PIN    9
#define MCP23017_ADDR       0x20    // A0=A1=A2 tied to GND
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
    uint16_t    long_pressed;       // Bits held > 500ms (fires once per press)

    // Internal
    uint16_t    _raw;               // Current debounced state
    uint16_t    _prev;              // Previous debounced state
    uint32_t    _debounce_ms;       // Last debounce timestamp
    uint32_t    _down_time[NUM_STEP_BUTTONS];   // Per-button press timestamp
    uint16_t    _long_fired;        // Per-button long press already emitted
} step_buttons_t;

// ─── API ───────────────────────────────────────────────────────

// Initialize I2C0, configure MCP23017 ports as inputs with pull-ups
void mcp23017_init(void);

// Poll button state — call from ui_update(). Any I2C error marks the chip
// unconfigured; the configuration is written again (after a failed attempt,
// at most every 100 ms) before the ports are decoded. IPOL is read back about
// once a second, and before a sample with three or more keys going down at
// once is accepted, to notice a chip that reset (its idle lines read as
// pressed); such a chip is reconfigured and its ports become the new baseline,
// with no events.
void mcp23017_poll(void);

// After a flash stall or a panic: the ports read now (or at the next good
// read) become the held state. Nothing that changed meanwhile produces a
// press, release or long press, and keys held now never long-press.
void mcp23017_resync(void);

// Get pointer to button state struct
// Caller should process pressed/released/long_pressed then clear them
step_buttons_t *mcp23017_get_buttons(void);

// Read raw 16-bit GPIO state (for diagnostics / LED feedback)
uint16_t mcp23017_read_raw(void);

#endif // MCP23017_H
