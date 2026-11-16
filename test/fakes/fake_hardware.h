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
extern bool fake_uart_fifo_enabled;     // last value passed to uart_set_fifo_enabled
extern bool fake_uart_tx_irq;
extern uint32_t fake_midi_time_us[FAKE_MIDI_CAPACITY]; // wire start time of each byte (wire-time mode)
typedef struct { uint8_t status, data1, data2; } fake_midi_msg_t;
// Decodes fake_midi into at most `max` messages and returns how many it stored: running
// status expanded, real-time bytes become {byte, 0, 0} in place. A message still incomplete
// at the end is left out; a data byte without any status before it aborts.
unsigned fake_midi_decode(fake_midi_msg_t *out, unsigned max);
unsigned fake_midi_count(uint8_t status); // decoded messages with this status byte
// Wire-time mode, from the first call until fake_hardware_reset(): the UART sends one byte
// per 320 us of simulated time (timer_hw->timerawl) and is writable only when the line is
// free. This call drains it through the TX IRQ handler, byte after byte, from the last
// byte's end up to (not including) `until_us`, then sets the time to `until_us`. Unused idle
// time is not banked: a byte never starts before the moment it was written.
void fake_uart_run_until(uint32_t until_us);
extern uint32_t fake_gates;
extern unsigned fake_gate_rises;
extern void (*fake_before_gate_commit)(void);
extern void (*fake_before_lock)(spin_lock_t *lock);
extern void (*fake_before_lockout)(void);
extern void (*fake_irq_handlers[2])(void);
void fake_hardware_reset(void);
#endif
