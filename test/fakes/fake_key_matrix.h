#ifndef REBIRTH_FAKE_KEY_MATRIX_H
#define REBIRTH_FAKE_KEY_MATRIX_H
#include <stdbool.h>
#include <stdint.h>

// Two MCP23017 (BANK 0, sequential register pointer) and the 8 x 10 key
// matrix of the PR #10 board: U801 at 0x22 (GPA0-6 = ROW0-6, GPB0 = ROW7,
// GPB1-7 = COL0-6, GPA7 = COL7) and U802 at 0x23 (GPA0 = COL8, GPA1 = COL9).
// The pin map is the fake's own copy, not the driver's constants.
//
// This file defines fake_i2c0_inst, i2c_init, i2c_write_timeout_us and
// i2c_read_timeout_us; a test linking it defines none of them. i2c_init
// fails the test: the scanner never brings the bus up (mcp23017_init does).
//
// Every read is derived from what was written: a column is low only when its
// pin is an output with OLAT 0. A row reads low when a closed key sits in a
// low column; otherwise high with its pull-up, otherwise (a chip that reset)
// at the floating level. IPOL inverts input pins; an output pin reads its
// OLAT. A write to GPIO sets OLAT.
//
// Every transfer is checked, and a violation prints file:line and exits 1:
// i2c0, address 0x22 or 0x23, timeout 2000 us; a 1-byte pointer write has
// nostop set and is followed by the read of the same chip; a read and every
// register write have nostop clear; no IODIR write sets bit 7; IOCON is never
// written; at most one of the ten columns is low whenever U801's GPIO
// registers are read.
//
// A failed transfer returns PICO_ERROR_TIMEOUT. Its wire bytes (and clock
// time) are the address byte plus what it moved: a failed write, the register
// byte and its landed data bytes when fake_matrix_fail_bytes is not 0; a
// failed read, nothing. The bytes a failed or short read did not fill are
// 0x00 (every row low).

void fake_matrix_reset(void);                  // both chips at power-on values, no key closed, no fault, counters 0
void fake_matrix_chip_reset(uint8_t addr);     // brown-out of one chip: IODIR = FF/FF, every other register 0
extern bool fake_matrix_key[8][10];            // closed keys [row][col]
extern bool fake_matrix_float_low;             // a floating pin reads low (default true: worst case)
extern int fake_matrix_txn;                    // transfers so far (failed ones included), 1-based numbering
extern int fake_matrix_fail_txn;               // this transfer fails once (0 = none)
extern int fake_matrix_fail_from;              // this transfer and all later ones fail (0 = none)
extern bool fake_matrix_fail_reads;            // every read fails; writes are unaffected
extern int fake_matrix_fail_bytes;             // a failing write first applies this many data bytes (0 = none, -1 = all)
extern int fake_matrix_short_txn;              // this transfer returns one byte less than asked, data applied/filled up to there
extern int fake_matrix_reset_txn;              // before this transfer, fake_matrix_chip_reset(fake_matrix_reset_addr)
extern uint8_t fake_matrix_reset_addr;
extern int fake_matrix_reset_in_read_txn;      // this read: chip resets after its first data byte, the rest reads 0xFF,
                                               // full count (a write with this number is unaffected)
extern int fake_matrix_faults;                 // faults applied so far: a failed or short transfer, a reset
                                               // before a transfer or inside a read
extern unsigned fake_matrix_wire_bytes;        // bytes on the wire, address bytes included
extern bool fake_matrix_clock;                 // each transfer advances timer_hw->timerawl by its wire time (22.5 us/byte)
extern int fake_matrix_low_column;             // column (0-9) low at the last U801 row read, -1 none
uint8_t fake_matrix_reg(uint8_t addr, uint8_t reg);   // a register as the chip would return it, no side effects

#endif
