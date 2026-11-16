#ifndef MOCK_HARDWARE
#include "pico/stdlib.h"
#include "hardware/spi.h"
#else
#include <stdint.h>
#include <stdbool.h>
void gpio_put(uint8_t pin, bool value);
void gpio_set_mask(uint32_t mask);
void gpio_clr_mask(uint32_t mask);
void spi_write_blocking(void *spi, const uint8_t *buf, int len);
void gpio_init(uint8_t pin);
void gpio_set_dir(uint8_t pin, bool out);
void spi_init(void *spi, uint32_t rate);
void gpio_set_function(uint8_t pin, int func);
#define spi1 ((void*)0)
#define GPIO_FUNC_SPI 0
#define GPIO_OUT 1
#endif
#include "sequencer.h"

// ─── Hardware Configuration ─────────────────────────────────────

#define CV_SPI          spi1
#define CV_SCK_PIN      10
#define CV_MOSI_PIN     11
#define CV_CS0_PIN      12      // MCP4822 #0 (CV channels 0,1)
#define CV_CS1_PIN      13      // MCP4822 #1 (CV channels 2,3)

#define GATE_PIN_0      14
#define GATE_PIN_1      15
#define GATE_PIN_2      16
#define GATE_PIN_3      17

static const uint8_t gate_pins[NUM_CV_CHANNELS] = {
    GATE_PIN_0, GATE_PIN_1, GATE_PIN_2, GATE_PIN_3
};

static const uint8_t cs_pins[2] = { CV_CS0_PIN, CV_CS1_PIN };

// ─── MCP4822 Protocol ───────────────────────────────────────────
//
// 16-bit SPI word:
//   Bit 15: Channel select (0=A, 1=B)
//   Bit 14: unused
//   Bit 13: Gain (0=2x, 1=1x); internal Vref is 2.048 V
//   Bit 12: Shutdown (1=active, 0=shutdown)
//   Bits 11–0: Data (12-bit DAC value)

_Static_assert(CV_DAC_GAIN == 1 || CV_DAC_GAIN == 2, "MCP4822 gain must be 1 or 2");

static void mcp4822_write(uint8_t dac_index, uint8_t channel, uint16_t value) {
    uint8_t cs_pin = cs_pins[dac_index];

    uint16_t word = 0;
    word |= (channel & 1) << 15;   // Channel A or B
    if (CV_DAC_GAIN == 1) word |= 1 << 13;
    word |= 1 << 12;                // Active (not shutdown)
    word |= (value & 0x0FFF);       // 12-bit data

    uint8_t buf[2] = { word >> 8, word & 0xFF };

    gpio_put(cs_pin, 0);
    spi_write_blocking(CV_SPI, buf, 2);
    gpio_put(cs_pin, 1);
}

// 1 V/octave, MIDI note 24 = 0 V. Round each absolute pitch to the nearest
// DAC code so a rounded semitone constant cannot accumulate octave error.

#define CV_BASE_NOTE    24      // MIDI note for 0V output

static uint16_t note_to_dac(uint8_t midi_note) {
    if (midi_note < CV_BASE_NOTE) return 0;
    int offset = midi_note - CV_BASE_NOTE;
    const uint32_t denominator = 12u * CV_DAC_FULL_SCALE_MV;
    uint32_t dac_val = ((uint32_t)offset * 4096u * 1000u + denominator / 2u) / denominator;
    if (dac_val > CV_DAC_MAX) dac_val = CV_DAC_MAX;
    return (uint16_t)dac_val;
}

// ─── Public API ─────────────────────────────────────────────────

void cv_gate_init(void) {
    // Init SPI1
    spi_init(CV_SPI, 1000000);  // 1 MHz — MCP4822 supports up to 20 MHz
    gpio_set_function(CV_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(CV_MOSI_PIN, GPIO_FUNC_SPI);

    // CS pins as GPIO (manual control)
    for (int i = 0; i < 2; i++) {
        gpio_init(cs_pins[i]);
        gpio_set_dir(cs_pins[i], GPIO_OUT);
        gpio_put(cs_pins[i], 1);  // CS inactive (high)
    }

    // Gate pins
    for (int i = 0; i < NUM_CV_CHANNELS; i++) {
        gpio_init(gate_pins[i]);
        gpio_set_dir(gate_pins[i], GPIO_OUT);
        gpio_put(gate_pins[i], 0);
    }

    // Zero all DAC outputs
    for (int i = 0; i < NUM_CV_CHANNELS; i++) {
        cv_set_voltage(i, 0);
    }
}

void cv_set_note(uint8_t channel, uint8_t midi_note) {
    if (channel >= NUM_CV_CHANNELS) return;
    uint16_t dac_val = note_to_dac(midi_note);
    cv_set_voltage(channel, dac_val);
}

void cv_set_voltage(uint8_t channel, uint16_t dac_value) {
    if (channel >= NUM_CV_CHANNELS) return;
    // Channels 0,1 → DAC 0 (channels A,B)
    // Channels 2,3 → DAC 1 (channels A,B)
    uint8_t dac_index = channel / 2;
    uint8_t dac_channel = channel % 2;
    mcp4822_write(dac_index, dac_channel, dac_value);
}

void gate_on(uint8_t channel) {
    if (channel >= NUM_CV_CHANNELS) return;
    gpio_put(gate_pins[channel], 1);
}

void gate_off(uint8_t channel) {
    if (channel >= NUM_CV_CHANNELS) return;
    gpio_put(gate_pins[channel], 0);
}

void gate_commit(uint32_t on_mask, uint32_t off_mask) {
    // Build a GPIO mask for simultaneous set/clear across all gate pins.
    // on_mask/off_mask: bit N = CV channel N.
    uint32_t gpio_set = 0;
    uint32_t gpio_clr = 0;
    for (int ch = 0; ch < NUM_CV_CHANNELS; ch++) {
        uint32_t pin_bit = 1u << gate_pins[ch];
        if (on_mask & (1u << ch))  gpio_set |= pin_bit;
        if (off_mask & (1u << ch)) gpio_clr |= pin_bit;
    }
    // Single register writes — all pins change in the same cycle
    if (gpio_clr) gpio_clr_mask(gpio_clr);
    if (gpio_set) gpio_set_mask(gpio_set);
}

void cv_gate_all_off(void) {
    for (int i = 0; i < NUM_CV_CHANNELS; i++) {
        gate_off(i);
        cv_set_voltage(i, 0);
    }
}
