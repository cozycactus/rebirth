// Boots the unmodified firmware on the virtual board and steps it through
// virtual time. src/main.c is compiled into this file so its static
// init_defaults() and its global g_seq are the real ones; only its endless
// loop is replaced, by core0_pass() below, which repeats the loop body.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void stdio_init_all(void);
void multicore_launch_core1(void (*entry)(void));

#define main firmware_main
#include "../../src/main.c"
#undef main

#include "kit.h"
#include "midi_song.h"
#include "hardware/flash.h"
#include "sim_internal.h"

static uint64_t next_pass_us;
static uint32_t last_display_ms;

void sim_boot(int keep_flash) {
    hal_reset(keep_flash != 0);
    sim_rgb_reset();

    // The body of main() up to its loop.
    stdio_init_all();
    init_defaults();
    midi_init();
    cv_gate_init();
    display_init();
    sequencer_init();
    ui_init();
    multicore_launch_core1(sequencer_core1_entry);

    next_pass_us = sim_now_us;
    last_display_ms = 0;
}

// One iteration of the loop in main().
static void core0_pass(void) {
    ui_update();

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_display_ms >= 33) {
        last_display_ms = now;
        display_clear();
        ui_render();
        display_update();
    }
}

// Interrupts run only between core 0 passes, never inside a critical section,
// and each at the instant it is due: the timer handler sees no latency. Timing
// noise of the real board (lock waits, flash cache misses) is not modelled.
void sim_run_until(double t_us) {
    uint64_t target = t_us <= 0 ? 0 : (uint64_t)t_us;
    while (!sim_dbg_fatal()) {
        uint64_t alarm = hal_alarm_due();
        uint64_t bell = hal_doorbell_due();
        uint64_t uart = hal_uart_irq_due();
        uint64_t input = hal_input_due();
        uint64_t pass = next_pass_us > sim_now_us ? next_pass_us : sim_now_us;
        uint64_t next = alarm;
        if (bell < next) next = bell;
        if (uart < next) next = uart;
        if (pass < next) next = pass;
        if (input < next) next = input;
        if (next > target) break;
        hal_set_now(next);          // applies the inputs due by then
        if (next == alarm) {
            hal_run_alarm();
        } else if (next == bell) {
            hal_run_doorbell();
        } else if (next == uart) {
            hal_run_uart_irq();
        } else if (next == pass) {
            core0_pass();
            // A pass that stalled (flash save, a dead I2C chip) does not run again
            // in the past.
            next_pass_us = (next_pass_us > sim_now_us ? next_pass_us : sim_now_us) + SIM_UI_PASS_US;
        }
    }
    hal_set_now(target);
}

double sim_now(void) { return (double)sim_now_us; }

#ifndef SIM_FEATURES
#define SIM_FEATURES 0
#endif
int sim_features(void) { return SIM_FEATURES; }
int sim_dac_full_scale_mv(void) { return CV_DAC_FULL_SCALE_MV; }
uint8_t *sim_midi_song_slot(void) { return (uint8_t *)(XIP_BASE + MIDI_SONG_FLASH_OFFSET); }
int sim_midi_song_capacity(void) { return MIDI_SONG_FLASH_BYTES; }
int sim_midi_song_open(void) { return ui_open_midi_song(); }

// ─── Inspection ─────────────────────────────────────────────────

int sim_dbg_transport_state(void) { return g_seq.transport.state; }
double sim_dbg_bpm(void) { return g_seq.transport.bpm; }
uint32_t sim_dbg_pulse(void) { return g_seq.transport.pulse_count; }
int sim_dbg_current(int track) { return g_seq.transport.current_pattern[track & (NUM_TRACKS - 1)]; }
int sim_dbg_queued(int track) { return g_seq.transport.queued_pattern[track & (NUM_TRACKS - 1)]; }
uint32_t sim_dbg_step(int location, int track, int step) {
    const step_t *s = &g_seq.patterns[location & 31].steps[track & (NUM_TRACKS - 1)][step & 15];
    return s->note | ((uint32_t)s->velocity << 8) | ((uint32_t)s->flags << 16);
}
uint32_t sim_dbg_kit_step(int location, int track, int step) {
    return (unsigned)track < KIT_TRACK_SLOTS ? g_seq.patterns[location & 31].kit_steps[track][step & 15] : 0;
}
int sim_dbg_track_length(int location, int track) {
    return pattern_get_track_length(&g_seq.patterns[location & 31], track & (NUM_TRACKS - 1));
}
uint32_t sim_dbg_track_config(int track) {
    const track_config_t *c = &g_seq.patterns[0].tracks[track & (NUM_TRACKS - 1)];
    return (uint32_t)c->type | ((uint32_t)c->midi_channel << 8) |
           ((uint32_t)c->gate_length_pct << 16) | ((uint32_t)c->output_mode << 24) |
           ((uint32_t)c->cv_channel << 26);
}
int sim_dbg_mutes(void) { return seq_get_track_mutes(); }
int sim_dbg_clock_source(void) { return g_seq.clock_source; }
int sim_dbg_clock_state(void) {
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    return (snap.ext_following ? 1 : 0) | (snap.ext_armed ? 2 : 0) | (snap.ext_present ? 4 : 0);
}
int sim_dbg_ext_tenths(void) {
    transport_snapshot_t snap;
    sequencer_snapshot(&snap);
    return snap.ext_shown_x16 ? (int)(400000000.0 / snap.ext_shown_x16 + 0.5) : 0;
}
uint32_t sim_dbg_ext_losses(void) {
    sequencer_jitter_stats_t stats;
    sequencer_get_jitter_stats(&stats);
    return stats.ext_losses;
}
uint32_t sim_dbg_overruns(void) {
    sequencer_jitter_stats_t stats;
    sequencer_get_jitter_stats(&stats);
    return stats.overruns;
}
int sim_dbg_midi_overflow(void) { return midi_output_overflowed(); }
