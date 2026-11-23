// Density probe: how much playback one DIN MIDI wire (320 us per byte, fake UART in
// wire-time mode) carries before the TX queue overflows. Built by run_probe.py in a
// temporary copy of the tree, never in test/. It reuses every helper of test_playback.c
// (reset, make_kit, set_gate, set_flam, tick, ...) by including the file with its main()
// renamed.
//
//   density_probe              the full table, CSV on stdout
//   density_probe long         the 64-bar cells
//   density_probe cell NAME SYNTH KITS FLAM ONE_CHANNEL GATE BPM   one cell, CSV
//
// Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
#define main playback_tests_main
#include "test_playback.c"
#undef main

extern uint16_t probe_tx_peak;
extern uint32_t probe_tx_enqueued;
uint16_t probe_tx_fill(void);
extern bool probe_tx_overflow, probe_rt_overflow, probe_buf_overflow;

typedef struct {
    const char *name;
    uint8_t synth;          // SYNTH tracks
    bool kits;              // kits on tracks 0 and 1; SYNTH tracks then start at track 2
    bool flam;              // every kit voice KIT_FLAM, flam_pulses 3
    bool one_channel;       // every SYNTH track on channel 0 (MIDI channel 1)
    uint8_t gate;           // gate % on every track
} probe_case_t;

#define CSV_HEADER "name;bpm;result;stopped_step;peak_queued;overflow_cause;flam_drops;wire_bytes;wire_bytes_per_step;queued_bytes_per_step;clocks_expected;clocks_on_wire;max_clock_late_us;overruns;fill_at_end_of_run\n"
#define BARS 8u
#define STEPS (BARS * 16u)
#define CLOCK_DIV (PPQN / MIDI_PPQN)

static void setup(const probe_case_t *c) {
    uint8_t first = c->kits ? 2 : 0;
    if (c->kits) {
        for (uint8_t kit = 0; kit < 2; kit++) {
            make_kit(kit, (uint8_t)(9 + kit));      // channels 10 and 11 (0-based 9, 10)
            for (uint8_t p = 0; p < MAX_PATTERNS; p++) {
                g_seq.patterns[p].tracks[kit].output_mode = OUTPUT_MIDI_ONLY;
            }
            set_flam(kit, c->flam ? 3 : g_seq.kit_config[kit].flam_pulses);
            uint32_t word = 0;
            for (uint8_t lane = 1; lane < KIT_LANES; lane++) {
                word = kit_set(word, lane, c->flam ? KIT_FLAM : KIT_NOTE);
            }
            for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) g_seq.patterns[0].kit_steps[kit][s] = word;
            sequencer_refresh_track_playback(kit, 0);
        }
    }
    for (uint8_t i = 0; i < c->synth; i++) {
        uint8_t t = (uint8_t)(first + i);
        track_config_t *tc = &g_seq.patterns[0].tracks[t];
        tc->type = TRACK_TYPE_SYNTH;
        tc->output_mode = OUTPUT_MIDI_ONLY;
        tc->midi_channel = c->one_channel ? 0 : (uint8_t)(t % 16);
        for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
            // 52 + t: a distinct note per track, above the kit notes (36-51).
            pattern_set_step(&g_seq.patterns[0], t, s, (uint8_t)(52 + t), 100, STEP_FLAG_ACTIVE);
        }
        sequencer_refresh_track_playback(t, 0);
    }
    uint8_t used = (uint8_t)(first + c->synth);
    for (uint8_t t = 0; t < used; t++) set_gate(t, c->gate);
}

static uint32_t clock_due[STEPS * PULSES_PER_STEP / CLOCK_DIV];

static void run_case(const probe_case_t *c, float bpm) {
    reset();
    probe_tx_peak = 0; probe_tx_enqueued = 0;
    probe_tx_overflow = probe_rt_overflow = probe_buf_overflow = false;
    fake_uart_run_until(0);                 // wire-time mode
    transport_set_bpm(bpm);
    setup(c);
    transport_start();
    unsigned clocks = 0;
    int stopped_step = -1;
    uint16_t peak = 0;
    for (unsigned pulse = 0; pulse < STEPS * PULSES_PER_STEP; pulse++) {
        fake_uart_run_until(timer_hw->alarm[0]);
        if (pulse % CLOCK_DIV == 0) clock_due[clocks++] = timer_hw->alarm[0];
        tick();
        if (g_seq.transport.state != TRANSPORT_PLAYING) { stopped_step = (int)(pulse / PULSES_PER_STEP); break; }
        peak = probe_tx_peak;               // peak up to the last pulse that survived
    }
    unsigned fill_end = stopped_step < 0 ? probe_tx_fill() : 0; // queued bytes after the last pulse, before the drain
    sequencer_jitter_stats_t js;
    sequencer_get_jitter_stats(&js);
    uint32_t enq = probe_tx_enqueued;
    unsigned wire_bytes = 0, wire_clocks = 0;
    unsigned max_late = 0;
    if (stopped_step < 0) {
        CHECK(!midi_output_overflowed());
        fake_uart_run_until(timer_hw->timerawl + 3000000); // the backlog drains
        CHECK(!fake_uart_tx_irq);
        wire_bytes = fake_midi_length;
        unsigned k = 0;
        for (unsigned i = 0; i < fake_midi_length; i++) {
            if (fake_midi[i] != MIDI_CLOCK) continue;
            wire_clocks++;
            if (k < clocks) {
                unsigned late = fake_midi_time_us[i] - clock_due[k];
                if (late > max_late) max_late = late;
            }
            k++;
        }
    }
    // name;bpm;result;stopped_step;peak;cause;flam_drops;wire_bytes;wire_per_step;queued_per_step;clocks_expected;clocks_wire;max_clock_late_us;overruns
    bool any = probe_tx_overflow || probe_rt_overflow || probe_buf_overflow;
    printf("%s;%g;%s;%d;%u;%s%s%s%s;%u;%u;%.2f;%.2f;%u;%u;%u;%u;%u\n",
           c->name, (double)bpm, stopped_step < 0 ? "ok" : "STOP", stopped_step, peak,
           probe_tx_overflow ? "tx" : "", probe_rt_overflow ? "rt" : "", probe_buf_overflow ? "buf" : "", any ? "" : "-",
           (unsigned)js.flam_drops, wire_bytes, stopped_step < 0 ? (wire_bytes - 1u) / (double)STEPS : 0.0,
           enq / (double)(stopped_step < 0 ? STEPS : (unsigned)stopped_step + 1u),
           clocks, wire_clocks, max_late, (unsigned)js.overruns, fill_end);
}

static const float tempos[] = {100, 120, 140, 160, 180, 200, 240, 300};

static void run_all(const probe_case_t *c) {
    for (unsigned i = 0; i < sizeof(tempos) / sizeof(tempos[0]); i++) run_case(c, tempos[i]);
}

// Supplementary: one cell for up to `bars` bars; prints the queue fill at the end of every
// 8 bars, or the step that stopped playback.
static void run_long(const char *name, uint8_t synth, bool kits, float bpm, unsigned bars, bool flam) {
    probe_case_t c = {name, synth, kits, flam, false, 50};
    reset();
    fake_uart_run_until(0);
    transport_set_bpm(bpm);
    setup(&c);
    transport_start();
    printf("%s @ %g BPM, %u bars:", name, (double)bpm, bars);
    for (unsigned pulse = 0; pulse < bars * 16u * PULSES_PER_STEP; pulse++) {
        fake_uart_run_until(timer_hw->alarm[0]);
        tick();
        if (g_seq.transport.state != TRANSPORT_PLAYING) {
            printf(" STOP at step %u (0-based; bar %u)\n", pulse / PULSES_PER_STEP, pulse / PULSES_PER_STEP / 16u + 1u);
            return;
        }
        if ((pulse + 1u) % (8u * 16u * PULSES_PER_STEP) == 0) printf(" bar%u=%u", (pulse + 1u) / (16u * PULSES_PER_STEP), probe_tx_fill());
    }
    sequencer_jitter_stats_t js;
    sequencer_get_jitter_stats(&js);
    printf(" survived (flam_drops %u)\n", (unsigned)js.flam_drops);
}

int main(int argc, char **argv) {
    seq_lock_init();
    if (argc > 1 && strcmp(argv[1], "cell") == 0) {
        int synth = argc == 9 ? atoi(argv[3]) : -1, kits = argc == 9 ? atoi(argv[4]) != 0 : 0;
        int gate = argc == 9 ? atoi(argv[7]) : 0;
        if (argc != 9 || synth < 0 || synth + (kits ? 2 : 0) > NUM_TRACKS || gate < 10 || gate > 100) {
            fprintf(stderr, "usage: density_probe cell NAME SYNTH KITS FLAM ONE_CHANNEL GATE BPM\n"
                            "       SYNTH + 2 x KITS <= %d, GATE 10-100\n", NUM_TRACKS);
            return 2;
        }
        probe_case_t c = {argv[2], (uint8_t)synth, kits != 0, atoi(argv[5]) != 0,
                          atoi(argv[6]) != 0, (uint8_t)gate};
        printf(CSV_HEADER);
        run_case(&c, (float)atof(argv[8]));
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "long") == 0) {
        run_long("A N=26", 26, false, 300, 64, false);
        run_long("A N=32", 32, false, 240, 64, false);
        run_long("A N=48", 48, false, 160, 64, false);
        run_long("A N=25 (control)", 25, false, 300, 64, false);
        run_long("A N=64 (control)", 64, false, 120, 64, false);
        run_long("C K=14 (control)", 14, true, 240, 64, false);
        run_long("D K=14", 14, true, 240, 64, true);
        run_long("D K=22", 22, true, 200, 64, true);
        run_long("D K=30", 30, true, 160, 64, true);
        run_long("D K=6", 6, true, 300, 64, true);
        return 0;
    }
    printf(CSV_HEADER);
    static const uint8_t a_n[] = {8, 16, 24, 25, 26, 32, 48, 64};
    char names[64][32];
    unsigned ni = 0;
    for (unsigned i = 0; i < sizeof(a_n); i++) {
        snprintf(names[ni], 32, "A N=%u", a_n[i]);
        probe_case_t c = {names[ni++], a_n[i], false, false, false, 50};
        run_all(&c);
    }
    static const uint8_t b_n[] = {16, 32};
    for (unsigned i = 0; i < sizeof(b_n); i++) {
        snprintf(names[ni], 32, "B N=%u one-ch", b_n[i]);
        probe_case_t c = {names[ni++], b_n[i], false, false, true, 50};
        run_all(&c);
    }
    static const uint8_t k_n[] = {0, 6, 14, 22, 30};
    for (unsigned f = 0; f < 2; f++) {
        for (unsigned i = 0; i < sizeof(k_n); i++) {
            snprintf(names[ni], 32, "%s K=%u", f ? "D" : "C", k_n[i]);
            probe_case_t c = {names[ni++], k_n[i], true, f != 0, false, 50};
            run_all(&c);
        }
    }
    static const uint8_t gates[] = {100, 10};
    for (unsigned g = 0; g < 2; g++) {
        snprintf(names[ni], 32, "E A N=16 gate %u", gates[g]);
        probe_case_t a = {names[ni++], 16, false, false, false, gates[g]};
        run_all(&a);
        snprintf(names[ni], 32, "E C K=14 gate %u", gates[g]);
        probe_case_t c = {names[ni++], 14, true, false, false, gates[g]};
        run_all(&c);
    }
    return 0;
}
