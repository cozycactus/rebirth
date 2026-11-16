#ifndef TCA8418_H
#define TCA8418_H

#include <stdbool.h>
#include <stdint.h>

#define TCA8418_ADDR 0x34

// tca8418_read_events results other than a count.
#define TCA8418_READ_FAILED    (-1)  // the count read failed: nothing consumed, retry
#define TCA8418_READ_UNCERTAIN (-2)  // a later read failed: events may be lost

// Configures rows R0-R7 and columns C0-C9 as keypad, chip debounce off,
// CFG = 0xA9 (AI, overflow mode, overflow and key-event interrupt enables).
// Register writes only: the event FIFO is left alone.
bool tca8418_init(void);

// Reads CFG back. False on an I2C error; otherwise *configured tells whether
// it still holds the value tca8418_init wrote (a chip that reset reads 0x00).
bool tca8418_read_config(bool *configured);

// Reads up to max queued key events (bit 7 press, bits 6:0 key 1..80) and
// returns how many, or TCA8418_READ_FAILED / TCA8418_READ_UNCERTAIN.
// overflow is set when the chip reported a FIFO overflow.
int tca8418_read_events(uint8_t *events, int max, bool *overflow);

// Drains the FIFO and clears INT_STAT. True only once the FIFO was read empty
// and the interrupt flags are cleared.
bool tca8418_flush(void);

#endif
