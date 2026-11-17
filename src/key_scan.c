#include <stddef.h>
#include "hardware/i2c.h"
#include "key_scan.h"

#define SCAN_I2C i2c0
#define SCAN_TIMEOUT_US 2000

// MCP23017 registers (BANK = 0, sequential): port A, then port B.
#define REG_IODIRA 0x00
#define REG_IPOLA 0x02
#define REG_GPPUA 0x0C
#define REG_GPIOA 0x12
#define REG_OLATA 0x14

// U801: ROW0-6 on GPA0-6 and ROW7 on GPB0 are inputs with pull-ups (IODIR and
// GPPU bit 1); COL7 on GPA7 and COL0-6 on GPB1-7 are outputs. Bit 7 of IODIR
// stays 0 on every port: GPA7/GPB7 are output-only on current silicon.
#define ROWS_A 0x7F
#define ROWS_B 0x01
#define ROW_IPOL 0x00       // the firmware inverts: a set bit would read every key down
// U802: COL8 on GPA0, COL9 on GPA1; the open pins are outputs too.
#define COL_IODIR 0x00
#define RELEASED 0xFF       // an output latch with no column low

#define U801_COLS 8         // COL0-7 on U801, COL8-9 on U802
#define SELECTION_UNKNOWN (-1)

// The column that may be low, or unknown after init or a failed transfer.
// Nothing else resets it (a resync included), so the next step always
// releases a column a failed write may have left low.
static int selected = SELECTION_UNKNOWN;

// One transfer: a register, then its value and its neighbour's.
static bool write_pair(uint8_t addr, uint8_t reg, uint8_t a, uint8_t b) {
    uint8_t buf[3] = { reg, a, b };
    return i2c_write_timeout_us(SCAN_I2C, addr, buf, 3, false, SCAN_TIMEOUT_US) == 3;
}

static bool write_reg(uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_write_timeout_us(SCAN_I2C, addr, buf, 2, false, SCAN_TIMEOUT_US) == 2;
}

// Pointer write, repeated start, then len registers from reg.
static bool read_regs(uint8_t addr, uint8_t reg, uint8_t *val, size_t len) {
    if (i2c_write_timeout_us(SCAN_I2C, addr, &reg, 1, true, SCAN_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(SCAN_I2C, addr, val, len, false, SCAN_TIMEOUT_US) == (int)len;
}

bool key_scan_init(void) {
    selected = SELECTION_UNKNOWN;
    // IODIR is each chip's canary, so it goes first: a reset after it reads
    // back 0xFF there whatever was written later, and a reset before it is
    // repaired by the writes that follow.
    return write_pair(KEY_SCAN_ROW_ADDR, REG_IODIRA, ROWS_A, ROWS_B)
        && write_pair(KEY_SCAN_ROW_ADDR, REG_IPOLA, ROW_IPOL, ROW_IPOL)
        && write_pair(KEY_SCAN_ROW_ADDR, REG_GPPUA, ROWS_A, ROWS_B)
        && write_pair(KEY_SCAN_ROW_ADDR, REG_OLATA, RELEASED, RELEASED)
        && write_pair(KEY_SCAN_COL_ADDR, REG_IODIRA, COL_IODIR, COL_IODIR)
        && write_pair(KEY_SCAN_COL_ADDR, REG_OLATA, RELEASED, RELEASED);
}

bool key_scan_check(bool *intact) {
    uint8_t row[4];     // U801 IODIRA, IODIRB, IPOLA, IPOLB
    uint8_t col[2];     // U802 IODIRA, IODIRB
    if (!read_regs(KEY_SCAN_ROW_ADDR, REG_IODIRA, row, sizeof(row))) return false;
    if (!read_regs(KEY_SCAN_COL_ADDR, REG_IODIRA, col, sizeof(col))) return false;
    *intact = row[0] == ROWS_A && row[1] == ROWS_B
           && row[2] == ROW_IPOL && row[3] == ROW_IPOL
           && col[0] == COL_IODIR && col[1] == COL_IODIR;
    return true;
}

// Only the canaries a reset clears, read after the sample of col: U801's IODIR
// (the rows), and U802's too for COL8/9 (it drives them). A U802 reset only
// floats COL8/COL9, which cannot pull a row low while a U801 column is
// sampled. IPOL is not read: a reset leaves it at 0x00, the configured value,
// so only a stray write changes it, and key_scan_check, which starts every
// scan, still covers it. The selection stays.
bool key_scan_verify(uint8_t col, bool *intact) {
    if (col >= KEY_SCAN_COLS) return false;     // no transfer
    uint8_t row[2];     // U801 IODIRA, IODIRB
    if (!read_regs(KEY_SCAN_ROW_ADDR, REG_IODIRA, row, sizeof(row))) return false;
    bool held = row[0] == ROWS_A && row[1] == ROWS_B;
    if (col >= U801_COLS) {
        uint8_t cols[2];    // U802 IODIRA, IODIRB
        if (!read_regs(KEY_SCAN_COL_ADDR, REG_IODIRA, cols, sizeof(cols))) return false;
        held = held && cols[0] == COL_IODIR && cols[1] == COL_IODIR;
    }
    *intact = held;
    return true;
}

// U801's latch with only col (0-7) low: COL0-6 are GPB1-7, COL7 is GPA7.
// From a column in OLATB to COL7 both are low for one byte time; no row is
// read then.
static bool select_on_u801(uint8_t col) {
    uint8_t a = RELEASED;
    uint8_t b = RELEASED;
    if (col == 7) a = (uint8_t)~0x80u;
    else b = (uint8_t)~(1u << (col + 1));
    return write_pair(KEY_SCAN_ROW_ADDR, REG_OLATA, a, b);
}

// U802's OLATA with only col (8-9) low; writing it also releases the other.
static bool select_on_u802(uint8_t col) {
    return write_reg(KEY_SCAN_COL_ADDR, REG_OLATA, (uint8_t)~(1u << (col - U801_COLS)));
}

bool key_scan_column(uint8_t col, uint8_t *down) {
    if (col >= KEY_SCAN_COLS) return false;     // no transfer: the selection still holds
    bool u801_may_be_low = selected == SELECTION_UNKNOWN || selected < U801_COLS;
    bool u802_may_be_low = selected == SELECTION_UNKNOWN || selected >= U801_COLS;
    selected = SELECTION_UNKNOWN;   // until this step has succeeded

    // Deselect first, then select: at most one column is low at the read.
    bool col_low;
    if (col < U801_COLS) {
        col_low = (!u802_may_be_low || write_reg(KEY_SCAN_COL_ADDR, REG_OLATA, RELEASED))
               && select_on_u801(col);
    } else {
        col_low = (!u801_may_be_low
                   || write_pair(KEY_SCAN_ROW_ADDR, REG_OLATA, RELEASED, RELEASED))
               && select_on_u802(col);
    }
    // A separate transfer on purpose: the pointer write and the read's address
    // byte give a released row time to rise through its pull-up.
    uint8_t port[2];    // GPIOA, GPIOB
    if (!col_low || !read_regs(KEY_SCAN_ROW_ADDR, REG_GPIOA, port, sizeof(port))) {
        return false;
    }
    // A closed key pulls its row low: ROW0-6 on GPA0-6, ROW7 on GPB0 (GPA7 is COL7).
    *down = (uint8_t)((~port[0] & ROWS_A) | ((~port[1] & ROWS_B) << 7));
    selected = col;
    return true;
}
