#ifndef REBIRTH_SH101_BOARD_H
#define REBIRTH_SH101_BOARD_H

#include <stdint.h>

uint8_t *sh101_rom_data(void);
void sh101_reset(void);
void sh101_power_on(void);
void sh101_inputs(uint32_t keys, uint8_t buttons, uint8_t switches, int pedal, int ext_clock);
void sh101_clock(double hz, int external);
double sh101_phase(void);
double sh101_cycles(void);
uint32_t sh101_state(void);
uint32_t sh101_run(uint32_t cycles);
const uint32_t *sh101_events(void);

#endif
