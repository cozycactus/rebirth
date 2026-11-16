#ifndef KEY_SCAN_H
#define KEY_SCAN_H

#include <stdbool.h>
#include <stdint.h>

// The 8 x 10 key matrix on two MCP23017 (I2C0, BANK = 0). Each key is
// row -> switch -> diode -> column: rows are inputs with pull-ups, columns
// push-pull outputs, the selected one low.
#define KEY_SCAN_ROW_ADDR 0x22   // U801: GPA0-6 = ROW0-6, GPB0 = ROW7, GPB1-7 = COL0-6, GPA7 = COL7
#define KEY_SCAN_COL_ADDR 0x23   // U802: GPA0 = COL8, GPA1 = COL9, other pins open
#define KEY_SCAN_ROWS 8
#define KEY_SCAN_COLS 10

// Configures both chips, IODIR (the canary) first on each: U801 IODIR 7F/01,
// IPOL 00/00, GPPU 7F/01, OLAT FF/FF, then U802 IODIR 00/00, OLAT FF/FF.
// Does not bring the bus up (mcp23017_init does). The column selection is
// unknown afterwards, whatever the result.
bool key_scan_init(void);

// Reads U801 IODIR and IPOL and U802 IODIR back. False on an I2C error;
// otherwise *intact tells whether all six bytes hold what key_scan_init
// wrote. A chip that reset after its canary was written reads not intact.
bool key_scan_check(bool *intact);

// Selects column col (0-9) and reads the rows: *down bit r = ROW r low. A
// column that may still be low is released first, so at most one column is
// low when the rows are read. False on an I2C error: *down is not written
// and the selection becomes unknown, so the next step releases both chips'
// columns as needed.
bool key_scan_column(uint8_t col, uint8_t *down);

#endif
