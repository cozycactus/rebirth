#ifndef MOCK_HARDWARE
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#else
#include <stdint.h>
void uart_putc_raw(void *uart, uint8_t c);
#define uart0 ((void*)0)
#endif
#include "sequencer.h"

// ─── Configuration ──────────────────────────────────────────────

#define MIDI_UART       uart0
#define MIDI_TX_PIN     0
#define MIDI_RX_PIN     1   // For future MIDI input/sync

// ─── UART output buffer ─────────────────────────────────────────
// Buffer MIDI messages during step processing, flush all at once.
// Max per step: 8 note-offs (3B) + 8 note-ons (3B) = 48 bytes.

#define MIDI_BUF_SIZE   64
#define MIDI_TX_QUEUE_SIZE 256

static uint8_t midi_buf[MIDI_BUF_SIZE];
static uint8_t midi_buf_len = 0;

#ifndef MOCK_HARDWARE
_Static_assert((MIDI_TX_QUEUE_SIZE & (MIDI_TX_QUEUE_SIZE - 1u)) == 0,
               "MIDI_TX_QUEUE_SIZE must be a power of two");

static uint8_t midi_tx_queue[MIDI_TX_QUEUE_SIZE];
static volatile uint16_t midi_tx_head;
static volatile uint16_t midi_tx_tail;
static uint32_t midi_tx_spinlock_num;
static bool midi_tx_spinlock_ready;
static volatile uint32_t midi_tx_dropped_bytes;

static inline uint16_t midi_tx_next(uint16_t idx) {
    return (uint16_t)((idx + 1u) & (MIDI_TX_QUEUE_SIZE - 1u));
}

static uint32_t midi_tx_lock(void) {
    if (!midi_tx_spinlock_ready) return 0;
    return spin_lock_blocking(spin_lock_instance(midi_tx_spinlock_num));
}

static void midi_tx_unlock(uint32_t irq_state) {
    if (!midi_tx_spinlock_ready) return;
    spin_unlock(spin_lock_instance(midi_tx_spinlock_num), irq_state);
}

static bool midi_tx_pop(uint8_t *out) {
    uint32_t lock_state = midi_tx_lock();
    if (midi_tx_tail == midi_tx_head) {
        midi_tx_unlock(lock_state);
        return false;
    }

    *out = midi_tx_queue[midi_tx_tail];
    midi_tx_tail = midi_tx_next(midi_tx_tail);
    midi_tx_unlock(lock_state);
    return true;
}

static bool midi_tx_pending(void) {
    uint32_t lock_state = midi_tx_lock();
    bool pending = midi_tx_tail != midi_tx_head;
    midi_tx_unlock(lock_state);
    return pending;
}

static void midi_tx_enqueue_byte(uint8_t b) {
    uint32_t lock_state = midi_tx_lock();
    uint16_t next = midi_tx_next(midi_tx_head);

    if (next == midi_tx_tail) {
        midi_tx_dropped_bytes++;
        midi_tx_unlock(lock_state);
        return;
    }

    midi_tx_queue[midi_tx_head] = b;
    midi_tx_head = next;
    midi_tx_unlock(lock_state);
}

static void midi_tx_kick(void) {
    uint8_t b;
    while (uart_is_writable(MIDI_UART) && midi_tx_pop(&b)) {
        uart_putc_raw(MIDI_UART, b);
    }

    uart_set_irq_enables(MIDI_UART, false, midi_tx_pending());
}

static void __isr midi_uart_irq_handler(void) {
    midi_tx_kick();
}
#endif

// ─── Init ───────────────────────────────────────────────────────

void midi_init(void) {
#ifndef MOCK_HARDWARE
    midi_tx_spinlock_num = spin_lock_claim_unused(true);
    midi_tx_spinlock_ready = true;
    midi_tx_head = 0;
    midi_tx_tail = 0;
    midi_tx_dropped_bytes = 0;

    uart_init(MIDI_UART, MIDI_BAUD);
    gpio_set_function(MIDI_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(MIDI_RX_PIN, GPIO_FUNC_UART);

    // MIDI is 8N1
    uart_set_format(MIDI_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(MIDI_UART, true);

    irq_set_exclusive_handler(UART0_IRQ, midi_uart_irq_handler);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(MIDI_UART, false, false);
#endif

    midi_buf_len = 0;
}

// ─── Low-level send ─────────────────────────────────────────────

static inline void midi_send_byte(uint8_t b) {
#ifndef MOCK_HARDWARE
    midi_tx_enqueue_byte(b);
    midi_tx_kick();
#else
    uart_putc_raw(MIDI_UART, b);
#endif
}

// ─── Buffered output ────────────────────────────────────────────

static inline void midi_buf_push(uint8_t b) {
    if (midi_buf_len < MIDI_BUF_SIZE) {
        midi_buf[midi_buf_len++] = b;
    }
}

void midi_buf_clear(void) {
    midi_buf_len = 0;
}

void midi_buf_flush(void) {
#ifndef MOCK_HARDWARE
    for (uint8_t i = 0; i < midi_buf_len; i++) {
        midi_tx_enqueue_byte(midi_buf[i]);
    }
    midi_buf_len = 0;
    midi_tx_kick();
#else
    for (uint8_t i = 0; i < midi_buf_len; i++) {
        midi_send_byte(midi_buf[i]);
    }
    midi_buf_len = 0;
#endif
}

void midi_clear_output_queue(void) {
    midi_buf_len = 0;
#ifndef MOCK_HARDWARE
    uint32_t lock_state = midi_tx_lock();
    midi_tx_tail = midi_tx_head;
    midi_tx_unlock(lock_state);
    uart_set_irq_enables(MIDI_UART, false, false);
#endif
}

void midi_buf_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    midi_buf_push(MIDI_NOTE_ON | (channel & 0x0F));
    midi_buf_push(note & 0x7F);
    midi_buf_push(velocity & 0x7F);
}

void midi_buf_note_off(uint8_t channel, uint8_t note) {
    midi_buf_push(MIDI_NOTE_OFF | (channel & 0x0F));
    midi_buf_push(note & 0x7F);
    midi_buf_push(0);
}

// ─── MIDI Messages ──────────────────────────────────────────────

void midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    midi_send_byte(MIDI_NOTE_ON | (channel & 0x0F));
    midi_send_byte(note & 0x7F);
    midi_send_byte(velocity & 0x7F);
}

void midi_send_note_off(uint8_t channel, uint8_t note) {
    midi_send_byte(MIDI_NOTE_OFF | (channel & 0x0F));
    midi_send_byte(note & 0x7F);
    midi_send_byte(0);
}

void midi_send_cc(uint8_t channel, uint8_t cc, uint8_t value) {
    midi_send_byte(MIDI_CC | (channel & 0x0F));
    midi_send_byte(cc & 0x7F);
    midi_send_byte(value & 0x7F);
}

void midi_send_clock(void) {
    midi_send_byte(MIDI_CLOCK);
}

void midi_send_start(void) {
    midi_send_byte(MIDI_START);
}

void midi_send_stop(void) {
    midi_send_byte(MIDI_STOP);
}

void midi_send_continue(void) {
    midi_send_byte(MIDI_CONTINUE);
}

void midi_send_all_notes_off(void) {
    for (uint8_t ch = 0; ch < 16; ch++) {
        midi_send_cc(ch, MIDI_CC_ALL_SOUND_OFF, 0);
        midi_send_cc(ch, MIDI_CC_ALL_NOTES_OFF, 0);
    }
}
