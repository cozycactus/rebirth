#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <stdint.h>
#include <stdbool.h>

// ─── Configuration ──────────────────────────────────────────────

#define NUM_TRACKS          8
#define STEPS_PER_PATTERN   16
#define MAX_PATTERNS        64
#define MAX_SONG_ENTRIES    256
#define PPQN                96      // Pulses per quarter note
#define PULSES_PER_STEP     (PPQN / 4)  // 24 pulses per 16th note at 96 PPQN
#define DEFAULT_BPM         120.0f
#define MIN_BPM             20.0f
#define MAX_BPM             300.0f

// CV/Gate
#define NUM_CV_CHANNELS     4
#define CV_DAC_BITS         12
#define CV_DAC_MAX          4095
#define VOLTS_PER_OCTAVE    1.0f    // 1V/oct standard

// MIDI
#define MIDI_BAUD           31250
#define MIDI_NOTE_ON        0x90
#define MIDI_NOTE_OFF       0x80
#define MIDI_CC             0xB0
#define MIDI_CLOCK          0xF8
#define MIDI_START          0xFA
#define MIDI_STOP           0xFC
#define MIDI_CONTINUE       0xFB

// ─── Data Types ─────────────────────────────────────────────────

// Step flags
#define STEP_FLAG_ACTIVE    (1 << 0)    // Step triggers
#define STEP_FLAG_ACCENT    (1 << 1)    // Accent (higher velocity)
#define STEP_FLAG_SLIDE     (1 << 2)    // Legato/slide to next step
#define STEP_FLAG_TIE       (1 << 3)    // Tie to next step (no retrigger)

typedef struct {
    uint8_t note;           // MIDI note 0–127
    uint8_t velocity;       // 0–127
    uint8_t flags;          // STEP_FLAG_*
} step_t;

typedef enum {
    TRACK_TYPE_SYNTH,       // Melodic — uses note field
    TRACK_TYPE_DRUM,        // Percussive — fixed note per track
} track_type_t;

typedef enum {
    OUTPUT_MIDI_ONLY,
    OUTPUT_CV_GATE,
    OUTPUT_BOTH,
} output_mode_t;

typedef struct {
    track_type_t type;
    uint8_t      midi_channel;      // 0–15
    uint8_t      drum_note;         // Fixed note for drum tracks
    output_mode_t output_mode;
    uint8_t      cv_channel;        // 0–3 (which CV/Gate pair)
    uint8_t      gate_length_pct;   // Gate length as % of step (10–100)
    bool         muted;
} track_config_t;

typedef struct {
    step_t         steps[NUM_TRACKS][STEPS_PER_PATTERN];
    track_config_t tracks[NUM_TRACKS];
    uint8_t        swing;           // 0–99, percentage of swing
    char           name[9];         // 8 chars + null
} pattern_t;

// Pattern with A/B variations (like ReBirth)
typedef struct {
    pattern_t      variation[2];    // 0 = A, 1 = B
} pattern_pair_t;

// Song mode
typedef struct {
    uint8_t pattern_index;  // Which pattern to play
    uint8_t variation;      // 0 = A, 1 = B
    uint8_t repeats;        // How many times (1–255, 0 = skip)
} song_entry_t;

typedef enum {
    SONG_END_LOOP,          // Loop back to start
    SONG_END_STOP,          // Stop transport at end
} song_end_mode_t;

typedef struct {
    song_entry_t entries[MAX_SONG_ENTRIES];
    uint16_t     length;    // Number of valid entries
    song_end_mode_t end_mode;
    char         name[9];
} song_t;

// Transport state
typedef enum {
    TRANSPORT_STOPPED,
    TRANSPORT_PLAYING,
    TRANSPORT_PAUSED,
} transport_state_t;

typedef enum {
    PLAY_MODE_PATTERN,      // Loop single pattern
    PLAY_MODE_SONG,         // Play song chain
} play_mode_t;

typedef struct {
    transport_state_t state;
    play_mode_t       mode;
    float             bpm;
    uint32_t          pulse_count;      // Current pulse within pattern
    uint8_t           current_step;     // 0–15
    uint8_t           current_pattern;  // Pattern index for pattern mode
    uint8_t           current_variation;// 0 = A, 1 = B
    uint8_t           queued_pattern;   // Next pattern (0xFF = none)
    uint8_t           queued_variation; // Next variation
    uint16_t          song_position;    // Current entry in song
    uint8_t           song_repeat_count;// Repeats remaining
    bool              step_triggered;   // Flag: new step just started
} transport_t;

// ─── Shared State (accessed by both cores) ──────────────────────

typedef struct {
    volatile transport_t transport;
    pattern_pair_t       patterns[MAX_PATTERNS];
    song_t               song;
    uint8_t              active_pattern;     // Pattern being edited
    uint8_t              active_variation;   // Which variation (A/B) is being edited
    uint8_t              track_mutes;        // Bitmask: bit N = track N muted for playback
    // Spinlock index for mutex
    uint32_t             spinlock_num;
} sequencer_state_t;

// ─── Global ─────────────────────────────────────────────────────

extern sequencer_state_t g_seq;

// ─── API ────────────────────────────────────────────────────────

// sequencer.c — Core 1
void sequencer_core1_entry(void);

// transport
void transport_start(void);
void transport_stop(void);
void transport_continue(void);
void transport_set_bpm(float bpm);

// pattern.c
void pattern_init(pattern_t *p, uint8_t index);
void pattern_set_step(pattern_t *p, uint8_t track, uint8_t step,
                      uint8_t note, uint8_t velocity, uint8_t flags);
void pattern_clear_step(pattern_t *p, uint8_t track, uint8_t step);
void pattern_toggle_step(pattern_t *p, uint8_t track, uint8_t step);
void pattern_copy(pattern_t *dst, const pattern_t *src);

// Pattern access helpers (returns active variation)
pattern_t *seq_active_pattern(void);
pattern_t *seq_playing_pattern(void);

// Song editing
bool song_append(uint8_t pattern_index, uint8_t variation, uint8_t repeats);
bool song_insert(uint16_t position, uint8_t pattern_index, uint8_t variation, uint8_t repeats);
bool song_remove(uint16_t position);
bool song_move(uint16_t from, uint16_t to);
void song_clear(void);
void song_set_end_mode(song_end_mode_t mode);

// Pattern queue — set next pattern to switch at end of current
void transport_queue_pattern(uint8_t pattern_index, uint8_t variation);
void transport_clear_queue(void);

// Track mutes (bitmask, instant)
void seq_set_track_mute(uint8_t track, bool muted);
void seq_toggle_track_mute(uint8_t track);
uint8_t seq_get_track_mutes(void);

// midi_out.c
void midi_init(void);
void midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_send_note_off(uint8_t channel, uint8_t note);
void midi_send_cc(uint8_t channel, uint8_t cc, uint8_t value);
void midi_send_clock(void);
void midi_send_start(void);
void midi_send_stop(void);
void midi_send_continue(void);
// Buffered output — collect messages, then flush all at once
void midi_buf_clear(void);
void midi_buf_flush(void);
void midi_buf_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_buf_note_off(uint8_t channel, uint8_t note);

// cv_gate.c
void cv_gate_init(void);
void cv_set_note(uint8_t channel, uint8_t midi_note);
void cv_set_voltage(uint8_t channel, uint16_t dac_value);
void gate_on(uint8_t channel);
void gate_off(uint8_t channel);
void gate_commit(uint32_t on_mask, uint32_t off_mask);
void cv_gate_all_off(void);

// display.c
void display_init(void);
void display_clear(void);
void display_update(void);
void display_draw_pattern_view(const pattern_t *p, uint8_t track, uint8_t step);
void display_draw_song_view(const song_t *s, uint16_t pos);
void display_draw_bpm(float bpm);
void display_draw_text(uint8_t x, uint8_t y, const char *text);

// ui.c — Core 0
void ui_init(void);
void ui_update(void);  // Called in main loop

#endif // SEQUENCER_H
