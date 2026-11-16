#include "sequencer.h"
#include "ssd1309.h"
#include <stdio.h>
#include <string.h>

// ─── Track Names ───────────────────────────────────────────────

static const char *track_names[NUM_TRACKS] = {
    "T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8",
};

static void pattern_label(uint8_t pattern, char *buf, size_t len) {
    snprintf(buf, len, "%c%d",
             'A' + pattern_bank(pattern),
             pattern_slot(pattern) + 1);
}

// ─── Note Name Helpers ─────────────────────────────────────────

static const char *note_letters[12] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B",
};

// Format MIDI note as "C#4" into buf (must be >= 5 chars)
static void note_to_str(uint8_t note, char *buf) {
    if (note > 127) { snprintf(buf, 5, "???"); return; }
    int octave = (int)note / 12 - 1;    // MIDI 0 = C-1
    int semitone = note % 12;
    snprintf(buf, 5, "%s%d", note_letters[semitone], octave);
}

// ─── Lifecycle ─────────────────────────────────────────────────

void display_init(void) {
    ssd1309_init();
}

void display_clear(void) {
    ssd1309_clear();
}

void display_update(void) {
    ssd1309_flush();
}

// ─── Text ──────────────────────────────────────────────────────

void display_draw_text(uint8_t x, uint8_t y, const char *text) {
    ssd1309_draw_string(x, y, text, false);
}

// ─── BPM Display ───────────────────────────────────────────────

void display_draw_bpm(float bpm) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%5.1f", (double)bpm);
    ssd1309_draw_string(78, 0, buf, false);
}

// ─── Step Grid (shared by multiple views) ──────────────────────
//
// 16 steps, each 7px wide with 1px gap = 8px per cell
// Total width: 16 × 8 = 128px (exact fit)
// y_top..y_top+11: grid area (12px tall)

#define GRID_Y      20      // Top of step grid
#define GRID_H      12      // Height of each step cell
#define CELL_W      7       // Cell width (+ 1px gap = 8px pitch)
#define CELL_PITCH  8

static void draw_step_grid(const pattern_t *p, uint8_t track,
                           uint8_t play_step, int8_t selected_step) {
    uint8_t length = pattern_get_track_length(p, track);

    for (int s = 0; s < STEPS_PER_PATTERN; s++) {
        uint8_t x = s * CELL_PITCH;
        const step_t *st = &p->steps[track][s];
        bool active = (st->flags & STEP_FLAG_ACTIVE) != 0;
        bool in_range = s < length;

        if (!in_range) {
            ssd1309_draw_rect(x, GRID_Y, CELL_W, GRID_H, true);
            ssd1309_draw_hline(x + 1, x + CELL_W - 2, GRID_Y + GRID_H / 2, true);
        } else if (active) {
            // Filled box for active steps
            ssd1309_fill_rect(x, GRID_Y, CELL_W, GRID_H, true);

            // Accent indicator: small notch at top-left
            if (st->flags & STEP_FLAG_ACCENT) {
                ssd1309_set_pixel(x + 1, GRID_Y + 1, false);
                ssd1309_set_pixel(x + 2, GRID_Y + 1, false);
            }

            // Slide indicator: connecting line to next step
            if (st->flags & STEP_FLAG_SLIDE) {
                uint8_t nx = (s + 1) * CELL_PITCH;
                if (s < 15) {
                    ssd1309_draw_hline(x + CELL_W, nx, GRID_Y + GRID_H / 2, true);
                }
            }

            // Tie indicator: bracket at bottom
            if (st->flags & STEP_FLAG_TIE) {
                ssd1309_set_pixel(x + 1, GRID_Y + GRID_H - 2, false);
                ssd1309_set_pixel(x + 2, GRID_Y + GRID_H - 2, false);
                ssd1309_set_pixel(x + 3, GRID_Y + GRID_H - 2, false);
            }
        } else {
            // Empty box outline for inactive steps
            ssd1309_draw_rect(x, GRID_Y, CELL_W, GRID_H, true);
        }

        // Current playback step: invert the cell
        if (s == play_step &&
            p == seq_playing_pattern(track) &&
            g_seq.transport.state == TRANSPORT_PLAYING) {
            for (uint8_t px = x; px < x + CELL_W; px++) {
                for (uint8_t py = GRID_Y; py < GRID_Y + GRID_H; py++) {
                    bool cur = ssd1309_get_pixel(px, py);
                    ssd1309_set_pixel(px, py, !cur);
                }
            }
        }

        // Selected step for editing: draw bracket below
        if (s == selected_step) {
            ssd1309_draw_hline(x, x + CELL_W - 1, GRID_Y + GRID_H + 1, true);
            ssd1309_set_pixel(x, GRID_Y + GRID_H + 2, true);
            ssd1309_set_pixel(x + CELL_W - 1, GRID_Y + GRID_H + 2, true);
        }
    }

    // Beat separators: thin dots between groups of 4
    for (int beat = 1; beat < 4; beat++) {
        uint8_t x = beat * 4 * CELL_PITCH - 1;
        ssd1309_set_pixel(x, GRID_Y + GRID_H + 1, true);
    }

    // Pattern-length marker: vertical divider after last active step
    if (length < STEPS_PER_PATTERN) {
        uint8_t x = length * CELL_PITCH - 1;
        ssd1309_draw_vline(x, GRID_Y - 1, GRID_Y + GRID_H + 2, true);
    }
}

// ─── Session View ──────────────────────────────────────────────
//
// Layout (128×64):
//   Line 0 (y=0):   BANK A  ROW 3      (or MIXED)
//   Line 1 (y=14):  T3  B4  LEN 16     (focused track, location, length)
//   Line 2 (y=28):  120.0 BPM 1 BAR
//   Bar (y=44–51):  16 step boxes, the global step in the bar filled

#define BAR_Y       44
#define BAR_H       8

static const char *const launch_quant_names[LAUNCH_QUANT_COUNT] = {
    [LAUNCH_QUANT_TRACK_LOOP] = "TRACK LOOP",
    [LAUNCH_QUANT_BAR]        = "1 BAR",
    [LAUNCH_QUANT_BEAT]       = "1 BEAT",
    [LAUNCH_QUANT_STEP]       = "NEXT STEP",
};

const char *display_launch_quant_name(launch_quant_t quant) {
    return (unsigned)quant < LAUNCH_QUANT_COUNT ? launch_quant_names[quant] : "?";
}

void display_draw_session_view(uint8_t bank, uint8_t common_row,
                               uint8_t focus_track, uint8_t focus_location, uint8_t focus_length,
                               float bpm, launch_quant_t quant, uint8_t bar_step) {
    if (bank >= PATTERN_BANKS) bank = 0;
    if (focus_track >= NUM_TRACKS) focus_track = 0;
    if (focus_location >= MAX_PATTERNS) focus_location = 0;

    char line[22];
    if (common_row < PATTERNS_PER_BANK) {
        snprintf(line, sizeof(line), "BANK %c  ROW %u", 'A' + bank, common_row + 1u);
    } else {
        snprintf(line, sizeof(line), "BANK %c  MIXED", 'A' + bank);
    }
    ssd1309_draw_string(0, 0, line, false);

    char label[4];
    pattern_label(focus_location, label, sizeof(label));
    snprintf(line, sizeof(line), "%s  %s  LEN %u",
             track_names[focus_track], label, (unsigned)focus_length);
    ssd1309_draw_string(0, 14, line, false);

    snprintf(line, sizeof(line), "%5.1f BPM %s", (double)bpm,
             display_launch_quant_name(quant));
    ssd1309_draw_string(0, 28, line, false);

    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        uint8_t x = (uint8_t)(s * CELL_PITCH);
        if (s == bar_step) {
            ssd1309_fill_rect(x, BAR_Y, CELL_W, BAR_H, true);
        } else {
            ssd1309_draw_rect(x, BAR_Y, CELL_W, BAR_H, true);
        }
    }
    // Beat marks under the first step of each beat.
    for (uint8_t beat = 0; beat < 4; beat++) {
        uint8_t x = (uint8_t)(beat * 4 * CELL_PITCH);
        ssd1309_draw_hline(x, x + CELL_W - 1, BAR_Y + BAR_H + 1, true);
    }
}

// ─── Pattern View ──────────────────────────────────────────────
//
// Layout (128×64):
//   Line 0 (y=0):   A1  SY1  120.0 >
//   Line 1 (y=10):  Ch:10 MIDI  L:16
//   Grid (y=20–31): 16 step boxes
//   Line 4 (y=36):  Beat markers / position
//   Line 5+ (y=44): Mute status

void display_draw_pattern_view(const pattern_t *p, uint8_t track, uint8_t step) {
    // ── Header line ──
    const char *transport_sym;
    switch (g_seq.transport.state) {
        case TRANSPORT_PLAYING: transport_sym = ">"; break;
        case TRANSPORT_PAUSED:  transport_sym = "||"; break;
        default:                transport_sym = "[]"; break;
    }

    char label[4];
    pattern_label(g_seq.active_pattern[track], label, sizeof(label));

    char header[22];
    snprintf(header, sizeof(header), "%-3s %-3s", label, track_names[track]);
    ssd1309_draw_string(0, 0, header, false);

    // BPM is drawn separately (by display_draw_bpm or ui_render)
    // Transport symbol at right edge
    ssd1309_draw_string(OLED_WIDTH - FONT_CHAR_W * 2, 0, transport_sym, false);

    // ── Track info line ──
    char info[22];
    const track_config_t *tc = &p->tracks[track];
    const char *out_str;
    switch (tc->output_mode) {
        case OUTPUT_CV_GATE:    out_str = "CV"; break;
        case OUTPUT_BOTH:       out_str = "M+C"; break;
        default:                out_str = "MIDI"; break;
    }
    snprintf(info, sizeof(info), "Ch:%-2d %-4s L:%-2d",
             tc->midi_channel + 1, out_str, pattern_get_track_length(p, track));
    ssd1309_draw_string(0, 10, info, false);

    // ── Step grid ──
    draw_step_grid(p, track, step, -1);     // -1 = no selected step

    // ── Mute indicators ──
    uint8_t mutes = seq_get_track_mutes();
    char mute_str[22];
    snprintf(mute_str, sizeof(mute_str), "M:");
    for (int t = 0; t < NUM_TRACKS; t++) {
        char c = (mutes & (1 << t)) ? '_' : ('1' + t);
        mute_str[2 + t * 2] = c;
        mute_str[3 + t * 2] = ' ';
    }
    mute_str[2 + NUM_TRACKS * 2] = '\0';
    ssd1309_draw_string(0, 44, mute_str, false);

    // Highlight current track in mute display
    uint8_t mx = (2 + track * 2) * FONT_CHAR_W;
    ssd1309_draw_hline(mx, mx + FONT_CHAR_W - 1, 52, true);

    // ── Pattern queue indicator ──
    if (g_seq.transport.queued_pattern[track] != NO_QUEUED_PATTERN) {
        char q_label[4];
        pattern_label(g_seq.transport.queued_pattern[track], q_label, sizeof(q_label));
        char q[12];
        snprintf(q, sizeof(q), "Q:%s", q_label);
        ssd1309_draw_string(0, 56, q, false);
    }
}

// ─── Step Edit View ────────────────────────────────────────────

void display_draw_step_edit(const pattern_t *p, uint8_t track, uint8_t step) {
    const step_t *st = &p->steps[track][step];

    // Header
    char header[22];
    snprintf(header, sizeof(header), "EDIT %s Step %02d",
             track_names[track], step + 1);
    ssd1309_draw_string(0, 0, header, false);

    // Note and velocity
    char note_str[5];
    note_to_str(st->note, note_str);

    char line2[22];
    snprintf(line2, sizeof(line2), "Note:%-4s Vel:%3d", note_str, st->velocity);
    ssd1309_draw_string(0, 10, line2, false);

    // Flags
    bool act  = (st->flags & STEP_FLAG_ACTIVE) != 0;
    bool acc  = (st->flags & STEP_FLAG_ACCENT) != 0;
    bool sld  = (st->flags & STEP_FLAG_SLIDE) != 0;
    bool tie  = (st->flags & STEP_FLAG_TIE) != 0;

    // Draw each flag as a labeled box
    const char *flag_labels[] = { "ACT", "ACC", "SLD", "TIE" };
    const bool flag_vals[] = { act, acc, sld, tie };

    for (int f = 0; f < 4; f++) {
        uint8_t fx = f * 32;
        ssd1309_draw_rect(fx, 36, 30, 10, true);
        ssd1309_draw_string(fx + 2, 37, flag_labels[f], flag_vals[f]);
        if (flag_vals[f]) {
            // Fill background for active flags
            ssd1309_fill_rect(fx + 1, 37, 28, 8, true);
            ssd1309_draw_string(fx + 2, 37, flag_labels[f], true);
        }
    }

    // Step grid with selection marker
    draw_step_grid(p, track, g_seq.transport.current_track_step[track], step);

    // Hint
    ssd1309_draw_string(0, 56, "E1:note E2:vel", false);
}

// ─── Pattern Select View ───────────────────────────────────────

void display_draw_pattern_select(uint8_t pattern,
                                  uint8_t length,
                                  uint8_t queued_pat,
                                  uint8_t mutes) {
    // Title
    ssd1309_draw_string(16, 0, "SELECT PATTERN", false);

    char label[4];
    pattern_label(pattern, label, sizeof(label));

    // Big pattern display
    char big[12];
    snprintf(big, sizeof(big), ">> %s <<", label);
    ssd1309_draw_string(12, 16, big, true);

    // Queue info
    if (queued_pat != NO_QUEUED_PATTERN) {
        char q_label[4];
        pattern_label(queued_pat, q_label, sizeof(q_label));
        char q[22];
        snprintf(q, sizeof(q), "Queued: %s", q_label);
        ssd1309_draw_string(12, 30, q, false);
    } else {
        ssd1309_draw_string(12, 30, "Queued: ---", false);
    }

    char len[22];
    snprintf(len, sizeof(len), "Len: %2d step", length);
    ssd1309_draw_string(12, 37, len, false);

    // Track mutes display
    char mute_line[24] = "Mutes: ";
    for (int t = 0; t < NUM_TRACKS; t++) {
        char c = (mutes & (1 << t)) ? '_' : ('1' + t);
        mute_line[7 + t * 2] = c;
        mute_line[8 + t * 2] = ' ';
    }
    mute_line[7 + NUM_TRACKS * 2] = '\0';
    ssd1309_draw_string(0, 44, mute_line, false);

    sequencer_jitter_stats_t jit;
    sequencer_get_jitter_stats(&jit);
    char jit_line[22];
    snprintf(jit_line, sizeof(jit_line), "J:%ld P:%lu I:%lu",
             (long)jit.max_irq_latency_us,
             (unsigned long)jit.max_abs_period_error_us,
             (unsigned long)jit.max_isr_duration_us);
    ssd1309_draw_string(0, 56, jit_line, false);
}

// ─── Track Config View ─────────────────────────────────────────

void display_draw_track_config(const track_config_t *tc, uint8_t track,
                                uint8_t param_cursor) {
    // Title
    char title[22];
    snprintf(title, sizeof(title), "TRACK %d CONFIG", track + 1);
    ssd1309_draw_string(0, 0, title, false);

    // Type
    const char *type_str = (tc->type == TRACK_TYPE_SYNTH) ? "Synth" : "Drum";
    char line0[22];
    snprintf(line0, sizeof(line0), " Type:   %s", type_str);
    ssd1309_draw_string(0, 12, line0, param_cursor == 0);

    // MIDI channel
    char line1[22];
    snprintf(line1, sizeof(line1), " MIDI Ch: %d", tc->midi_channel + 1);
    ssd1309_draw_string(0, 22, line1, param_cursor == 1);

    // Output mode
    const char *out_str;
    switch (tc->output_mode) {
        case OUTPUT_CV_GATE: out_str = "CV/Gate"; break;
        case OUTPUT_BOTH:    out_str = "MIDI+CV"; break;
        default:             out_str = "MIDI"; break;
    }
    char line2[22];
    snprintf(line2, sizeof(line2), " Output: %s", out_str);
    ssd1309_draw_string(0, 32, line2, param_cursor == 2);

    // Gate length
    char line3[22];
    snprintf(line3, sizeof(line3), " Gate:   %d%%", tc->gate_length_pct);
    ssd1309_draw_string(0, 42, line3, param_cursor == 3);

    // Drum note (only for drum tracks)
    if (tc->type == TRACK_TYPE_DRUM) {
        char note_str[5];
        note_to_str(tc->drum_note, note_str);
        char line4[22];
        snprintf(line4, sizeof(line4), " Note:   %s (%d)", note_str, tc->drum_note);
        ssd1309_draw_string(0, 52, line4, param_cursor == 4);
    }

    // Cursor arrow on selected param
    ssd1309_draw_char(0, 12 + param_cursor * 10, '>', false);
}

// ─── Song View ─────────────────────────────────────────────────

void display_draw_song_view(const song_t *s, uint16_t pos, uint8_t track) {
    if (track >= NUM_TRACKS) track = 0;

    // Title
    char title[22];
    snprintf(title, sizeof(title), "SONG T%d", track + 1);
    ssd1309_draw_string(0, 0, title, false);

    // End mode
    const char *end_str = (s->end_mode == SONG_END_LOOP) ? "LOOP" : "STOP";
    ssd1309_draw_string(96, 0, end_str, false);

    // Show up to 5 entries centered on current position
    int start = (int)pos - 2;
    if (start < 0) start = 0;
    if (start + 5 > (int)s->length) start = (int)s->length - 5;
    if (start < 0) start = 0;

    for (int row = 0; row < 5; row++) {
        int idx = start + row;
        if (idx >= (int)s->length) break;

        uint8_t y = 12 + row * 10;
        bool is_current = (idx == (int)pos);

        char line[22];
        char label[4];
        pattern_label(s->entries[idx].pattern_index[track], label, sizeof(label));
        snprintf(line, sizeof(line), "%3d: %-3s x%d",
                 idx + 1,
                 label,
                 s->entries[idx].repeats);
        ssd1309_draw_string(0, y, line, is_current);
    }

    // Empty song message
    if (s->length == 0) {
        ssd1309_draw_string(18, 28, "(empty song)", false);
    }

    // Position occupies the footer; all five entry rows remain unobstructed.
    char position[22];
    snprintf(position, sizeof(position), "%u/%u", s->length ? pos + 1 : 0, s->length);
    ssd1309_draw_string(84, 56, position, false);
}
