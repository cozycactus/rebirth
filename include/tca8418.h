#ifndef TCA8418_H
#define TCA8418_H

#include <stdbool.h>
#include <stdint.h>

#define TCA8418_ADDR 0x34

// Configures rows R0-R7 and columns C0-C9 as keypad, chip debounce off,
// CFG = 0xA9 (AI, overflow mode, overflow and key-event interrupt enables).
bool tca8418_init(void);

// Reads up to max queued key events (bit 7 press, bits 6:0 key 1..80).
// Returns the number read, or -1 on an I2C error before any event was read.
// overflow is set when the chip reported a FIFO overflow.
int tca8418_read_events(uint8_t *events, int max, bool *overflow);

// Drains the FIFO and clears INT_STAT. True only once the FIFO was read empty
// and the interrupt flags are cleared.
bool tca8418_flush(void);

#endif
