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

// One debounced control: a matrix key or PLAY.
typedef struct {
    bool raw;           // last state seen
    bool accepted;      // last state reported
    bool suppressed;    // down at a chip re-init, or PLAY at a resync: silent until
                        // seen up for the debounce time
    uint32_t since_ms;  // when raw last changed
} debounce_t;

static debounce_t keys[PANEL_KEYS];
static debounce_t play;

// Key events are read only while the chip is configured and in sync.
static bool chip_configured;  // tca8418_init accepted and no I2C error since
static bool chip_synced;      // FIFO drained since the key state was last unknown
static bool retry_armed;      // the last recovery attempt failed
static uint32_t retry_from_ms;
static bool check_started;    // the CFG readback clock runs from the first poll
static uint32_t check_ms;

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

static void see(debounce_t *d, bool down, uint32_t now_ms) {
    if (d->raw == down) return;
    d->raw = down;
    d->since_ms = now_ms;
}

// True when a control has a change that held for window_ms and is to be
// reported. A suppressed control reports nothing; it is let go once it has
// been seen up for window_ms, so a bounce or a re-reported press is ignored.
static bool settle(debounce_t *d, uint32_t now_ms, uint32_t window_ms) {
    if (d->raw == d->accepted && !d->suppressed) return false;
    if ((uint32_t)(now_ms - d->since_ms) < window_ms) return false;
    if (d->suppressed) {
        if (!d->raw) d->suppressed = false;
        return false;
    }
    return true;
}

// Key events from before a drain or a lost batch are dropped; queued PLAY
// events stay.
static void drop_key_events(void) {
    panel_event_t kept_events[EVENT_RING_SIZE];
    uint8_t kept = 0;
    for (uint8_t i = 0; i < ring_count; i++) {
        panel_event_t event = ring[(ring_head + i) % EVENT_RING_SIZE];
        if (event.kind == PANEL_PLAY) kept_events[kept++] = event;
    }
    memcpy(ring, kept_events, kept * sizeof(ring[0]));
    ring_head = 0;
    ring_count = kept;
}

// The FIFO was drained and the chip kept its configuration: a key held across
// the drain is not reported again, only its release later, which then changes
// nothing. So every key reads as up and nothing stays suppressed, and a key
// whose release was drained presses normally again.
static void release_keys(void) {
    for (uint8_t i = 0; i < PANEL_KEYS; i++) {
        keys[i].raw = false;
        keys[i].accepted = false;
        keys[i].suppressed = false;
    }
}

// The chip is (re-)initialised and may report a held key again: nothing is
// reported down any more and a key that was down stays suppressed until it is
// seen released.
static void suppress_keys(void) {
    for (uint8_t i = 0; i < PANEL_KEYS; i++) {
        debounce_t *key = &keys[i];
        key->suppressed = key->suppressed || key->raw || key->accepted;
        key->accepted = false;
    }
}

static void forget_play(void) {
    play.suppressed = play.suppressed || play.raw || play.accepted || play_pin_down();
    play.accepted = false;
}

// Brings the chip back after an I2C error, a lost sync or a resync: re-init
// when it may have lost its configuration, then drain the FIFO. An attempt
// after a failed one waits PANEL_RETRY_MS, so a dead bus costs one timeout
// per 100 ms instead of one per pass.
static bool chip_ready(uint32_t now_ms) {
    if (chip_configured && chip_synced) return true;
    if (retry_armed && (uint32_t)(now_ms - retry_from_ms) < PANEL_RETRY_MS) return false;
    if (!chip_configured) chip_configured = tca8418_init();
    if (chip_configured && !chip_synced) {
        chip_synced = tca8418_flush();
        if (!chip_synced) chip_configured = false;  // re-init on the next attempt
    }
    retry_armed = !(chip_configured && chip_synced);
    retry_from_ms = now_ms;
    return !retry_armed;
}

// Events were lost: drop the batch and drain the FIFO. Keys read as up when a
// configured chip was only drained; after a re-init they stay suppressed.
static void lose_sync(uint32_t now_ms) {
    bool kept_config = chip_configured;
    chip_synced = false;
    drop_key_events();
    chip_ready(now_ms);
    if (kept_config && chip_configured && chip_synced) release_keys();
    else suppress_keys();
}

// About once a second: a chip that reset (brown-out, ESD) still answers but
// has CFG back at its default and scans nothing.
static bool check_due(uint32_t now_ms) {
    if (!check_started) {
        check_started = true;
        check_ms = now_ms;
        return false;
    }
    if ((uint32_t)(now_ms - check_ms) < PANEL_CHECK_MS) return false;
    check_ms = now_ms;
    return true;
}

void panel_input_init(void) {
    gpio_init(PLAY_BUTTON_PIN);
    gpio_set_dir(PLAY_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(PLAY_BUTTON_PIN);
    memset(keys, 0, sizeof(keys));
    memset(&play, 0, sizeof(play));
    ring_head = 0;
    ring_count = 0;
    forget_play();              // PLAY held at power-up does nothing
    check_started = false;
    retry_armed = false;        // a failed boot init is retried by the first poll
    chip_configured = tca8418_init();
    chip_synced = chip_configured && tca8418_flush();
    if (!chip_synced) chip_configured = false;
}

void panel_input_resync(void) {
    forget_play();
    ring_count = 0;             // PLAY events too: nothing from the stall is a command
    // A working chip only needs its FIFO drained; it is re-initialised (next
    // poll) only when that fails.
    chip_synced = chip_configured && tca8418_flush();
    if (chip_synced) {
        release_keys();
    } else {
        chip_configured = false;
        suppress_keys();
    }
    retry_armed = false;
}

static void apply_chip_event(uint8_t event, uint32_t now_ms) {
    uint8_t code = event & KEY_CODE_MASK;
    if (code < 1 || code > PANEL_KEYS) return;
    see(&keys[code - 1], (event & KEY_PRESS_BIT) != 0, now_ms);
}

static void emit_stable_keys(uint32_t now_ms) {
    for (uint8_t i = 0; i < PANEL_KEYS; i++) {
        debounce_t *key = &keys[i];
        if (!settle(key, now_ms, PANEL_DEBOUNCE_MS)) continue;
        const key_control_t *control = &key_map[i / PANEL_COLS][i % PANEL_COLS];
        // A full ring leaves the change pending; it is retried on the next poll.
        if (ring_push(control->kind, control->track, control->row, key->raw)) {
            key->accepted = key->raw;
        }
    }
}

static void poll_play(uint32_t now_ms) {
    see(&play, play_pin_down(), now_ms);
    if (settle(&play, now_ms, PLAY_DEBOUNCE_MS) && ring_push(PANEL_PLAY, 0, 0, play.raw)) {
        play.accepted = play.raw;
    }
}

void panel_input_poll(uint32_t now_ms) {
    // PLAY is a plain GPIO: sampled first, whatever the bus does.
    poll_play(now_ms);
    if (!chip_ready(now_ms)) return;

    if (check_due(now_ms)) {
        bool configured;
        if (!tca8418_read_config(&configured)) {
            chip_configured = false;
            return;
        }
        if (!configured) {
            // The chip reset: its FIFO and the key state it reported are gone.
            chip_configured = false;
            lose_sync(now_ms);
            return;
        }
    }

    uint8_t events[CHIP_READ_MAX];
    bool overflow = false;
    int count = tca8418_read_events(events, CHIP_READ_MAX, &overflow);
    if (count == TCA8418_READ_FAILED) {
        // Nothing was consumed: re-init (the chip may have reset), then read again.
        chip_configured = false;
    } else if (count == TCA8418_READ_UNCERTAIN || overflow) {
        // Events were lost or the batch is incomplete: none of it is reported.
        if (count == TCA8418_READ_UNCERTAIN) chip_configured = false;
        lose_sync(now_ms);
        return;
    } else {
        for (int i = 0; i < count; i++) apply_chip_event(events[i], now_ms);
    }
    emit_stable_keys(now_ms);
}

bool panel_input_next(panel_event_t *out) {
    if (ring_count == 0) return false;
    *out = ring[ring_head];
    ring_head = (ring_head + 1) % EVENT_RING_SIZE;
    ring_count--;
    return true;
}
