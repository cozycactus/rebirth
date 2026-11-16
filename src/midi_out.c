#ifndef MOCK_HARDWARE
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#else
#include <stdint.h>
void uart_putc_raw(void *uart, uint8_t c);
#define uart0 ((void*)0)
#define __not_in_flash_func(name) name
#endif
#include "sequencer.h"

// ─── Configuration ──────────────────────────────────────────────

#define MIDI_UART       uart0
#define MIDI_TX_PIN     0
#define MIDI_RX_PIN     1   // For future MIDI input/sync

// ─── UART output buffer ─────────────────────────────────────────
// Buffer MIDI messages during step processing, flush all at once.

#define MIDI_BUF_SIZE   160
#define MIDI_TX_QUEUED_MAX (MIDI_TX_QUEUE_SIZE - 1u)   // bytes the TX queue can hold
#define MIDI_RT_QUEUE_SIZE 4

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
// The queue keeps whole channel messages with their status; the drain applies running status.
static uint8_t midi_tx_message_left;    // bytes of the message being written still queued
static uint8_t midi_tx_last_status;     // last channel status written; 0 = none
static bool midi_tx_ran_empty;          // the queue emptied after that status was written
// Start, Continue and Stop wait here and leave at the next message boundary, ahead of
// the note queue, then a pending Clock. Neither changes the running status.
static uint8_t midi_rt_queue[MIDI_RT_QUEUE_SIZE];
static uint8_t midi_rt_head;
static uint8_t midi_rt_count;
static bool midi_clock_pending;         // a flag, not a count: Clocks never leave in a bunch

static inline uint16_t midi_tx_next(uint16_t idx) {
    return (uint16_t)((idx + 1u) & (MIDI_TX_QUEUE_SIZE - 1u));
}

static inline uint8_t midi_message_length(uint8_t status) {
    return ((status & 0xE0u) == 0xC0u) ? 2u : 3u; // 0xC0-0xDF carry one data byte
}

static inline bool midi_tx_pending_locked(void) {
    return midi_tx_tail != midi_tx_head || midi_rt_count != 0 || midi_clock_pending;
}

static uint32_t midi_tx_lock(void) {
    if (!midi_tx_spinlock_ready) return 0;
    return spin_lock_blocking(spin_lock_instance(midi_tx_spinlock_num));
}

static void midi_tx_unlock(uint32_t irq_state) {
    if (!midi_tx_spinlock_ready) return;
    spin_unlock(spin_lock_instance(midi_tx_spinlock_num), irq_state);
}

// The writable check, dequeue, write and IRQ mask change share one lock across
// both cores and the UART ISR. No second consumer can overtake a byte.
// One byte per writable check; with the UART FIFO off the UART holds at most the
// byte on the line and one in its holding register. A pending Clock may wait for
// the byte being shifted out and a complete channel message whose status is
// already in the holding register: four byte times, 1.28 ms, before interrupt
// latency. At a message boundary a queued Start/Continue/Stop goes
// first, then a pending Clock, then the next message.
// A message whose status equals the last one written goes without it, unless the
// queue ran empty since: the first message after a gap always carries its status,
// so a receiver that lost a byte resyncs.
// The MIDI path the timer ISR and the UART interrupt run is in RAM: the drain, the
// TX interrupt handler, the sends and the step buffer.
static void __not_in_flash_func(midi_tx_drain_locked)(void) {
    while (uart_is_writable(MIDI_UART)) {
        if (midi_tx_message_left == 0) {
            if (midi_rt_count != 0) {
                uart_putc_raw(MIDI_UART, midi_rt_queue[midi_rt_head]);
                midi_rt_head = (uint8_t)((midi_rt_head + 1u) % MIDI_RT_QUEUE_SIZE);
                midi_rt_count--;
                continue;
            }
            if (midi_clock_pending) {
                midi_clock_pending = false;
                uart_putc_raw(MIDI_UART, MIDI_CLOCK);
                continue;
            }
            if (midi_tx_tail == midi_tx_head) break;
            uint8_t status = midi_tx_queue[midi_tx_tail];
            midi_tx_message_left = midi_message_length(status);
            if (status == midi_tx_last_status && !midi_tx_ran_empty) {
                midi_tx_tail = midi_tx_next(midi_tx_tail); // running status
                midi_tx_message_left--;
            } else {
                midi_tx_last_status = status;
                midi_tx_ran_empty = false;
            }
        }
        uart_putc_raw(MIDI_UART, midi_tx_queue[midi_tx_tail]);
        midi_tx_tail = midi_tx_next(midi_tx_tail);
        midi_tx_message_left--;
        if (midi_tx_tail == midi_tx_head) midi_tx_ran_empty = true;
    }
    uart_set_irq_enables(MIDI_UART, false, midi_tx_pending_locked());
}

// Queues whole messages if the queue then holds at most `max_queued` bytes
// (MIDI_TX_QUEUED_MAX or less).
static bool midi_tx_enqueue_locked(const uint8_t *data, uint16_t length, uint16_t max_queued) {
    // Move bytes into a UART that has room before its TX IRQ runs.
    midi_tx_drain_locked();
    uint16_t queued = (midi_tx_head - midi_tx_tail) & (MIDI_TX_QUEUE_SIZE - 1u);
    if ((uint32_t)queued + length > max_queued) return false;
    // Publish complete messages/batches, never a partial MIDI packet.
    for (uint16_t i = 0; i < length; i++) {
        midi_tx_queue[midi_tx_head] = data[i];
        midi_tx_head = midi_tx_next(midi_tx_head);
    }
    midi_tx_drain_locked();
    return true;
}

static void midi_tx_reset_locked(void) {
    midi_tx_tail = midi_tx_head;
    midi_tx_message_left = 0;
    midi_tx_last_status = 0;
    midi_tx_ran_empty = true;
    midi_rt_head = 0;
    midi_rt_count = 0;
    midi_clock_pending = false;
}

static void __isr __not_in_flash_func(midi_uart_irq_handler)(void) {
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
    midi_tx_reset_locked();
    midi_tx_dropped_bytes = 0;

    uart_init(MIDI_UART, MIDI_BAUD);
    gpio_set_function(MIDI_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(MIDI_RX_PIN, GPIO_FUNC_UART);

    // MIDI is 8N1
    uart_set_format(MIDI_UART, 8, 1, UART_PARITY_NONE);
    // One byte at a time: a Clock waits at most for the byte being shifted out
    // and one whole message, four byte times (1.28 ms).
    uart_set_fifo_enabled(MIDI_UART, false);

    irq_set_exclusive_handler(UART0_IRQ, midi_uart_irq_handler);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(MIDI_UART, false, false);
#endif

    midi_buf_len = 0;
    midi_overflow = false;
}

// ─── Low-level send ─────────────────────────────────────────────

void __not_in_flash_func(midi_send_bytes)(const uint8_t *data, uint16_t length) {
#ifndef MOCK_HARDWARE
    uint32_t lock_state = midi_tx_lock();
    if (!midi_tx_enqueue_locked(data, length, MIDI_TX_QUEUED_MAX)) {
        midi_tx_dropped_bytes += length;
        midi_overflow = true;
    }
    midi_tx_unlock(lock_state);
#else
    for (uint16_t i = 0; i < length; i++) uart_putc_raw(MIDI_UART, data[i]);
#endif
}

// In RAM: the timer ISR calls it for every flam's second hits.
bool __not_in_flash_func(midi_try_send_bytes_capped)(const uint8_t *data, uint16_t length,
                                                     uint16_t max_queued) {
#ifndef MOCK_HARDWARE
    if (max_queued > MIDI_TX_QUEUED_MAX) max_queued = MIDI_TX_QUEUED_MAX;
    uint32_t lock_state = midi_tx_lock();
    bool queued = midi_tx_enqueue_locked(data, length, max_queued);
    midi_tx_unlock(lock_state);
    return queued;
#else
    (void)max_queued;
    for (uint16_t i = 0; i < length; i++) uart_putc_raw(MIDI_UART, data[i]);
    return true;
#endif
}

static void midi_send_realtime(uint8_t byte, bool cancel_clock) {
#ifndef MOCK_HARDWARE
    uint32_t lock_state = midi_tx_lock();
    if (midi_rt_count < MIDI_RT_QUEUE_SIZE) {
        midi_rt_queue[(midi_rt_head + midi_rt_count) % MIDI_RT_QUEUE_SIZE] = byte;
        midi_rt_count++;
    } else {
        midi_tx_dropped_bytes++;
        midi_overflow = true;
    }
    if (cancel_clock) midi_clock_pending = false;
    midi_tx_drain_locked();
    midi_tx_unlock(lock_state);
#else
    (void)cancel_clock;
    uart_putc_raw(MIDI_UART, byte);
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

void __not_in_flash_func(midi_buf_clear)(void) {
    midi_buf_len = 0;
}

void __not_in_flash_func(midi_buf_flush)(void) {
    midi_send_bytes(midi_buf, midi_buf_len);
    midi_buf_len = 0;
}

void midi_clear_output_queue(void) {
    midi_buf_len = 0;
#ifndef MOCK_HARDWARE
    uint32_t lock_state = midi_tx_lock();
    midi_tx_reset_locked();
    midi_overflow = false;
    uart_set_irq_enables(MIDI_UART, false, false);
    midi_tx_unlock(lock_state);
#else
    midi_overflow = false;
#endif
}

bool __not_in_flash_func(midi_output_overflowed)(void) {
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
        pending = midi_tx_pending_locked();
        midi_tx_unlock(lock_state);
    } while (pending);
    uart_tx_wait_blocking(MIDI_UART);
#endif
}

void __not_in_flash_func(midi_buf_note_on)(uint8_t channel, uint8_t note, uint8_t velocity) {
    midi_buf_message(MIDI_NOTE_ON | (channel & 0x0F), note & 0x7F, velocity & 0x7F);
}

void __not_in_flash_func(midi_buf_note_off)(uint8_t channel, uint8_t note) {
    midi_buf_message(MIDI_NOTE_OFF | (channel & 0x0F), note & 0x7F, 0);
}

// ─── MIDI Messages ──────────────────────────────────────────────

void midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    const uint8_t message[] = {MIDI_NOTE_ON | (channel & 0x0F), note & 0x7F, velocity & 0x7F};
    midi_send_bytes(message, sizeof(message));
}

void __not_in_flash_func(midi_send_note_off)(uint8_t channel, uint8_t note) {
    const uint8_t message[] = {MIDI_NOTE_OFF | (channel & 0x0F), note & 0x7F, 0};
    midi_send_bytes(message, sizeof(message));
}

void midi_send_cc(uint8_t channel, uint8_t cc, uint8_t value) {
    const uint8_t message[] = {MIDI_CC | (channel & 0x0F), cc & 0x7F, value & 0x7F};
    midi_send_bytes(message, sizeof(message));
}

void __not_in_flash_func(midi_send_clock)(void) {
#ifndef MOCK_HARDWARE
    uint32_t lock_state = midi_tx_lock();
    midi_clock_pending = true;
    midi_tx_drain_locked();
    midi_tx_unlock(lock_state);
#else
    uart_putc_raw(MIDI_UART, MIDI_CLOCK);
#endif
}

void midi_send_start(void) {
    midi_send_realtime(MIDI_START, false);
}

// No Clock leaves after a Stop: it is cancelled in the same lock section.
void midi_send_stop(void) {
    midi_send_realtime(MIDI_STOP, true);
}

void midi_send_continue(void) {
    midi_send_realtime(MIDI_CONTINUE, false);
}

// One send for all 32 messages keeps Panic to one short TX lock section.
void midi_send_all_notes_off(void) {
    uint8_t messages[16 * 6];
    for (uint8_t ch = 0; ch < 16; ch++) {
        uint8_t *m = &messages[ch * 6];
        m[0] = MIDI_CC | ch;
        m[1] = MIDI_CC_ALL_SOUND_OFF;
        m[2] = 0;
        m[3] = MIDI_CC | ch;
        m[4] = MIDI_CC_ALL_NOTES_OFF;
        m[5] = 0;
    }
    midi_send_bytes(messages, sizeof(messages));
}
