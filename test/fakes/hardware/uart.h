#ifndef REBIRTH_FAKE_UART_H
#define REBIRTH_FAKE_UART_H
#include <stdbool.h>
#include <stdint.h>
#define uart0 ((void *)0)
#define UART_PARITY_NONE 0
void uart_init(void *uart, uint32_t baud);
void uart_set_format(void *uart, int bits, int stop, int parity);
void uart_set_fifo_enabled(void *uart, bool enabled);
void uart_set_irq_enables(void *uart, bool rx, bool tx);
bool uart_is_writable(void *uart);
bool uart_is_readable(void *uart);
uint8_t uart_getc(void *uart);
void uart_putc_raw(void *uart, uint8_t value);
void uart_tx_wait_blocking(void *uart);
#endif
