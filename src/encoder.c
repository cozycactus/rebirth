#ifndef MOCK_HARDWARE
#include "pico/stdlib.h"
#else
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
static uint32_t _mock_ms = 0;
static bool _mock_pins[30] = {0};
void gpio_init(uint8_t pin) { (void)pin; }
void gpio_set_dir(uint8_t pin, bool out) { (void)pin; (void)out; }
void gpio_pull_up(uint8_t pin) { (void)pin; }
bool gpio_get(uint8_t pin) { return _mock_pins[pin]; }
uint32_t to_ms_since_boot(uint64_t t) { (void)t; return _mock_ms; }
uint64_t get_absolute_time(void) { return 0; }
#define GPIO_IN 0
#endif

#include <string.h>
#include "encoder.h"

// ─── Pin Table ─────────────────────────────────────────────────

static const uint8_t enc_pins[NUM_ENCODERS][3] = {
    { ENC1_A_PIN, ENC1_B_PIN, ENC1_SW_PIN },   // Encoder 1
    { ENC2_A_PIN, ENC2_B_PIN, ENC2_SW_PIN },    // Encoder 2
};

// ─── State ─────────────────────────────────────────────────────

static encoder_state_t encoders[NUM_ENCODERS];

// Quadrature direction lookup table
// Index: (prev_AB << 2) | curr_AB
// Values: 0 = no movement, +1 = CW, -1 = CCW
static const int8_t quad_table[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0,
};

// ─── Initialization ────────────────────────────────────────────

void encoder_init(void) {
    memset(encoders, 0, sizeof(encoders));

    for (int i = 0; i < NUM_ENCODERS; i++) {
        // A pin
        gpio_init(enc_pins[i][0]);
        gpio_set_dir(enc_pins[i][0], GPIO_IN);
        gpio_pull_up(enc_pins[i][0]);

        // B pin
        gpio_init(enc_pins[i][1]);
        gpio_set_dir(enc_pins[i][1], GPIO_IN);
        gpio_pull_up(enc_pins[i][1]);

        // Switch pin
        gpio_init(enc_pins[i][2]);
        gpio_set_dir(enc_pins[i][2], GPIO_IN);
        gpio_pull_up(enc_pins[i][2]);

        // Read initial A/B state
        bool a = gpio_get(enc_pins[i][0]);
        bool b = gpio_get(enc_pins[i][1]);
        encoders[i]._prev_ab = (a << 1) | b;
    }
}

// ─── Polling ───────────────────────────────────────────────────

static void add_saturating(int16_t *total, int16_t value) {
    int32_t sum = (int32_t)*total + value;
    if (sum > INT16_MAX) sum = INT16_MAX;
    if (sum < INT16_MIN) sum = INT16_MIN;
    *total = (int16_t)sum;
}

void encoder_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < NUM_ENCODERS; i++) {
        encoder_state_t *e = &encoders[i];

        // ── Quadrature decoding ──
        bool a = gpio_get(enc_pins[i][0]);
        bool b = gpio_get(enc_pins[i][1]);
        uint8_t new_ab = (a << 1) | b;
        uint8_t transition = (e->_prev_ab << 2) | new_ab;
        int8_t d = quad_table[transition];
        e->_prev_ab = new_ab;

        // A detent is ENC_EDGES_PER_DETENT edges the same way; a turn back
        // within it undoes its edges. Each one is timed for acceleration here,
        // when it completes.
        if (d != 0) {
            e->_edges += d;
            if (e->_edges == d * ENC_EDGES_PER_DETENT) {
                e->_edges = 0;
                add_saturating(&e->_detents, d);
                add_saturating(&e->_amount, (int16_t)(d * encoder_accel_detent(&e->_accel, d, now)));
            }
        }

        // ── Button debounce ──
        bool raw_btn = !gpio_get(enc_pins[i][2]);   // Active-low with pull-up

        if (now - e->_btn_debounce_ms >= ENC_DEBOUNCE_MS) {
            e->_btn_debounce_ms = now;
            e->_btn_raw = raw_btn;
        }

        // ── Button edge detection & long press ──
        if (e->_btn_raw && !e->_btn_prev) {
            // Just pressed
            e->_btn_down_ms = now;
            e->_long_fired = false;
            e->button_held = true;
        }

        if (e->_btn_raw && e->_btn_prev) {
            // Still held
            if (!e->_long_fired &&
                (now - e->_btn_down_ms >= ENC_LONG_PRESS_MS)) {
                e->button_event = BTN_LONG_PRESS;
                e->_long_fired = true;
            }
        }

        if (!e->_btn_raw && e->_btn_prev) {
            // Just released
            e->button_held = false;
            if (e->_long_fired) {
                e->button_event = BTN_RELEASE;
            } else {
                e->button_event = BTN_PRESS;
            }
        }

        e->_btn_prev = e->_btn_raw;
    }
}

void encoder_resync(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < NUM_ENCODERS; i++) {
        encoder_state_t *e = &encoders[i];
        bool down = !gpio_get(enc_pins[i][2]);
        // The button's state now is its debounced state, with no edge: a
        // release during the stall reports nothing, and a button held now
        // counts as long-fired, so it never becomes a press or a long press.
        e->_btn_raw = down;
        e->_btn_prev = down;
        e->button_held = down;
        e->_long_fired = down;
        e->_btn_down_ms = now;
        e->_btn_debounce_ms = now;
        e->button_event = BTN_NONE;
    }
}

// ─── Accessors ─────────────────────────────────────────────────

static int8_t clamp_to_i8(int16_t value) {
    if (value > INT8_MAX) return INT8_MAX;
    if (value < INT8_MIN) return INT8_MIN;
    return (int8_t)value;
}

encoder_turn_t encoder_get_turn(uint8_t enc) {
    encoder_turn_t turn = {0, 0};
    if (enc >= NUM_ENCODERS) return turn;
    encoder_state_t *e = &encoders[enc];

    turn.detents = clamp_to_i8(e->_detents);
    turn.amount = clamp_to_i8(e->_amount);
    e->_detents = 0;
    e->_amount = 0;
    return turn;
}

button_event_t encoder_get_button(uint8_t enc) {
    if (enc >= NUM_ENCODERS) return BTN_NONE;
    button_event_t ev = encoders[enc].button_event;
    encoders[enc].button_event = BTN_NONE;
    return ev;
}

bool encoder_button_held(uint8_t enc) {
    if (enc >= NUM_ENCODERS) return false;
    return encoders[enc].button_held;
}
