#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "clock_in.h"
#include "sequencer.h"

// ─── Event ring: core 0's UART interrupt -> core 1 ──────────────
// One producer (the UART interrupt, core 0) and one consumer (core 1 under the
// playback lock). The indices run free; a barrier orders an entry before the
// head that publishes it, and the read of an entry before the tail that frees it.

_Static_assert((CLOCK_IN_RING_SIZE & (CLOCK_IN_RING_SIZE - 1u)) == 0,
               "CLOCK_IN_RING_SIZE must be a power of two");
_Static_assert((CLOCK_EST_HISTORY & (CLOCK_EST_HISTORY - 1u)) == 0,
               "CLOCK_EST_HISTORY must be a power of two");
_Static_assert(CLOCK_EST_HISTORY > CLOCK_EST_SHOWN, "the history holds the shown window");

static clock_in_event_t ring[CLOCK_IN_RING_SIZE];
static volatile uint32_t ring_head;     // written by core 0
static volatile uint32_t ring_tail;     // written by core 1
static volatile uint32_t ring_dropped;
static bool pushed;                     // core 0 only

// SPP being received: 0 none, 1 waiting for its LSB, 2 for its MSB.
static uint8_t spp_state;
static uint8_t spp_lsb;

static void __not_in_flash_func(push)(uint8_t kind, uint16_t value, uint32_t t_us) {
    uint32_t head = ring_head;
    if (head - ring_tail >= CLOCK_IN_RING_SIZE) {
        ring_dropped++;
        return;
    }
    clock_in_event_t *e = &ring[head & (CLOCK_IN_RING_SIZE - 1u)];
    e->t_us = t_us;
    e->value = value;
    e->kind = kind;
    __dmb();
    ring_head = head + 1u;
    pushed = true;
}

bool __not_in_flash_func(clock_in_rx_byte)(uint8_t byte, uint32_t t_us) {
    if (byte >= 0xF8u) {
        switch (byte) {
        case MIDI_CLOCK:    push(CLOCK_IN_TICK, 0, t_us); return true;
        case MIDI_START:    push(CLOCK_IN_START, 0, t_us); return true;
        case MIDI_CONTINUE: push(CLOCK_IN_CONTINUE, 0, t_us); return true;
        case MIDI_STOP:     push(CLOCK_IN_STOP, 0, t_us); return true;
        case 0xFFu:         spp_state = 0; return false;     // System Reset
        default:            return false;   // Active Sensing, undefined: no effect
        }
    }
    if (byte == 0xF2u) {
        spp_state = 1;
        return false;
    }
    if (byte & 0x80u) {
        spp_state = 0;      // any other status ends an SPP in progress
        return false;
    }
    if (spp_state == 1) {
        spp_lsb = byte;
        spp_state = 2;
    } else if (spp_state == 2) {
        push(CLOCK_IN_SPP, (uint16_t)(spp_lsb | ((uint16_t)byte << 7)), t_us);
        spp_state = 0;
    }
    return false;
}

bool __not_in_flash_func(clock_in_take_pushed)(void) {
    bool was = pushed;
    pushed = false;
    return was;
}

bool __not_in_flash_func(clock_in_pop)(clock_in_event_t *out) {
    uint32_t tail = ring_tail;
    if (tail == ring_head) return false;
    __dmb();
    *out = ring[tail & (CLOCK_IN_RING_SIZE - 1u)];
    __dmb();
    ring_tail = tail + 1u;
    return true;
}

uint32_t clock_in_dropped(void) {
    return ring_dropped;
}

void clock_in_reset(void) {
    ring_head = 0;
    ring_tail = 0;
    ring_dropped = 0;
    pushed = false;
    spp_state = 0;
}

// ─── Tempo estimate ─────────────────────────────────────────────

#define HISTORY_MASK (CLOCK_EST_HISTORY - 1u)

void __not_in_flash_func(clock_est_reset)(clock_est_t *e) {
    e->count = 0;
    e->window_start = 0;
    e->interval_us = 0;
    e->shown_x16 = 0;
    e->plausible = 0;
    e->off = 0;
}

uint32_t __not_in_flash_func(clock_est_last)(const clock_est_t *e) {
    return e->t[(e->count - 1u) & HISTORY_MASK];
}

uint32_t __not_in_flash_func(clock_est_timeout_us)(const clock_est_t *e) {
    // The longer of the two estimates: a burst shortens only the short one.
    uint32_t interval = e->interval_us;
    if (e->shown_x16 / 16u > interval) interval = e->shown_x16 / 16u;
    if (interval == 0) return CLOCK_EST_LOSS_UNKNOWN_US;
    uint32_t three = 3u * interval;
    return three > CLOCK_EST_LOSS_MIN_US ? three : CLOCK_EST_LOSS_MIN_US;
}

uint32_t __not_in_flash_func(clock_est_tick)(clock_est_t *e, uint32_t t_us) {
    uint32_t interval = 0;
    if (e->count > 0) {
        interval = t_us - clock_est_last(e);
        if (interval > clock_est_timeout_us(e)) {
            clock_est_reset(e);
            interval = 0;
        }
    }
    if (interval) {
        bool fits = e->interval_us == 0 ||
                    (interval >= e->interval_us / 2u && interval <= 2u * e->interval_us);
        e->plausible = fits ? (uint8_t)(e->plausible < 255u ? e->plausible + 1u : 255u) : 0;
    }
    e->t[e->count & HISTORY_MASK] = t_us;
    e->count++;
    uint32_t last = e->count - 1u;

    uint32_t k = last < CLOCK_EST_SHORT ? last : CLOCK_EST_SHORT;
    if (k) e->interval_us = (t_us - e->t[(last - k) & HISTORY_MASK]) / k;

    // The shown interval: the window's span over its intervals, then a running
    // average over Clocks once the window holds 16 intervals.
    if (e->interval_us && e->shown_x16) {
        uint32_t short_x16 = e->interval_us * 16u;
        uint32_t diff = short_x16 > e->shown_x16 ? short_x16 - e->shown_x16
                                                 : e->shown_x16 - short_x16;
        // Only an interval of a tempo (a quarter to four times the shown one) counts:
        // the bunched Clocks that end a stall are a burst, not a tempo change.
        uint32_t shown_us = e->shown_x16 / 16u;
        bool tempo = interval >= shown_us / 4u && interval <= 4u * shown_us;
        e->off = tempo && diff * 25u > e->shown_x16 ? (uint8_t)(e->off + 1u) : 0;
        if (e->off >= 3) {
            // A tempo change: restart the window from the last two intervals, which
            // three Clocks off in a row place after the change.
            e->window_start = last >= 2u ? last - 2u : 0;
            e->shown_x16 = 0;
            e->off = 0;
        }
    }
    uint32_t first = e->window_start;
    if (last > CLOCK_EST_SHOWN && last - CLOCK_EST_SHOWN > first) first = last - CLOCK_EST_SHOWN;
    uint32_t span = last - first;
    if (span) {
        uint32_t raw_x16 = ((t_us - e->t[first & HISTORY_MASK]) * 16u) / span;
        if (e->shown_x16 == 0 || span < 16u) {
            e->shown_x16 = raw_x16;
        } else {
            int32_t step = ((int32_t)raw_x16 - (int32_t)e->shown_x16) / 16;
            e->shown_x16 = (uint32_t)((int32_t)e->shown_x16 + step);
        }
    }
    return interval;
}
