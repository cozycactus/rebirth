#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Host-side tests for the two-phase synchronization layer.
 *
 * We mock the MIDI UART and GPIO hardware, then verify that:
 *  - midi_buf collects messages without sending until flush
 *  - gate_commit sets all gate pins in a single batch
 *  - The collect→commit sequence produces the right output order
 */

/* ── Mock infrastructure ──────────────────────────────────── */

/* Capture MIDI bytes written to "UART" */
#define MOCK_MIDI_MAX 256
static uint8_t mock_midi_bytes[MOCK_MIDI_MAX];
static int     mock_midi_len = 0;

/* Capture GPIO state */
#define NUM_GPIO 30
static uint8_t mock_gpio[NUM_GPIO];

/* Track order of operations for verifying commit ordering */
#define OP_LOG_MAX 64
typedef enum { OP_GPIO_SET, OP_GPIO_CLR, OP_UART_BYTE } op_type_t;
typedef struct { op_type_t type; uint32_t value; } op_entry_t;
static op_entry_t op_log[OP_LOG_MAX];
static int op_log_len = 0;

static void op_log_push(op_type_t type, uint32_t value) {
    if (op_log_len < OP_LOG_MAX) {
        op_log[op_log_len].type = type;
        op_log[op_log_len].value = value;
        op_log_len++;
    }
}

static void mock_reset(void) {
    mock_midi_len = 0;
    memset(mock_gpio, 0, sizeof(mock_gpio));
    op_log_len = 0;
}

/* ── Hardware mock functions (linked instead of pico SDK) ─── */

/* UART mock */
void uart_putc_raw(void *uart, uint8_t c) {
    (void)uart;
    if (mock_midi_len < MOCK_MIDI_MAX)
        mock_midi_bytes[mock_midi_len++] = c;
    op_log_push(OP_UART_BYTE, c);
}

/* GPIO mock */
void gpio_put(uint8_t pin, bool value) {
    if (pin < NUM_GPIO) mock_gpio[pin] = value ? 1 : 0;
}
void gpio_set_mask(uint32_t mask) {
    for (int i = 0; i < NUM_GPIO; i++) {
        if (mask & (1u << i)) mock_gpio[i] = 1;
    }
    op_log_push(OP_GPIO_SET, mask);
}
void gpio_clr_mask(uint32_t mask) {
    for (int i = 0; i < NUM_GPIO; i++) {
        if (mask & (1u << i)) mock_gpio[i] = 0;
    }
    op_log_push(OP_GPIO_CLR, mask);
}

/* SPI mock (for cv_set_note / cv_set_voltage) */
void spi_write_blocking(void *spi, const uint8_t *buf, int len) {
    (void)spi; (void)buf; (void)len;
}
void gpio_init(uint8_t pin) { (void)pin; }
void gpio_set_dir(uint8_t pin, bool out) { (void)pin; (void)out; }
void spi_init(void *spi, uint32_t rate) { (void)spi; (void)rate; }
void gpio_set_function(uint8_t pin, int func) { (void)pin; (void)func; }

/* ── Include the actual source we're testing ─────────────── */
/*
 * We compile midi_out.c and cv_gate.c with our mocks above.
 * The sequencer.h defines are needed for constants.
 */
#include "sequencer.h"

sequencer_state_t g_seq;

/* ── Test macros ─────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do {                              \
    tests_run++;                                    \
    printf("  %-55s", #name);                       \
    mock_reset();                                   \
    name();                                         \
    tests_passed++;                                 \
    printf("PASS\n");                               \
} while (0)

#define ASSERT_EQ(a, b) do {                         \
    if ((a) != (b)) {                                \
        printf("FAIL\n    %s:%d: %d != %d\n",       \
               __FILE__, __LINE__, (int)(a), (int)(b)); \
        exit(1);                                     \
    }                                                \
} while (0)
#define ASSERT_TRUE(x)  ASSERT_EQ(!!(x), 1)
#define ASSERT_FALSE(x) ASSERT_EQ(!!(x), 0)

/* ── midi_buf tests ──────────────────────────────────────── */

TEST(test_midi_buf_no_send_until_flush) {
    midi_buf_clear();
    midi_buf_note_on(0, 60, 100);
    midi_buf_note_on(1, 48, 80);
    /* Nothing should have been sent to UART yet */
    ASSERT_EQ(mock_midi_len, 0);
}

TEST(test_midi_buf_flush_sends_all) {
    midi_buf_clear();
    midi_buf_note_on(0, 60, 100);   /* 3 bytes */
    midi_buf_note_off(0, 60);       /* 3 bytes */
    midi_buf_note_on(1, 48, 80);    /* 3 bytes */
    ASSERT_EQ(mock_midi_len, 0);

    midi_buf_flush();
    ASSERT_EQ(mock_midi_len, 9);

    /* Verify first note-on: 0x90, 60, 100 */
    ASSERT_EQ(mock_midi_bytes[0], 0x90);
    ASSERT_EQ(mock_midi_bytes[1], 60);
    ASSERT_EQ(mock_midi_bytes[2], 100);

    /* Verify note-off: 0x80, 60, 0 */
    ASSERT_EQ(mock_midi_bytes[3], 0x80);
    ASSERT_EQ(mock_midi_bytes[4], 60);
    ASSERT_EQ(mock_midi_bytes[5], 0);

    /* Verify second note-on: 0x91, 48, 80 */
    ASSERT_EQ(mock_midi_bytes[6], 0x91);
    ASSERT_EQ(mock_midi_bytes[7], 48);
    ASSERT_EQ(mock_midi_bytes[8], 80);
}

TEST(test_midi_buf_clear_discards) {
    midi_buf_clear();
    midi_buf_note_on(0, 60, 100);
    midi_buf_clear();
    midi_buf_flush();
    ASSERT_EQ(mock_midi_len, 0);
}

TEST(test_midi_buf_flush_clears_buffer) {
    midi_buf_clear();
    midi_buf_note_on(0, 60, 100);
    midi_buf_flush();
    ASSERT_EQ(mock_midi_len, 3);

    /* Second flush should send nothing new */
    int prev_len = mock_midi_len;
    midi_buf_flush();
    ASSERT_EQ(mock_midi_len, prev_len);
}

TEST(test_midi_buf_multiple_channels) {
    midi_buf_clear();
    for (int ch = 0; ch < 8; ch++) {
        midi_buf_note_on(ch, 36 + ch, 100);
    }
    midi_buf_flush();
    /* 8 note-ons × 3 bytes = 24 bytes */
    ASSERT_EQ(mock_midi_len, 24);

    /* Verify each channel */
    for (int ch = 0; ch < 8; ch++) {
        ASSERT_EQ(mock_midi_bytes[ch * 3], 0x90 | ch);
        ASSERT_EQ(mock_midi_bytes[ch * 3 + 1], 36 + ch);
        ASSERT_EQ(mock_midi_bytes[ch * 3 + 2], 100);
    }
}

TEST(test_midi_buf_channel_mask) {
    midi_buf_clear();
    midi_buf_note_on(15, 60, 127);
    midi_buf_flush();
    /* Channel 15: status = 0x9F */
    ASSERT_EQ(mock_midi_bytes[0], 0x9F);
}

TEST(test_midi_buf_note_off_format) {
    midi_buf_clear();
    midi_buf_note_off(3, 72);
    midi_buf_flush();
    ASSERT_EQ(mock_midi_bytes[0], 0x83); /* 0x80 | 3 */
    ASSERT_EQ(mock_midi_bytes[1], 72);
    ASSERT_EQ(mock_midi_bytes[2], 0);    /* velocity 0 */
}

TEST(test_midi_send_all_notes_off) {
    midi_send_all_notes_off();
    ASSERT_EQ(mock_midi_len, 16 * 6);

    ASSERT_EQ(mock_midi_bytes[0], 0xB0);
    ASSERT_EQ(mock_midi_bytes[1], MIDI_CC_ALL_SOUND_OFF);
    ASSERT_EQ(mock_midi_bytes[2], 0);
    ASSERT_EQ(mock_midi_bytes[3], 0xB0);
    ASSERT_EQ(mock_midi_bytes[4], MIDI_CC_ALL_NOTES_OFF);
    ASSERT_EQ(mock_midi_bytes[5], 0);

    ASSERT_EQ(mock_midi_bytes[mock_midi_len - 6], 0xBF);
    ASSERT_EQ(mock_midi_bytes[mock_midi_len - 5], MIDI_CC_ALL_SOUND_OFF);
    ASSERT_EQ(mock_midi_bytes[mock_midi_len - 4], 0);
    ASSERT_EQ(mock_midi_bytes[mock_midi_len - 3], 0xBF);
    ASSERT_EQ(mock_midi_bytes[mock_midi_len - 2], MIDI_CC_ALL_NOTES_OFF);
    ASSERT_EQ(mock_midi_bytes[mock_midi_len - 1], 0);
}

TEST(test_midi_clear_output_queue_clears_staged_buffer) {
    midi_buf_clear();
    midi_buf_note_on(0, 60, 100);
    midi_clear_output_queue();
    midi_buf_flush();
    ASSERT_EQ(mock_midi_len, 0);
}

/* ── gate_commit tests ───────────────────────────────────── */

/* Gate pins: 14, 15, 16, 17 (from cv_gate.c) */

TEST(test_gate_commit_on) {
    /* Turn on channels 0 and 2 */
    gate_commit(0x05, 0x00);  /* on_mask = 0b0101, off_mask = 0 */
    ASSERT_TRUE(mock_gpio[14]);   /* ch 0 → pin 14 */
    ASSERT_FALSE(mock_gpio[15]);  /* ch 1 → pin 15 not set */
    ASSERT_TRUE(mock_gpio[16]);   /* ch 2 → pin 16 */
    ASSERT_FALSE(mock_gpio[17]);  /* ch 3 → pin 17 not set */
}

TEST(test_gate_commit_off) {
    /* Pre-set some gates high */
    mock_gpio[14] = 1;
    mock_gpio[15] = 1;
    /* Turn off ch 0 and ch 1 */
    gate_commit(0x00, 0x03);
    ASSERT_FALSE(mock_gpio[14]);
    ASSERT_FALSE(mock_gpio[15]);
}

TEST(test_gate_commit_simultaneous_on_off) {
    mock_gpio[14] = 1;  /* ch 0 on */
    mock_gpio[16] = 0;  /* ch 2 off */
    /* Turn off ch 0, turn on ch 2 in one call */
    gate_commit(0x04, 0x01);
    ASSERT_FALSE(mock_gpio[14]);
    ASSERT_TRUE(mock_gpio[16]);
}

TEST(test_gate_commit_no_ops) {
    mock_gpio[14] = 1;
    /* Empty masks should not change anything */
    gate_commit(0x00, 0x00);
    ASSERT_TRUE(mock_gpio[14]);
    ASSERT_EQ(op_log_len, 0);  /* No GPIO calls made */
}

TEST(test_gate_commit_all_channels) {
    gate_commit(0x0F, 0x00);  /* All 4 on */
    ASSERT_TRUE(mock_gpio[14]);
    ASSERT_TRUE(mock_gpio[15]);
    ASSERT_TRUE(mock_gpio[16]);
    ASSERT_TRUE(mock_gpio[17]);
}

/* ── Commit ordering tests ───────────────────────────────── */

TEST(test_commit_order_gates_before_midi) {
    /*
     * Simulate the two-phase commit sequence:
     * 1. Buffer some MIDI
     * 2. Call gate_commit
     * 3. Call midi_buf_flush
     * Verify GPIO ops precede UART ops in the log.
     */
    midi_buf_clear();
    midi_buf_note_on(0, 60, 100);
    midi_buf_note_on(1, 48, 80);

    /* Phase 2: commit */
    gate_commit(0x03, 0x00);  /* ch 0,1 on → GPIO_SET */
    midi_buf_flush();         /* → UART bytes */

    /* Find the first GPIO op and first UART op */
    int first_gpio = -1, first_uart = -1;
    for (int i = 0; i < op_log_len; i++) {
        if ((op_log[i].type == OP_GPIO_SET || op_log[i].type == OP_GPIO_CLR) && first_gpio < 0)
            first_gpio = i;
        if (op_log[i].type == OP_UART_BYTE && first_uart < 0)
            first_uart = i;
    }
    ASSERT_TRUE(first_gpio >= 0);
    ASSERT_TRUE(first_uart >= 0);
    /* Gates must come before MIDI */
    ASSERT_TRUE(first_gpio < first_uart);
}

TEST(test_commit_order_gate_off_before_on) {
    /*
     * When gate_commit has both off and on masks, off should
     * execute first (clear before set) to ensure retriggering
     * works for CV instruments that need to see a gate-off→gate-on.
     */
    gate_commit(0x01, 0x02);  /* on ch0, off ch1 */
    ASSERT_TRUE(op_log_len >= 2);
    /* First op should be CLR (off), second SET (on) */
    ASSERT_EQ(op_log[0].type, OP_GPIO_CLR);
    ASSERT_EQ(op_log[1].type, OP_GPIO_SET);
}

/* ── Timing constants ────────────────────────────────────── */

TEST(test_96ppqn_uses_24_pulses_per_16th_step) {
    ASSERT_EQ(PPQN, 96);
    ASSERT_EQ(PULSES_PER_STEP, 24);
}

/* ── Main ────────────────────────────────────────────────── */

int main(void) {
    printf("\n=== rebirth sync unit tests ===\n\n");

    /* MIDI buffer */
    RUN(test_midi_buf_no_send_until_flush);
    RUN(test_midi_buf_flush_sends_all);
    RUN(test_midi_buf_clear_discards);
    RUN(test_midi_buf_flush_clears_buffer);
    RUN(test_midi_buf_multiple_channels);
    RUN(test_midi_buf_channel_mask);
    RUN(test_midi_buf_note_off_format);
    RUN(test_midi_send_all_notes_off);
    RUN(test_midi_clear_output_queue_clears_staged_buffer);

    /* Gate commit */
    RUN(test_gate_commit_on);
    RUN(test_gate_commit_off);
    RUN(test_gate_commit_simultaneous_on_off);
    RUN(test_gate_commit_no_ops);
    RUN(test_gate_commit_all_channels);

    /* Commit ordering */
    RUN(test_commit_order_gates_before_midi);
    RUN(test_commit_order_gate_off_before_on);

    /* Timing constants */
    RUN(test_96ppqn_uses_24_pulses_per_16th_step);

    printf("\n  %d/%d tests passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
