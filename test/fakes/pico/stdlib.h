#ifndef REBIRTH_FAKE_STDLIB_H
#define REBIRTH_FAKE_STDLIB_H
#include <stdbool.h>
#include <stdint.h>

#define __isr
#define __time_critical_func(name) name
#define __not_in_flash_func(name) name
#define GPIO_IN 0
#define GPIO_OUT 1
#define GPIO_FUNC_UART 2
#define GPIO_FUNC_SPI 3
#define GPIO_FUNC_I2C 4

void gpio_init(uint8_t pin);
void gpio_set_dir(uint8_t pin, bool output);
void gpio_set_function(uint8_t pin, int function);
void gpio_put(uint8_t pin, bool value);
bool gpio_get(uint8_t pin);
void gpio_pull_up(uint8_t pin);
void gpio_disable_pulls(uint8_t pin);
extern bool fake_gpio_level[32];
extern bool fake_gpio_pulled_up[32];
extern bool fake_gpio_pulls_disabled[32];
void gpio_set_mask(uint32_t mask);
void gpio_clr_mask(uint32_t mask);
uint64_t get_absolute_time(void);
uint32_t to_ms_since_boot(uint64_t time);
void sleep_ms(uint32_t milliseconds);
void fake_wait_for_interrupt(void);
#define __wfi fake_wait_for_interrupt

#endif
