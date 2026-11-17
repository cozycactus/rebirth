#ifndef SIM_API_H
#define SIM_API_H

#include <stdint.h>

/* The simulator's side of the device: virtual hardware around the unmodified
 * firmware. Everything here is physical: levels on pins, keys in the matrix,
 * bytes on the MIDI wire, light from LEDs. Time is virtual microseconds. */

// Powers the device up at time 0: clears flash unless keep_flash, then runs
// the same init sequence as src/main.c.
void sim_boot(int keep_flash);

// Runs the device until t_us: the 96 PPQN timer interrupt at its alarm, the
// UART TX interrupt when the holding register empties, and a core 0 pass every
// SIM_UI_PASS_US (ui_update, then the ~30 fps redraw, as the loop in main.c).
void sim_run_until(double t_us);
double sim_now(void);

// ─── Inputs (levels, applied at the current time) ───────────────
void sim_key(int row, int col, int down);       // TCA8418 matrix: cols 0-7 pads, 8 SCENE, 9 MUTE
void sim_step_button(int index, int down);      // MCP23017 0x20, 0..15
void sim_gpio_in(int pin, int level);           // PLAY 28; encoders 18,19,20 / 21,22,26

// The same inputs at a time of their own, now or later. They are applied when
// device time reaches them, also while the firmware is stalled in a flash
// save: a key pressed during the stall is down when the firmware looks again.
#define SIM_IN_KEY  0       // a = row, b = column
#define SIM_IN_STEP 1       // a = index
#define SIM_IN_GPIO 2       // a = pin
void sim_input_at(double t_us, int kind, int a, int b, int level);
void sim_inputs_clear(void);
int sim_inputs_pending(void);

// ─── Outputs ────────────────────────────────────────────────────
const uint8_t *sim_oled(void);                  // 1024 bytes as last flushed over SPI
uint32_t sim_oled_seq(void);                    // counts flushes
int sim_oled_on(void);
const uint8_t *sim_leds(void);                  // 82 x RGB as last accepted frame
uint32_t sim_leds_seq(void);
uint32_t sim_leds_skipped(void);                // frames refused (previous still on the wire)
uint32_t sim_step_leds(void);                   // MCP23017 0x21 output latches driving LEDs
uint32_t sim_gpio_levels(void);                 // bit per GPIO: gates 14-17, tempo LED 25
const uint16_t *sim_dac(void);                  // 4 MCP4822 codes

// Timestamped output events since the last sim_events_clear().
// Events are listed in the order the firmware produced them. A MIDI byte is
// stamped with the time its stop bit ends, up to two byte times (640 us) after
// the moment it was handed to the UART, so stamps are not strictly ascending.
#define SIM_EV_MIDI 0       // value: one byte
#define SIM_EV_GATE 1       // value: gate mask (bit per CV channel)
#define SIM_EV_DAC  2       // value: channel << 16 | code
typedef struct { double t_us; uint32_t kind; uint32_t value; } sim_event_t;
const sim_event_t *sim_events(void);
int sim_events_count(void);
uint32_t sim_events_dropped(void);
void sim_events_clear(void);

// ─── Faults ─────────────────────────────────────────────────────
#define SIM_I2C_OK      0
#define SIM_I2C_NACK    1   // the transfer fails at once
#define SIM_I2C_TIMEOUT 2   // the transfer takes its whole timeout, then fails
void sim_i2c_fault(int addr, int mode);
void sim_chip_reset(int addr);                  // brown-out: registers back to power-on values

// ─── Flash image (last 128 KiB of the 2 MiB), for keeping a save ─
uint8_t *sim_flash_store(void);
int sim_flash_store_size(void);

// ─── Inspection: for checks and the on-screen meters only. The scripted
// artist must not read these; it sees the OLED and the lights. ──
int sim_dbg_transport_state(void);
double sim_dbg_bpm(void);
uint32_t sim_dbg_pulse(void);
int sim_dbg_current(int track);                 // location the track plays
int sim_dbg_queued(int track);
uint32_t sim_dbg_step(int location, int track, int step);   // note | velocity<<8 | flags<<16
uint32_t sim_dbg_kit_step(int location, int track, int step);
int sim_dbg_track_length(int location, int track);
uint32_t sim_dbg_track_config(int track);       // type | channel<<8 | gate%<<16 | output<<24
int sim_dbg_mutes(void);
uint32_t sim_dbg_overruns(void);
int sim_dbg_midi_overflow(void);
uint32_t sim_dbg_lock_errors(void);
const char *sim_dbg_fatal(void);                // NULL while healthy

#endif
