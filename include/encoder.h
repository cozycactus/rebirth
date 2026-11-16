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
#define ENC_ACCEL_FAST_MS   60      // a detent less than this after the last one is fast
#define ENC_ACCEL_LO_AFTER  3       // more fast detents in a row than this: x MULT_LO
#define ENC_ACCEL_HI_AFTER  6       // more than this: x MULT_HI
#define ENC_ACCEL_MULT_LO   4
#define ENC_ACCEL_MULT_HI   8

#define ENC_EDGES_PER_DETENT 4      // quadrature edges per detent (EC11 type: 4)

// ─── Types ─────────────────────────────────────────────────────

typedef enum {
    BTN_NONE,               // No event
    BTN_PRESS,              // Short press (fired on release, <500ms)
    BTN_LONG_PRESS,         // Long press (fired while held, >500ms)
    BTN_RELEASE,            // Release after long press
} button_event_t;

// Detent-rate acceleration of one encoder (encoder_accel_detent).
typedef struct {
    uint32_t        last_ms;            // time of the last detent
    int8_t          last_dir;           // its direction (+1, -1; 0 before the first)
    uint8_t         fast;               // fast detents in a row, saturating
} encoder_accel_t;

// One encoder's turn since the last read. A choice (a list row, a type, a
// slot, a lane, a stop) moves by `detents`, one per click however fast; a
// number moves by `amount`, the same clicks accelerated on a fast turn.
typedef struct {
    int8_t          detents;            // whole detents, + clockwise
    int8_t          amount;             // the detents, each x1, x4 or x8
} encoder_turn_t;

typedef struct {
    // Public — consumed by caller
    button_event_t  button_event;       // Current button event
    bool            button_held;        // True while button is physically held

    // Internal — do not access directly
    uint8_t         _prev_ab;           // Previous A/B pin state (2 bits)
    uint32_t        _btn_down_ms;       // Timestamp of button press
    bool            _btn_raw;           // Current debounced button state
    bool            _btn_prev;          // Previous debounced state
    uint32_t        _btn_debounce_ms;   // Last debounce sample time
    bool            _long_fired;        // Long press already emitted
    int8_t          _edges;             // edges of the detent in progress
    int16_t         _detents;           // whole detents not yet read
    int16_t         _amount;            // their accelerated amount
    encoder_accel_t _accel;
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

// Read and consume the turn since the last call: whole detents
// (ENC_EDGES_PER_DETENT edges each; a detent in progress stays for the next
// call) and their accelerated amount. Positive = clockwise.
encoder_turn_t encoder_get_turn(uint8_t enc);

// Read and consume button event
button_event_t encoder_get_button(uint8_t enc);

// Check if button is currently held (non-consuming)
bool encoder_button_held(uint8_t enc);

// ─── Acceleration ──────────────────────────────────────────────

// Counts one detent in direction `dir` (+1 or -1) at `now_ms` and returns what
// it is worth to a number. A detent less than ENC_ACCEL_FAST_MS after the last
// one, in the same direction, is fast. After more than ENC_ACCEL_LO_AFTER fast
// detents in a row each is worth ENC_ACCEL_MULT_LO, and after more than
// ENC_ACCEL_HI_AFTER, ENC_ACCEL_MULT_HI. A slow detent or a turn back is worth 1
// and starts over. The one place numbers are accelerated; the UI test's encoder
// fake shares it.
static inline int8_t encoder_accel_detent(encoder_accel_t *a, int8_t dir, uint32_t now_ms) {
    if (dir == a->last_dir && now_ms - a->last_ms < ENC_ACCEL_FAST_MS) {
        if (a->fast < UINT8_MAX) a->fast++;
    } else {
        a->fast = 0;
    }
    a->last_ms = now_ms;
    a->last_dir = dir;
    if (a->fast > ENC_ACCEL_HI_AFTER) return ENC_ACCEL_MULT_HI;
    if (a->fast > ENC_ACCEL_LO_AFTER) return ENC_ACCEL_MULT_LO;
    return 1;
}

#endif // ENCODER_H
