#ifndef STEP_LEDS_H
#define STEP_LEDS_H

#include <stdint.h>
#include <stdbool.h>

// LED output expander config (separate MCP23017 from button scanner).
// Keep this on a different address than MCP23017_ADDR (0x20).
#define STEP_LEDS_ENABLED    1
#define STEP_LEDS_ADDR       0x21

// Initialize LED output driver.
void step_leds_init(void);

// Set a 16-bit LED mask. Bit 0 = step/button 1, bit 15 = step/button 16.
// A mask the chip already holds costs no bus traffic. Any I2C error marks the
// chip unconfigured: the port direction is written again (after a failed
// attempt, at most every 100 ms), then the mask. About once a second the
// direction is read back to notice a chip that reset.
void step_leds_set_mask(uint16_t mask);

// Turn all step LEDs off.
void step_leds_clear(void);

// Driver availability (false if disabled at compile-time).
bool step_leds_available(void);

#endif // STEP_LEDS_H
