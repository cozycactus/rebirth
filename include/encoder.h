#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>
#include <stdbool.h>

// ─── Configuration ─────────────────────────────────────────────

#define NUM_ENCODERS    2

// Encoder 1: Navigation / Track select
#define ENC1_A_PIN      18
#define ENC1_B_PIN      19
#define ENC1_SW_PIN     20

// Encoder 2: Value / Transport
#define ENC2_A_PIN      21
#define ENC2_B_PIN      22
#define ENC2_SW_PIN     26

// Timing
#define ENC_DEBOUNCE_MS     20      // Button debounce window
#define ENC_LONG_PRESS_MS   500     // Long press threshold
#define ENC_ACCEL_FAST_MS   50      // Acceleration threshold (ms between ticks)
#define ENC_ACCEL_MULT_LO   4       // Multiplier after >3 fast ticks
#define ENC_ACCEL_MULT_HI   8       // Multiplier after >6 fast ticks

// ─── Types ─────────────────────────────────────────────────────

typedef enum {
    BTN_NONE,               // No event
    BTN_PRESS,              // Short press (fired on release, <500ms)
    BTN_LONG_PRESS,         // Long press (fired while held, >500ms)
    BTN_RELEASE,            // Release after long press
} button_event_t;

typedef struct {
    // Public — consumed by caller
    int8_t          delta;              // Accumulated rotation since last read
    button_event_t  button_event;       // Current button event
    bool            button_held;        // True while button is physically held

    // Internal — do not access directly
    uint8_t         _prev_ab;           // Previous A/B pin state (2 bits)
    uint32_t        _btn_down_ms;       // Timestamp of button press
    bool            _btn_raw;           // Current debounced button state
    bool            _btn_prev;          // Previous debounced state
    uint32_t        _btn_debounce_ms;   // Last debounce sample time
    bool            _long_fired;        // Long press already emitted
    int8_t          _accel_count;       // Rapid rotation counter
    uint32_t        _last_tick_ms;      // Time of last rotation tick
} encoder_state_t;

// ─── API ───────────────────────────────────────────────────────

// Initialize GPIO pins (pull-ups, input mode)
void encoder_init(void);

// Poll all encoders — call as often as possible from ui_update()
void encoder_poll(void);

// After a flash stall: each button's state now becomes its debounced state.
// A button released during the stall reports nothing; one held through it
// never becomes BTN_PRESS or BTN_LONG_PRESS (its release is BTN_RELEASE).
void encoder_resync(void);

// Read and consume accumulated rotation delta
// Positive = clockwise, negative = counter-clockwise
// Applies acceleration: fast turns yield larger deltas
int8_t encoder_get_delta(uint8_t enc);

// Read and consume button event
button_event_t encoder_get_button(uint8_t enc);

// Check if button is currently held (non-consuming)
bool encoder_button_held(uint8_t enc);

#endif // ENCODER_H
