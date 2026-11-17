#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hardware/i2c.h"
#include "hardware/timer.h"
#include "fake_key_matrix.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

// MCP23017 registers, BANK 0: port A at an even address, port B next to it.
#define REG_IODIRA 0x00
#define REG_IODIRB 0x01
#define REG_IPOLA 0x02
#define REG_IOCONA 0x0A
#define REG_IOCONB 0x0B
#define REG_GPPUA 0x0C
#define REG_GPIOA 0x12
#define REG_GPIOB 0x13
#define REG_OLATA 0x14
#define REG_COUNT 0x16

#define ROW_CHIP 0          // U801, 0x22
#define COL_CHIP 1          // U802, 0x23
#define ROWS 8
#define COLS 10
#define TIMEOUT_US 2000
#define HALF_US_PER_BYTE 45 // 22.5 us per byte with its ACK at 400 kHz

// A pin of one chip: 0-7 = GPA0-7, 8-15 = GPB0-7.
typedef struct { int chip; int pin; } pin_t;

// The PR #10 board.
static const pin_t row_pin[ROWS] = {
    { ROW_CHIP, 0 }, { ROW_CHIP, 1 }, { ROW_CHIP, 2 }, { ROW_CHIP, 3 },
    { ROW_CHIP, 4 }, { ROW_CHIP, 5 }, { ROW_CHIP, 6 }, { ROW_CHIP, 8 },
};
static const pin_t col_pin[COLS] = {
    { ROW_CHIP, 9 }, { ROW_CHIP, 10 }, { ROW_CHIP, 11 }, { ROW_CHIP, 12 },
    { ROW_CHIP, 13 }, { ROW_CHIP, 14 }, { ROW_CHIP, 15 }, { ROW_CHIP, 7 },
    { COL_CHIP, 0 }, { COL_CHIP, 1 },
};

i2c_inst_t fake_i2c0_inst;

bool fake_matrix_key[ROWS][COLS];
bool fake_matrix_float_low = true;
int fake_matrix_txn;
int fake_matrix_fail_txn;
int fake_matrix_fail_from;
bool fake_matrix_fail_reads;
int fake_matrix_fail_bytes;
int fake_matrix_short_txn;
int fake_matrix_reset_txn;
uint8_t fake_matrix_reset_addr;
int fake_matrix_reset_in_read_txn;
int fake_matrix_faults;
unsigned fake_matrix_wire_bytes;
bool fake_matrix_clock;
int fake_matrix_low_column = -1;

static uint8_t regs[2][REG_COUNT] = { { 0xFF, 0xFF }, { 0xFF, 0xFF } };
static uint8_t pointer[2];
static int pointer_pending = -1;    // chip whose pointer write awaits its read, -1 none
static unsigned clock_half_us;      // wire time not yet added to the timer

static int chip_of(uint8_t addr) {
    CHECK(addr == 0x22 || addr == 0x23);
    return addr == 0x23 ? COL_CHIP : ROW_CHIP;
}

static void reset_chip(int chip) {
    memset(regs[chip], 0, REG_COUNT);
    regs[chip][REG_IODIRA] = regs[chip][REG_IODIRB] = 0xFF;
    pointer[chip] = 0;
}

void fake_matrix_chip_reset(uint8_t addr) {
    reset_chip(chip_of(addr));
}

void fake_matrix_reset(void) {
    reset_chip(ROW_CHIP);
    reset_chip(COL_CHIP);
    memset(fake_matrix_key, 0, sizeof(fake_matrix_key));
    fake_matrix_float_low = true;
    fake_matrix_txn = 0;
    fake_matrix_fail_txn = fake_matrix_fail_from = fake_matrix_fail_bytes = 0;
    fake_matrix_fail_reads = false;
    fake_matrix_short_txn = fake_matrix_reset_txn = fake_matrix_reset_in_read_txn = 0;
    fake_matrix_reset_addr = 0;
    fake_matrix_faults = 0;
    fake_matrix_wire_bytes = 0;
    fake_matrix_clock = false;
    fake_matrix_low_column = -1;
    pointer_pending = -1;
    clock_half_us = 0;
}

// ---- Pins ------------------------------------------------------------------

// A pin's bit in a port pair register (port A at reg, port B at reg + 1).
static bool reg_bit(pin_t p, uint8_t reg) {
    return (regs[p.chip][reg + p.pin / 8] >> (p.pin % 8)) & 1;
}

static bool is_output(pin_t p) { return !reg_bit(p, REG_IODIRA); }

static bool column_low(int col) {
    pin_t p = col_pin[col];
    return is_output(p) && !reg_bit(p, REG_OLATA);
}

static bool is_row(pin_t p, int row) {
    return row_pin[row].chip == p.chip && row_pin[row].pin == p.pin;
}

// Driven by its latch, pulled low through a closed key and its diode into a
// low column, pulled up, or floating.
static bool pin_level(pin_t p) {
    if (is_output(p)) return reg_bit(p, REG_OLATA);
    for (int r = 0; r < ROWS; r++) {
        if (!is_row(p, r)) continue;
        for (int c = 0; c < COLS; c++) {
            if (fake_matrix_key[r][c] && column_low(c)) return false;
        }
    }
    if (reg_bit(p, REG_GPPUA)) return true;
    return !fake_matrix_float_low;
}

static uint8_t port_value(int chip, int port) {
    uint8_t value = 0;
    for (int bit = 0; bit < 8; bit++) {
        pin_t p = { chip, port * 8 + bit };
        bool level = pin_level(p);
        if (!is_output(p) && reg_bit(p, REG_IPOLA)) level = !level;
        if (level) value |= (uint8_t)(1u << bit);
    }
    return value;
}

static uint8_t reg_value(int chip, uint8_t reg) {
    if (reg == REG_GPIOA || reg == REG_GPIOB) return port_value(chip, reg - REG_GPIOA);
    return regs[chip][reg];
}

uint8_t fake_matrix_reg(uint8_t addr, uint8_t reg) {
    CHECK(reg < REG_COUNT);
    return reg_value(chip_of(addr), reg);
}

// U801's rows are sampled: the driver may have at most one column low.
static void note_row_read(void) {
    int columns_low = 0;
    fake_matrix_low_column = -1;
    for (int c = 0; c < COLS; c++) {
        if (!column_low(c)) continue;
        columns_low++;
        fake_matrix_low_column = c;
    }
    CHECK(columns_low <= 1);
}

// ---- Transfers -------------------------------------------------------------

static uint8_t next_reg(uint8_t reg) { return (uint8_t)((reg + 1) % REG_COUNT); }

static bool failing(void) {
    return (fake_matrix_fail_txn != 0 && fake_matrix_txn == fake_matrix_fail_txn)
        || (fake_matrix_fail_from != 0 && fake_matrix_txn >= fake_matrix_fail_from);
}

static bool short_count(void) {
    return fake_matrix_short_txn != 0 && fake_matrix_txn == fake_matrix_short_txn;
}

// Counts the transfer, applies a reset due before it, checks the bus arguments.
static int begin(i2c_inst_t *i2c, uint8_t addr, size_t len, unsigned timeout_us) {
    fake_matrix_txn++;
    if (fake_matrix_reset_txn != 0 && fake_matrix_txn == fake_matrix_reset_txn) {
        fake_matrix_chip_reset(fake_matrix_reset_addr);
        fake_matrix_faults++;
    }
    CHECK(i2c == i2c0);
    CHECK(timeout_us == TIMEOUT_US);
    CHECK(len >= 1);
    return chip_of(addr);
}

// The address byte and the bytes moved go on the wire, and take their time.
static void end(size_t moved) {
    unsigned bytes = 1 + (unsigned)moved;
    fake_matrix_wire_bytes += bytes;
    if (!fake_matrix_clock) return;
    clock_half_us += bytes * HALF_US_PER_BYTE;
    timer_hw->timerawl += clock_half_us / 2;
    clock_half_us %= 2;
}

// What the driver asked for, whatever lands: IOCON stays at BANK 0, and
// GPA7/GPB7 (output-only on current silicon) are never made inputs.
static void check_write(const uint8_t *src, size_t len) {
    CHECK(src[0] < REG_COUNT);
    uint8_t reg = src[0];
    for (size_t i = 1; i < len; i++, reg = next_reg(reg)) {
        CHECK(reg != REG_IOCONA && reg != REG_IOCONB);
        if (reg == REG_IODIRA || reg == REG_IODIRB) CHECK((src[i] & 0x80) == 0);
    }
}

// Applies the first n bytes: the register pointer, then data.
static void apply_write(int chip, const uint8_t *src, size_t n) {
    if (n == 0) return;
    pointer[chip] = src[0];
    for (size_t i = 1; i < n; i++) {
        uint8_t reg = pointer[chip];
        if (reg == REG_GPIOA || reg == REG_GPIOB) reg += REG_OLATA - REG_GPIOA;
        regs[chip][reg] = src[i];
        pointer[chip] = next_reg(pointer[chip]);
    }
}

unsigned i2c_init(i2c_inst_t *i2c, unsigned baudrate) {
    (void)i2c; (void)baudrate;
    fprintf(stderr, "%s:%d: i2c_init called: the scanner never brings the bus up\n",
            __FILE__, __LINE__);
    exit(1);
}

int i2c_write_timeout_us(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len,
                         bool nostop, unsigned timeout_us) {
    int chip = begin(i2c, addr, len, timeout_us);
    CHECK(pointer_pending < 0);     // a pointer write is followed by its read
    CHECK(nostop == (len == 1));    // only a pointer write keeps the bus for its read
    check_write(src, len);

    size_t moved = len;
    int result = (int)len;
    if (failing()) {
        if (fake_matrix_fail_bytes == 0) moved = 0;
        else if (fake_matrix_fail_bytes > 0 && (size_t)fake_matrix_fail_bytes + 1 < len) {
            moved = (size_t)fake_matrix_fail_bytes + 1;
        }
        result = PICO_ERROR_TIMEOUT;
        fake_matrix_faults++;
    } else if (short_count()) {
        moved = len - 1;
        result = (int)moved;
        fake_matrix_faults++;
    }
    apply_write(chip, src, moved);
    if (len == 1 && result == 1) pointer_pending = chip;
    end(moved);
    return result;
}

int i2c_read_timeout_us(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len,
                        bool nostop, unsigned timeout_us) {
    int chip = begin(i2c, addr, len, timeout_us);
    CHECK(pointer_pending == chip); // the read follows its pointer write
    CHECK(!nostop);
    pointer_pending = -1;

    memset(dst, 0, len);
    if (failing() || fake_matrix_fail_reads) {
        fake_matrix_faults++;
        end(0);
        return PICO_ERROR_TIMEOUT;
    }
    size_t moved = short_count() ? len - 1 : len;
    bool reset_inside = fake_matrix_reset_in_read_txn != 0
                     && fake_matrix_txn == fake_matrix_reset_in_read_txn;
    if (moved < len || reset_inside) fake_matrix_faults++;
    for (size_t i = 0; i < moved; i++) {
        if (reset_inside && i > 0) {    // the interface restarted: the rest reads high
            dst[i] = 0xFF;
            continue;
        }
        uint8_t reg = pointer[chip];
        if (chip == ROW_CHIP && (reg == REG_GPIOA || reg == REG_GPIOB)) note_row_read();
        dst[i] = reg_value(chip, reg);
        pointer[chip] = next_reg(reg);
        if (reset_inside) reset_chip(chip);
    }
    end(moved);
    return (int)moved;
}
