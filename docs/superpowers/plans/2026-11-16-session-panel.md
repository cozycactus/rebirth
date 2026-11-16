# Session-Style Live Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the RP2040 sequencer playable by hand like a Session view: an 8x8 RGB clip grid with scene and mute buttons, the 16 step buttons editing the focused clip, a PLAY button, quantized launches.

**Architecture:** The sequencer core (Core 1 ISR) gains only a per-step launch-quantization check, a scene-queue API, a clip-presence mask and a locked transport snapshot for Core 0. Everything new on the panel runs on Core 0: a TCA8418 driver and a debounced panel-input layer, a session model over the transport API, a pure LED-frame builder, and a PIO/DMA driver for the SK6812 chain. `ui.c` gets a new home view (`UI_SESSION`) and a global handler for the grid, scenes, mutes and PLAY that runs before the per-screen dispatch.

**Tech Stack:** C11, Raspberry Pi Pico SDK 2.2.0 (RP2040: PIO, DMA, I2C), CMake; host tests are plain C executables under `test/` run by CTest, with fakes in `test/fakes`.

**Spec:** `docs/superpowers/specs/2026-11-16-session-panel-design.md`

## Global Constraints

- Core 1's timer ISR gets no new work beyond the launch-quantization check; no new locks in it; lock order stays playback -> shared state; spinlocks are not reentrant, so no function that takes a lock is called while that lock is held.
- Core 1 never touches the LEDs or I2C.
- Save format unchanged (version 5); launch quantization and the shown bank are not saved.
- GPIO 27 = LED data (PIO), GPIO 28 = PLAY (pull-up, active low); I2C0 on GPIO 8/9 at 400 kHz with external pull-ups (internal ones off); TCA8418 at 0x34 next to MCP23017 0x20/0x21.
- 82 LEDs: 64 pads, 8 SCENE, 8 MUTE, PLAY, BEAT. The whole device stays within 500 mA at 5 V; RGB budget 200 mA.
- Press-to-event within 15 ms; panel keys debounced in firmware for 8 ms; PLAY for 20 ms.
- Every I2C transaction (init included) uses the timeout API (2000 us) and checks the exact byte count; a failure never blocks the UI.
- Default launch quantization `1 BAR`; `TRACK LOOP` (today's rule) stays as an option.
- Track routing (`track_config_t`) belongs to the track: the same config in all 32 locations.
- Host tests pass under CTest and under the CI sanitizer build (clang, `-Wall -Wextra -Werror -fsanitize=address,undefined`); unused static functions left behind by a task are deleted in that task. The firmware builds without warnings with `-DREBIRTH_CV_DAC_GAIN=1` and `=2`.

## Review Focus

- Shortening the playing clip under `1 BAR` must not apply a pending queue early — test in Task 1.
- A pad pressed right after a bar line waits a whole bar, never lands mid-step — test in Task 1.
- A track whose queue points into a bank that is not shown must show that something is pending (MUTE blinks on 8ths) — test in Task 5.
- Keys and PLAY held through a flash save/load or a panic must not fire afterwards — tests in Tasks 6, 8 and 10.
- A step button pressed in the Session view and released after the screen changed must not toggle anything — test in Task 8.

## Working Method (every task)

Write the listed tests first; build the target and see it fail to compile/link or fail its checks; implement; run the task's checks; commit only the task's files with `git add <files> && git commit -m "<message>"`. Names of helpers, internal structure and code layout are the implementer's.

## Commands

Run from the worktree root.

- Host tests: `cmake -S test -B build/host && cmake --build build/host --parallel && ctest --test-dir build/host --output-on-failure` — expected `100% tests passed`. A new executable is checked first with `cmake --build build/host --target <name>`; run it directly (`build/host/<name>`): each test prints `PASS <name>`, a failure prints `file:line: condition` and exits 1.
- Sanitizers (as CI): `cmake -S test -B build/san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang '-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' && cmake --build build/san --parallel && ctest --test-dir build/san --output-on-failure`.
- Firmware, for G in 1 and 2: `PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0 cmake -S . -B build/fw$G -G Ninja -DCMAKE_BUILD_TYPE=Release -DREBIRTH_CV_DAC_GAIN=$G && cmake --build build/fw$G > build/fw$G/build.log 2>&1` — both commands exit 0, `grep -Eic 'warning|error' build/fw$G/build.log` prints `0`, and `build/fw$G/rp2040_sequencer.uf2` exists.

---

### Task 1: Launch quantization in the sequencer

**Files:**
- Modify: `include/sequencer.h`, `src/sequencer.c:218-230` (`apply_track_queue_if_ready`) and `:523-544` (ISR step loop), `src/main.c` (`init_defaults`), `ARCHITECTURE.md` ("Playback Rules")
- Test: `test/test_playback.c`; `reset()` in `test/test_playback.c`, `test/test_song.c` and `test/test_ui.c` calls `transport_set_launch_quant(LAUNCH_QUANT_TRACK_LOOP)` explicitly

**Interfaces:**
- Produces:
  ```c
  typedef enum {
      LAUNCH_QUANT_TRACK_LOOP = 0, // at the end of that track's loop (old rule)
      LAUNCH_QUANT_BAR,            // global step that is a multiple of 16
      LAUNCH_QUANT_BEAT,           // global step that is a multiple of 4
      LAUNCH_QUANT_STEP,           // next step
      LAUNCH_QUANT_COUNT
  } launch_quant_t;
  // in sequencer_state_t:  volatile uint8_t launch_quant;
  void transport_set_launch_quant(launch_quant_t q);  // ignores q >= COUNT; written under the playback lock
  launch_quant_t transport_get_launch_quant(void);
  ```

ISR rule, on every step boundary in Pattern mode, per track, before the step's event is read (Song mode keeps its own bar logic; `pulse_count` is never reset by launches):

```text
q = g_seq.launch_quant; len = clamp(length of the prepared row)
due = queued[tr] != NONE && (q == TRACK_LOOP ? track_step_count[tr] >= len
                                             : global_step % N(q) == 0)      // N: BAR 16, BEAT 4, STEP 1
if due:
    if queued[tr] == current[tr]: clear queued[tr]                            // phase kept
    else: apply (existing apply_track_queue_if_ready), reload prepared, len = new length,
          track_step_count[tr] = (q == TRACK_LOOP) ? 0 : (global_step % 16) % len
if track_step_count[tr] >= len: track_step_count[tr] = 0                     // plain wrap, never applies
```

- [ ] **Tests** (`test/test_playback.c`). Helper:

```c
static void run_through_step(uint32_t step) {   // returns right after the ISR processed `step`
    while (g_seq.transport.pulse_count <= step * PULSES_PER_STEP) tick();
}
```

Each test puts a note on step 0 of locations 0 and 1 for track 0 (`set_note`), sets the mode, then `transport_start()`.

```text
test_bar_launch_waits_for_bar_line:      BAR; run_through_step(5); queue track 0 -> 1;
    run_through_step(15) -> current_pattern[0]==0, queued_pattern[0]==1;
    run_through_step(16) -> current==1, queued==NO_QUEUED_PATTERN, current_track_step[0]==0
test_bar_launch_on_short_loop_waits_for_bar: set_length(0,0,12), set_length(1,0,12); BAR;
    run_through_step(3); queue 1; run_through_step(12) -> current==0;
    run_through_step(16) -> current==1 && current_track_step[0]==0
test_beat_launch_joins_in_phase:         BEAT; set_note(1,0,8,64,0); run_through_step(5); queue 1;
    run_through_step(7) -> current==0; run_through_step(8) -> current==1 && current_track_step[0]==8
    and a note-on for 64 was sent at that step
test_launch_phase_matrix:                old length 16, queue at step 5, new location 1 with length L;
    BAR  -> due 16: L=5,12,16 -> current_track_step 0,0,0
    BEAT -> due 8:  L=5,12,16 -> 3,8,8
    STEP -> due 6:  L=5,12,16 -> 1,6,6
    TRACK_LOOP -> due 16: L=5,12,16 -> 0,0,0;  one step before "due" current is still 0 in every case
test_requeue_of_playing_location_keeps_phase: set_length(0,0,12); BAR; run_through_step(13);
    queue track 0 -> 0; run_through_step(16) -> queued==NO_QUEUED_PATTERN && current_track_step[0]==4
test_quant_change_retargets_pending_queue: BAR; run_through_step(2); queue 1;
    transport_set_launch_quant(LAUNCH_QUANT_STEP); run_through_step(3) -> current==1
test_shortening_under_bar_does_not_launch_early: BAR; run_through_step(9); queue 1;
    set_length(0,0,8); run_through_step(12) -> current==0; run_through_step(16) -> current==1
test_queue_right_after_bar_waits_a_whole_bar: BAR; run_through_step(16); queue 1;
    run_through_step(31) -> current==0; run_through_step(32) -> current==1
```

- [ ] **Implement** the enum, field, API and ISR rule; `init_defaults` sets `LAUNCH_QUANT_BAR`; ARCHITECTURE describes the four modes, the phase rule and that `TRACK LOOP` starts at step 1.
- [ ] **Check:** host tests (all existing tests unchanged and passing), sanitizers, firmware.
- [ ] **Commit:** "Quantize pattern launches to bar, beat or step".

### Task 2: Clip mask, scene queue and transport snapshot

**Files:**
- Modify: `include/sequencer.h`, `src/sequencer.c`
- Test: `test/test_playback.c`

**Interfaces:**
- Consumes: Task 1.
- Produces:
  ```c
  uint64_t sequencer_clip_mask(uint8_t bank);  // bit (track * 8 + slot): location bank*8+slot has an ACTIVE step
                                               // within that track's length (tracks[].muted ignored);
                                               // takes seq_lock itself: call with no lock held
  bool transport_queue_scene(uint8_t pattern_index); // Pattern mode only; playback lock, then seq_lock, plain writes:
                                               // playing -> tracks not on the location are queued to it, tracks on it
                                               // lose any queue; stopped -> every track selects it at once
  typedef struct {
      transport_state_t state; play_mode_t mode; uint32_t pulse_count;
      uint8_t current[NUM_TRACKS], queued[NUM_TRACKS], track_step[NUM_TRACKS];
      uint8_t live_mutes;    // g_seq.track_mutes
      uint8_t stored_mutes;  // bit c: patterns[current[c]].tracks[c].muted
  } transport_snapshot_t;
  void sequencer_snapshot(transport_snapshot_t *out); // playback lock, then seq_lock; plain copies only, calls nothing that locks
  ```
  The mask lives in `static uint64_t clip_mask[PATTERN_BANKS]`, recomputed in `sequencer_refresh_track_playback` from the copied steps and length (before the muted masking of `fill_prepared_track`) and stored under `seq_lock`; `flash_storage_load` already refreshes every location.

- [ ] **Tests:**

```text
test_clip_mask_tracks_edits: sequencer_clip_mask(1)==0; set_note(9,2,3,60,0) -> bit (2*8+1) set;
    set_length(9,2,3) -> bit clear
test_clip_mask_ignores_stored_mute: patterns[9].tracks[2].muted = true; set_note(9,2,0,60,0) -> bit (2*8+1) set
test_scene_queue_skips_tracks_already_there: transport_start(); queue track 1 -> 9; run_through_step(16);
    current_pattern[1]==9; queue track 1 -> 4; queue track 3 -> 2; CHECK(transport_queue_scene(9));
    queued[1]==NO_QUEUED_PATTERN, queued[3]==9, queued[0]==9
test_empty_scene_silences_all_tracks: notes on every step of tracks 0..3 at location 0; BAR; start;
    run_through_step(3); transport_queue_scene(5) (empty); run_through_step(16) -> every current==5;
    the note-on count does not grow between run_through_step(16) and run_through_step(24)
test_scene_selects_at_once_when_stopped: CHECK(transport_queue_scene(4)); every current==4, no queues
test_scene_rejected_in_song_mode: transport_set_mode(PLAY_MODE_SONG); CHECK(!transport_queue_scene(4))
test_snapshot_reports_queues_and_mutes: start; queue track 2 -> 7; seq_set_track_mute(1, true);
    patterns[0].tracks[3].muted = true; snapshot -> queued[2]==7, live_mutes==0x02, stored_mutes & 0x08, state PLAYING
```

- [ ] **Implement.**
- [ ] **Check:** host tests, sanitizers, firmware.
- [ ] **Commit:** "Track clip presence, queue scenes atomically, snapshot the transport".

### Task 3: Routing per track

**Files:**
- Modify: `include/sequencer.h`, `src/song.c`, `src/ui.c:621-708` (`handle_track_config`), `src/main.c` (`init_defaults`), `ARCHITECTURE.md`
- Test: `test/test_song.c`, `test/test_playback.c`, `test/test_ui.c`

**Interfaces:**
- Produces: `void seq_set_track_config(uint8_t track, const track_config_t *config);` — copies `*config` into `patterns[0..31].tracks[track]` under `seq_lock`, releases it, then refreshes that track's prepared rows for all 32 locations. Call with no lock held.

`handle_track_config` copies the active location's config into a local under `seq_lock`, releases the lock, edits the local, and calls `seq_set_track_config` when it changed. `init_defaults` copies each track's config from location 0 to the other 31 after the demo setup (before `sequencer_init`, which refreshes everything; firmware-only, no host test). ARCHITECTURE: routing belongs to the track; older saves whose locations differ play as before until that track's config is edited.

- [ ] **Tests:**

```text
test_track_config_applies_to_every_location (test_song.c): cfg = patterns[0].tracks[2]; cfg.midi_channel = 7;
    seq_set_track_config(2, &cfg) -> all 32 patterns[p].tracks[2].midi_channel == 7; patterns[5].tracks[1] unchanged
test_launch_keeps_track_routing (test_playback.c): cfg.midi_channel = 5, OUTPUT_MIDI_ONLY for track 0;
    seq_set_track_config(0, &cfg); set_note(3,0,0,60,0); start; queue track 0 -> 3; run_through_step(16)
    -> a 0x95 note-on for 60 is in fake_midi
test_track_config_edit_reaches_all_locations (test_ui.c): press_encoder(1, BTN_LONG_PRESS) (pattern select),
    long press step button 1 (track config of track 0), e1d = +1, e2d = +1
    -> every patterns[p].tracks[0].midi_channel == old + 1
```

- [ ] **Implement.**
- [ ] **Check:** host tests, sanitizers, firmware.
- [ ] **Commit:** "Keep track routing the same in every location".

### Task 4: Session model

**Files:**
- Create: `include/session.h`, `src/session.c`, `test/test_session.c`
- Modify: `test/CMakeLists.txt` (`test_session` from `test_session.c`, `fakes/fake_hardware.c` and the sources `session.c sequencer.c pattern.c song.c shared_state.c midi_out.c`; `add_test(NAME session_tests ...)`), `CMakeLists.txt` (add `src/session.c`)

**Interfaces:**
- Consumes: `sequencer_snapshot`, `transport_queue_track_pattern`, `transport_clear_track_queue`, `transport_clear_queue`, `transport_queue_scene`, `seq_toggle_track_mute`.
- Produces:
  ```c
  typedef struct { uint8_t track; uint8_t location; } session_focus_t;
  void session_init(void);                     // bank 0; focus = (0, current location of track 0)
  uint8_t session_bank(void);
  void session_next_bank(void);                // 0->1->2->3->0; focus unchanged
  session_focus_t session_get_focus(void);
  void session_move_focus(int8_t delta);       // track clamped to 0..7; location = queued if pending, else current
  // move_focus: also move the focus to (track, bank*8+row), whether or not the launch is accepted.
  // Returns false when the launch is rejected (Song mode).
  bool session_pad(uint8_t track, uint8_t row, bool move_focus);
  bool session_scene(uint8_t row);             // false = rejected (Song mode)
  void session_mute(uint8_t track);
  uint8_t session_common_row(void);            // slot 0..7 of the shown bank when every track's current
                                               // location is bank*8+slot, else 0xFF
  ```
  Pad rule (L = bank*8+row): queued == L -> clear that track's queue; current == L -> clear any queue (no restart); else queue L. Scene rule: any track queued to L -> `transport_clear_queue()`; else `transport_queue_scene(L)`. Session reads the transport only through `sequencer_snapshot` and calls the transport API with no lock held.

- [ ] **Tests** (`reset()` uses only public APIs — `sequencer.c` is a separate source here — and calls `session_init()`):

```text
test_pad_queues_and_focuses: start; session_pad(2,3,true) -> queued[2]==3, focus (2,3)
test_pad_on_queued_location_cancels: start; session_pad(2,3,true) twice -> queued[2]==NO_QUEUED_PATTERN
test_pad_on_playing_location_without_queue_is_noop: start; session_pad(2,0,true) -> no queue, current[2]==0
test_pad_on_playing_location_cancels_other_queue: start; session_pad(2,3,true); session_pad(2,0,true)
    -> no queue, current[2]==0
test_pad_while_stopped_selects_immediately: session_pad(4,6,true) -> current[4]==6
test_empty_pad_launches_silence: start; session_pad(1,7,true) (empty location) -> queued[1]==7
test_scene_launches_row_of_shown_bank: session_next_bank(); start; session_scene(3) -> every queued==11
test_empty_row_scene_queues_every_track: start; session_scene(5) (no clips) -> every queued==5
test_scene_press_while_pending_cancels_all: start; session_scene(3); session_pad(0,5,false); session_scene(3) -> no queues
test_stop_ends_pending_scene: start; session_scene(3); transport_stop(); transport_start(); session_scene(3) -> every queued==3
test_song_mode_rejects_launch_but_moves_focus: transport_set_mode(PLAY_MODE_SONG);
    CHECK(!session_pad(5,2,true)); focus (5,2); CHECK(!session_scene(2))
test_move_focus_follows_queue_then_current: start; session_pad(3,6,false); session_move_focus(+3) -> (3,6);
    session_move_focus(+1) -> (4, current[4])
test_bank_change_keeps_focus: session_pad(1,2,true); session_next_bank() -> focus (1,2), session_bank()==1
test_common_row: all tracks select location 2 while stopped -> 2; one track elsewhere -> 0xFF;
    session_next_bank() with all on 2 -> 0xFF
```

- [ ] **Implement.**
- [ ] **Check:** host tests, sanitizers, firmware.
- [ ] **Commit:** "Add the session model: pads, scenes, mutes, focus and banks".

### Task 5: LED frame builder

**Files:**
- Create: `include/led_frame.h`, `src/led_frame.c`, `test/test_led_frame.c`
- Modify: `test/CMakeLists.txt` (`test_led_frame` from `test_led_frame.c` + `led_frame.c`; `add_test(NAME led_frame_tests ...)`), `CMakeLists.txt` (add `src/led_frame.c`)

**Interfaces:**
- Produces:
  ```c
  #define LED_COUNT 82
  #define LED_PAD(track, row) ((row) * 8 + (track))
  #define LED_SCENE(row)      (64 + (row))
  #define LED_MUTE(track)     (72 + (track))
  #define LED_PLAY            80
  #define LED_BEAT            81
  #define LED_LEVEL_DIM   3      // starting values for the budget test; retuned on the bench
  #define LED_LEVEL_HALF  12
  #define LED_LEVEL_FULL  32
  #define LED_RGB_BUDGET_MA 200
  #define LED_CHANNEL_MA_FULL 20   // conservative until the part is measured
  #define LED_IDLE_UA 1000         // per LED
  typedef struct { uint8_t r, g, b; } rgb_t;
  typedef struct {
      uint8_t  bank;                    // shown bank
      uint8_t  current[NUM_TRACKS];     // locations 0..31
      uint8_t  queued[NUM_TRACKS];      // NO_QUEUED_PATTERN when none
      uint64_t clip_mask;               // sequencer_clip_mask(bank)
      uint8_t  muted;                   // bit per track: live | stored mute
      uint8_t  focus_track;
      bool     playing;
      uint32_t pulse;                   // pulse_count while playing; a 120 BPM pulse clock when stopped
  } led_state_t;
  void led_frame_build(const led_state_t *s, rgb_t out[LED_COUNT]);
  uint32_t led_frame_estimate_ma(const rgb_t frame[LED_COUNT]);   // idle + dynamic, rounded up
  void led_frame_apply_budget(rgb_t frame[LED_COUNT], uint32_t budget_ma);
  ```
  Hues (channel = hue * level / 255, integer): red {255,0,0}, orange {255,80,0}, yellow {255,200,0}, green {0,255,40}, cyan {0,220,255}, blue {0,60,255}, violet {140,0,255}, pink {255,0,120}; white {255,255,255}; scene green {0,255,0}. 8th-note blink is on when `(pulse / (PPQN / 2)) % 2 == 0`; slow blink on when `(pulse / (PPQN * 2)) % 2 == 0`.
  Rules exactly as the spec's light tables (pad priority list, SCENE list where "plays" needs the transport playing, MUTE with its two blinks, PLAY, BEAT). BEAT while playing: `pulse % (PPQN*4) < PPQN/8` -> white FULL; else `pulse % PPQN < PPQN/8` -> white HALF; else off.
  Budget: dynamic current from the channel values; if `dynamic == 0` or the frame fits, leave it unchanged; else multiply every channel by `(budget - idle_total) / dynamic`, truncating.

- [ ] **Tests** (`base_state()`: bank 0, all current 0, no queues, no clips, nothing muted, focus 0, stopped, pulse 0):

```text
test_empty_pad_is_off                     test_stored_clip_is_dim_hue
test_playing_clip_is_full_hue             test_stopped_current_clip_is_half_hue
test_queued_clip_blinks_on_eighths        // pulse 0 -> full hue, pulse 48 -> off
test_queued_beats_stored                  // queued cell with a clip blinks full hue, never shows dim
test_queued_empty_blinks_dim_white        test_current_empty_is_dim_white_while_playing
test_scene_rules                          // pending blink; all 8 play row = full green; some = dim green;
                                          // stopped with all on the row = dim white (clip) / off (empty);
                                          // has clip = dim white; empty = off; two rows pending both blink
test_mute_rules                           // audible half hue; muted off; focus audible full hue;
                                          // focus muted dim white; plays other bank: pulse 0 on / 192 off;
                                          // focus moved to each track 0..7
test_queue_in_other_bank_blinks_mute_fast // pulse 0 on, pulse 48 off; overrides the slow blink
test_play_and_beat                        // PLAY green full while playing, off stopped; BEAT full on bar,
                                          // half on beat 2, off between beats
test_busiest_designed_frame_fits_budget   // 64 cells with clips, 8 tracks play row 0, focus 0, beat on bar:
                                          // estimate <= LED_RGB_BUDGET_MA with no scaling
test_budget_scales_only_dynamic_current   // all channels 255 -> estimate <= 200 and > idle; an under-budget
                                          // frame and an all-black frame are unchanged
```

- [ ] **Implement** (pure: no locks, no hardware).
- [ ] **Check:** host tests, sanitizers, firmware.
- [ ] **Commit:** "Add the pure LED frame builder with the light rules and current budget".

### Task 6: TCA8418 driver, PLAY button and panel input

**Files:**
- Create: `include/tca8418.h`, `src/tca8418.c`, `include/panel_input.h`, `src/panel_input.c`, `test/fakes/hardware/i2c.h` (SDK-shaped declarations: `i2c_inst_t`, `i2c0`, `i2c_write_timeout_us`, `i2c_read_timeout_us`, `PICO_ERROR_TIMEOUT`), `test/test_panel_input.c` (defines `sequencer_state_t g_seq` for `fake_hardware.c` and holds the fake TCA8418: register file, 10-event FIFO, overflow flag, injectable I2C error)
- Modify: `test/fakes/pico/stdlib.h` + `test/fakes/fake_hardware.c` (`gpio_get`, `gpio_pull_up`, `gpio_disable_pulls`, `bool fake_gpio_level[32]` defaulting high), `test/CMakeLists.txt` (`test_panel_input` from it + `tca8418.c` + `panel_input.c` + `fakes/fake_hardware.c`), `CMakeLists.txt` (both sources)

**Interfaces:**
- Produces:
  ```c
  // tca8418.h
  #define TCA8418_ADDR 0x34
  bool tca8418_init(void);   // rows R0-R7 + cols C0-C9 as keypad, chip debounce off, CFG = 0xA9
  int  tca8418_read_events(uint8_t *events, int max, bool *overflow); // >= 0 events read, -1 on I2C error
  bool tca8418_flush(void);  // true only after the FIFO was read empty and INT_STAT cleared
  // panel_input.h
  typedef enum { PANEL_PAD, PANEL_SCENE, PANEL_MUTE, PANEL_PLAY } panel_kind_t;
  typedef struct { panel_kind_t kind; uint8_t track; uint8_t row; bool down; } panel_event_t;
  #define PANEL_DEBOUNCE_MS 8
  #define PLAY_DEBOUNCE_MS 20
  #define PLAY_BUTTON_PIN 28
  void panel_input_init(void);              // after mcp23017_init (I2C0 already up)
  void panel_input_poll(uint32_t now_ms);   // every ui_update pass
  bool panel_input_next(panel_event_t *out);
  void panel_input_resync(void);            // see below
  ```
  Register map (TI datasheet; addresses and bit positions as in the Linux `tca8418_keypad` driver): CFG 0x01 (AI 0x80, OVR_FLOW_M 0x20, OVR_FLOW_IEN 0x08, KE_IEN 0x01 -> 0xA9), INT_STAT 0x02 (OVR_FLOW_INT 0x08, K_INT 0x01; write 1 to clear), KEY_LCK_EC 0x03 (count in bits 3:0), KEY_EVENT_A 0x04 (bit 7 press, bits 6:0 key 1..80), KP_GPIO1..3 0x1D..0x1F = 0xFF,0xFF,0x03, DEBOUNCE_DIS1..3 0x29..0x2B = 0xFF,0xFF,0x03. Key `k`: row = (k-1)/10, col = (k-1)%10. A `static const` table maps (row, col) to controls: col 0-7 -> PAD(track = col, row), col 8 -> SCENE(row), col 9 -> MUTE(track = row); a board wired differently edits only this table.
  Debounce: each key keeps a raw state (from events) and an accepted state; an event is emitted once raw differs from accepted and has been stable for `PANEL_DEBOUNCE_MS` (shorter taps are ignored); PLAY the same from `gpio_get` with `PLAY_DEBOUNCE_MS`. A ring of 32 events; when full, new ones are dropped. Overflow -> flush, all keys accepted released without events, ring cleared.
  `panel_input_resync()`: clear the ring and debounce state, treat every key as released, and keep PLAY (and any key) that is physically down suppressed until it is seen released, so a held control never becomes a new press. If `tca8418_flush` fails, stay in recovery: retry the flush once per poll and emit nothing until it succeeds.

- [ ] **Tests:**

```text
test_init_configures_keypad: CHECK(tca8418_init()); CFG==0xA9; KP_GPIO1..3 == FF,FF,03; DEBOUNCE_DIS1..3 == FF,FF,03
test_key_codes_map_to_controls: keys (r2,c5), (r4,c8), (r6,c9) pressed; polls to t=8
    -> PAD track 5 row 2 down; SCENE row 4 down; MUTE track 6 down
test_bounce_yields_one_press: press, release, press at t=0,1,2 (poll each ms); nothing before t=10; exactly one PAD down at t=10
test_release_is_reported: press t=0, release t=20 -> down at t>=8, up at t>=28
test_ten_key_chord_all_reported: 10 presses in one burst -> 10 down events
test_overflow_resyncs_without_stuck_keys: full FIFO + overflow flag -> no events, INT_STAT cleared; a new press is reported
test_i2c_error_does_not_block: error on the next read -> poll returns, no events; the next poll reads normally
test_play_button_debounce: fake_gpio_level[28]=false at t=0 -> PLAY down at t>=20 (not before); true at t=50 -> up at t>=70
test_resync_drops_pending_events: press; poll to t=8; panel_input_resync(); CHECK(!panel_input_next(&e))
test_held_play_survives_resync_silently: PLAY held and accepted; resync; polls to t=200 -> no PLAY down;
    release then press -> exactly one PLAY down
test_held_pad_survives_resync_silently: pad held; resync; its release event arrives -> no event; a new press -> one down
test_failed_flush_stays_in_recovery: I2C error during resync; old FIFO events still in the fake; polls emit nothing
    until a flush succeeds; those old events never appear
```

- [ ] **Implement.**
- [ ] **Check:** host tests, sanitizers, firmware.
- [ ] **Commit:** "Add the TCA8418 driver and debounced panel input with PLAY".

### Task 7: SK6812 chain driver (PIO + DMA)

**Files:**
- Create: `include/rgb_leds.h`, `src/rgb_leds.c`, a WS2812-style PIO program in `src/` (for example pico-examples' `ws2812.pio`, BSD-3-Clause, header kept)
- Modify: `CMakeLists.txt` (`src/rgb_leds.c`, `pico_generate_pio_header` for the program, link `hardware_pio hardware_dma`)

**Interfaces:**
- Consumes: `rgb_t`, `LED_COUNT` (Task 5).
- Produces:
  ```c
  #define RGB_LEDS_PIN 27
  void rgb_leds_init(void);                          // 800 kHz, GRB 24-bit; claims a DMA channel; sends an all-off frame
  bool rgb_leds_show(const rgb_t frame[LED_COUNT]);  // false = previous frame not finished, this one skipped
  ```
  A `static const uint8_t chain_order[LED_COUNT]` maps logical index to chain position (identity until the board exists). The previous frame counts as finished only when its DMA is done, the PIO TX FIFO is empty, and at least 3 ms have passed since it was accepted (82 x 30 us on the wire plus the reset gap). Frames are packed into a static SRAM buffer; the function never waits and never disables interrupts.

- [ ] **Implement** (hardware-only; frame content is covered by Task 5).
- [ ] **Check:** firmware at both gains.
- [ ] **Commit:** "Drive the SK6812 chain from PIO with DMA".

### Task 8: Session view controls

**Files:**
- Modify: `src/ui.c`, `test/test_ui.c`, `test/CMakeLists.txt` (`test_ui` also links `session.c` and `led_frame.c`), `README.md` ("Live Controls" and the feature list)

**Interfaces:**
- Consumes: Tasks 1-6.
- Produces: `UI_SESSION` replaces `UI_PERFORMANCE` (home); `ui_state_t` gains `ui_mode_t step_edit_return` and `uint16_t step_short_pending`; `performance_bank` is removed (the bank is `session_bank()`). `ui_init` calls `session_init()` and, after `mcp23017_init()`, `panel_input_init()`.

Behavior (spec "Session view"):
- `ui_update` calls `panel_input_poll(now_ms)` first. When the panic combo fires it also calls `panel_input_resync()` and clears `step_short_pending`. Otherwise, before the per-screen switch and with no lock held, it drains `panel_input_next` (down events only): PAD -> `session_pad(track, row, ui.mode == UI_SESSION)`; SCENE -> `session_scene(row)`; MUTE -> `session_mute(track)`; PLAY -> `toggle_transport()`; a rejected pad or scene shows `SONG MODE`.
- `UI_SESSION`: TRACK rotate `session_move_focus`; TRACK press opens the pattern view on the focus (`selected_track = focus.track`, `active_pattern[track] = focus.location`); TRACK long `session_next_bank()` and a bank notice; VALUE rotate tempo (as now); VALUE press cycles `transport_set_launch_quant` and shows the mode; VALUE long opens pattern select on the focus (`selected_track`, `select_pattern = focus.location`).
- Step buttons in `UI_SESSION`, only for steps below the focused cell's length: a short press toggles on release (then `sequencer_refresh_track_playback`); a long press sets `selected_track`, `active_pattern[track]` and `edit_step` from the focus, `step_edit_return = UI_SESSION`, and opens the step editor without toggling. `step_short_pending` is cleared whenever the mode is not `UI_SESSION`.
- The step editor returns to `step_edit_return` (`UI_PATTERN_VIEW` when opened from there). Every `toggle_transport()` on an encoder press is removed; song-edit buttons 2/3 only call `transport_set_mode`. Panic returns to `UI_SESSION`.
- Until Task 9, `ui_render` and `update_step_leds` handle `UI_SESSION` through the old performance-view paths (focus track and `session_bank()`). `ui_pattern_label` is deleted if nothing uses it any more.
- `test_ui.c` gets stubs for `panel_input_*` with a scripted queue `push_panel(kind, track, row)` and for `rgb_leds_init`/`rgb_leds_show`; the remaining `UI_PERFORMANCE` uses (panic test, `render_views`) become `UI_SESSION`; the end of the panic test starts the transport with `push_panel(PANEL_PLAY, 0, 0)` instead of the encoder; `test_song_mode_has_a_return_to_pattern_playback` expects `TRANSPORT_STOPPED` after button 3, then PLAY starts it; `test_performance_mutes_do_not_edit_notes` and `test_editor_follows_performance_selection` are replaced by the tests below.

- [ ] **Tests:**

```text
test_session_is_home_and_steps_edit_focus: mode == UI_SESSION; PAD(2,3); step 5 pressed then released
    -> patterns[3].steps[2][4] ACTIVE
test_mute_button_mutes_without_editing: MUTE(0) -> track_mutes == 1; no step changed
test_step_editor_targets_the_focus: PAD(2,3); step 5 pressed, long-pressed, released
    -> UI_STEP_EDIT, active_pattern[2]==3, edit_step==4, step 4 not toggled; press_encoder(0, BTN_PRESS)
    -> patterns[3].steps[2][4] has ACCENT
test_step_edit_returns_to_session: then press_encoder(1, BTN_PRESS) -> UI_SESSION
test_step_beyond_length_does_nothing: focus cell length 4; short and long press of step 6 -> nothing changes, mode stays
test_step_release_after_screen_change_does_not_toggle: step 3 pressed; press_encoder(0, BTN_PRESS) (pattern view);
    step 3 released -> focus cell unchanged
test_play_is_the_only_transport_toggle: press_encoder(1, BTN_PRESS) in session -> quant advanced, STOPPED;
    in pattern view press_encoder(1, BTN_PRESS) -> STOPPED; PLAY -> PLAYING
test_grid_works_in_step_editor_without_moving_its_target: editor on (track 0, loc 0); transport_start(); PAD(0,5)
    -> queued[0]==5, UI_STEP_EDIT, active_pattern[0]==0, focus unchanged
test_track_long_press_changes_bank: press_encoder(0, BTN_LONG_PRESS); PAD(0,0) while stopped -> current[0]==8
test_value_long_opens_pattern_select_on_focus: PAD(3,2); press_encoder(1, BTN_LONG_PRESS)
    -> UI_PATTERN_SELECT, select_pattern==2, selected_track==3
test_launch_in_song_mode_notifies: transport_set_mode(PLAY_MODE_SONG); SCENE(1) -> notify "SONG MODE"; PAD(1,1) -> same
test_panic_drops_queued_panel_events: transport_start(); push PAD(0,4); trigger the panic combo -> resync called,
    queued[0] unchanged afterwards; a step held through the panic does not toggle on release
```

- [ ] **Implement.**
- [ ] **Check:** host tests, sanitizers, firmware.
- [ ] **Commit:** "Add the Session view and global grid, scene, mute and PLAY handling".

### Task 9: Session lights and OLED

**Files:**
- Modify: `src/ui.c` (`ui_init`, `ui_render`, `update_step_leds`, new `update_panel_lights`), `src/display.c`, `include/sequencer.h` (`display_draw_session_view`; remove `display_draw_performance_view` and `build_performance_mask`)
- Modify: `test/test_ui.c` (`#include "../src/sequencer.c"` and a `tick()` like `test_playback.c:16-19`), `test/CMakeLists.txt` (drop `sequencer.c` from the `test_ui` sources)

**Interfaces:**
- Consumes: `sequencer_snapshot`, `sequencer_clip_mask`, `session_*`, `led_frame_*`, `rgb_leds_*`.
- Produces:
  ```c
  void display_draw_session_view(uint8_t bank, uint8_t common_row,   // session_common_row(); 0xFF = MIXED
                                 uint8_t focus_track, uint8_t focus_location, uint8_t focus_length,
                                 float bpm, launch_quant_t quant, uint8_t bar_step);
  ```
  `ui_init` calls `rgb_leds_init()`. `update_panel_lights()` runs from `ui_render` (~30 fps): `sequencer_clip_mask(session_bank())` and `sequencer_snapshot()` with no lock held; `muted = live_mutes | stored_mutes`; stopped pulse = `(uint32_t)(((uint64_t)now_ms * 2 * PPQN) / 1000)`; build, apply `LED_RGB_BUDGET_MA`, `rgb_leds_show`. Session step LEDs: active steps of the focused cell within its length; when the focused cell is the playing location of its track, its playhead blinks on 8ths of `pulse_count`. The OLED shows bank and common row (slot + 1) or MIXED, focused track, location and length, tempo, quantization, and a 16-segment bar indicator; exact layout is the implementer's.

- [ ] **Tests** (the `rgb_leds_show` stub copies the last frame):

```text
test_session_lights_reach_the_chain: note on (track 0, loc 0) via pattern_set_step + sequencer_refresh_track_playback;
    transport_start(); tick(); ui_render()
    -> LED_PAD(0,0) == red at LED_LEVEL_FULL; LED_PAD(1,0) == white at LED_LEVEL_DIM (playing, empty);
       LED_PAD(1,1) == {0,0,0}; LED_PLAY.g > 0
test_session_step_leds_show_focus_steps: notes on steps 0 and 4 of (track 2, loc 3); PAD(2,3); ui_update()
    -> led_mask == 0x0011
test_session_step_leds_blink_playhead: focus cell playing; tick to step 2; the mask at pulse phase on and off
    differs exactly in bit 2
test_session_oled_renders: ui_render() in UI_SESSION leaves the framebuffer not blank
```

- [ ] **Implement.**
- [ ] **Check:** host tests, sanitizers, firmware.
- [ ] **Commit:** "Show the session on the RGB lights, step LEDs and OLED".

### Task 10: Flash resync, I2C hardening and docs

**Files:**
- Modify: `src/flash_storage.c` (`panel_input_resync()` after a successful save, right after `multicore_lockout_end_blocking`, and after a successful load), `src/mcp23017.c`, `src/step_leds.c`
- Create: `test/test_expanders.c` (fake `i2c_*_timeout_us` for MCP23017 0x20/0x21); Modify: `test/CMakeLists.txt` (`test_expanders` from it + `mcp23017.c` + `step_leds.c` + `fakes/fake_hardware.c`, without `MOCK_HARDWARE`), `test/test_storage.c` (stub counting `panel_input_resync`)
- Modify: `ARCHITECTURE.md` (hardware table: GPIO 27, GPIO 28, I2C 0x34, external pull-ups, power), `HARDWARE_STATUS.md` (bench list from the spec's "Testing"), `JITTER_MEASUREMENT.md` (load case: scenes launching + LEDs animating + TCA8418 polled + OLED refresh)

MCP23017 and step-LED I2C: every helper (init writes included) uses the timeout API and checks the byte count; the two ports are read in one 2-byte transfer; a failed read keeps the previous accepted button state; the LED mask is written OLATA/OLATB in one 3-byte transfer and the cached mask changes only after a successful write, so a failed mask is retried on the next call; `gpio_pull_up` on GPIO 8/9 becomes `gpio_disable_pulls`.

- [ ] **Tests:**

```text
test_save_resyncs_panel_input (test_storage.c): a successful save -> resync count == 1
test_load_resyncs_panel_input (test_storage.c): a successful load -> resync count == 1
test_failed_port_read_keeps_buttons (test_expanders.c): button 3 held; next read returns an error -> held still has bit 3, no release event
test_failed_led_write_is_retried (test_expanders.c): write error on step_leds_set_mask(0x0005); next call with 0x0005 writes it
test_i2c_pins_have_no_internal_pulls (test_expanders.c): after mcp23017_init, GPIO 8/9 had pulls disabled
```

- [ ] **Implement**, then update the three docs.
- [ ] **Check:** host tests, sanitizers, firmware at both gains.
- [ ] **Commit:** "Resync panel input after flash access, harden I2C and document the panel".
