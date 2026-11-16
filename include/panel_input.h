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

// Call after mcp23017_init (I2C0 is already up).
void panel_input_init(void);
// Call on every ui_update pass; now_ms is the only clock this module sees.
void panel_input_poll(uint32_t now_ms);
bool panel_input_next(panel_event_t *out);
// Drops pending events and debounce state, treats every key as released and
// keeps anything physically held suppressed until it is seen released. If the
// chip FIFO cannot be flushed, stays in recovery (flush retried each poll,
// nothing emitted) until it succeeds.
void panel_input_resync(void);

#endif
