#ifndef SIM_INTERNAL_H
#define SIM_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "sim_api.h"

#define SIM_UI_PASS_US 1000u    // assumed core 0 loop period; not measured on hardware

extern uint64_t sim_now_us;

void hal_reset(bool keep_flash);
void hal_set_now(uint64_t t_us);
uint64_t hal_alarm_due(void);       // UINT64_MAX when no alarm is armed
void hal_run_alarm(void);
uint64_t hal_uart_irq_due(void);    // UINT64_MAX when the TX interrupt is off
uint64_t hal_input_due(void);       // UINT64_MAX when no input is scheduled
void hal_run_uart_irq(void);
void hal_fatal(const char *message);

void sim_rgb_reset(void);

#endif
