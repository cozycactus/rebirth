#ifndef REBIRTH_FAKE_HARDWARE_H
#define REBIRTH_FAKE_HARDWARE_H
#include <stdbool.h>
#include <stdint.h>
#include "hardware/sync.h"
#define FAKE_MIDI_CAPACITY 65536
extern uint8_t fake_midi[FAKE_MIDI_CAPACITY];
extern unsigned fake_midi_length;
extern bool fake_uart_writable;
extern unsigned fake_uart_budget;       // bytes the UART takes before it reports not writable;
                                        // UINT_MAX (no limit) after reset
extern bool fake_uart_fifo_enabled;     // last value passed to uart_set_fifo_enabled;
                                        // uart_init() enables the FIFO, as the SDK does
extern bool fake_uart_tx_irq;
extern unsigned fake_uart_wait_midi_length; // fake_midi_length at the last uart_tx_wait_blocking()
extern bool fake_uart_rx_irq;           // the receive interrupt enable, as the transmit one
// Bytes received on the input, as the UART's data register would give them: a queue, not
// the PL011's one-byte holding register. The interrupt handler is not run by a push.
void fake_uart_rx_push(const uint8_t *bytes, unsigned count);
unsigned fake_uart_rx_pending(void);    // bytes pushed and not yet read
extern uint32_t fake_midi_time_us[FAKE_MIDI_CAPACITY]; // wire start time of each byte (wire-time mode)
typedef struct { uint8_t status, data1, data2; } fake_midi_msg_t;
// Decodes fake_midi into at most `max` messages and returns how many it stored: running
// status expanded, real-time bytes become {byte, 0, 0} in place. A message cut short by a
// status byte or by the end is left out; a data byte with no status before it aborts.
unsigned fake_midi_decode(fake_midi_msg_t *out, unsigned max);
unsigned fake_midi_count(uint8_t status); // decoded messages with this status byte
// Wire-time mode, from the first call until fake_hardware_reset(): the UART sends one byte
// per 320 us of simulated time (timer_hw->timerawl). It models the PL011 with its FIFO off,
// a shift register and a one-byte holding register: it is writable one byte time before the
// line is free, so up to two bytes are committed (the one on the line and the one waiting).
// Each byte starts on the line at max(the moment it was written, the previous byte's end);
// fake_midi_time_us records that start. This call drains the queue through the TX IRQ
// handler, waking it each time the holding register empties, up to (not including)
// `until_us`, then sets the time to `until_us`. Unused idle time is not banked: a byte never
// starts before the moment it was written. Time moves only here and where a test sets
// timer_hw->timerawl, so code that busy-waits on the UART (midi_wait_output_idle) must not
// run in this mode.
void fake_uart_run_until(uint32_t until_us);
extern uint32_t fake_gates;
extern unsigned fake_gate_rises;
extern void (*fake_before_gate_commit)(void);
extern void (*fake_before_lock)(spin_lock_t *lock);
extern void (*fake_before_lockout)(void);
#include "hardware/irq.h"
extern void (*fake_irq_handlers[FAKE_IRQ_COUNT])(void);
void fake_hardware_reset(void);
#endif
