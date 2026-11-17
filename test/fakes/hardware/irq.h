#ifndef REBIRTH_FAKE_IRQ_H
#define REBIRTH_FAKE_IRQ_H
#include <stdbool.h>
#define TIMER_IRQ_0 0
#define TIMER_IRQ_1 1
#define UART0_IRQ 2
#define FAKE_IRQ_COUNT 3
void irq_set_exclusive_handler(int irq, void (*handler)(void));
void irq_set_enabled(int irq, bool enabled);
void irq_clear(int irq);
#endif
