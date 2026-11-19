#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <stdint.h>
#include <stdbool.h>

// ─── Configuration ──────────────────────────────────────────────

#define PANEL_TRACKS        8
#define NUM_TRACKS          64
typedef uint64_t track_mask_t;
#define TRACK_BIT(track) (UINT64_C(1) << (track))
#define ALL_TRACKS UINT64_MAX
#define STEPS_PER_PATTERN   16
#define PATTERN_BANKS       4
#define PATTERNS_PER_BANK   8
#define MAX_PATTERNS        (PATTERN_BANKS * PATTERNS_PER_BANK)
#define MAX_SONG_ENTRIES    256
#define NO_QUEUED_PATTERN   0xFF
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
#ifndef CV_DAC_GAIN
#define CV_DAC_GAIN         1      // 1x: 2.048 V; 2x requires suitable supply/headroom
#endif
#define CV_DAC_FULL_SCALE_MV (2048u * CV_DAC_GAIN)

// MIDI
#define MIDI_BAUD           31250
#define MIDI_PPQN           24
#define MIDI_NOTE_ON        0x90
#define MIDI_NOTE_OFF       0x80
#define MIDI_CC             0xB0
#define MIDI_CC_ALL_SOUND_OFF 120
#define MIDI_CC_ALL_NOTES_OFF 123
#define MIDI_CLOCK          0xF8
#define MIDI_START          0xFA
#define MIDI_STOP           0xFC
#define MIDI_CONTINUE       0xFB
// The UART TX queue (a power of two; one byte stays unused, so it holds one less).
#define MIDI_TX_QUEUE_SIZE  1024
// One slide per track and both kits hitting all their voices on one step.
#define MIDI_STEP_BUFFER_BYTES (6 * NUM_TRACKS + 3 * MAX_KIT_TRACKS * KIT_VOICES)

// Jitter measurement
#define JITTER_PROBE_PIN    7       // High while sequencer timer ISR runs

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
    TRACK_TYPE_KIT,         // 909 drum machine over MIDI — 12 lanes per step
} track_type_t;

// Kit lanes: 0 AC, 1 BD, 2 SD, 3 LT, 4 MT, 5 HT, 6 RS, 7 CP, 8 CH, 9 OH, 10 CC, 11 RC.
// Voice v is lane v + 1. A step word holds lane i in bits 2i..2i+1.
#define KIT_LANES       12
#define KIT_VOICES      11
#define KIT_LANE_AC     0
#define KIT_LANE_BD     1
#define KIT_LANE_SD     2
#define KIT_LANE_CH     8
#define KIT_LANE_OH     9
#define KIT_TRACK_SLOTS 8
#define MAX_KIT_TRACKS  2
#define KIT_FLAM_MIN    1
#define KIT_FLAM_MAX    12

typedef enum { KIT_OFF = 0, KIT_NOTE = 1, KIT_ACCENT = 2, KIT_FLAM = 3 } kit_value_t;

typedef struct {
    uint8_t note[KIT_VOICES];          // lane order BD..RC
    uint8_t vel_normal, vel_accent, vel_max;
    uint8_t flam_pulses;
} kit_config_t;

// Rows of a kit track's config screen, in order (OUTPUT and CV are not used by a kit).
typedef enum {
    KIT_PARAM_TYPE,
    KIT_PARAM_CHANNEL,
    KIT_PARAM_GATE,
    KIT_PARAM_FLAM,
    KIT_PARAM_VEL_NORMAL,
    KIT_PARAM_VEL_ACCENT,
    KIT_PARAM_VEL_MAX,
    KIT_PARAM_NOTE,                     // NOTE BD; voice v is KIT_PARAM_NOTE + v
    KIT_PARAM_COUNT = KIT_PARAM_NOTE + KIT_VOICES,
} kit_param_t;

typedef enum {
    OUTPUT_MIDI_ONLY,
    OUTPUT_CV_GATE,
    OUTPUT_BOTH,
} output_mode_t;

// SYNTH uses the NOTE row's space for CV/Gate. DRUM keeps its NOTE row.
typedef enum {
    TRACK_PARAM_TYPE,
    TRACK_PARAM_CHANNEL,
    TRACK_PARAM_OUTPUT,
    TRACK_PARAM_GATE,
    TRACK_PARAM_NOTE,
    TRACK_PARAM_DRUM_CV,
    TRACK_PARAM_SYNTH_CV = TRACK_PARAM_NOTE,
} track_param_t;

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
    uint8_t        length;          // Default/fallback step count: 1-16
    uint8_t        track_length[NUM_TRACKS]; // Per-track/section step count: 1-16
    uint8_t        swing;           // 0–99, percentage of swing
    char           name[9];         // 8 chars + null
    uint32_t       kit_steps[KIT_TRACK_SLOTS][STEPS_PER_PATTERN]; // Kit step words
} pattern_t;

// Song mode
typedef struct {
    uint8_t pattern_index[NUM_TRACKS]; // Pattern location per track/section
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
    PLAY_MODE_MIDI_SONG,    // Play the timed event stream from flash
} play_mode_t;

typedef enum {
    LAUNCH_QUANT_TRACK_LOOP = 0, // at the end of that track's loop (old rule)
    LAUNCH_QUANT_BAR,            // global step that is a multiple of 16
    LAUNCH_QUANT_BEAT,           // global step that is a multiple of 4
    LAUNCH_QUANT_STEP,           // next step
    LAUNCH_QUANT_COUNT
} launch_quant_t;

typedef struct {
    transport_state_t state;
    play_mode_t       mode;
    float             bpm;
    uint32_t          pulse_count;      // Current pulse within pattern
    uint8_t           current_step;     // 0–15
    uint32_t          track_step_count[NUM_TRACKS]; // Next step per track; wraps at its length
    uint8_t           current_track_step[NUM_TRACKS]; // Per-track playhead
    uint8_t           current_pattern[NUM_TRACKS]; // Pattern per track/section
    uint8_t           queued_pattern[NUM_TRACKS];  // Next per-track pattern (0xFF = none)
    uint16_t          song_position;    // Current entry in song
    uint8_t           song_repeat_count;// Repeats remaining
    bool              step_triggered;   // Flag: new step just started
} transport_t;

typedef struct {
    uint32_t samples;
    int32_t  last_irq_latency_us;       // ISR entry time - scheduled alarm time
    int32_t  min_irq_latency_us;
    int32_t  max_irq_latency_us;
    int32_t  last_period_error_us;      // Actual ISR period - target period
    int32_t  min_period_error_us;
    int32_t  max_period_error_us;
    uint32_t max_abs_period_error_us;
    uint32_t last_isr_duration_us;
    uint32_t max_isr_duration_us;
    uint32_t overruns;                  // ISR entered later than one pulse interval
    uint32_t flam_drops;                // dropped flam sends: one per kit per flam pulse whose
                                        // second hits were skipped: the TX queue would
                                        // have held more than the flam cap
} sequencer_jitter_stats_t;

// ─── Shared State (accessed by both cores) ──────────────────────

typedef struct {
    volatile transport_t transport;
    pattern_t            patterns[MAX_PATTERNS];
    song_t               song;
    uint8_t              active_pattern[NUM_TRACKS]; // Edited pattern per track/section
    kit_config_t         kit_config[NUM_TRACKS];     // Kit settings per track
    volatile uint8_t     launch_quant;       // launch_quant_t; not saved
    volatile track_mask_t     track_mutes;        // Bitmask: bit N = track N muted for playback
    // Spinlock index for mutex
    uint32_t             spinlock_num;
    bool                 spinlock_ready;
} sequencer_state_t;

// ─── Global ─────────────────────────────────────────────────────

extern sequencer_state_t g_seq;

// ─── API ────────────────────────────────────────────────────────

// shared_state.c — shared state locking
void seq_lock_init(void);
uint32_t seq_lock(void);
void seq_unlock(uint32_t irq_state);

// sequencer.c — Core 1
void sequencer_init(void); // Core 0, before launching Core 1
void sequencer_core1_entry(void);

// transport
void transport_start(void);
void transport_stop(void);
void transport_continue(void);
void transport_set_bpm(float bpm);
void transport_set_mode(play_mode_t mode);
void transport_set_launch_quant(launch_quant_t q);  // ignores q >= COUNT
launch_quant_t transport_get_launch_quant(void);
void sequencer_panic(void);
void sequencer_refresh_track_playback(uint8_t track, uint8_t pattern_index);
void sequencer_get_jitter_stats(sequencer_jitter_stats_t *out);
void sequencer_reset_jitter_stats(void);

// pattern.c
void pattern_init(pattern_t *p, uint8_t index);
void pattern_set_step(pattern_t *p, uint8_t track, uint8_t step,
                      uint8_t note, uint8_t velocity, uint8_t flags);
void pattern_clear_step(pattern_t *p, uint8_t track, uint8_t step);
void pattern_toggle_step(pattern_t *p, uint8_t track, uint8_t step);
void pattern_copy(pattern_t *dst, const pattern_t *src);
void pattern_clear(pattern_t *p);
void pattern_clear_track(pattern_t *p, uint8_t track);
// Copies one track's steps, kit steps and effective length from src to dst.
// The track configuration is not copied. No-op for an invalid track or
// dst == src.
void pattern_copy_track(pattern_t *dst, const pattern_t *src, uint8_t track);
void pattern_shift(pattern_t *p, int8_t direction);
void pattern_shift_track(pattern_t *p, uint8_t track, int8_t direction);
uint8_t pattern_get_track_length(const pattern_t *p, uint8_t track);
void pattern_set_track_length(pattern_t *p, uint8_t track, uint8_t length);
uint8_t pattern_bank(uint8_t pattern_index);
uint8_t pattern_slot(uint8_t pattern_index);
uint8_t pattern_make_index(uint8_t bank, uint8_t slot);

// Pattern access helpers
pattern_t *seq_active_pattern(uint8_t track);
pattern_t *seq_playing_pattern(uint8_t track);

// Song editing
bool song_append(uint8_t pattern_index, uint8_t repeats);
bool song_append_patterns(const uint8_t pattern_index[NUM_TRACKS], uint8_t repeats);
bool song_insert(uint16_t position, uint8_t pattern_index, uint8_t repeats);
bool song_insert_patterns(uint16_t position, const uint8_t pattern_index[NUM_TRACKS],
                          uint8_t repeats);
bool song_remove(uint16_t position);
bool song_move(uint16_t from, uint16_t to);
void song_clear(void);
void song_set_end_mode(song_end_mode_t mode);

// Core 0: queue for the configured launch quantization (default 1 BAR; see
// launch_quant_t), or select immediately while stopped. Returns false for
// invalid locations or Song mode.
bool transport_queue_pattern(uint8_t pattern_index);
bool transport_queue_track_pattern(uint8_t track, uint8_t pattern_index);
void transport_clear_queue(void);
void transport_clear_track_queue(uint8_t track);
// A pad (one track) or scene (all tracks) press, decided and written in one
// critical section, Pattern mode only. If any track in track_mask is queued to
// the location, their queues are cancelled. Otherwise each one already on it
// loses any queue and keeps playing, and each other one is queued to it
// (selected at once while stopped). False for an invalid location or Song mode.
bool transport_launch(track_mask_t track_mask, uint8_t pattern_index);

typedef struct {
    transport_state_t state;
    play_mode_t       mode;
    uint32_t          pulse_count;
    uint8_t           current[NUM_TRACKS];
    uint8_t           queued[NUM_TRACKS];
    uint8_t           track_step[NUM_TRACKS];
    track_mask_t           live_mutes;    // g_seq.track_mutes
    track_mask_t           stored_mutes;  // bit c: patterns[current[c]].tracks[c].muted
} transport_snapshot_t;

// Consistent copy of the transport; takes the locks itself.
void sequencer_snapshot(transport_snapshot_t *out);

// Bit (track * 8 + slot): location bank * 8 + slot has an active step within
// the track's length (stored mutes ignored). Takes seq_lock: call with none held.
uint64_t sequencer_page_clips(uint8_t bank, uint8_t page);
uint64_t sequencer_clip_mask(uint8_t bank);

// Track mutes (bitmask, instant)
void seq_set_track_mute(uint8_t track, bool muted);
// Copies a track's routing into all 32 locations, each keeping its own stored
// `muted`, then refreshes that track's prepared rows. Returns false, nothing
// changed, when `config` makes a third track a kit: its type is KIT, the track is
// a kit in no location and MAX_KIT_TRACKS other tracks are. Takes seq_lock: call
// with none held.
bool seq_set_track_config(uint8_t track, const track_config_t *config);
// Stores the track's kit settings, clamped to their ranges, then refreshes that
// track's prepared rows. Takes seq_lock: call with none held.
void seq_set_kit_config(uint8_t track, const kit_config_t *config);
// Bit t set when track t is a kit in any location. Takes seq_lock: call with none
// held.
track_mask_t seq_kit_tracks(void);
// The MUTE button. A track that is muted (live, or stored at the location it
// plays) becomes audible: the live bit and the stored mute of every location
// are cleared and its prepared rows refreshed. An audible track gets the live
// mute. Takes seq_lock: call with none held.
void seq_toggle_track_mute(uint8_t track);
// The muted tracks, by the rule MUTE uses: bit c set when the live mute is set
// or the location track c plays has its stored mute. Takes seq_lock: call with
// none held.
track_mask_t seq_get_track_mutes(void);

// midi_out.c
void midi_init(void);
// Whole channel messages, each with its status byte (the drain applies running
// status). Sets the overflow flag when they do not fit in the TX queue.
void midi_send_bytes(const uint8_t *data, uint16_t length);
// Queues them only if the TX queue then holds at most `max_queued` bytes; otherwise
// returns false and leaves the overflow flag alone. One call under the TX lock.
bool midi_try_send_bytes_capped(const uint8_t *data, uint16_t length, uint16_t max_queued);
void midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_send_note_off(uint8_t channel, uint8_t note);
void midi_send_cc(uint8_t channel, uint8_t cc, uint8_t value);
// Clock sets a pending flag (no queue byte); Start, Continue and Stop wait in a
// small queue of their own. Both go out at the next message boundary, ahead of
// queued note messages; Stop also cancels a pending Clock.
void midi_send_clock(void);
void midi_send_start(void);
void midi_send_stop(void);
void midi_send_continue(void);
void midi_send_all_notes_off(void);
void midi_clear_output_queue(void);
bool midi_output_overflowed(void);
void midi_wait_output_idle(void); // Core 0 only, transport stopped
// Buffered output — collect messages, then flush all at once
void midi_buf_clear(void);
void midi_buf_flush(void);
void midi_buf_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_buf_note_off(uint8_t channel, uint8_t note);
// The DIN input. The UART interrupt (core 0) pushes every byte it receives into a ring;
// the main loop on core 0 takes them out. No lock. Under MOCK_HARDWARE there is no input.
#define MIDI_IN_RING_SIZE 64    // a power of two
bool midi_in_read(uint8_t *byte);       // false when the ring is empty
uint32_t midi_in_dropped(void);         // bytes that found the ring full, counted since boot
void midi_in_flush(void);               // empties the ring, leaves the dropped count

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
// common_row: session_common_row(&snap), 0xFF = MIXED. bar_step: global step in the
// bar (0-15); 16 or more lights none (stopped). mode: shown as P or S at the end
// of line 0. On a kit focus kit_lane names the lane ("SD"), kit_strip comes from
// kit_lane_glyphs and kit_playhead is the cell's step (16 = none); kit_lane NULL
// on any other focus.
void display_draw_session_view(uint8_t bank, uint8_t common_row, play_mode_t mode,
                               uint8_t focus_track, uint8_t focus_location, uint8_t focus_length,
                               float bpm, launch_quant_t quant, uint8_t bar_step,
                               const char *kit_lane, const char *kit_strip, uint8_t kit_playhead);
// Session screen on a kit focus: the pixel rows of the lane strip and its playhead.
#define DISPLAY_KIT_STRIP_Y 37
#define DISPLAY_KIT_STRIP_H 11
const char *display_launch_quant_name(launch_quant_t quant);
// held_step: the step held for editing (its values replace the track info
// line, the step is marked in the grid); -1 = none.
void display_draw_pattern_view(const pattern_t *p, uint8_t track, uint8_t step, int8_t held_step);
// Pattern view on a kit cell: lane names the lane, strip from kit_lane_glyphs,
// playhead the cell's step (16 = none). The BPM is drawn by the caller.
void display_draw_kit_lane_view(uint8_t track, uint8_t location, const char *lane,
                                const char *strip, uint8_t playhead);
void display_draw_step_edit(const pattern_t *p, uint8_t track, uint8_t step);
// One kit step word: the 12 lane values, the cursor on cursor_lane.
void display_draw_kit_step_edit(uint8_t track, uint8_t step, uint32_t word, uint8_t cursor_lane);
void display_draw_pattern_select(uint8_t pattern,
                                  uint8_t length,
                                  uint8_t queued_pat,
                                  uint8_t mutes);
void display_draw_track_config(const track_config_t *tc, uint8_t track,
                                uint8_t param_cursor);
// A kit track's config: param is a kit_param_t row; FLAM shows its time at the
// current tempo.
void display_draw_kit_config(const track_config_t *tc, const kit_config_t *kc,
                             uint8_t track, uint8_t param);
void display_draw_song_view(const song_t *s, uint16_t pos, uint8_t track);
void display_draw_bpm(float bpm);
void display_draw_text(uint8_t x, uint8_t y, const char *text);

// ui.c — Core 0
void ui_init(void);
void ui_update(void);   // Poll inputs, run state machine
void ui_render(void);   // Dispatch display rendering based on UI mode

#endif // SEQUENCER_H
