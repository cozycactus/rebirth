#ifndef PANEL_INPUT_H
#define PANEL_INPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { PANEL_PAD, PANEL_SCENE, PANEL_MUTE, PANEL_PLAY } panel_kind_t;

typedef struct {
    panel_kind_t kind;
    uint8_t track;
    uint8_t row;
    bool down;
} panel_event_t;

#define PANEL_DEBOUNCE_MS 8
#define PLAY_DEBOUNCE_MS 20
#define PLAY_BUTTON_PIN 28
#define PANEL_RETRY_MS 100   // between failed chip recovery attempts
#define PANEL_CHECK_MS 1000  // CFG readback, to notice a chip that reset

// Call after mcp23017_init (I2C0 is already up).
void panel_input_init(void);
// Call on every ui_update pass; now_ms is the only clock this module sees.
// PLAY is sampled first and keeps working whatever the bus does. Any I2C error
// marks the chip unconfigured and it is re-initialised before the next read;
// an overflow or an incomplete read drops the batch and drains the FIFO (an
// incomplete read re-initialises the chip first); the keys then read as after
// a resync, by whether the chip was re-initialised. A failed recovery attempt
// is retried after PANEL_RETRY_MS, with nothing emitted meanwhile.
void panel_input_poll(uint32_t now_ms);
bool panel_input_next(panel_event_t *out);
// After a flash stall or a panic: drops every pending event (PLAY included)
// and reports nothing as down; the chip FIFO is drained. When that drain
// works the chip kept its configuration, so a key held across it is never
// reported again, only its release, which then changes nothing: every key
// reads as up, and a key whose release was drained presses normally again.
// When it fails the chip is re-initialised and may report a held key again,
// so every key that was down stays suppressed until it has been seen released
// for 8 ms. PLAY, a plain GPIO, always stays suppressed that way (20 ms) when
// it was down or is down now.
void panel_input_resync(void);

// True while the pad is reported down (debounced, accepted). False for a
// track or row out of range.
bool panel_input_pad_down(uint8_t track, uint8_t row);
// Changes whenever key state may have been lost: a chip (re)initialisation,
// a lost sync, a resync, any I2C error. A hold recorded under an older value
// must not be trusted.
uint32_t panel_input_generation(void);

#endif
