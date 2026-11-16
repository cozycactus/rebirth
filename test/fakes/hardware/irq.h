#include <stdbool.h>
#define TIMER_IRQ_0 0
#define UART0_IRQ 1
void irq_set_exclusive_handler(int irq, void (*handler)(void));
void irq_set_enabled(int irq, bool enabled);
void irq_clear(int irq);
