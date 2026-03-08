#ifndef MOCK_HARDWARE
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "tusb.h"
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

static uint8_t midi_buf[MIDI_BUF_SIZE];
static uint8_t midi_buf_len = 0;

// ─── USB MIDI TX ring buffer ────────────────────────────────────
// Single-producer (Core 1 ISR) / single-consumer (Core 0 main loop).
// Must be a power of 2 so the index wrap uses a bitmask.

#define USB_TX_SIZE     256u    // bytes — comfortably handles bursts

#ifndef MOCK_HARDWARE
static uint8_t          usb_tx_buf[USB_TX_SIZE];
static volatile uint8_t usb_tx_head = 0;   // written by Core 1
static volatile uint8_t usb_tx_tail = 0;   // written by Core 0

static inline void usb_tx_push(uint8_t b) {
    uint8_t next = (uint8_t)((usb_tx_head + 1u) & (USB_TX_SIZE - 1u));
    if (next != usb_tx_tail) {
        usb_tx_buf[usb_tx_head] = b;
        __dmb();                    // store data before advancing head
        usb_tx_head = next;
    }
}
#endif

// ─── Init ───────────────────────────────────────────────────────

void midi_init(void) {
#ifndef MOCK_HARDWARE
    uart_init(MIDI_UART, MIDI_BAUD);
    gpio_set_function(MIDI_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(MIDI_RX_PIN, GPIO_FUNC_UART);

    // MIDI is 8N1
    uart_set_format(MIDI_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(MIDI_UART, true);

    // TinyUSB MIDI device
    tud_init(0);
#endif

    midi_buf_len = 0;
}

// ─── USB MIDI task (call from Core 0 main loop) ─────────────────

void midi_usb_task(void) {
#ifndef MOCK_HARDWARE
    tud_task();

    if (!tud_midi_mounted()) return;

    // Drain the ring buffer into TinyUSB one byte at a time.
    // tud_midi_stream_write handles the USB MIDI packet framing.
    while (usb_tx_tail != usb_tx_head) {
        uint8_t b = usb_tx_buf[usb_tx_tail];
        __dmb();    // read data before advancing tail
        usb_tx_tail = (uint8_t)((usb_tx_tail + 1u) & (USB_TX_SIZE - 1u));
        tud_midi_stream_write(0, &b, 1);
    }
#endif
}

// ─── Low-level send ─────────────────────────────────────────────

static inline void midi_send_byte(uint8_t b) {
    uart_putc_raw(MIDI_UART, b);
#ifndef MOCK_HARDWARE
    usb_tx_push(b);
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
    for (uint8_t i = 0; i < midi_buf_len; i++) {
        midi_send_byte(midi_buf[i]);
    }
    midi_buf_len = 0;
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
