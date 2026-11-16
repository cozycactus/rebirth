#include <string.h>
#include "pico/stdlib.h"
#include "panel_input.h"
#include "tca8418.h"

#define PANEL_ROWS 8
#define PANEL_COLS 10
#define PANEL_KEYS (PANEL_ROWS * PANEL_COLS)
#define EVENT_RING_SIZE 32
#define CHIP_READ_MAX 10
#define KEY_PRESS_BIT 0x80
#define KEY_CODE_MASK 0x7F

typedef struct {
    panel_kind_t kind;
    uint8_t track;
    uint8_t row;
} key_control_t;

// Matrix position to control. Columns 0-7 are the pads of tracks 0-7,
// column 8 the scene key and column 9 the mute key of that row. A board wired
// differently edits only this table.
#define PAD(col, r) { PANEL_PAD, (col), (r) }
#define ROW_KEYS(r) \
    PAD(0, r), PAD(1, r), PAD(2, r), PAD(3, r), PAD(4, r), PAD(5, r), PAD(6, r), PAD(7, r), \
    { PANEL_SCENE, 0, (r) }, { PANEL_MUTE, (r), 0 }
static const key_control_t key_map[PANEL_ROWS][PANEL_COLS] = {
    { ROW_KEYS(0) }, { ROW_KEYS(1) }, { ROW_KEYS(2) }, { ROW_KEYS(3) },
    { ROW_KEYS(4) }, { ROW_KEYS(5) }, { ROW_KEYS(6) }, { ROW_KEYS(7) },
};

typedef struct {
    bool raw;
    bool accepted;
    uint32_t since_ms;  // when raw last changed
} debounce_t;

static debounce_t keys[PANEL_KEYS];
static debounce_t play;
static bool play_suppressed;  // held across a resync; ignored until seen released
static bool recovering;       // chip FIFO flush failed; emit nothing until it works

static panel_event_t ring[EVENT_RING_SIZE];
static uint8_t ring_head;
static uint8_t ring_count;

static bool ring_push(panel_kind_t kind, uint8_t track, uint8_t row, bool down) {
    if (ring_count == EVENT_RING_SIZE) return false;
    ring[(ring_head + ring_count) % EVENT_RING_SIZE] =
        (panel_event_t){ .kind = kind, .track = track, .row = row, .down = down };
    ring_count++;
    return true;
}

static bool play_pin_down(void) {
    return !gpio_get(PLAY_BUTTON_PIN);
}

// Releases every key silently and drops queued key events; PLAY events stay.
static void clear_keys(void) {
    memset(keys, 0, sizeof(keys));
    uint8_t kept = 0;
    for (uint8_t i = 0; i < ring_count; i++) {
        panel_event_t event = ring[(ring_head + i) % EVENT_RING_SIZE];
        if (event.kind == PANEL_PLAY) ring[kept++] = event;
    }
    ring_head = 0;
    ring_count = kept;
}

static void clear_play(void) {
    memset(&play, 0, sizeof(play));
    play_suppressed = play_pin_down();
}

static bool chip_recover(void) {
    return tca8418_init() && tca8418_flush();
}

static void resync_chip(void) {
    recovering = !chip_recover();
    clear_keys();
    ring_count = 0;
    clear_play();
}

void panel_input_init(void) {
    gpio_init(PLAY_BUTTON_PIN);
    gpio_set_dir(PLAY_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(PLAY_BUTTON_PIN);
    resync_chip();
}

void panel_input_resync(void) {
    resync_chip();
}

static void apply_chip_event(uint8_t event, uint32_t now_ms) {
    uint8_t code = event & KEY_CODE_MASK;
    if (code < 1 || code > PANEL_KEYS) return;
    debounce_t *key = &keys[code - 1];
    bool down = (event & KEY_PRESS_BIT) != 0;
    if (key->raw == down) return;
    key->raw = down;
    key->since_ms = now_ms;
}

static void emit_stable_keys(uint32_t now_ms) {
    for (uint8_t i = 0; i < PANEL_KEYS; i++) {
        debounce_t *key = &keys[i];
        if (key->raw == key->accepted) continue;
        if ((uint32_t)(now_ms - key->since_ms) < PANEL_DEBOUNCE_MS) continue;
        const key_control_t *control = &key_map[i / PANEL_COLS][i % PANEL_COLS];
        // A full ring leaves the change pending; it is retried on the next poll.
        if (ring_push(control->kind, control->track, control->row, key->raw)) {
            key->accepted = key->raw;
        }
    }
}

static void poll_play(uint32_t now_ms) {
    bool down = play_pin_down();
    if (play_suppressed) {
        if (down) return;
        play_suppressed = false;
    }
    if (down != play.raw) {
        play.raw = down;
        play.since_ms = now_ms;
    }
    if (play.raw == play.accepted) return;
    if ((uint32_t)(now_ms - play.since_ms) < PLAY_DEBOUNCE_MS) return;
    if (ring_push(PANEL_PLAY, 0, 0, play.raw)) play.accepted = play.raw;
}

void panel_input_poll(uint32_t now_ms) {
    // PLAY is a plain GPIO and is sampled even while the chip is recovering.
    if (recovering) {
        if (chip_recover()) {
            recovering = false;
            clear_keys();
        }
    } else {
        uint8_t events[CHIP_READ_MAX];
        bool overflow = false;
        int count = tca8418_read_events(events, CHIP_READ_MAX, &overflow);
        if (overflow) {
            // Events were lost, so key state is unknown: release everything silently.
            recovering = !tca8418_flush();
            clear_keys();
        } else {
            for (int i = 0; i < count; i++) apply_chip_event(events[i], now_ms);
        }
        if (!recovering) emit_stable_keys(now_ms);
    }
    poll_play(now_ms);
}

bool panel_input_next(panel_event_t *out) {
    if (ring_count == 0) return false;
    *out = ring[ring_head];
    ring_head = (ring_head + 1) % EVENT_RING_SIZE;
    ring_count--;
    return true;
}
