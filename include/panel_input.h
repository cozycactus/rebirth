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

#define PANEL_DEBOUNCE_MS 5
#define PLAY_DEBOUNCE_MS 20
#define PLAY_BUTTON_PIN 28
#define PANEL_RETRY_MS 100   // between failed scanner configuration attempts

// Call after mcp23017_init (I2C0 is already up). Configures the scanner once
// (the first poll retries a failure) and takes every key as held, like a
// resync: a key or PLAY held at power-up does nothing.
void panel_input_init(void);
// Call on every ui_update pass; now_ms is the only clock this module sees.
// PLAY is sampled first and keeps working whatever the bus does. Then at most
// one step of the free-running key scan: the scanner check, then columns 0-9,
// one per pass. A key change is reported at the first sample of its column at
// least PANEL_DEBOUNCE_MS after the sample that first saw it, every sample in
// between agreeing; a change that does not last until then is lost, as is one
// that starts and ends while its column is not sampled (bus down, scanner
// being reconfigured). Any I2C error, or a check that finds a chip changed
// (reset), cancels every pending change and configures both chips again (a
// pass of its own) before the next sample; reported states stay, so a key
// held through it is not reported again. A failed configuration is retried
// after PANEL_RETRY_MS, with nothing emitted meanwhile.
void panel_input_poll(uint32_t now_ms);
// The next queued event. The queue holds 32; a change that finds it full stays
// pending until its column's next sample.
bool panel_input_next(panel_event_t *out);
// After a flash stall or a panic: drops every queued event (PLAY included),
// does no I2C and restarts the scan at the check. Every key is taken as held
// and ends that silently once it has been seen up for PANEL_DEBOUNCE_MS: a key
// held through the stall reports nothing, nor does its release, and its next
// press is normal; a key released during the stall presses normally again; a
// key pressed during the stall, or before it was seen up, reports nothing
// until released. PLAY, a plain GPIO, stays suppressed the same way (20 ms)
// when it was down or is down now.
void panel_input_resync(void);

#endif
