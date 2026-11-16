#ifndef RGB_LEDS_H
#define RGB_LEDS_H

#include <stdbool.h>
#include "led_frame.h"

/* SK6812 chain on one PIO state machine, fed by one DMA channel. Core 0 only. */

#define RGB_LEDS_PIN 27

// 800 kHz, GRB 24-bit. Claims a state machine and a DMA channel and sends an
// all-off frame.
void rgb_leds_init(void);

// Starts sending the frame and returns at once. false = the previous frame is
// not finished (DMA, PIO FIFO, or less than 3 ms on the wire), so this one is
// skipped.
bool rgb_leds_show(const rgb_t frame[LED_COUNT]);

#endif
