// Native check of the simulator core: boots the firmware, presses PLAY and
// looks at what leaves the MIDI wire and the panel. No browser needed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_api.h"

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

typedef struct { double t; uint8_t status, d1, d2; } msg_t;
static msg_t msgs[65536];
static int msg_count;

// Decodes the byte stream: running status, real-time bytes in place.
static void decode(void) {
    const sim_event_t *ev = sim_events();
    uint8_t status = 0, data[2];
    int have = 0;
    for (int i = 0; i < sim_events_count(); i++) {
        if (ev[i].kind != SIM_EV_MIDI) continue;
        uint8_t b = (uint8_t)ev[i].value;
        if (b >= 0xF8) {
            msgs[msg_count++] = (msg_t){ ev[i].t_us, b, 0, 0 };
        } else if (b & 0x80) {
            status = b;
            have = 0;
        } else {
            CHECK(status != 0);
            data[have++] = b;
            if (have == 2) {
                msgs[msg_count++] = (msg_t){ ev[i].t_us, status, data[0], data[1] };
                have = 0;
            }
        }
    }
    sim_events_clear();
}

static double now_ms(void) { return sim_now() / 1000.0; }
static void run_ms(double ms) { sim_run_until(sim_now() + ms * 1000.0); }

static void tap_play(void) {
    sim_gpio_in(28, 0);
    run_ms(60);
    sim_gpio_in(28, 1);
    run_ms(60);
}

static int count(uint8_t status, int d1) {
    int n = 0;
    for (int i = 0; i < msg_count; i++) {
        if (msgs[i].status == status && (d1 < 0 || msgs[i].d1 == d1) &&
            (status < 0xF0 ? msgs[i].d2 > 0 || (status & 0xF0) == 0x80 : 1)) n++;
    }
    return n;
}

static void print_oled(void) {
    const uint8_t *fb = sim_oled();
    for (int y = 0; y < 64; y += 2) {
        for (int x = 0; x < 128; x++) {
            int top = fb[(y / 8) * 128 + x] >> (y % 8) & 1;
            int bottom = fb[((y + 1) / 8) * 128 + x] >> ((y + 1) % 8) & 1;
            fputs(top && bottom ? "█" : top ? "▀" : bottom ? "▄" : " ", stdout);
        }
        putchar('\n');
    }
}

int main(void) {
    sim_boot(0);
    run_ms(200);
    CHECK(sim_dbg_fatal() == NULL);
    CHECK(sim_oled_seq() > 3);
    CHECK(sim_oled_on());
    CHECK(sim_leds_seq() > 3);
    CHECK(sim_dbg_transport_state() == 0);
    printf("boot ok at %.0f ms: %u OLED frames, %u LED frames\n", now_ms(), sim_oled_seq(), sim_leds_seq());
    print_oled();

    // PLAY: the power-up demo (kick on track 1, kit on track 2) at 120 BPM.
    tap_play();
    CHECK(sim_dbg_transport_state() == 1);
    double started = now_ms();
    run_ms(4000);
    decode();
    printf("played %.0f ms: %d messages\n", now_ms() - started, msg_count);
    int clocks = count(0xF8, -1);
    int kicks = 0, hats = 0, snares = 0;
    double first_kick = -1, last_kick = -1;
    for (int i = 0; i < msg_count; i++) {
        if (msgs[i].status == 0x99 && msgs[i].d2 > 0) {
            if (msgs[i].d1 == 36) {
                kicks++;
                if (first_kick < 0) first_kick = msgs[i].t;
                last_kick = msgs[i].t;
            }
            if (msgs[i].d1 == 42) hats++;
            if (msgs[i].d1 == 38) snares++;
        }
    }
    printf("clocks %d, kicks %d, hats %d, snares %d, start %d\n", clocks, kicks, hats, snares, count(0xFA, -1));
    CHECK(count(0xFA, -1) == 1);
    CHECK(clocks >= 192 && clocks <= 198);          // 48 a second at 120 BPM, about 4.1 s of playing
    CHECK(kicks == 8 || kicks == 9);
    CHECK(hats >= 31 && hats <= 33);
    CHECK(snares == 4);
    double kick_period = (last_kick - first_kick) / (kicks - 1) / 1000.0;
    printf("kick period %.3f ms\n", kick_period);
    CHECK(kick_period > 499.5 && kick_period < 500.5);

    // A pad on row 2 of track 1 while playing: queued, then applied on the bar.
    CHECK(sim_dbg_current(0) == 0);
    sim_key(1, 0, 1);
    run_ms(80);
    sim_key(1, 0, 0);
    run_ms(20);
    CHECK(sim_dbg_queued(0) == 1);
    run_ms(2100);
    CHECK(sim_dbg_current(0) == 1);
    CHECK(sim_dbg_queued(0) == 0xFF);

    // Step button 3 toggles a step of the focused clip (track 1, slot 2) on release.
    CHECK((sim_dbg_step(1, 0, 2) >> 16 & 1) == 0);
    sim_step_button(2, 1);
    run_ms(100);
    sim_step_button(2, 0);
    run_ms(100);
    CHECK((sim_dbg_step(1, 0, 2) >> 16 & 1) == 1);
    CHECK(sim_step_leds() & (1u << 2));

    // Encoder 2, one detent clockwise: tempo up by 0.1.
    double bpm = sim_dbg_bpm();
    static const int quad[4][2] = { {0, 1}, {0, 0}, {1, 0}, {1, 1} };
    for (int dir = 0; dir < 2; dir++) {
        for (int e = 0; e < 4; e++) {
            const int *ab = quad[dir ? 3 - e : e];
            sim_gpio_in(21, dir ? quad[(6 - e) % 4][0] : ab[0]);
            sim_gpio_in(22, dir ? quad[(6 - e) % 4][1] : ab[1]);
            run_ms(3);
        }
        run_ms(200);
        printf("encoder 2 turn %d: %.1f BPM\n", dir, sim_dbg_bpm());
    }
    CHECK(sim_dbg_bpm() > bpm - 0.25 && sim_dbg_bpm() < bpm + 0.25);

    // Stop.
    tap_play();
    CHECK(sim_dbg_transport_state() == 0);
    decode();
    msg_count = 0;

    print_oled();
    CHECK(sim_dbg_fatal() == NULL);
    CHECK(sim_dbg_lock_errors() == 0);
    CHECK(sim_dbg_overruns() == 0);
    CHECK(!sim_dbg_midi_overflow());
    CHECK(sim_events_dropped() == 0);
    printf("PASS smoke (%u LED frames skipped)\n", sim_leds_skipped());
    return 0;
}
