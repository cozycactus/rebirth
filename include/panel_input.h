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
// an overflow or an incomplete read drops the batch, forgets the key state
// (keys that were down stay suppressed, as after a resync) and drains the
// FIFO. A failed recovery attempt is retried after PANEL_RETRY_MS, with
// nothing emitted meanwhile.
void panel_input_poll(uint32_t now_ms);
bool panel_input_next(panel_event_t *out);
// After a flash stall or a panic: drops every pending event (PLAY included),
// reports nothing as down and keeps every key and PLAY that was down, or is
// down now, suppressed until it has been seen released for its debounce time
// (8 ms keys, 20 ms PLAY), so a bounce or a press the chip reports again never
// becomes a command. The chip FIFO is drained; the chip is re-initialised only
// when that fails.
void panel_input_resync(void);

#endif
