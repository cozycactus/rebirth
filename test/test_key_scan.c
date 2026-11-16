#include <stdio.h>
#include <stdlib.h>
#include "fake_hardware.h"
#include "fake_key_matrix.h"
#include "hardware/i2c.h"
#include "hardware/timer.h"
#include "key_scan.h"
#include "sequencer.h"

sequencer_state_t g_seq;   // referenced by the shared fake hardware

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define RUN(name) do { name(); printf("PASS %s\n", #name); } while (0)

// The board as the test knows it (PR #10), not from the driver's constants.
#define U801 0x22   // ROW0-7, COL0-7
#define U802 0x23   // COL8, COL9
#define IODIRA 0x00
#define IODIRB 0x01
#define IPOLA 0x02
#define IPOLB 0x03
#define GPPUA 0x0C
#define GPPUB 0x0D
#define OLATA 0x14
#define OLATB 0x15

static void start(void) {
    fake_hardware_reset();
    fake_matrix_reset();
}

static void start_configured(void) {
    start();
    CHECK(key_scan_init());
}

// Sets one register by hand, as a stray write would.
static void poke(uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    CHECK(i2c_write_timeout_us(i2c0, addr, buf, 2, false, 2000) == 2);
}

static bool reg_pair_is(uint8_t addr, uint8_t reg, uint8_t a, uint8_t b) {
    return fake_matrix_reg(addr, reg) == a && fake_matrix_reg(addr, reg + 1) == b;
}

static bool check_intact(void) {
    bool intact = false;
    CHECK(key_scan_check(&intact));
    return intact;
}

// A column step with no fault: only col was low when the rows were read.
static uint8_t column(uint8_t col) {
    uint8_t down = 0;
    CHECK(key_scan_column(col, &down));
    CHECK(fake_matrix_low_column == col);
    return down;
}

// A column step costs exactly this many transfers and wire bytes.
static void check_step(uint8_t col, int transfers, unsigned wire_bytes) {
    int txn = fake_matrix_txn;
    unsigned bytes = fake_matrix_wire_bytes;
    column(col);
    CHECK(fake_matrix_txn - txn == transfers);
    CHECK(fake_matrix_wire_bytes - bytes == wire_bytes);
}

// ---- Configuration -------------------------------------------------------

// The init writes in order; was = the value before init (IPOL is poked first,
// so that its write can be seen).
static const struct { uint8_t addr, reg, a, b, was; } init_writes[6] = {
    { U801, IODIRA, 0x7F, 0x01, 0xFF },   // the canary first
    { U801, IPOLA, 0x00, 0x00, 0xFF },
    { U801, GPPUA, 0x7F, 0x01, 0x00 },
    { U801, OLATA, 0xFF, 0xFF, 0x00 },
    { U802, IODIRA, 0x00, 0x00, 0xFF },   // the canary first
    { U802, OLATA, 0xFF, 0xFF, 0x00 },
};

static void test_init_writes_configuration(void) {
    start_configured();
    CHECK(fake_matrix_txn == 6);
    CHECK(fake_matrix_wire_bytes == 24);    // six 3-byte writes and their address bytes
    CHECK(reg_pair_is(U801, IODIRA, 0x7F, 0x01));
    CHECK(reg_pair_is(U801, IPOLA, 0x00, 0x00));
    CHECK(reg_pair_is(U801, GPPUA, 0x7F, 0x01));
    CHECK(reg_pair_is(U801, OLATA, 0xFF, 0xFF));
    CHECK(reg_pair_is(U802, IODIRA, 0x00, 0x00));
    CHECK(reg_pair_is(U802, OLATA, 0xFF, 0xFF));

    // Only the first k writes land: they are the first k of the list, so
    // IODIR is the first write to each chip.
    for (int k = 0; k <= 6; k++) {
        start();
        poke(U801, IPOLA, 0xFF);
        poke(U801, IPOLB, 0xFF);
        fake_matrix_fail_from = fake_matrix_txn + k + 1;
        CHECK(key_scan_init() == (k == 6));
        for (int j = 0; j < 6; j++) {
            uint8_t a = j < k ? init_writes[j].a : init_writes[j].was;
            uint8_t b = j < k ? init_writes[j].b : init_writes[j].was;
            CHECK(reg_pair_is(init_writes[j].addr, init_writes[j].reg, a, b));
        }
    }
}

static void test_init_failure_returns_false(void) {
    for (int k = 1; k <= 6; k++) {
        start();
        fake_matrix_fail_txn = k;
        CHECK(!key_scan_init());
        start();
        fake_matrix_fail_txn = k;
        fake_matrix_fail_bytes = -1;          // everything landed, still an error
        CHECK(!key_scan_init());
        start();
        fake_matrix_short_txn = k;
        CHECK(!key_scan_init());
    }
}

static void test_check_reads_both_chips(void) {
    start_configured();
    int txn = fake_matrix_txn;
    CHECK(check_intact());
    CHECK(fake_matrix_txn - txn == 4);       // pointer + 4 bytes on U801, pointer + 2 on U802

    start_configured();
    fake_matrix_chip_reset(U801);
    CHECK(!check_intact());
    start_configured();
    fake_matrix_chip_reset(U802);
    CHECK(!check_intact());

    // Each of the six bytes is compared.
    static const struct { uint8_t addr, reg, value; } stray[] = {
        { U801, IPOLA, 0xFF }, { U801, IPOLB, 0x01 }, { U801, IODIRA, 0x7E },
        { U801, IODIRB, 0x03 }, { U802, IODIRA, 0x01 }, { U802, IODIRB, 0x01 },
    };
    for (unsigned i = 0; i < sizeof(stray) / sizeof(stray[0]); i++) {
        start_configured();
        poke(stray[i].addr, stray[i].reg, stray[i].value);
        CHECK(!check_intact());
    }

    // A reset inside either read: the rest of it reads 0xFF.
    for (int k = 2; k <= 4; k += 2) {
        start_configured();
        fake_matrix_reset_in_read_txn = fake_matrix_txn + k;
        CHECK(!check_intact());
    }

    for (int k = 1; k <= 4; k++) {
        bool intact;
        start_configured();
        fake_matrix_fail_txn = fake_matrix_txn + k;
        CHECK(!key_scan_check(&intact));
        start_configured();
        fake_matrix_short_txn = fake_matrix_txn + k;
        CHECK(!key_scan_check(&intact));
    }
}

// A reset after a chip's canary write is caught; one before it is repaired by
// the writes that follow. U801's canary is init transfer 1, U802's is 5.
static void test_reset_during_init_is_caught(void) {
    for (int k = 1; k <= 6; k++) {
        start_configured();
        fake_matrix_reset_txn = fake_matrix_txn + k;
        fake_matrix_reset_addr = U801;
        CHECK(key_scan_init());
        CHECK(check_intact() == (k < 2));

        start_configured();
        fake_matrix_reset_txn = fake_matrix_txn + k;
        fake_matrix_reset_addr = U802;
        CHECK(key_scan_init());
        CHECK(check_intact() == (k < 6));
    }
}

// ---- Column steps --------------------------------------------------------

static void test_each_column_selects_one_column(void) {
    start_configured();
    for (uint8_t c = 0; c < 10; c++) column(c);
    column(0);                                // from 9
    column(7);
    column(8);                                // from 7
}

static void test_rows_follow_the_pin_map(void) {
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 10; c++) {
            start_configured();
            fake_matrix_key[r][c] = true;
            for (uint8_t col = 0; col < 10; col++) {
                CHECK(column(col) == (col == c ? 1u << r : 0u));
            }
        }
    }
}

static void test_all_keys_closed_no_ghost(void) {
    start_configured();
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 10; c++) fake_matrix_key[r][c] = true;
    }
    for (int scan = 0; scan < 2; scan++) {
        for (uint8_t c = 0; c < 10; c++) CHECK(column(c) == 0xFF);
    }
}

// COL9 selected with MUTE of row 3 held; the U802 release of the next col-0
// step fails with nothing landed, so COL9 is still low.
static void fail_u802_release(void) {
    start_configured();
    fake_matrix_key[3][9] = true;
    column(9);
    fake_matrix_fail_txn = fake_matrix_txn + 1;
    fake_matrix_fail_bytes = 0;
    uint8_t down;
    CHECK(!key_scan_column(0, &down));
    CHECK(fake_matrix_reg(U802, OLATA) == 0xFD);
    fake_matrix_fail_txn = 0;
}

static void test_failed_u802_release_is_redone(void) {
    // The next step's first write is the U802 release: when its second fails,
    // COL8/9 are released and COL0 is not selected yet.
    fail_u802_release();
    fake_matrix_fail_txn = fake_matrix_txn + 2;
    uint8_t down;
    CHECK(!key_scan_column(0, &down));
    CHECK(fake_matrix_reg(U802, OLATA) == 0xFF);
    CHECK(reg_pair_is(U801, OLATA, 0xFF, 0xFF));

    // With no fault it reads with only COL0 low: row 3 does not ghost.
    fail_u802_release();
    int txn = fake_matrix_txn;
    CHECK(column(0) == 0);
    CHECK(fake_matrix_txn - txn == 4);
}

// COL3 selected with a pad of row 5 held; the U801 release of the next col-8
// step fails with one byte landed: OLATA is released, OLATB (COL3) is not.
static void fail_u801_release(void) {
    start_configured();
    fake_matrix_key[5][3] = true;
    column(3);
    fake_matrix_fail_txn = fake_matrix_txn + 1;
    fake_matrix_fail_bytes = 1;
    uint8_t down;
    CHECK(!key_scan_column(8, &down));
    CHECK(reg_pair_is(U801, OLATA, 0xFF, 0xEF));
    fake_matrix_fail_txn = 0;
    fake_matrix_fail_bytes = 0;
}

static void test_failed_u801_write_is_redone(void) {
    // The next step releases U801 again before it selects COL8.
    fail_u801_release();
    fake_matrix_fail_txn = fake_matrix_txn + 2;
    uint8_t down;
    CHECK(!key_scan_column(8, &down));
    CHECK(reg_pair_is(U801, OLATA, 0xFF, 0xFF));
    CHECK(fake_matrix_reg(U802, OLATA) == 0xFF);

    fail_u801_release();
    int txn = fake_matrix_txn;
    CHECK(column(8) == 0);
    CHECK(fake_matrix_txn - txn == 4);
}

// A select write that lands but reports an error leaves its column low: the
// next step releases it, whichever chip it is on.
static void test_failed_select_is_released_by_the_next_step(void) {
    uint8_t down;
    start_configured();
    fake_matrix_key[4][8] = true;
    column(3);
    fake_matrix_fail_txn = fake_matrix_txn + 2;   // U801 release, then the U802 select of COL8
    fake_matrix_fail_bytes = -1;
    CHECK(!key_scan_column(8, &down));
    CHECK(fake_matrix_reg(U802, OLATA) == 0xFE);
    CHECK(column(0) == 0);                        // row 4 does not ghost from COL8

    start_configured();
    fake_matrix_key[4][2] = true;
    column(9);
    fake_matrix_fail_txn = fake_matrix_txn + 2;   // U802 release, then the U801 select of COL2
    fake_matrix_fail_bytes = -1;
    CHECK(!key_scan_column(2, &down));
    CHECK(reg_pair_is(U801, OLATA, 0xFF, 0xF7));
    CHECK(column(8) == 0);                        // row 4 does not ghost from COL2
}

static void test_column_step_transfers(void) {
    // Wire bytes: U801 column write 4, U802 write 3, pointer + 2-byte read 5.
    for (uint8_t c = 0; c < 10; c++) {
        start_configured();
        check_step(c, 4, c < 8 ? 3 + 4 + 5 : 4 + 3 + 5);   // right after init
    }
    start_configured();
    column(0);
    check_step(1, 3, 4 + 5);
    column(7);
    check_step(8, 4, 4 + 3 + 5);
    check_step(9, 3, 3 + 5);
    check_step(0, 4, 3 + 4 + 5);
}

// The spec's bus budget: a scan (the check, then columns 0-9 from COL9) is 36
// transfers and 107 wire bytes, about 2.41 ms at 400 kHz.
static void test_scan_wire_budget(void) {
    start_configured();
    for (int scan = 0; scan < 2; scan++) {
        int txn = fake_matrix_txn;
        unsigned bytes = fake_matrix_wire_bytes;
        uint32_t t0 = timer_hw->timerawl;
        fake_matrix_clock = scan == 1;
        CHECK(check_intact());
        for (uint8_t c = 0; c < 10; c++) column(c);
        if (scan == 0) continue;            // the first scan starts from an unknown selection
        CHECK(fake_matrix_txn - txn == 36);
        CHECK(fake_matrix_wire_bytes - bytes == 107);
        uint32_t elapsed = timer_hw->timerawl - t0;
        CHECK(elapsed >= 2407 && elapsed <= 2408);
    }
}

// A chip that reset stays reset until key_scan_init: column steps never write
// the canary, so the next check finds it.
static void test_column_steps_leave_a_reset_chip_to_the_check(void) {
    for (int float_low = 0; float_low <= 1; float_low++) {
        start_configured();
        fake_matrix_float_low = float_low;
        fake_matrix_chip_reset(U801);       // rows lose their pull-ups, columns float
        for (uint8_t c = 0; c < 10; c++) {
            uint8_t down;
            CHECK(key_scan_column(c, &down));
            CHECK(down == (float_low ? 0xFF : 0x00));
        }
        CHECK(reg_pair_is(U801, IODIRA, 0xFF, 0xFF));
        CHECK(!check_intact());
    }

    start_configured();
    fake_matrix_key[2][8] = true;
    fake_matrix_chip_reset(U802);           // COL8/9 float: no key there reads down
    for (uint8_t c = 0; c < 10; c++) {
        uint8_t down;
        CHECK(key_scan_column(c, &down));
        CHECK(down == 0);
    }
    CHECK(reg_pair_is(U802, IODIRA, 0xFF, 0xFF));
    CHECK(!check_intact());

    // A reset inside the row read: GPIOB reads 0xFF, so ROW7 can only look up.
    start_configured();
    for (int r = 0; r < 8; r++) fake_matrix_key[r][2] = true;
    fake_matrix_reset_in_read_txn = fake_matrix_txn + 4;   // U802 release, U801 write, pointer, read
    uint8_t down;
    CHECK(key_scan_column(2, &down));
    CHECK(down == 0x7F);
    CHECK(!check_intact());
}

int main(void) {
    RUN(test_init_writes_configuration);
    RUN(test_init_failure_returns_false);
    RUN(test_check_reads_both_chips);
    RUN(test_reset_during_init_is_caught);
    RUN(test_each_column_selects_one_column);
    RUN(test_rows_follow_the_pin_map);
    RUN(test_all_keys_closed_no_ghost);
    RUN(test_failed_u802_release_is_redone);
    RUN(test_failed_u801_write_is_redone);
    RUN(test_failed_select_is_released_by_the_next_step);
    RUN(test_column_step_transfers);
    RUN(test_scan_wire_budget);
    RUN(test_column_steps_leave_a_reset_chip_to_the_check);
    return 0;
}
