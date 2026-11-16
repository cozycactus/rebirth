#ifndef REBIRTH_FAKE_HARDWARE_H
#define REBIRTH_FAKE_HARDWARE_H
#include <stdbool.h>
#include <stdint.h>
#include "hardware/sync.h"
#define FAKE_MIDI_CAPACITY 65536
extern uint8_t fake_midi[FAKE_MIDI_CAPACITY];
extern unsigned fake_midi_length;
extern bool fake_uart_writable;
extern bool fake_uart_tx_irq;
extern uint32_t fake_gates;
extern unsigned fake_gate_rises;
extern void (*fake_before_gate_commit)(void);
extern void (*fake_before_lock)(spin_lock_t *lock);
extern void (*fake_irq_handlers[2])(void);
void fake_hardware_reset(void);
#endif
