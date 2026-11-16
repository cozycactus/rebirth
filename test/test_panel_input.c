#include <stdio.h>
#include <stdlib.h>
#include "fake_hardware.h"
#include "fake_key_matrix.h"
#include "hardware/i2c.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include "panel_input.h"
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
#define U801 0x22           // ROW0-7, COL0-7
#define U802 0x23           // COL8, COL9
#define IODIRA 0x00
#define IPOLA 0x02
#define GPPUA 0x0C
#define OLATA 0x14
#define ROWS 8
#define COLS 10
#define KEYS (ROWS * COLS)
#define SCENE_COL 8
#define MUTE_COL 9
#define SCAN_PASSES 11      // a scan is the check, then one column per pass

// ---- Keys and events -------------------------------------------------------

// The control a key is: col 0-7 the pad of track col in its row, col 8 the
// SCENE key of its row, col 9 the MUTE key of track row.
static panel_event_t control_of(int row, int col, bool down) {
    panel_event_t e = { PANEL_PAD, (uint8_t)col, (uint8_t)row, down };
    if (col == SCENE_COL) e = (panel_event_t){ PANEL_SCENE, 0, (uint8_t)row, down };
    if (col == MUTE_COL) e = (panel_event_t){ PANEL_MUTE, (uint8_t)row, 0, down };
    return e;
}

static bool is_key(const panel_event_t *e, int row, int col, bool down) {
    panel_event_t c = control_of(row, col, down);
    return e->kind == c.kind && e->track == c.track && e->row == c.row && e->down == c.down;
}

// The key (row * COLS + col) an event names, or -1 for PLAY.
static int key_of(const panel_event_t *e) {
    for (int k = 0; k < KEYS; k++) {
        if (is_key(e, k / COLS, k % COLS, e->down)) return k;
    }
    CHECK(e->kind == PANEL_PLAY);
    return -1;
}

static void poke(uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    CHECK(i2c_write_timeout_us(i2c0, addr, buf, 2, false, 2000) == 2);
}

// ---- The UI loop, one pass per ms -------------------------------------------

static uint32_t now_ms;
static uint32_t ms_per_pass;
static bool ui_drains;      // the UI reads the ring on every pass
static panel_event_t log_events[256];
static int log_count;

static void collect(void) {
    panel_event_t e;
    while (panel_input_next(&e)) {
        CHECK(log_count < (int)(sizeof(log_events) / sizeof(log_events[0])));
        log_events[log_count++] = e;
    }
}

// The events read since the last take; more than max fails the test.
static int take(panel_event_t *out, int max) {
    int n = log_count;
    CHECK(n <= max);
    for (int i = 0; i < n; i++) out[i] = log_events[i];
    log_count = 0;
    return n;
}

static void power_on(void) {
    fake_hardware_reset();
    fake_matrix_reset();
    now_ms = 0;
    ms_per_pass = 1;
    ui_drains = true;
    log_count = 0;
}

static void boot(void) {
    power_on();
    panel_input_init();
}

static void pass(void) {
    panel_input_poll(now_ms);
    now_ms += ms_per_pass;
    if (ui_drains) collect();
}

static void passes(int n) {
    while (n-- > 0) pass();
}

static void scans(int n) {
    passes(n * SCAN_PASSES);
}

// Passes up to and including the one at t.
static void pass_to(uint32_t t) {
    while (now_ms <= t) pass();
}

// One pass; true when it read the rows with column col low (-1: it read no
// rows, on working chips).
static bool pass_samples(int col) {
    fake_matrix_low_column = -1;
    pass();
    return fake_matrix_low_column == col;
}

// Waits out a retry period too.
static void until_sample_of(int col) {
    int left = 12 * SCAN_PASSES;
    while (!pass_samples(col)) CHECK(--left > 0);
}

// Booted with the bus working, the boot suppression over (S + 5 ms + S).
static void start(void) {
    boot();
    scans(3);
}

// One pass that must be the scan's check, and pass.
static void pass_check(void) {
    int txn = fake_matrix_txn;
    CHECK(pass_samples(-1));
    CHECK(fake_matrix_txn - txn == 4);
}

// The next two passes: a column (1-7, after the one before it) whose sample
// would settle a key, on a U801 that reset after the scan's check, then the
// pass after it. True when U801's canary, read after the sample in its own
// pass, showed the reset, so the next pass configured the scanner again.
static bool settling_sample_is_proved(void) {
    int txn = fake_matrix_txn;
    pass();
    bool proved = fake_matrix_txn - txn == 3 + 2;   // the column step, then U801 IODIR
    txn = fake_matrix_txn;
    pass();
    return proved && fake_matrix_txn - txn == 6;    // a configuration
}

// ---- Matrix -------------------------------------------------------------------

static void test_keys_map_to_controls(void) {
    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            start();
            panel_event_t e[4];
            fake_matrix_key[row][col] = true;
            scans(3);
            CHECK(take(e, 4) == 1);
            CHECK(is_key(&e[0], row, col, true));
        }
    }
    // The table itself, as the board's README states it.
    panel_event_t e = control_of(5, 3, true);
    CHECK(e.kind == PANEL_PAD && e.track == 3 && e.row == 5);
    e = control_of(5, SCENE_COL, true);
    CHECK(e.kind == PANEL_SCENE && e.row == 5);
    e = control_of(5, MUTE_COL, true);
    CHECK(e.kind == PANEL_MUTE && e.track == 5);
}

static void test_bounce_yields_one_press(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[2][5] = true;
    until_sample_of(5);                     // closed
    fake_matrix_key[2][5] = false;
    until_sample_of(5);                     // open
    fake_matrix_key[2][5] = true;
    until_sample_of(5);                     // closed: the change starts here
    CHECK(take(e, 4) == 0);
    while (!pass_samples(5)) CHECK(log_count == 0);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 2, 5, true));
    scans(3);
    CHECK(take(e, 4) == 0);
}

// At the first sample of its column at least 5 ms after the one that first
// saw it: scans that take no time put the next samples 4 and 5 ms later.
static void test_change_settles_at_5_ms(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[2][5] = true;
    until_sample_of(5);
    uint32_t seen = now_ms - 1;
    ms_per_pass = 0;
    now_ms = seen + 4;
    until_sample_of(5);
    CHECK(take(e, 4) == 0);
    now_ms = seen + 5;
    until_sample_of(5);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 2, 5, true));
}

static void test_release_is_reported(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[1][1] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 1, 1, true));
    fake_matrix_key[1][1] = false;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 1, 1, false));
}

static void test_all_80_keys_at_once(void) {
    start();
    panel_event_t e[KEYS + 8];
    for (int k = 0; k < KEYS; k++) fake_matrix_key[k / COLS][k % COLS] = true;
    scans(3);
    int n = take(e, KEYS + 8);
    CHECK(n == KEYS);
    bool seen[KEYS] = { false };
    int pads = 0, scenes = 0, mutes = 0;
    for (int i = 0; i < n; i++) {
        CHECK(e[i].down);
        int k = key_of(&e[i]);
        CHECK(k >= 0 && !seen[k]);
        seen[k] = true;
        pads += e[i].kind == PANEL_PAD;
        scenes += e[i].kind == PANEL_SCENE;
        mutes += e[i].kind == PANEL_MUTE;
    }
    CHECK(pads == 64 && scenes == 8 && mutes == 8);
}

// The fake fails the test whenever two columns are low at a row read.
static void test_one_column_low_per_read_in_scan_order(void) {
    start();
    for (int col = 0; col < COLS; col++) fake_matrix_key[col % ROWS][col] = true;
    until_sample_of(MUTE_COL);
    for (int scan = 0; scan < 2; scan++) {
        int txn = fake_matrix_txn;
        unsigned bytes = fake_matrix_wire_bytes;
        CHECK(pass_samples(-1));            // step 0 reads no rows:
        CHECK(fake_matrix_txn - txn == 4);  // the check, a pointer write and a read on each chip
        CHECK(fake_matrix_wire_bytes - bytes == 2 + 5 + 2 + 3);
        for (int col = 0; col < COLS; col++) CHECK(pass_samples(col));
    }
}

// A pass makes one bus step: a configuration (six 3-byte writes, nothing
// else), the check (4), or a column (3-4), then, when its sample would settle
// a key, the canaries read after it: U801's (2) for COL0-7, both chips' (4)
// for COL8/9. Keys keep changing, so samples settle.
static void test_one_bus_step_per_poll(void) {
    start();
    for (int k = 0; k < KEYS; k += 7) fake_matrix_key[k / COLS][k % COLS] = true;
    int faults = 0, configurations = 0;
    int proved[2] = { 0, 0 };               // samples of COL0-7, of COL8/9, with their canaries
    for (int i = 0; i < 40 * SCAN_PASSES; i++) {
        if (i % 13 == 0) {
            int k = (i / 13 * 11) % KEYS;
            fake_matrix_key[k / COLS][k % COLS] = !fake_matrix_key[k / COLS][k % COLS];
        }
        if (i % 37 == 5) {                  // this pass's first transfer fails
            fake_matrix_fail_txn = fake_matrix_txn + 1;
            faults++;
        }
        if (i % 37 == 23) {                 // a chip resets: the next check finds it
            fake_matrix_chip_reset(i % 2 ? U801 : U802);
            faults++;
        }
        int txn = fake_matrix_txn;
        unsigned bytes = fake_matrix_wire_bytes;
        fake_matrix_low_column = -1;
        pass();
        int transfers = fake_matrix_txn - txn;
        int col = fake_matrix_low_column;   // -1 when no column was low (a reset chip)
        CHECK(transfers <= 4 + 4);          // COL8 or COL9, then both canaries
        if (col >= 0 && col < SCENE_COL) CHECK(transfers <= 4 + 2);    // COL0-7, then U801's
        if (transfers == 6 && fake_matrix_wire_bytes - bytes == 6 * 4) {
            CHECK(col == -1);               // a configuration, and nothing else in its pass
            configurations++;
        } else if (transfers > 4 && col >= 0) {
            proved[col >= SCENE_COL]++;
        }
    }
    CHECK(faults > 0 && configurations == faults);
    CHECK(proved[0] > 0 && proved[1] > 0);
}

// ---- Faults ----------------------------------------------------------------------

// The contract: a tap that ends during an outage is lost.
static void test_single_sample_never_settles(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[2][5] = true;
    until_sample_of(5);                     // seen once
    fake_matrix_fail_from = fake_matrix_txn + 1;
    passes(1000);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[2][5] = false;
    fake_matrix_fail_from = 0;
    scans(15);                              // a retry period, then the scanner is back
    CHECK(take(e, 4) == 0);

    // Nor does the sample from before the outage count with the first one
    // after it: closed for both, then open before the next.
    fake_matrix_key[2][5] = true;
    until_sample_of(5);
    fake_matrix_fail_from = fake_matrix_txn + 1;
    passes(1000);
    fake_matrix_fail_from = 0;
    until_sample_of(5);
    fake_matrix_key[2][5] = false;
    scans(3);
    CHECK(take(e, 4) == 0);
}

// A chip that resets after the scan's check can still give the sample that
// would settle a change; the canary read after that sample must catch it.
// Four cases on key (2, 5) or (7, 5): column 5 is sampled six passes after
// the check, and five passes after column 4.

// A tap seen closed on one good sample, then U801 resets and its rows float
// low, so the next sample reads the key closed again: the tap is lost (the
// contract), and the key works normally afterwards.
static void test_reset_after_the_check_never_settles_a_tap(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[2][5] = true;
    until_sample_of(5);                     // seen closed once
    fake_matrix_key[2][5] = false;
    until_sample_of(MUTE_COL);
    pass_check();
    fake_matrix_chip_reset(U801);           // every row reads low: every key closed
    passes(5);                              // columns 0-4: first sights
    bool proved = settling_sample_is_proved();
    scans(15);
    CHECK(take(e, 4) == 0);
    CHECK(proved);
    fake_matrix_key[2][5] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 2, 5, true));
    fake_matrix_key[2][5] = false;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 2, 5, false));
}

// A held key chatters open for one good sample, then U801 resets and its rows
// float high, so the next sample reads the key open again: no release, and no
// second press when it reads held after the re-init.
static void test_reset_after_the_check_never_settles_a_chatter(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[2][5] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 2, 5, true));
    fake_matrix_key[2][5] = false;
    until_sample_of(5);                     // the chatter: seen open once
    fake_matrix_key[2][5] = true;
    until_sample_of(MUTE_COL);
    pass_check();
    fake_matrix_float_low = false;
    fake_matrix_chip_reset(U801);           // every row reads high: every key open
    passes(5);
    bool proved = settling_sample_is_proved();
    scans(15);
    CHECK(take(e, 4) == 0);                 // held throughout: nothing
    CHECK(proved);
    fake_matrix_key[2][5] = false;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 2, 5, false));
}

// The same chatter on a key in ROW7, with U801 resetting inside the row read:
// GPIOA comes back, then GPIOB as 0xFF, so ROW7 reads open.
static void test_reset_inside_the_row_read_never_settles_a_chatter(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[7][5] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 7, 5, true));
    fake_matrix_key[7][5] = false;
    until_sample_of(5);                     // the chatter
    fake_matrix_key[7][5] = true;
    until_sample_of(4);                     // past the check
    fake_matrix_reset_in_read_txn = fake_matrix_txn + 3;    // column 5: select, pointer, read
    bool proved = settling_sample_is_proved();
    scans(15);
    CHECK(take(e, 4) == 0);
    CHECK(proved);
    fake_matrix_key[7][5] = false;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 7, 5, false));
}

// After a resync the held key is suppressed. It chatters open for one good
// sample, then U801 resets and its rows float high, so the key reads open on
// two samples: that would end its suppression, and it would press again once
// it reads held after the re-init. It stays suppressed instead; a real
// release ends that silently, and the next press is normal.
static void test_reset_after_the_check_never_ends_a_suppression(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[2][5] = true;
    scans(3);
    CHECK(take(e, 4) == 1);
    panel_input_resync();
    scans(3);                               // every other key's suppression is over
    fake_matrix_key[2][5] = false;
    until_sample_of(5);                     // the chatter
    fake_matrix_key[2][5] = true;
    until_sample_of(MUTE_COL);
    pass_check();
    fake_matrix_float_low = false;
    fake_matrix_chip_reset(U801);
    passes(5);
    bool proved = settling_sample_is_proved();
    scans(15);
    CHECK(take(e, 4) == 0);                 // held throughout: nothing
    CHECK(proved);
    fake_matrix_key[2][5] = false;          // a real release ends the suppression
    scans(3);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[2][5] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 2, 5, true));
}

enum {
    FAIL_ONCE, FAIL_FIRST_BYTE_LANDED, SHORT_COUNT, RESET_U801, RESET_U801_FLOAT_HIGH,
    RESET_U802, RESET_IN_READ, FAULT_KINDS
};

static void inject(int kind, int txn) {
    switch (kind) {
        case FAIL_ONCE: fake_matrix_fail_txn = txn; break;
        case FAIL_FIRST_BYTE_LANDED: fake_matrix_fail_txn = txn; fake_matrix_fail_bytes = 1; break;
        case SHORT_COUNT: fake_matrix_short_txn = txn; break;
        case RESET_U801: fake_matrix_reset_txn = txn; fake_matrix_reset_addr = U801; break;
        case RESET_U801_FLOAT_HIGH:
            fake_matrix_reset_txn = txn;
            fake_matrix_reset_addr = U801;
            fake_matrix_float_low = false;
            break;
        case RESET_U802: fake_matrix_reset_txn = txn; fake_matrix_reset_addr = U802; break;
        case RESET_IN_READ: fake_matrix_reset_in_read_txn = txn; break;
    }
}

#define HELD_ROW 7          // its SCENE key held and accepted throughout; ROW7 is GPIOB,
                            // the byte a read cut by a reset returns as 0xFF (up)
#define CHATTER_COL MUTE_COL    // HELD_ROW's MUTE key: accepted down, open for the window's
                                // first scan only
#define DOWN_ROW 1          // in every column: closed at the window, then seen once
#define UP_ROW 2            // in every column: accepted down, opened at the window, then seen once
#define TAP_ROW 3           // in every column: closed for the window's first scan only
#define LATER_ROW 6
#define LATER_COL 5

// Up to the window: the held keys and the UP_ROW keys accepted down, then a
// failed transfer, so the window opens with a configuration; the DOWN_ROW,
// UP_ROW, TAP_ROW and chatter keys change right before it. Each column is
// seen once in the window's first scan and again in its second, so from the
// first column step to the end some key of each pending kind has been seen
// exactly once. None can be at a fault in the configuration: one only follows
// a loss, which cancels them. Returns the transfer count at the window.
static int open_window(void) {
    start();
    fake_matrix_key[HELD_ROW][SCENE_COL] = true;
    fake_matrix_key[HELD_ROW][CHATTER_COL] = true;
    for (int col = 0; col < COLS; col++) fake_matrix_key[UP_ROW][col] = true;
    scans(3);
    CHECK(log_count == 2 + COLS);
    log_count = 0;
    fake_matrix_fail_txn = fake_matrix_txn + 1;
    pass();
    fake_matrix_fail_txn = 0;
    fake_matrix_key[HELD_ROW][CHATTER_COL] = false;
    for (int col = 0; col < COLS; col++) {
        fake_matrix_key[DOWN_ROW][col] = true;
        fake_matrix_key[UP_ROW][col] = false;
        fake_matrix_key[TAP_ROW][col] = true;
    }
    return fake_matrix_txn;
}

// The window: the configuration and the first scan; then the taps and the
// chatter end, so they change between two samples of their columns (they last
// those 12 passes, in which no column is sampled twice: so neither may ever be
// reported); then the second scan, whose samples settle the DOWN_ROW and
// UP_ROW keys.
static void run_window(void) {
    passes(1 + SCAN_PASSES);
    fake_matrix_key[HELD_ROW][CHATTER_COL] = true;
    for (int col = 0; col < COLS; col++) fake_matrix_key[TAP_ROW][col] = false;
    passes(SCAN_PASSES);
}

static void test_fault_at_every_transfer(void) {
    // The window, fault-free: a configuration, then two scans, each sample of
    // the second followed by its canaries.
    int window_from = open_window();
    run_window();
    int window = fake_matrix_txn - window_from;
    CHECK(window >= 6 + 2 * 30 + 8 * 2 + 2 * 4);

    for (int kind = 0; kind < FAULT_KINDS; kind++) {
        for (int k = 1; k <= window; k++) {
            int from = open_window();
            inject(kind, from + k);
            run_window();
            scans(15);                      // a retry period, then the scanner is back
            // Only the real changes, each once: no phantom, the held key never
            // reported again.
            panel_event_t e[64];
            int n = take(e, 64);
            int downs[COLS] = { 0 }, ups[COLS] = { 0 };
            for (int i = 0; i < n; i++) {
                int key = key_of(&e[i]);
                CHECK(key >= 0);
                int row = key / COLS, col = key % COLS;
                CHECK(e[i].down == fake_matrix_key[row][col]);
                CHECK(row == (e[i].down ? DOWN_ROW : UP_ROW));
                if (e[i].down) downs[col]++;
                else ups[col]++;
            }
            for (int col = 0; col < COLS; col++) CHECK(downs[col] == 1 && ups[col] == 1);
            fake_matrix_key[LATER_ROW][LATER_COL] = true;
            scans(3);
            CHECK(take(e, 64) == 1 && is_key(&e[0], LATER_ROW, LATER_COL, true));
        }
    }
}

// SCENE of row 3 held; the write that selects COL8 lands but reports an
// error, so COL8 stays low. No row is read until both chips are configured
// again, which releases it.
static void test_failed_u802_write_never_ghosts(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[3][SCENE_COL] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 3, SCENE_COL, true));
    until_sample_of(7);
    fake_matrix_fail_txn = fake_matrix_txn + 2;     // the U801 release, then the U802 select
    fake_matrix_fail_bytes = -1;
    CHECK(pass_samples(-1));
    CHECK(fake_matrix_reg(U802, OLATA) == 0xFE);

    int txn = fake_matrix_txn;
    CHECK(pass_samples(-1));                // the configuration
    CHECK(fake_matrix_txn - txn == 6);
    CHECK(fake_matrix_reg(U802, OLATA) == 0xFF);
    CHECK(fake_matrix_reg(U801, OLATA) == 0xFF && fake_matrix_reg(U801, OLATA + 1) == 0xFF);
    txn = fake_matrix_txn;
    CHECK(pass_samples(-1));                // the check
    CHECK(fake_matrix_txn - txn == 4);
    for (int col = 0; col < COLS; col++) CHECK(pass_samples(col));
    scans(3);
    CHECK(take(e, 4) == 0);                 // nothing for row 3's pads
}

// After the resync every key is suppressed, so the event checks alone cannot
// see a ghost. The guard is the fake's CHECK(columns_low <= 1) in
// note_row_read: COL8 still low at the read of column 0 fails the test there.
static void test_resync_with_col8_selected_never_ghosts(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[3][SCENE_COL] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 3, SCENE_COL, true));
    until_sample_of(SCENE_COL);
    CHECK(fake_matrix_reg(U802, OLATA) == 0xFE);    // COL8 still low
    panel_input_resync();
    scans(3);
    CHECK(take(e, 4) == 0);                 // no PAD event for row 3
    fake_matrix_key[3][SCENE_COL] = false;
    scans(3);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[3][SCENE_COL] = true;   // the suppression is over
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 3, SCENE_COL, true));
}

// A chip that resets mid-scan: its rows (U801) or columns (U802) float, and
// a floating row reads low, as if every key were down.
static void test_reset_is_found_by_the_next_check(void) {
    for (int chip = 0; chip < 2; chip++) {
        start();
        panel_event_t e[4];
        CHECK(fake_matrix_float_low);
        fake_matrix_key[1][2] = true;
        fake_matrix_key[3][SCENE_COL] = true;
        scans(3);
        CHECK(take(e, 4) == 2);
        until_sample_of(4);
        fake_matrix_chip_reset(chip == 0 ? U801 : U802);
        passes(200);
        CHECK(take(e, 4) == 0);
        if (chip == 0) {
            CHECK(fake_matrix_reg(U801, IODIRA) == 0x7F && fake_matrix_reg(U801, IODIRA + 1) == 0x01);
            CHECK(fake_matrix_reg(U801, IPOLA) == 0x00 && fake_matrix_reg(U801, IPOLA + 1) == 0x00);
            CHECK(fake_matrix_reg(U801, GPPUA) == 0x7F && fake_matrix_reg(U801, GPPUA + 1) == 0x01);
        } else {
            CHECK(fake_matrix_reg(U802, IODIRA) == 0x00 && fake_matrix_reg(U802, IODIRA + 1) == 0x00);
        }
        fake_matrix_key[1][2] = false;
        fake_matrix_key[3][SCENE_COL] = false;
        scans(3);
        CHECK(take(e, 4) == 2);             // the releases, in scan order
        int pad = is_key(&e[0], 1, 2, false) ? 0 : 1;
        CHECK(is_key(&e[pad], 1, 2, false) && is_key(&e[1 - pad], 3, SCENE_COL, false));
    }
}

static void test_i2c_error_reconfigures_before_reading(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[0][1] = true;
    poke(U801, GPPUA, 0x00);                // only a configuration writes it back
    fake_matrix_fail_txn = fake_matrix_txn + 1;
    CHECK(pass_samples(-1));
    int txn = fake_matrix_txn;
    unsigned bytes = fake_matrix_wire_bytes;
    CHECK(pass_samples(-1));
    CHECK(fake_matrix_txn - txn == 6);
    CHECK(fake_matrix_wire_bytes - bytes == 6 * 4);    // six 3-byte writes
    CHECK(fake_matrix_reg(U801, GPPUA) == 0x7F);
    txn = fake_matrix_txn;
    CHECK(pass_samples(-1));                // then the check
    CHECK(fake_matrix_txn - txn == 4);
    CHECK(pass_samples(0));                 // then the scan
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 0, 1, true));
}

// The first poll retries a failed boot configuration at once; a second
// failure waits PANEL_RETRY_MS.
static void test_init_failure_at_boot_recovers(void) {
    power_on();
    panel_event_t e[4];
    fake_matrix_fail_from = 1;
    panel_input_init();
    CHECK(fake_matrix_txn == 1);
    pass();                                 // t = 0
    CHECK(fake_matrix_txn == 2);
    fake_matrix_fail_from = 0;
    pass_to(99);
    CHECK(fake_matrix_txn == 2);
    pass();                                 // t = 100
    CHECK(fake_matrix_txn == 2 + 6);
    scans(3);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[4][4] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 4, 4, true));
}

static void test_failed_init_stays_in_recovery(void) {
    start();
    panel_event_t e[4];
    fake_matrix_fail_from = fake_matrix_txn + 1;
    pass();                                 // a step fails
    uint32_t failed_at = now_ms;
    pass();                                 // the configuration at once fails too
    int txn = fake_matrix_txn;
    fake_matrix_key[2][2] = true;
    pass_to(failed_at + 40);
    fake_matrix_fail_from = 0;
    pass_to(failed_at + 99);
    CHECK(fake_matrix_txn == txn);
    CHECK(take(e, 4) == 0);
    pass();                                 // failed_at + 100
    CHECK(fake_matrix_txn == txn + 6);
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 2, 2, true));
}

static void test_dead_bus_is_retried_at_most_every_100_ms(void) {
    start();
    panel_event_t e[4];
    fake_matrix_fail_from = fake_matrix_txn + 1;
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    int txn = fake_matrix_txn;
    passes(1000);
    CHECK(fake_matrix_txn - txn <= 12);
    CHECK(take(e, 4) == 1 && e[0].kind == PANEL_PLAY && e[0].down);
}

// 1 s of passes: how many configurations, checking each is at least
// PANEL_RETRY_MS after the one before.
static int paced_configurations(bool reset_u802_every_pass) {
    int configurations = 0;
    uint32_t last = 0;
    for (int i = 0; i < 1000; i++) {
        if (reset_u802_every_pass) fake_matrix_chip_reset(U802);
        int txn = fake_matrix_txn;
        uint32_t t = now_ms;
        pass();
        // Only a configuration makes 6: no key changes, so no sample brings its check.
        if (fake_matrix_txn - txn != 6) continue;
        CHECK(configurations == 0 || t - last >= PANEL_RETRY_MS);
        last = t;
        configurations++;
    }
    return configurations;
}

// The check after a configuration ends that attempt: when it fails, the next
// attempt waits PANEL_RETRY_MS, as after a failed key_scan_init. On a bus where
// writes work but every read fails, an attempt is six good writes, then the
// check's pointer write and its failed read (a 2 ms timeout on the device). So
// 1 s holds at most 11 attempts (one at once after the loss, then one per
// 100 ms), so at most 12 failed reads with the first loss: the dead bus's
// bound of 12 timeouts. Transfers: at most 4 for the step that failed, plus 8
// per attempt.
static void test_failed_first_check_is_a_failed_attempt(void) {
    start();
    panel_event_t e[4];
    fake_matrix_fail_reads = true;
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    int txn = fake_matrix_txn;
    CHECK(paced_configurations(false) <= 11);
    CHECK(fake_matrix_txn - txn <= 4 + 11 * (6 + 2));
    CHECK(take(e, 4) == 1 && e[0].kind == PANEL_PLAY && e[0].down);

    // The same when the check reads a chip changed: U802 keeps resetting.
    start();
    CHECK(paced_configurations(true) <= 11);
    CHECK(take(e, 4) == 0);
}

// ---- PLAY ----------------------------------------------------------------------

static void test_play_button_debounce(void) {
    boot();
    panel_event_t e[8];
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    while (now_ms < 20) {
        pass();
        CHECK(take(e, 8) == 0);
    }
    pass();                                 // t = 20
    CHECK(take(e, 8) == 1 && e[0].kind == PANEL_PLAY && e[0].down);
    pass_to(49);
    fake_gpio_level[PLAY_BUTTON_PIN] = true;
    while (now_ms < 70) {
        pass();
        CHECK(take(e, 8) == 0);
    }
    pass();                                 // t = 70
    CHECK(take(e, 8) == 1 && e[0].kind == PANEL_PLAY && !e[0].down);
}

static void test_held_play_survives_resync_silently(void) {
    boot();
    panel_event_t e[8];
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    pass_to(30);
    CHECK(take(e, 8) == 1);
    panel_input_resync();
    pass_to(200);
    CHECK(take(e, 8) == 0);
    fake_gpio_level[PLAY_BUTTON_PIN] = true;
    pass_to(260);
    CHECK(take(e, 8) == 0);
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    pass_to(300);
    CHECK(take(e, 8) == 1 && e[0].kind == PANEL_PLAY && e[0].down);
}

static void test_play_held_through_resync_needs_a_stable_release(void) {
    boot();
    panel_event_t e[8];
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    pass_to(30);
    CHECK(take(e, 8) == 1);
    panel_input_resync();
    pass_to(40);
    fake_gpio_level[PLAY_BUTTON_PIN] = true;    // one bounce sample
    pass_to(41);
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    pass_to(100);
    CHECK(take(e, 8) == 0);
    fake_gpio_level[PLAY_BUTTON_PIN] = true;    // a stable release ends it silently
    pass_to(130);
    CHECK(take(e, 8) == 0);
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    pass_to(160);
    CHECK(take(e, 8) == 1 && e[0].kind == PANEL_PLAY && e[0].down);
}

static void test_play_works_while_scanner_recovers(void) {
    boot();
    panel_event_t e[8];
    fake_matrix_fail_from = fake_matrix_txn + 1;
    panel_input_resync();
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    while (now_ms < 20) pass();
    CHECK(take(e, 8) == 0);
    pass();                                 // t = 20
    CHECK(take(e, 8) == 1 && e[0].kind == PANEL_PLAY && e[0].down);
}

static void test_queued_play_survives_scanner_errors(void) {
    boot();
    panel_event_t e[4];
    ui_drains = false;
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    pass_to(20);                            // PLAY down is queued
    fake_matrix_chip_reset(U801);
    scans(2);
    fake_matrix_fail_from = fake_matrix_txn + 1;
    scans(20);
    fake_matrix_fail_from = 0;
    scans(15);
    collect();
    CHECK(take(e, 4) == 1 && e[0].kind == PANEL_PLAY && e[0].down);
}

// ---- Resync and boot ---------------------------------------------------------

static void test_resync_drops_pending_events(void) {
    start();
    panel_event_t e[4];
    ui_drains = false;
    fake_matrix_key[0][0] = true;
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    scans(3);                               // a pad and PLAY are queued
    panel_input_resync();
    collect();
    CHECK(take(e, 4) == 0);
}

// It restarts the scan at the check, and COL8, left low, is released before
// column 0 is read.
static void test_resync_does_no_i2c(void) {
    start();
    until_sample_of(SCENE_COL);             // COL8 selected
    int txn = fake_matrix_txn;
    unsigned bytes = fake_matrix_wire_bytes;
    panel_input_resync();
    CHECK(fake_matrix_txn == txn && fake_matrix_wire_bytes == bytes);
    CHECK(pass_samples(-1));                // the check
    CHECK(fake_matrix_txn - txn == 4);
    txn = fake_matrix_txn;
    CHECK(pass_samples(0));
    CHECK(fake_matrix_txn - txn == 4);      // the U802 release comes first
    pass();                                 // mid-scan
    txn = fake_matrix_txn;
    panel_input_resync();
    CHECK(fake_matrix_txn == txn);
    CHECK(pass_samples(-1));                // the check again
    CHECK(fake_matrix_txn - txn == 4);
    fake_matrix_fail_txn = fake_matrix_txn + 1;
    pass();                                 // the scanner is lost
    txn = fake_matrix_txn;
    panel_input_resync();
    CHECK(fake_matrix_txn == txn);
}

static void test_held_pad_survives_resync_silently(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[5][2] = true;
    scans(3);
    CHECK(take(e, 4) == 1);
    panel_input_resync();
    scans(3);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[5][2] = false;          // released, with a bounce
    until_sample_of(2);
    fake_matrix_key[5][2] = true;
    until_sample_of(2);
    fake_matrix_key[5][2] = false;
    scans(3);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[5][2] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 5, 2, true));
}

static void test_pad_released_during_stall_presses_again(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[5][2] = true;
    scans(3);
    CHECK(take(e, 4) == 1);
    fake_matrix_key[5][2] = false;          // during the stall
    panel_input_resync();
    scans(3);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[5][2] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 5, 2, true));
}

static void test_pad_pressed_during_stall_is_silent(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[5][2] = true;
    panel_input_resync();
    scans(5);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[5][2] = false;
    scans(3);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[5][2] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 5, 2, true));
}

static void test_key_held_at_power_up_is_silent(void) {
    power_on();
    panel_event_t e[4];
    fake_matrix_key[5][MUTE_COL] = true;
    panel_input_init();
    scans(5);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[5][MUTE_COL] = false;
    scans(3);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[5][MUTE_COL] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 5, MUTE_COL, true));
}

// A loss puts a suppressed key back to held: ending the suppression takes two
// up samples after the scanner is back, so a press before the second is a
// press during the suppression and reports nothing until released.
static void test_loss_puts_a_suppressed_key_back_to_held(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[5][2] = true;
    scans(3);
    CHECK(take(e, 4) == 1);
    panel_input_resync();                   // held through a stall
    fake_matrix_key[5][2] = false;
    until_sample_of(2);                     // seen up once
    fake_matrix_fail_from = fake_matrix_txn + 1;
    passes(50);
    fake_matrix_fail_from = 0;
    until_sample_of(2);                     // and once after the outage
    fake_matrix_key[5][2] = true;
    scans(3);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[5][2] = false;
    scans(3);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[5][2] = true;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 5, 2, true));
}

static void test_reinit_never_re_reports_held_key(void) {
    start();
    panel_event_t e[4];
    fake_matrix_key[5][2] = true;
    scans(3);
    CHECK(take(e, 4) == 1);
    fake_matrix_fail_txn = fake_matrix_txn + 1;
    pass();                                 // a step fails
    int txn = fake_matrix_txn;
    pass();
    CHECK(fake_matrix_txn - txn == 6);      // the configuration
    scans(5);
    CHECK(take(e, 4) == 0);
    fake_matrix_key[5][2] = false;
    scans(3);
    CHECK(take(e, 4) == 1 && is_key(&e[0], 5, 2, false));
}

// ---- The ring ------------------------------------------------------------------

static void test_full_ring_keeps_changes_pending(void) {
    start();
    panel_event_t e[40];
    ui_drains = false;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < ROWS; row++) fake_matrix_key[row][col] = true;
    }
    fake_matrix_key[0][SCENE_COL] = true;
    fake_matrix_key[1][SCENE_COL] = true;
    scans(4);
    collect();
    CHECK(take(e, 40) == 32);
    for (int i = 0; i < 32; i++) CHECK(e[i].kind == PANEL_PAD && e[i].track < 4 && e[i].down);
    scans(2);
    collect();
    CHECK(take(e, 40) == 2);
    CHECK(is_key(&e[0], 0, SCENE_COL, true) && is_key(&e[1], 1, SCENE_COL, true));
}

static void test_ring_keeps_order_across_the_wrap(void) {
    start();
    panel_event_t e[40];
    ui_drains = false;
    for (int k = 0; k < 30; k++) fake_matrix_key[k % ROWS][k / ROWS] = true;
    scans(3);
    collect();
    CHECK(take(e, 40) == 30);               // the ring's head is now at slot 30
    fake_gpio_level[PLAY_BUTTON_PIN] = false;
    scans(2);                               // PLAY down: slot 30
    fake_matrix_key[6][4] = true;
    scans(3);                               // slot 31
    fake_matrix_key[6][5] = true;
    scans(3);                               // slot 0
    fake_gpio_level[PLAY_BUTTON_PIN] = true;
    scans(2);                               // PLAY up: slot 1
    fake_matrix_key[6][6] = true;
    scans(3);                               // slot 2
    collect();
    CHECK(take(e, 40) == 5);
    CHECK(e[0].kind == PANEL_PLAY && e[0].down);
    CHECK(is_key(&e[1], 6, 4, true));
    CHECK(is_key(&e[2], 6, 5, true));
    CHECK(e[3].kind == PANEL_PLAY && !e[3].down);
    CHECK(is_key(&e[4], 6, 6, true));
}

// ---- The Core 0 loop on the wire clock ---------------------------------------

// Each pass polls the panel at now_ms = us / 1000 (every transfer advances the
// clock by its wire time), spends 30 us on the rest of ui_update and 0.5 ms on
// the step-row sample when one is due (every 20 ms), then reads the events; the
// frame (2.5 ms, after ui_update) follows when due (every 33 ms). Both grids
// run from t = 0.
#define LOOP_REST_US 30
#define STEP_ROW_US 500
#define STEP_ROW_PERIOD_US 20000
#define FRAME_US 2500
#define FRAME_PERIOD_US 33000

static uint32_t next_step_row_us;
static uint32_t next_frame_us;

static void loop_boot(void) {
    power_on();
    fake_matrix_clock = true;
    next_step_row_us = 0;
    next_frame_us = 0;
    panel_input_init();
}

// One pass; returns when its events are read (they stay in the ring).
static uint32_t loop_pass(void) {
    panel_input_poll(timer_hw->timerawl / 1000);
    timer_hw->timerawl += LOOP_REST_US;
    if (timer_hw->timerawl >= next_step_row_us) {
        timer_hw->timerawl += STEP_ROW_US;
        next_step_row_us += STEP_ROW_PERIOD_US;
    }
    uint32_t read_us = timer_hw->timerawl;
    if (timer_hw->timerawl >= next_frame_us) {
        timer_hw->timerawl += FRAME_US;
        next_frame_us += FRAME_PERIOD_US;
    }
    return read_us;
}

#define SWEEP_FROM_US 40000     // the boot suppression is over
#define SWEEP_PERIOD_US 660000  // both grids repeat
#define PRESS_TO_EVENT_US 15000

// A settle costs bus time (the canaries read after it), so keys changing at
// the same time slow each other down: the 15 ms requirement is checked on
// playing, below, and heavy traffic only for a bound (test_press_to_event_
// under_stress).

// ---- Playing ------------------------------------------------------------------

// Isolated cases on one timeline: case i presses at SWEEP_FROM_US + i * 33 ms
// + i * 18.5 us, so within each of the twenty 33 ms stretches of the 660 ms
// period its phase steps by 0.37 ms, and 1800 cases cover the period. Its keys
// close at the first pass boundary at or after that time (which can only make
// an event later) and open at their down events; every event of a case is
// read before the next case starts.
#define CASE_US 33000
#define CASE_STRETCHES (SWEEP_PERIOD_US / CASE_US)
#define CASE_PHASES 90                      // 90 x 0.37 ms covers a stretch
#define ISOLATED_CASES (CASE_STRETCHES * CASE_PHASES)

static uint32_t case_press_us(int i) {
    return SWEEP_FROM_US + (uint32_t)i * CASE_US + (uint32_t)i * 37 / 2;
}

// Every case: keys_of(i, keys) gives case i's keys and returns how many. Each
// key gets exactly one down, within PRESS_TO_EVENT_US of the press, then
// exactly one up; nothing else is reported. Returns the worst press-to-event.
static uint32_t isolated_cases(int (*keys_of)(int i, int *keys)) {
    loop_boot();
    uint32_t worst = 0;
    for (int i = 0; i < ISOLATED_CASES; i++) {
        int keys[COLS];
        bool down[COLS] = { false }, up[COLS] = { false };
        int n = keys_of(i, keys), ups = 0;
        uint32_t press_us = case_press_us(i);
        panel_event_t e;
        while (timer_hw->timerawl < press_us) {
            loop_pass();
            CHECK(!panel_input_next(&e));
        }
        for (int j = 0; j < n; j++) fake_matrix_key[keys[j] / COLS][keys[j] % COLS] = true;
        while (ups < n) {
            uint32_t read_us = loop_pass();
            CHECK(timer_hw->timerawl < case_press_us(i + 1));  // over before the next case
            while (panel_input_next(&e)) {
                int key = key_of(&e), j = 0;
                while (j < n && keys[j] != key) j++;
                CHECK(j < n);
                if (e.down) {
                    CHECK(!down[j]);
                    uint32_t latency = read_us - press_us;
                    CHECK(latency <= PRESS_TO_EVENT_US);
                    if (latency > worst) worst = latency;
                    down[j] = true;
                    fake_matrix_key[key / COLS][key % COLS] = false;
                } else {
                    CHECK(down[j] && !up[j]);
                    up[j] = true;
                    ups++;
                }
            }
        }
    }
    return worst;
}

static int single_col;

// One key alone, in column single_col, its row varying.
static int single_key(int i, int *keys) {
    keys[0] = (i % ROWS) * COLS + single_col;
    return 1;
}

// Ten keys at once, one per column, the rows varying.
static int chord(int i, int *keys) {
    for (int col = 0; col < COLS; col++) keys[col] = ((i + col) % ROWS) * COLS + col;
    return COLS;
}

#define STREAM_PRESSES 500
#define STREAM_HOLD_US 60000

// 50 presses a second, beyond a fast two-handed roll: 10-30 ms apart
// (pseudo-random), each on the next key of a stride through all 80, held
// 60 ms (up to six at once), for 10 s. Each press gets one down within
// PRESS_TO_EVENT_US, then one up after its release. Returns the worst.
static uint32_t playing_stream(void) {
    enum { IDLE, HELD, DOWN, RELEASED };
    loop_boot();
    int state[KEYS] = { IDLE };
    uint32_t press_us[KEYS], release_us[KEYS];
    uint32_t next_us = SWEEP_FROM_US, seed = 1, worst = 0;
    int pressed = 0, done = 0;
    while (done < STREAM_PRESSES) {
        uint32_t now = timer_hw->timerawl;
        CHECK(now < SWEEP_FROM_US + STREAM_PRESSES * 30000u + STREAM_HOLD_US + 100000u);
        if (pressed < STREAM_PRESSES && now >= next_us) {
            int k = pressed * 37 % KEYS;
            CHECK(state[k] == IDLE);
            fake_matrix_key[k / COLS][k % COLS] = true;
            state[k] = HELD;
            press_us[k] = next_us;
            release_us[k] = next_us + STREAM_HOLD_US;
            seed = seed * 1103515245u + 12345u;
            next_us += 10000 + (seed >> 16) % 20001;
            pressed++;
        }
        for (int k = 0; k < KEYS; k++) {
            if ((state[k] == HELD || state[k] == DOWN) && now >= release_us[k]) {
                CHECK(state[k] == DOWN);        // its down came first
                fake_matrix_key[k / COLS][k % COLS] = false;
                state[k] = RELEASED;
            }
        }
        uint32_t read_us = loop_pass();
        panel_event_t e;
        while (panel_input_next(&e)) {
            int k = key_of(&e);
            CHECK(k >= 0);
            if (e.down) {
                CHECK(state[k] == HELD);
                uint32_t latency = read_us - press_us[k];
                CHECK(latency <= PRESS_TO_EVENT_US);
                if (latency > worst) worst = latency;
                state[k] = DOWN;
            } else {
                CHECK(state[k] == RELEASED);
                state[k] = IDLE;
                done++;
            }
        }
    }
    return worst;
}

static void test_press_to_event_within_15_ms(void) {
    uint32_t worst = 0;
    for (single_col = 0; single_col < COLS; single_col++) {
        uint32_t w = isolated_cases(single_key);
        if (w > worst) worst = w;
    }
    printf("  press-to-event, one key: %d presses, worst %u us\n",
           COLS * ISOLATED_CASES, (unsigned)worst);
    printf("  press-to-event, ten-key chord: %d chords, worst %u us\n",
           ISOLATED_CASES, (unsigned)isolated_cases(chord));
    printf("  press-to-event, 50 presses/s held 60 ms: %d presses, worst %u us\n",
           STREAM_PRESSES, (unsigned)playing_stream());
}

// ---- Stress -------------------------------------------------------------------

#define STRESS_BOUND_US 25000
#define STRESS_HOLD_US 30000
#define STRESS_LANES 3

enum { KEY_FREE, KEY_WAITING, KEY_CLOSED, KEY_HELD, KEY_RELEASING, KEY_DONE };

// A press every step_us, sustained: thousands of key changes a second, far
// beyond playing. Case i closes key i % KEYS (so column i % 10) at
// SWEEP_FROM_US + i * step_us, over the 660 ms period; it opens at its down
// event or 30 ms after its press, whichever is later. Lane l takes the blocks
// of KEYS cases with (i / KEYS) % STRESS_LANES == l, so a key is reused at
// most every 88.8 ms and must be free again by then. Each press gets exactly
// one down, within STRESS_BOUND_US, then one up: a press never down would
// hold its lane past the time limit. Returns the worst press-to-event.
static uint32_t stress_worst(uint32_t step_us) {
    int cases = (int)(SWEEP_PERIOD_US / step_us) + 1;
    uint32_t worst = 0;
    int done = 0;
    for (int lane = 0; lane < STRESS_LANES; lane++) {
        loop_boot();
        int state[KEYS];
        int block[KEYS];                    // the key's current case is block * KEYS + key
        uint32_t press_us[KEYS];
        for (int k = 0; k < KEYS; k++) {
            state[k] = KEY_FREE;
            block[k] = lane - STRESS_LANES;
        }
        int busy = KEYS;
        uint32_t last_pass_us = 0;
        while (busy > 0) {
            uint32_t pass_us = timer_hw->timerawl;
            CHECK(pass_us < 2 * SWEEP_PERIOD_US);
            for (int k = 0; k < KEYS; k++) {
                if (state[k] == KEY_FREE) {
                    block[k] += STRESS_LANES;
                    int i = block[k] * KEYS + k;
                    if (i >= cases) {
                        state[k] = KEY_DONE;
                        busy--;
                        continue;
                    }
                    press_us[k] = SWEEP_FROM_US + (uint32_t)i * step_us;
                    CHECK(press_us[k] > last_pass_us);     // no earlier boundary missed
                    state[k] = KEY_WAITING;
                }
                if (state[k] == KEY_WAITING && pass_us >= press_us[k]) {
                    fake_matrix_key[k / COLS][k % COLS] = true;
                    state[k] = KEY_CLOSED;
                }
                if (state[k] == KEY_HELD && pass_us >= press_us[k] + STRESS_HOLD_US) {
                    fake_matrix_key[k / COLS][k % COLS] = false;
                    state[k] = KEY_RELEASING;
                }
            }
            uint32_t read_us = loop_pass();
            last_pass_us = pass_us;
            panel_event_t e;
            while (panel_input_next(&e)) {
                int k = key_of(&e);
                CHECK(k >= 0);
                if (e.down) {
                    CHECK(state[k] == KEY_CLOSED);
                    uint32_t latency = read_us - press_us[k];
                    CHECK(latency <= STRESS_BOUND_US);
                    if (latency > worst) worst = latency;
                    state[k] = KEY_HELD;
                    done++;
                } else {
                    CHECK(state[k] == KEY_RELEASING);
                    state[k] = KEY_FREE;
                }
            }
        }
    }
    CHECK(done == cases);
    return worst;
}

static void test_press_to_event_under_stress(void) {
    static const uint32_t steps_us[] = {
        370, 400, 430, 460, 500, 550, 600, 700, 850, 1000, 1250, 1500, 2000,
    };
    uint32_t worst = 0, worst_step_us = 0;
    for (unsigned s = 0; s < sizeof(steps_us) / sizeof(steps_us[0]); s++) {
        uint32_t w = stress_worst(steps_us[s]);
        if (w > worst) {
            worst = w;
            worst_step_us = steps_us[s];
        }
    }
    printf("  press-to-event under stress (a press every 0.37-2 ms, held 30 ms): "
           "worst %u us, with a press every %u us\n", (unsigned)worst, (unsigned)worst_step_us);
}

static void test_bus_load_is_bounded(void) {
    loop_boot();
    int scans_done = 0;
    while (timer_hw->timerawl < 1000000) {
        fake_matrix_low_column = -1;
        loop_pass();
        if (fake_matrix_low_column == MUTE_COL) scans_done++;
    }
    CHECK(scans_done >= 250);
    CHECK(fake_matrix_wire_bytes <= 110u * (unsigned)scans_done);
    printf("  bus load: %d scans in 1 s, %u wire bytes per scan\n",
           scans_done, fake_matrix_wire_bytes / (unsigned)scans_done);
}

int main(void) {
    RUN(test_keys_map_to_controls);
    RUN(test_bounce_yields_one_press);
    RUN(test_change_settles_at_5_ms);
    RUN(test_release_is_reported);
    RUN(test_all_80_keys_at_once);
    RUN(test_one_column_low_per_read_in_scan_order);
    RUN(test_one_bus_step_per_poll);
    RUN(test_single_sample_never_settles);
    RUN(test_reset_after_the_check_never_settles_a_tap);
    RUN(test_reset_after_the_check_never_settles_a_chatter);
    RUN(test_reset_inside_the_row_read_never_settles_a_chatter);
    RUN(test_reset_after_the_check_never_ends_a_suppression);
    RUN(test_fault_at_every_transfer);
    RUN(test_failed_u802_write_never_ghosts);
    RUN(test_resync_with_col8_selected_never_ghosts);
    RUN(test_reset_is_found_by_the_next_check);
    RUN(test_i2c_error_reconfigures_before_reading);
    RUN(test_init_failure_at_boot_recovers);
    RUN(test_failed_init_stays_in_recovery);
    RUN(test_dead_bus_is_retried_at_most_every_100_ms);
    RUN(test_failed_first_check_is_a_failed_attempt);
    RUN(test_play_button_debounce);
    RUN(test_held_play_survives_resync_silently);
    RUN(test_play_held_through_resync_needs_a_stable_release);
    RUN(test_play_works_while_scanner_recovers);
    RUN(test_queued_play_survives_scanner_errors);
    RUN(test_resync_drops_pending_events);
    RUN(test_resync_does_no_i2c);
    RUN(test_held_pad_survives_resync_silently);
    RUN(test_pad_released_during_stall_presses_again);
    RUN(test_pad_pressed_during_stall_is_silent);
    RUN(test_key_held_at_power_up_is_silent);
    RUN(test_loss_puts_a_suppressed_key_back_to_held);
    RUN(test_reinit_never_re_reports_held_key);
    RUN(test_full_ring_keeps_changes_pending);
    RUN(test_ring_keeps_order_across_the_wrap);
    RUN(test_press_to_event_within_15_ms);
    RUN(test_press_to_event_under_stress);
    RUN(test_bus_load_is_bounded);
    return 0;
}
