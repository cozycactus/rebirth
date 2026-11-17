#include <string.h>
#include "pico/stdlib.h"
#include "panel_input.h"
#include "key_scan.h"

#define PANEL_ROWS KEY_SCAN_ROWS
#define PANEL_COLS KEY_SCAN_COLS
#define EVENT_RING_SIZE 32
#define CHECK_STEP 0                    // a scan: the check, then one step per column
#define SCAN_STEPS (1 + PANEL_COLS)

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
    bool suppressed;    // taken as held (boot, resync): silent until seen up
                        // for the debounce time
    uint32_t since_ms;  // when raw last changed
} debounce_t;

// A key is touched only in the pass that sampled its column.
static debounce_t keys[PANEL_ROWS][PANEL_COLS];
static debounce_t play;

// Keys are sampled only while the scanner is configured: key_scan_init
// accepted and no failed transfer or check since. Every scan starts with the
// check, so no column is read before a check has passed.
static bool scanner_configured;
static bool scanner_checked;    // a check has passed since key_scan_init
static bool retry_armed;        // the last configuration attempt failed
static uint32_t retry_from_ms;
static uint8_t scan_step;       // CHECK_STEP, or 1 + the column to sample
// Bumped whenever key state may have been lost; see panel_input_generation().
static uint32_t generation;

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
// been seen up for window_ms, so a bounce or a held key is ignored.
static bool settle(debounce_t *d, uint32_t now_ms, uint32_t window_ms) {
    if (d->raw == d->accepted && !d->suppressed) return false;
    if ((uint32_t)(now_ms - d->since_ms) < window_ms) return false;
    if (d->suppressed) {
        if (!d->raw) d->suppressed = false;
        return false;
    }
    return true;
}

// After a stall, a panic or at power-up nothing is known of the keys: each is
// taken as held, reports nothing as down, and lets go once it has been seen up
// (two samples), so a key held now never fires and its next press is normal.
static void hold_keys(void) {
    for (uint8_t row = 0; row < PANEL_ROWS; row++) {
        for (uint8_t col = 0; col < PANEL_COLS; col++) {
            debounce_t *key = &keys[row][col];
            key->raw = true;
            key->accepted = false;
            key->suppressed = true;
        }
    }
}

static void forget_play(void) {
    play.suppressed = play.suppressed || play.raw || play.accepted || play_pin_down();
    play.accepted = false;
}

// A failed transfer or check: a chip may have reset, or a failed write may
// have left a column low, so nothing is sampled until both chips are
// configured and checked again. A pending change may rest on a bogus sample
// and is cancelled, so a bogus first sight never settles; and a settling
// sample counts only once the canaries read after it hold (scan_step_once),
// so every event, and every end of a suppression, rests on at least two good
// samples. A reported state stays, since a scanner reads levels and a held key
// reads as held after the re-init, not as a new press. A suppressed key needs
// two up samples again.
static void lose_scanner(uint32_t now_ms) {
    scanner_configured = false;
    generation++;
    for (uint8_t row = 0; row < PANEL_ROWS; row++) {
        for (uint8_t col = 0; col < PANEL_COLS; col++) {
            debounce_t *key = &keys[row][col];
            key->raw = key->accepted || key->suppressed;
            key->since_ms = now_ms;
        }
    }
}

// A configuration attempt is key_scan_init, the whole of its pass's bus work,
// and then the check that starts the next pass: the attempt fails if either
// does. An attempt after a failed one waits PANEL_RETRY_MS, so a dead bus, or
// one on which only reads fail, costs one timeout per 100 ms instead of one
// every pass or two. A configuration that succeeds starts a new generation, so
// a press the UI handled while the scanner was lost is not trusted as held:
// every pad press after it comes from a sample of the new configuration.
static void configure(uint32_t now_ms) {
    if (retry_armed && (uint32_t)(now_ms - retry_from_ms) < PANEL_RETRY_MS) return;
    scanner_configured = key_scan_init();
    if (scanner_configured) generation++;
    scanner_checked = false;
    retry_armed = !scanner_configured;
    retry_from_ms = now_ms;
    scan_step = CHECK_STEP;
}

// True when this sample would settle the key: report its change, or end its
// suppression. It runs see() and settle() on a copy, so it cannot disagree
// with sample_column.
static bool would_settle(const debounce_t *d, bool down, uint32_t now_ms) {
    debounce_t next = *d;
    see(&next, down, now_ms);
    return settle(&next, now_ms, PANEL_DEBOUNCE_MS) || next.suppressed != d->suppressed;
}

static bool column_would_settle(uint8_t col, uint8_t down, uint32_t now_ms) {
    for (uint8_t row = 0; row < PANEL_ROWS; row++) {
        if (would_settle(&keys[row][col], (down >> row) & 1u, now_ms)) return true;
    }
    return false;
}

static bool scanner_intact(void) {
    bool intact;
    return key_scan_check(&intact) && intact;
}

static bool sample_proved(uint8_t col) {
    bool intact;
    return key_scan_verify(col, &intact) && intact;
}

// The column's eight keys, from its fresh sample (bit r = ROW r down).
static void sample_column(uint8_t col, uint8_t down, uint32_t now_ms) {
    for (uint8_t row = 0; row < PANEL_ROWS; row++) {
        debounce_t *key = &keys[row][col];
        see(key, (down >> row) & 1u, now_ms);
        if (!settle(key, now_ms, PANEL_DEBOUNCE_MS)) continue;
        const key_control_t *control = &key_map[row][col];
        // A full ring leaves the change pending until this column's next sample.
        if (ring_push(control->kind, control->track, control->row, key->raw)) {
            key->accepted = key->raw;
        }
    }
}

// One bus step: either the check, or the sample of one column. The check
// starts every scan, so it runs between any two samples of a column and
// cancels a bogus first sight before the key's next sample. A chip that resets
// after that check could still give the sample that settles a change, so such
// a sample is proved first: right after it, in the same pass, the canaries a
// reset clears are read (written first, read after the data) and the sample
// counts only if they hold. That is U801's IODIR, and U802's for COL8/9: a
// U802 reset only floats COL8/COL9, which cannot pull a row low while a U801
// column is sampled. IPOL is left to the full check that starts every scan.
// A pass is then at most 6 transfers for COL0-7 (4 for the column, 2 for the
// canary) and 8 for COL8/9 (4 and 4).
static void scan_step_once(uint32_t now_ms) {
    if (scan_step == CHECK_STEP) {
        if (!scanner_intact()) {
            // The first check after a configuration ends that attempt, and it
            // failed; a later one only loses the scanner, configured again at once.
            if (!scanner_checked) {
                retry_armed = true;
                retry_from_ms = now_ms;
            }
            lose_scanner(now_ms);
            return;
        }
        scanner_checked = true;
        scan_step++;
        return;
    }
    uint8_t col = (uint8_t)(scan_step - 1);
    uint8_t down;
    if (!key_scan_column(col, &down)) {
        lose_scanner(now_ms);
        return;
    }
    if (column_would_settle(col, down, now_ms) && !sample_proved(col)) {
        lose_scanner(now_ms);           // the sample is not applied
        return;
    }
    sample_column(col, down, now_ms);
    scan_step = (uint8_t)((scan_step + 1) % SCAN_STEPS);
}

void panel_input_init(void) {
    gpio_init(PLAY_BUTTON_PIN);
    gpio_set_dir(PLAY_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(PLAY_BUTTON_PIN);
    generation++;
    memset(keys, 0, sizeof(keys));
    memset(&play, 0, sizeof(play));
    ring_head = 0;
    ring_count = 0;
    forget_play();              // PLAY held at power-up does nothing
    hold_keys();                // nor does a key
    scan_step = CHECK_STEP;
    // A failed boot configuration is retried by the first poll; a configuration
    // whose first check fails waits PANEL_RETRY_MS from that check.
    retry_armed = false;
    scanner_configured = key_scan_init();
    scanner_checked = false;
}

void panel_input_resync(void) {
    generation++;
    forget_play();
    ring_count = 0;             // PLAY events too: nothing from the stall is a command
    hold_keys();
    // No I2C: the scanner keeps its configuration and key_scan its column
    // selection, so the next column step still releases a column left low.
    // The check comes first, and finds a chip that reset during the stall.
    scan_step = CHECK_STEP;
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
    // Then one bus step, so a pass never blocks for a whole scan.
    if (scanner_configured) scan_step_once(now_ms);
    else configure(now_ms);
}

bool panel_input_next(panel_event_t *out) {
    if (ring_count == 0) return false;
    *out = ring[ring_head];
    ring_head = (ring_head + 1) % EVENT_RING_SIZE;
    ring_count--;
    return true;
}

bool panel_input_pad_down(uint8_t track, uint8_t row) {
    if (track >= 8 || row >= 8) return false;
    for (uint8_t r = 0; r < PANEL_ROWS; r++) {
        for (uint8_t col = 0; col < PANEL_COLS; col++) {
            const key_control_t *control = &key_map[r][col];
            if (control->kind == PANEL_PAD && control->track == track && control->row == row) {
                return keys[r][col].accepted;
            }
        }
    }
    return false;
}

uint32_t panel_input_generation(void) {
    return generation;
}
