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
static volatile bool midi_overflow;

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

// The FIFO check, dequeue, write and IRQ mask change share one lock across
// both cores and the UART ISR. No second consumer can overtake a byte.
static void midi_tx_drain_locked(void) {
    while (uart_is_writable(MIDI_UART) && midi_tx_tail != midi_tx_head) {
        uart_putc_raw(MIDI_UART, midi_tx_queue[midi_tx_tail]);
        midi_tx_tail = midi_tx_next(midi_tx_tail);
    }
    uart_set_irq_enables(MIDI_UART, false, midi_tx_tail != midi_tx_head);
}

static void __isr midi_uart_irq_handler(void) {
    uint32_t lock_state = midi_tx_lock();
    midi_tx_drain_locked();
    midi_tx_unlock(lock_state);
}
#endif

// ─── Init ───────────────────────────────────────────────────────

void midi_init(void) {
#ifndef MOCK_HARDWARE
    if (!midi_tx_spinlock_ready) midi_tx_spinlock_num = spin_lock_claim_unused(true);
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
    midi_overflow = false;
}

// ─── Low-level send ─────────────────────────────────────────────

static void midi_send_message(const uint8_t *data, uint16_t length) {
#ifndef MOCK_HARDWARE
    uint32_t lock_state = midi_tx_lock();
    // Move bytes into a UART FIFO that has room before its TX IRQ runs.
    midi_tx_drain_locked();
    uint16_t free_bytes = (midi_tx_tail - midi_tx_head - 1u) & (MIDI_TX_QUEUE_SIZE - 1u);
    if (length > free_bytes) {
        midi_tx_dropped_bytes += length;
        midi_overflow = true;
    } else {
        // Publish complete messages/batches, never a partial MIDI packet.
        for (uint16_t i = 0; i < length; i++) {
            midi_tx_queue[midi_tx_head] = data[i];
            midi_tx_head = midi_tx_next(midi_tx_head);
        }
    }
    midi_tx_drain_locked();
    midi_tx_unlock(lock_state);
#else
    for (uint16_t i = 0; i < length; i++) uart_putc_raw(MIDI_UART, data[i]);
#endif
}

// ─── Buffered output ────────────────────────────────────────────

static void midi_buf_message(uint8_t status, uint8_t data1, uint8_t data2) {
    if (midi_buf_len + 3u <= MIDI_BUF_SIZE) {
        midi_buf[midi_buf_len++] = status;
        midi_buf[midi_buf_len++] = data1;
        midi_buf[midi_buf_len++] = data2;
    } else {
        midi_overflow = true;
    }
}

void midi_buf_clear(void) {
    midi_buf_len = 0;
}

void midi_buf_flush(void) {
    midi_send_message(midi_buf, midi_buf_len);
    midi_buf_len = 0;
}

void midi_clear_output_queue(void) {
    midi_buf_len = 0;
#ifndef MOCK_HARDWARE
    uint32_t lock_state = midi_tx_lock();
    midi_tx_tail = midi_tx_head;
    midi_overflow = false;
    uart_set_irq_enables(MIDI_UART, false, false);
    midi_tx_unlock(lock_state);
#else
    midi_overflow = false;
#endif
}

bool midi_output_overflowed(void) {
#ifndef MOCK_HARDWARE
    uint32_t lock_state = midi_tx_lock();
    bool overflowed = midi_overflow;
    midi_tx_unlock(lock_state);
    return overflowed;
#else
    return midi_overflow;
#endif
}

void midi_wait_output_idle(void) {
#ifndef MOCK_HARDWARE
    bool pending;
    do {
        uint32_t lock_state = midi_tx_lock();
        midi_tx_drain_locked();
        pending = midi_tx_tail != midi_tx_head;
        midi_tx_unlock(lock_state);
    } while (pending);
    uart_tx_wait_blocking(MIDI_UART);
#endif
}

void midi_buf_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    midi_buf_message(MIDI_NOTE_ON | (channel & 0x0F), note & 0x7F, velocity & 0x7F);
}

void midi_buf_note_off(uint8_t channel, uint8_t note) {
    midi_buf_message(MIDI_NOTE_OFF | (channel & 0x0F), note & 0x7F, 0);
}

// ─── MIDI Messages ──────────────────────────────────────────────

void midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    const uint8_t message[] = {MIDI_NOTE_ON | (channel & 0x0F), note & 0x7F, velocity & 0x7F};
    midi_send_message(message, sizeof(message));
}

void midi_send_note_off(uint8_t channel, uint8_t note) {
    const uint8_t message[] = {MIDI_NOTE_OFF | (channel & 0x0F), note & 0x7F, 0};
    midi_send_message(message, sizeof(message));
}

void midi_send_cc(uint8_t channel, uint8_t cc, uint8_t value) {
    const uint8_t message[] = {MIDI_CC | (channel & 0x0F), cc & 0x7F, value & 0x7F};
    midi_send_message(message, sizeof(message));
}

void midi_send_clock(void) {
    const uint8_t message = MIDI_CLOCK;
    midi_send_message(&message, 1);
}

void midi_send_start(void) {
    const uint8_t message = MIDI_START;
    midi_send_message(&message, 1);
}

void midi_send_stop(void) {
    const uint8_t message = MIDI_STOP;
    midi_send_message(&message, 1);
}

void midi_send_continue(void) {
    const uint8_t message = MIDI_CONTINUE;
    midi_send_message(&message, 1);
}

void midi_send_all_notes_off(void) {
    for (uint8_t ch = 0; ch < 16; ch++) {
        midi_send_cc(ch, MIDI_CC_ALL_SOUND_OFF, 0);
        midi_send_cc(ch, MIDI_CC_ALL_NOTES_OFF, 0);
    }
}
