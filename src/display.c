#include "sequencer.h"
#include "kit.h"
#include "ssd1309.h"
#include <stdio.h>
#include <string.h>

// ─── Track Names ───────────────────────────────────────────────

// All callers render on Core 0 and use one track label per draw call.
static const char *track_name(uint8_t track) {
    static char label[5];
    snprintf(label, sizeof(label), "T%u", (unsigned)track + 1u);
    return label;
}

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

static const char *tempo_prefix;    // NULL: INT, shown as always
static float tempo_bpm;             // 0: transport.bpm (no prefix) or not known

void display_set_tempo(const char *prefix, float bpm) {
    tempo_prefix = prefix;
    tempo_bpm = bpm;
}

// The tempo the screens show; <= 0 when an external one is not known.
static float shown_bpm(void) {
    if (!tempo_prefix && tempo_bpm <= 0.0f) return g_seq.transport.bpm;
    return tempo_bpm;
}

// "%5.1f", or with a prefix "EXT 145.0" / "EXT  --.-".
static void format_tempo(char *buf, size_t size, const char *prefix, float bpm) {
    if (!prefix) {
        snprintf(buf, size, "%5.1f", (double)bpm);
    } else if (bpm > 0.0f) {
        snprintf(buf, size, "%s %5.1f", prefix, (double)bpm);
    } else {
        snprintf(buf, size, "%s  --.-", prefix);
    }
}

void display_draw_bpm(const char *prefix, float bpm) {
    char buf[12];
    format_tempo(buf, sizeof(buf), prefix, bpm);
    // Without a prefix at x 78 as always; with one it ends left of the transport
    // symbol, clear of the header (at most 10 characters).
    uint8_t x = prefix ? (uint8_t)(OLED_WIDTH - FONT_CHAR_W * 2 - FONT_CHAR_W * strlen(buf)) : 78;
    ssd1309_draw_string(x, 0, buf, false);
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

// ─── Kit Lane Steps (shared by the kit views) ──────────────────
//
// A step is a CELL_W-wide box at the grid's pitch: off an outline, note an
// outline with a filled centre, accent filled, flam a short bar then a tall
// one (grace note, then the hit); beyond the length (' ') nothing. The
// playhead is a bar under its step, one row below it.

#define KIT_PLAYHEAD_GAP    1
#define KIT_PLAYHEAD_H      2

static void draw_kit_glyph(uint8_t x, uint8_t y, uint8_t h, char glyph) {
    switch (glyph) {
        case '.':
            ssd1309_draw_rect(x, y, CELL_W, h, true);
            break;
        case 'o':
            ssd1309_draw_rect(x, y, CELL_W, h, true);
            ssd1309_fill_rect(x + 2, y + 2, CELL_W - 4, h - 4, true);
            break;
        case 'O':
            ssd1309_fill_rect(x, y, CELL_W, h, true);
            break;
        case 'f':
            ssd1309_fill_rect(x, y + h / 2, 3, h - h / 2, true);
            ssd1309_fill_rect(x + 4, y, CELL_W - 4, h, true);
            break;
        default:
            break;
    }
}

// The 16 glyphs of `strip` (kit_lane_glyphs) in rows y..y+h-1, the playhead's
// bar below them (playhead >= STEPS_PER_PATTERN: none).
static void draw_kit_strip(uint8_t y, uint8_t h, const char *strip, uint8_t playhead) {
    for (uint8_t s = 0; s < STEPS_PER_PATTERN && strip[s]; s++) {
        draw_kit_glyph((uint8_t)(s * CELL_PITCH), y, h, strip[s]);
    }
    if (playhead < STEPS_PER_PATTERN) {
        ssd1309_fill_rect((uint8_t)(playhead * CELL_PITCH), y + h + KIT_PLAYHEAD_GAP,
                          CELL_W, KIT_PLAYHEAD_H, true);
    }
}

// ─── Session View ──────────────────────────────────────────────
//
// Layout (128×64):
//   Line 0 (y=0):   BANK A  ROW 3     P (or MIXED; P/S = Pattern/Song mode)
//   Line 1 (y=14):  T3  B4  LEN 16     (focused track, location, length)
//   Line 2 (y=28):  120.0 BPM 1 BAR
//   Bar (y=44–51):  16 step boxes, the global step in the bar filled
//
// On a kit focus line 1 names the lane (T3  B4  SD  LEN 16), the lane's strip
// takes y=37–47 (steps 37–44, playhead 46–47) and the bar moves to y=50–55.

#define BAR_Y       44
#define BAR_H       8
#define KIT_STRIP_STEP_H    8
#define KIT_BAR_Y   50
#define KIT_BAR_H   6

_Static_assert(DISPLAY_KIT_STRIP_H == KIT_STRIP_STEP_H + KIT_PLAYHEAD_GAP + KIT_PLAYHEAD_H,
               "the kit strip's rows");
_Static_assert(DISPLAY_KIT_STRIP_Y + DISPLAY_KIT_STRIP_H < KIT_BAR_Y, "strip above the bar");

static const char *const launch_quant_names[LAUNCH_QUANT_COUNT] = {
    [LAUNCH_QUANT_TRACK_LOOP] = "TRACK LOOP",
    [LAUNCH_QUANT_BAR]        = "1 BAR",
    [LAUNCH_QUANT_BEAT]       = "1 BEAT",
    [LAUNCH_QUANT_STEP]       = "NEXT STEP",
};

const char *display_launch_quant_name(launch_quant_t quant) {
    return (unsigned)quant < LAUNCH_QUANT_COUNT ? launch_quant_names[quant] : "?";
}

void display_draw_session_view(uint8_t bank, uint8_t common_row, play_mode_t mode,
                               uint8_t focus_track, uint8_t focus_location, uint8_t focus_length,
                               float bpm, launch_quant_t quant, uint8_t bar_step,
                               const char *kit_lane, const char *kit_strip, uint8_t kit_playhead) {
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
    unsigned first = (focus_track / PANEL_TRACKS) * PANEL_TRACKS + 1u;
    snprintf(line, sizeof(line), "%u-%u", first, first + PANEL_TRACKS - 1u);
    ssd1309_draw_string(13 * FONT_CHAR_W, 0, line, false);
    ssd1309_draw_string((FONT_COLS - 1) * FONT_CHAR_W, 0,
                        mode == PLAY_MODE_SONG ? "S" : "P", false);

    char label[4];
    pattern_label(focus_location, label, sizeof(label));
    bool kit = kit_lane && kit_strip;
    if (kit) {
        snprintf(line, sizeof(line), "%s  %s  %s  LEN %u",
                 track_name(focus_track), label, kit_lane, (unsigned)focus_length);
    } else {
        snprintf(line, sizeof(line), "%s  %s  LEN %u",
                 track_name(focus_track), label, (unsigned)focus_length);
    }
    ssd1309_draw_string(0, 14, line, false);

    if (tempo_prefix) {
        // "EXT 145.0 1 BAR": the prefix takes the place of the word BPM.
        char tempo[12];
        format_tempo(tempo, sizeof(tempo), tempo_prefix, bpm);
        snprintf(line, sizeof(line), "%s %s", tempo, display_launch_quant_name(quant));
    } else {
        snprintf(line, sizeof(line), "%5.1f BPM %s", (double)bpm,
                 display_launch_quant_name(quant));
    }
    ssd1309_draw_string(0, 28, line, false);

    uint8_t bar_y = BAR_Y, bar_h = BAR_H;
    if (kit) {
        draw_kit_strip(DISPLAY_KIT_STRIP_Y, KIT_STRIP_STEP_H, kit_strip, kit_playhead);
        bar_y = KIT_BAR_Y;
        bar_h = KIT_BAR_H;
    }
    for (uint8_t s = 0; s < STEPS_PER_PATTERN; s++) {
        uint8_t x = (uint8_t)(s * CELL_PITCH);
        if (s == bar_step) {
            ssd1309_fill_rect(x, bar_y, CELL_W, bar_h, true);
        } else {
            ssd1309_draw_rect(x, bar_y, CELL_W, bar_h, true);
        }
    }
    // Beat marks under the first step of each beat.
    for (uint8_t beat = 0; beat < 4; beat++) {
        uint8_t x = (uint8_t)(beat * 4 * CELL_PITCH);
        ssd1309_draw_hline(x, x + CELL_W - 1, bar_y + bar_h + 1, true);
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

// Transport symbol at the right edge of the header line.
static void draw_transport_symbol(void) {
    const char *transport_sym;
    switch (g_seq.transport.state) {
        case TRANSPORT_PLAYING: transport_sym = ">"; break;
        case TRANSPORT_PAUSED:  transport_sym = "||"; break;
        default:                transport_sym = "[]"; break;
    }
    ssd1309_draw_string(OLED_WIDTH - FONT_CHAR_W * 2, 0, transport_sym, false);
}

// Mute line (y=44, the track underlined) and the track's queued location (y=56).
static void draw_mutes_and_queue(uint8_t track) {
    uint8_t mutes = (uint8_t)(seq_get_track_mutes() >> (track / PANEL_TRACKS * PANEL_TRACKS));
    char mute_str[22];
    snprintf(mute_str, sizeof(mute_str), "M:");
    for (int t = 0; t < PANEL_TRACKS; t++) {
        char c = (mutes & (1 << t)) ? '_' : ('1' + t);
        mute_str[2 + t * 2] = c;
        mute_str[3 + t * 2] = ' ';
    }
    mute_str[2 + PANEL_TRACKS * 2] = '\0';
    ssd1309_draw_string(0, 44, mute_str, false);

    // Highlight current track in mute display
    uint8_t mx = (2 + (track % PANEL_TRACKS) * 2) * FONT_CHAR_W;
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

// ─── Piano Roll (Pattern view of a SYNTH clip) ─────────────────
//
// Time runs top to bottom (4 px per step, step 1 at the top), pitch left to
// right (4 px per semitone, 26 semitones). The info column takes x 0..23.
// Every coordinate is an int and is clipped to the roll before it reaches a
// drawing primitive, because those take uint8_t and would wrap.

#define ROLL_X0         24      // First roll column
#define ROLL_X1         127     // Last roll column
#define ROLL_SEMITONES  26
#define ROLL_STEP_H     4
#define ROLL_BASE_MAX   (127 - (ROLL_SEMITONES - 1))

static int roll_clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// A pixel of the roll; anything outside it is dropped.
static void roll_set(int x, int y, bool on) {
    if (x < ROLL_X0 || x > ROLL_X1 || y < 0 || y >= OLED_HEIGHT) return;
    ssd1309_set_pixel((uint8_t)x, (uint8_t)y, on);
}

static void roll_hline(int x0, int x1, int y) {
    for (int x = x0; x <= x1; x++) roll_set(x, y, true);
}

static bool roll_step_on(const pattern_t *p, uint8_t track, int s, uint8_t length) {
    return s >= 0 && s < (int)length && (p->steps[track][s].flags & STEP_FLAG_ACTIVE);
}

// The lowest note of the window: a pure function of the clip and the held
// step, so it needs no state between frames.
static int roll_window_base(const pattern_t *p, uint8_t track, uint8_t length,
                            int8_t held_step) {
    int lo = 127, hi = 0;
    bool any = false;
    for (int s = 0; s < STEPS_PER_PATTERN; s++) {
        if (!roll_step_on(p, track, s, length)) continue;
        int note = p->steps[track][s].note;
        if (note < lo) lo = note;
        if (note > hi) hi = note;
        any = true;
    }
    if (!any) lo = hi = 60;

    int span = hi - lo;
    int b = span <= ROLL_SEMITONES - 1 ? lo - (ROLL_SEMITONES - 1 - span) / 2 : lo;
    // A step being edited must never leave the window.
    if (held_step >= 0 && roll_step_on(p, track, held_step, length)) {
        int h = p->steps[track][held_step].note;
        b = roll_clamp(b, h - (ROLL_SEMITONES - 1), h);
    }
    return roll_clamp(b, 0, ROLL_BASE_MAX);
}

// First column of a note's block; a note outside the window gets the edge column.
static int roll_column(int note, int base) {
    if (note < base) return ROLL_X0;
    if (note >= base + ROLL_SEMITONES) return ROLL_X1;
    return ROLL_X0 + 4 * (note - base);
}

static void draw_piano_roll(const pattern_t *p, uint8_t track, uint8_t play_step,
                            int8_t held_step) {
    uint8_t length = pattern_get_track_length(p, track);
    if (length > STEPS_PER_PATTERN) length = STEPS_PER_PATTERN;
    int base = roll_window_base(p, track, length, held_step);
    bool held = held_step >= 0 && held_step < STEPS_PER_PATTERN;

    // Beat lines first, so that notes draw over them.
    for (int s = 3; s < STEPS_PER_PATTERN - 1; s += 4) {
        for (int n = 0; n < ROLL_SEMITONES; n++) {
            roll_set(ROLL_X0 + 4 * n + 3, s * ROLL_STEP_H + 3, true);
        }
    }

    for (int s = 0; s < STEPS_PER_PATTERN; s++) {
        int y = s * ROLL_STEP_H;
        if (s >= length) {
            // Dotted line: the step is outside the clip, and its notes stay hidden.
            for (int x = ROLL_X0; x <= ROLL_X1; x += 2) roll_set(x, y + 1, true);
            continue;
        }
        if (!roll_step_on(p, track, s, length)) continue;

        const step_t *st = &p->steps[track][s];
        int x = roll_column(st->note, base);
        bool inside = st->note >= base && st->note < base + ROLL_SEMITONES;
        int width = !inside ? 1 : ((st->flags & STEP_FLAG_ACCENT) ? 4 : 3);
        for (int row = 0; row < 3; row++) roll_hline(x, x + width - 1, y + row);

        // Row y+3 belongs to the slide and the tie.
        if ((st->flags & STEP_FLAG_SLIDE) && roll_step_on(p, track, s + 1, length)) {
            uint8_t next = p->steps[track][s + 1].note;
            int xn = roll_column(next, base);
            bool next_inside = next >= base && next < base + ROLL_SEMITONES;
            int from = x < xn ? x : xn;
            int to = x > xn ? x : xn;
            // The line ends on the last column of the far block; an edge bar
            // is one column wide, so the line stops on it.
            if (x > xn ? inside : next_inside) to += 2;
            roll_hline(from, to, y + 3);
        }
        if (st->flags & STEP_FLAG_TIE) roll_set(x + 1, y + 3, true);
    }

    // The playing step: invert its four rows, under the old grid's condition.
    if (play_step < length &&
        p == seq_playing_pattern(track) &&
        g_seq.transport.state == TRANSPORT_PLAYING) {
        for (int y = play_step * ROLL_STEP_H; y < (play_step + 1) * ROLL_STEP_H; y++) {
            for (int x = ROLL_X0; x <= ROLL_X1; x++) {
                roll_set(x, y, !ssd1309_get_pixel((uint8_t)x, (uint8_t)y));
            }
        }
    }

    // Info column: four characters per line, lines 8 px apart.
    char text[8];
    char label[4];
    pattern_label(g_seq.active_pattern[track], label, sizeof(label));
    snprintf(text, sizeof(text), "%.2sT%d", label, track + 1);
    ssd1309_draw_string(0, 0, text, false);
    snprintf(text, sizeof(text), "L%u", (unsigned)length);
    ssd1309_draw_string(0, 8, text, false);

    if (held) {
        const step_t *st = &p->steps[track][held_step];
        char note_str[5];
        note_to_str(st->note, note_str);
        snprintf(text, sizeof(text), "S%02d", held_step + 1);
        ssd1309_draw_string(0, 16, text, false);
        ssd1309_draw_string(0, 24, note_str, false);
        snprintf(text, sizeof(text), "v%d", st->velocity);
        ssd1309_draw_string(0, 32, text, false);
        char flags[4] = "";
        size_t n = 0;
        if (st->flags & STEP_FLAG_ACCENT) flags[n++] = 'A';
        if (st->flags & STEP_FLAG_SLIDE)  flags[n++] = 'S';
        if (st->flags & STEP_FLAG_TIE)    flags[n++] = 'T';
        flags[n] = '\0';
        ssd1309_draw_string(0, 40, flags, false);
    }

    char note_str[5];
    note_to_str((uint8_t)base, note_str);
    ssd1309_draw_string(0, 48, note_str, false);
    float bpm = shown_bpm();
    if (bpm > 0.0f) {
        snprintf(text, sizeof(text), "%d", (int)(bpm + 0.5f));
    } else {
        snprintf(text, sizeof(text), "--");
    }
    ssd1309_draw_string(0, 56, text, false);

    // Held step marker in column 23, the spare column of the info column. It
    // goes last because the text above clears the gap after every character.
    if (held) {
        for (int y = held_step * ROLL_STEP_H; y < (held_step + 1) * ROLL_STEP_H; y++) {
            ssd1309_set_pixel(ROLL_X0 - 1, (uint8_t)y, true);
        }
    }
}

void display_draw_pattern_view(const pattern_t *p, uint8_t track, uint8_t step, int8_t held_step) {
    if (p->tracks[track].type == TRACK_TYPE_SYNTH) {
        draw_piano_roll(p, track, step, held_step);
        return;
    }

    // ── Header line ──
    char label[4];
    pattern_label(g_seq.active_pattern[track], label, sizeof(label));

    char header[22];
    snprintf(header, sizeof(header), "%-3s %-3s", label, track_name(track));
    ssd1309_draw_string(0, 0, header, false);

    // BPM is drawn separately (by display_draw_bpm or ui_render)
    draw_transport_symbol();

    // ── Track info line ──
    char info[22];
    const track_config_t *tc = &p->tracks[track];
    const char *out_str;
    switch (tc->output_mode) {
        case OUTPUT_CV_GATE:    out_str = "CV"; break;
        case OUTPUT_BOTH:       out_str = "M+C"; break;
        default:                out_str = "MIDI"; break;
    }
    bool held = held_step >= 0 && held_step < STEPS_PER_PATTERN;
    if (held) {
        // Values of the step held for editing replace the track info
        const step_t *st = &p->steps[track][held_step];
        char note_str[5];
        note_to_str(st->note, note_str);
        snprintf(info, sizeof(info), "S%02d Note:%-4s Vel:%3d", held_step + 1, note_str, st->velocity);
    } else {
        snprintf(info, sizeof(info), "Ch:%-2d %-4s L:%-2d",
                 tc->midi_channel + 1, out_str, pattern_get_track_length(p, track));
    }
    ssd1309_draw_string(0, 10, info, false);

    // ── Step grid ──
    draw_step_grid(p, track, step, held ? held_step : -1);  // -1 = no selected step

    draw_mutes_and_queue(track);
}

// ─── Kit Lane View (Pattern view on a kit) ─────────────────────
//
// Layout (128×64):
//   Line 0 (y=0):   A1  T2  SD   120.0 >
//   Line 1 (y=10):  KIT Ch:10 L:16
//   Strip (y=20–31): the lane's 16 steps, playhead bar at y=33–34
//   Line 5+ (y=44): Mute status, queue

void display_draw_kit_lane_view(uint8_t track, uint8_t location, const char *lane,
                                const char *strip, uint8_t playhead) {
    if (track >= NUM_TRACKS) track = 0;
    if (location >= MAX_PATTERNS) location = 0;
    const pattern_t *p = &g_seq.patterns[location];

    char label[4];
    pattern_label(location, label, sizeof(label));
    char line[22];
    snprintf(line, sizeof(line), "%-3s %-3s %s", label, track_name(track), lane ? lane : "");
    ssd1309_draw_string(0, 0, line, false);
    draw_transport_symbol();                // BPM is drawn by ui_render

    snprintf(line, sizeof(line), "KIT Ch:%-2d L:%-2d",
             p->tracks[track].midi_channel + 1, pattern_get_track_length(p, track));
    ssd1309_draw_string(0, 10, line, false);

    if (strip) draw_kit_strip(GRID_Y, GRID_H, strip, playhead);
    // Beat separators between groups of 4, in the gaps beside the playhead bar.
    for (int beat = 1; beat < 4; beat++) {
        ssd1309_set_pixel(beat * 4 * CELL_PITCH - 1, GRID_Y + GRID_H + 1, true);
    }

    draw_mutes_and_queue(track);
}

// ─── Step Edit View ────────────────────────────────────────────

void display_draw_step_edit(const pattern_t *p, uint8_t track, uint8_t step) {
    const step_t *st = &p->steps[track][step];

    // Header
    char header[22];
    snprintf(header, sizeof(header), "EDIT %s Step %02d",
             track_name(track), step + 1);
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

// ─── Kit Step Edit View ────────────────────────────────────────
//
// Layout (128×64):
//   Line 0 (y=0):     KIT T2  STEP 05
//   y=12, 24, 36:     the 12 lanes, four per row: name and value box; the
//                     cursor lane's name inverted and framed
//   Line (y=52):      SD  ACCENT       (the cursor lane's value in words)

#define KIT_EDIT_Y          12
#define KIT_EDIT_ROW        12
#define KIT_EDIT_COL        32
#define KIT_EDIT_LANES_ROW  4
#define KIT_EDIT_BOX_X      18
#define KIT_EDIT_BOX_H      9

void display_draw_kit_step_edit(uint8_t track, uint8_t step, uint32_t word, uint8_t cursor_lane) {
    static const char *const value_names[4] = {"OFF", "NOTE", "ACCENT", "FLAM"};
    if (track >= NUM_TRACKS) track = 0;
    if (cursor_lane >= KIT_LANES) cursor_lane = KIT_LANE_AC;

    char line[22];
    snprintf(line, sizeof(line), "KIT %s  STEP %02u", track_name(track), step + 1u);
    ssd1309_draw_string(0, 0, line, false);

    for (uint8_t lane = 0; lane < KIT_LANES; lane++) {
        uint8_t x = (uint8_t)((lane % KIT_EDIT_LANES_ROW) * KIT_EDIT_COL);
        uint8_t y = (uint8_t)(KIT_EDIT_Y + (lane / KIT_EDIT_LANES_ROW) * KIT_EDIT_ROW);
        bool cursor = lane == cursor_lane;
        ssd1309_draw_string(x + 2, y + 1, kit_lane_name(lane), cursor);
        draw_kit_glyph(x + KIT_EDIT_BOX_X, y, KIT_EDIT_BOX_H, kit_value_glyph(kit_get(word, lane)));
        if (cursor) ssd1309_draw_rect(x, y - 1, KIT_EDIT_COL - 4, KIT_EDIT_BOX_H + 2, true);
    }

    uint8_t value = kit_get(word, cursor_lane);
    const char *value_name = cursor_lane == KIT_LANE_AC ? (value ? "ON" : "OFF")
                                                        : value_names[value];
    snprintf(line, sizeof(line), "%s  %s", kit_lane_name(cursor_lane), value_name);
    ssd1309_draw_string(0, 52, line, false);
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
    // The clock source (step 15), right of the queue line.
    static const char *const sources[CLOCK_SOURCE_COUNT] = {"INT", "EXT", "AUTO"};
    char clk[9];
    snprintf(clk, sizeof(clk), "CLK %s",
             g_seq.clock_source < CLOCK_SOURCE_COUNT ? sources[g_seq.clock_source] : "?");
    ssd1309_draw_string(OLED_WIDTH - FONT_CHAR_W * (uint8_t)strlen(clk), 30, clk, false);

    char len[22];
    snprintf(len, sizeof(len), "Len: %2d step", length);
    ssd1309_draw_string(12, 37, len, false);

    // Track mutes display
    char mute_line[24] = "Mutes: ";
    for (int t = 0; t < PANEL_TRACKS; t++) {
        char c = (mutes & (1 << t)) ? '_' : ('1' + t);
        mute_line[7 + t * 2] = c;
        mute_line[8 + t * 2] = ' ';
    }
    mute_line[7 + PANEL_TRACKS * 2] = '\0';
    ssd1309_draw_string(0, 44, mute_line, false);

    sequencer_jitter_stats_t jit;
    sequencer_get_jitter_stats(&jit);
    // Flam second hits skipped on a full MIDI queue, between title and label.
    if (jit.flam_drops > 0) {
        char drops[22];
        snprintf(drops, sizeof(drops), "FLAM DROP %lu", (unsigned long)jit.flam_drops);
        ssd1309_draw_string(0, 8, drops, false);
    }
    char jit_line[22];
    if (g_seq.clock_source != CLOCK_SOURCE_INT) {
        // The external clock: largest Clock deviation, losses, bursts caught up.
        snprintf(jit_line, sizeof(jit_line), "X:%lu L:%lu C:%lu",
                 (unsigned long)jit.ext_max_dev_us, (unsigned long)jit.ext_losses,
                 (unsigned long)jit.ext_catchups);
    } else {
        snprintf(jit_line, sizeof(jit_line), "J:%ld P:%lu I:%lu",
                 (long)jit.max_irq_latency_us,
                 (unsigned long)jit.max_abs_period_error_us,
                 (unsigned long)jit.max_isr_duration_us);
    }
    ssd1309_draw_string(0, 56, jit_line, false);
}

// ─── Track Config View ─────────────────────────────────────────

#define CONFIG_Y        12
#define CONFIG_PITCH    10
#define CONFIG_ROWS     5

static void track_config_row(const track_config_t *tc, uint8_t param,
                             char *line, size_t len) {
    switch (param) {
        case TRACK_PARAM_TYPE:
            snprintf(line, len, " Type:   %s", tc->type == TRACK_TYPE_SYNTH ? "Synth" : "Drum");
            break;
        case TRACK_PARAM_CHANNEL:
            snprintf(line, len, " MIDI Ch: %d", tc->midi_channel + 1);
            break;
        case TRACK_PARAM_OUTPUT: {
            const char *out = "MIDI";
            if (tc->output_mode == OUTPUT_CV_GATE) out = "CV/Gate";
            if (tc->output_mode == OUTPUT_BOTH) out = "MIDI+CV";
            snprintf(line, len, " Output: %s", out);
            break;
        }
        case TRACK_PARAM_GATE:
            snprintf(line, len, " Gate:   %d%%", tc->gate_length_pct);
            break;
        case TRACK_PARAM_NOTE:
            if (tc->type == TRACK_TYPE_DRUM) {
                char note[5];
                note_to_str(tc->drum_note, note);
                snprintf(line, len, " Note:   %s (%d)", note, tc->drum_note);
                break;
            }
            // Falls through.
        default:
            snprintf(line, len, " CV/Gate: %d", tc->cv_channel + 1);
            break;
    }
}

void display_draw_track_config(const track_config_t *tc, uint8_t track,
                                uint8_t param_cursor) {
    char line[22];
    snprintf(line, sizeof(line), "TRACK %d CONFIG", track + 1);
    ssd1309_draw_string(0, 0, line, false);

    const uint8_t first = param_cursor == TRACK_PARAM_DRUM_CV ? 1 : 0;
    for (uint8_t row = 0; row < CONFIG_ROWS; row++) {
        uint8_t param = first + row;
        uint8_t y = CONFIG_Y + row * CONFIG_PITCH;
        track_config_row(tc, param, line, sizeof(line));
        ssd1309_draw_string(0, y, line, param_cursor == param);
        if (param_cursor == param) ssd1309_draw_char(0, y, '>', false);
    }
    if (first) ssd1309_draw_char(OLED_WIDTH - FONT_CHAR_W, CONFIG_Y, '^', false);
    else if (tc->type == TRACK_TYPE_DRUM) {
        ssd1309_draw_char(OLED_WIDTH - FONT_CHAR_W, CONFIG_Y + (CONFIG_ROWS - 1) * CONFIG_PITCH, 'v', false);
    }
}

// ─── Kit Config View ───────────────────────────────────────────
//
// Title, then five of the KIT_PARAM_COUNT rows (y=12..52, 10 px apart),
// scrolled to keep the cursor row in view; '^'/'v' at the right edge when rows
// lie above/below.

// One row of the kit list into `line`: label, then value.
static void kit_config_row(const track_config_t *tc, const kit_config_t *kc, uint8_t param,
                           char *line, size_t len) {
    char label[10];
    char value[12];
    switch (param) {
        case KIT_PARAM_TYPE:
            snprintf(label, sizeof(label), "TYPE");
            snprintf(value, sizeof(value), "KIT");
            break;
        case KIT_PARAM_CHANNEL:
            snprintf(label, sizeof(label), "CHANNEL");
            snprintf(value, sizeof(value), "%u", tc->midi_channel + 1u);
            break;
        case KIT_PARAM_GATE:
            snprintf(label, sizeof(label), "GATE");
            snprintf(value, sizeof(value), "%u%%", (unsigned)tc->gate_length_pct);
            break;
        case KIT_PARAM_FLAM: {
            // Pulses, and their time at the current tempo.
            float bpm = shown_bpm();
            float ms = bpm > 0.0f ? (float)kc->flam_pulses * 60000.0f / (bpm * (float)PPQN) : 0.0f;
            snprintf(label, sizeof(label), "FLAM");
            snprintf(value, sizeof(value), "%u %.1fms", (unsigned)kc->flam_pulses, (double)ms);
            break;
        }
        case KIT_PARAM_VEL_NORMAL:
            snprintf(label, sizeof(label), "VEL NORM");
            snprintf(value, sizeof(value), "%u", (unsigned)kc->vel_normal);
            break;
        case KIT_PARAM_VEL_ACCENT:
            snprintf(label, sizeof(label), "VEL ACC");
            snprintf(value, sizeof(value), "%u", (unsigned)kc->vel_accent);
            break;
        case KIT_PARAM_VEL_MAX:
            snprintf(label, sizeof(label), "VEL MAX");
            snprintf(value, sizeof(value), "%u", (unsigned)kc->vel_max);
            break;
        default: {
            uint8_t voice = (uint8_t)(param - KIT_PARAM_NOTE);
            char note_str[5];
            note_to_str(kc->note[voice], note_str);
            snprintf(label, sizeof(label), "NOTE %s", kit_lane_name(voice + 1u));
            snprintf(value, sizeof(value), "%s (%u)", note_str, (unsigned)kc->note[voice]);
            break;
        }
    }
    snprintf(line, len, " %-9s%s", label, value);
}

void display_draw_kit_config(const track_config_t *tc, const kit_config_t *kc,
                             uint8_t track, uint8_t param) {
    if (track >= NUM_TRACKS) track = 0;
    if (param >= KIT_PARAM_COUNT) param = KIT_PARAM_COUNT - 1;

    char line[22];
    snprintf(line, sizeof(line), "TRACK %d CONFIG", track + 1);
    ssd1309_draw_string(0, 0, line, false);

    uint8_t first = param < 2 ? 0 : (uint8_t)(param - 2);
    if (first > KIT_PARAM_COUNT - CONFIG_ROWS) first = KIT_PARAM_COUNT - CONFIG_ROWS;
    for (uint8_t row = 0; row < CONFIG_ROWS; row++) {
        uint8_t p = (uint8_t)(first + row);
        uint8_t y = (uint8_t)(CONFIG_Y + row * CONFIG_PITCH);
        kit_config_row(tc, kc, p, line, sizeof(line));
        ssd1309_draw_string(0, y, line, p == param);
        if (p == param) ssd1309_draw_char(0, y, '>', false);
    }
    uint8_t edge = OLED_WIDTH - FONT_CHAR_W;
    if (first > 0) ssd1309_draw_char(edge, CONFIG_Y, '^', false);
    if (first + CONFIG_ROWS < KIT_PARAM_COUNT) {
        ssd1309_draw_char(edge, CONFIG_Y + (CONFIG_ROWS - 1) * CONFIG_PITCH, 'v', false);
    }
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
