#ifndef CLOCK_IN_H
#define CLOCK_IN_H

#include <stdbool.h>
#include <stdint.h>

// The external clock's way from the DIN input to core 1 (see ARCHITECTURE.md,
// "External clock"). The UART interrupt on core 0 hands every received byte to
// clock_in_rx_byte() with the time it read it; Clock, Start, Continue, Stop and
// Song Position Pointer become events in a single-producer ring that core 1
// drains under the playback lock. Core 0 then forces TIMER_IRQ_1 on core 1
// (bit CLOCK_IN_ALARM of timer_hw->intf; that alarm is never armed).

#define CLOCK_IN_ALARM      1       // the timer alarm whose interrupt is the doorbell
#define CLOCK_IN_RING_SIZE  32      // events; a power of two

typedef enum {
    CLOCK_IN_TICK,          // 0xF8
    CLOCK_IN_START,         // 0xFA
    CLOCK_IN_CONTINUE,      // 0xFB
    CLOCK_IN_STOP,          // 0xFC
    CLOCK_IN_SPP,           // 0xF2 lsb msb; value in sixteenth notes
} clock_in_kind_t;

typedef struct {
    uint32_t t_us;          // when the UART interrupt read the event's last byte
    uint16_t value;         // SPP: 0-16383
    uint8_t  kind;          // clock_in_kind_t
} clock_in_event_t;

// Core 0, UART interrupt (in RAM). One received byte, read at t_us. Returns true
// for Clock, Start, Continue and Stop, which end here and do not go to the note
// parser. An SPP's bytes return false: they go there too, as before. Real-time
// bytes between the bytes of an SPP do not interrupt it; any other status does.
bool clock_in_rx_byte(uint8_t byte, uint32_t t_us);
// Core 0, UART interrupt: true once after events were pushed (ring the doorbell).
bool clock_in_take_pushed(void);
// Core 1, playback lock held: the oldest event; false when the ring is empty.
bool clock_in_pop(clock_in_event_t *out);
// Events that found the ring full, since boot.
uint32_t clock_in_dropped(void);
// Empties the ring and the SPP parser. Only while nothing else uses them (init).
void clock_in_reset(void);

// ─── Tempo estimate (pure) ──────────────────────────────────────
// Kept by core 1 from the Clock times.

#define CLOCK_EST_HISTORY       128     // Clock times kept; a power of two, > the shown window
#define CLOCK_EST_SHORT         4       // intervals in the sub-pulse estimate
#define CLOCK_EST_SHOWN         96      // intervals in the shown tempo's window (one bar)
#define CLOCK_EST_LOSS_MIN_US   50000u  // the loss timeout is never shorter
#define CLOCK_EST_LOSS_UNKNOWN_US 375000u // three Clock intervals at 20 BPM

typedef struct {
    uint32_t t[CLOCK_EST_HISTORY];
    uint32_t count;         // Clocks since the estimate last restarted
    uint32_t window_start;  // index (in count) of the shown window's first Clock
    uint32_t interval_us;   // mean of the last CLOCK_EST_SHORT intervals; 0 = not known
    uint32_t shown_x16;     // shown interval in us x 16; 0 = not known
    uint8_t  plausible;     // intervals in a row within half to twice the estimate
    uint8_t  off;           // Clocks in a row with the short estimate > 4 % off the shown
} clock_est_t;

void clock_est_reset(clock_est_t *e);
// A Clock at t_us. An interval longer than the loss timeout restarts the
// estimate first. Returns the interval to the previous Clock, 0 if none.
uint32_t clock_est_tick(clock_est_t *e, uint32_t t_us);
// The time of the last Clock; meaningful when count > 0.
uint32_t clock_est_last(const clock_est_t *e);
// No Clock for this long means the clock is lost: max(3 x interval, 50 ms), the
// interval being the longer of the short and the shown estimate (a burst shortens
// only the short one); 375 ms while neither is known.
uint32_t clock_est_timeout_us(const clock_est_t *e);

#endif
