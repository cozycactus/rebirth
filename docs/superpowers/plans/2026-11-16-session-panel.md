# Session-Style Live Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the RP2040 sequencer playable by hand like a Session view: an 8x8 RGB clip grid with scene and mute buttons, the 16 step buttons editing the focused clip, a PLAY button, quantized launches.

**Architecture:** The sequencer core (Core 1 ISR) gains only a per-step launch-quantization check and a scene-queue API. Everything new on the panel runs on Core 0: a TCA8418 driver and a debounced panel-input layer, a pure session model over the existing transport API, a pure LED-frame builder, and a PIO/DMA driver for the SK6812 chain. `ui.c` gets a new home view (`UI_SESSION`) and a global handler for the grid, scenes, mutes and PLAY that runs before the per-screen dispatch.

**Tech Stack:** C11, Raspberry Pi Pico SDK 2.2.0 (RP2040: PIO, DMA, I2C), CMake; host tests are plain C executables under `test/` run by CTest, with fakes in `test/fakes`.

**Spec:** `docs/superpowers/specs/2026-11-16-session-panel-design.md`

## Global Constraints

- Core 1's timer ISR gets no new work beyond the launch-quantization check; no new locks in it; lock order stays playback -> shared state.
- Core 1 never touches the LEDs or I2C.
- Save format unchanged (version 5); launch quantization and the shown bank are not saved.
- GPIO 27 = LED data (PIO), GPIO 28 = PLAY (pull-up, active low); I2C0 on GPIO 8/9 at 400 kHz; TCA8418 at 0x34 next to MCP23017 0x20/0x21.
- 82 LEDs: 64 pads, 8 SCENE, 8 MUTE, PLAY, BEAT. The whole device stays within 500 mA at 5 V; RGB budget 200 mA.
- Press-to-event within 15 ms; panel keys debounced in firmware for 8 ms; PLAY debounced for 20 ms.
- Every I2C transaction has a timeout and a checked result; a failed read never blocks the UI.
- Default launch quantization `1 BAR`; `TRACK LOOP` (today's rule) stays as an option.
- Track routing (`track_config_t`) belongs to the track: the same config in all 32 locations.
- Host tests pass under CTest and under the CI sanitizer build (`-Wall -Wextra -Werror -fsanitize=address,undefined`, clang); the firmware builds without warnings with `-DREBIRTH_CV_DAC_GAIN=1` and `=2`.

## Review Focus

- Shortening the playing clip under `1 BAR` must not apply a pending queue early (today a shorten-wrap also applies the queue) — test in Task 1.
- A pad pressed right after a bar line waits a whole bar, never lands mid-step — test in Task 1.
- A track whose queue points into a bank that is not shown must still show that something is pending: its MUTE light blinks on 8ths — test in Task 5.
- Keys held or pressed during a flash save/load must not fire afterwards — tests in Tasks 6 and 10.
- A step button pressed in the Session view and released after the screen changed must not toggle anything — test in Task 8.

## Commands

Run from the worktree root.

- Host tests: `cmake -S test -B build/host && cmake --build build/host --parallel && ctest --test-dir build/host --output-on-failure` — expected `100% tests passed`.
- One host executable: `cmake --build build/host --target test_playback && build/host/test_playback` — every test prints `PASS <name>`; a failure prints `file:line: condition` and exits 1.
- Sanitizers (as CI): `cmake -S test -B build/san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang '-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' && cmake --build build/san --parallel && ctest --test-dir build/san --output-on-failure`.
- Firmware: `PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0 cmake -S . -B build/fw1 -G Ninja -DCMAKE_BUILD_TYPE=Release -DREBIRTH_CV_DAC_GAIN=1 && cmake --build build/fw1 2>&1 | grep -Ei 'warning|error'` — expected no output and `build/fw1/rp2040_sequencer.uf2` present; same with `fw2` and `=2`.

---

### Task 1: Launch quantization in the sequencer

**Files:**
- Modify: `include/sequencer.h` (enum, `g_seq` field, API)
- Modify: `src/sequencer.c:218-230` (`apply_track_queue_if_ready`), `src/sequencer.c:523-544` (ISR step loop), new setter/getter near `transport_set_mode`
- Modify: `src/main.c` (`init_defaults`: default `LAUNCH_QUANT_BAR`)
- Modify: `ARCHITECTURE.md` ("Playback Rules": replace "Queues apply at that track's loop end" with the four modes and the phase rule)
- Test: `test/test_playback.c`

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
  // in sequencer_state_t:  volatile uint8_t launch_quant;   (zero-init == TRACK_LOOP)
  void transport_set_launch_quant(launch_quant_t q);  // ignores q >= COUNT; written under the playback lock
  launch_quant_t transport_get_launch_quant(void);
  ```

- [ ] **Step 1: Write the failing tests** in `test/test_playback.c`, registered with `RUN(...)`. Helper:

```c
static void run_through_step(uint32_t step) {   // returns right after the ISR processed `step`
    while (g_seq.transport.pulse_count <= step * PULSES_PER_STEP) tick();
}
```

Each test puts a note on step 0 of locations 0 and 1 for track 0 (`set_note`), sets the mode with `transport_set_launch_quant`, then `transport_start()`.

```c
test_bar_launch_waits_for_bar_line:      BAR; run_through_step(5); queue track 0 -> 1;
    run_through_step(15) -> current_pattern[0]==0, queued_pattern[0]==1;
    run_through_step(16) -> current==1, queued==NO_QUEUED_PATTERN, current_track_step[0]==0
test_bar_launch_on_short_loop_waits_for_bar: set_length(0,0,12), set_length(1,0,12); BAR;
    run_through_step(3); queue 1; run_through_step(12) -> current==0;
    run_through_step(16) -> current==1 && current_track_step[0]==0
test_beat_launch_joins_in_phase:         BEAT; run_through_step(5); queue 1;
    run_through_step(7) -> current==0; run_through_step(8) -> current==1 && current_track_step[0]==8
test_launched_short_pattern_takes_bar_phase: set_length(1,0,5); BEAT; run_through_step(5); queue 1;
    run_through_step(8) -> current==1 && current_track_step[0]==3        // (8 % 16) % 5
test_next_step_launch:                   STEP; run_through_step(5); queue 1;
    run_through_step(6) -> current==1 && current_track_step[0]==6
test_requeue_of_playing_location_keeps_phase: set_length(0,0,12); BAR; run_through_step(13);
    queue track 0 -> 0; run_through_step(16) -> queued==NO_QUEUED_PATTERN && current_track_step[0]==4
test_quant_change_retargets_pending_queue: BAR; run_through_step(2); queue 1;
    transport_set_launch_quant(LAUNCH_QUANT_STEP); run_through_step(3) -> current==1
test_shortening_under_bar_does_not_launch_early: BAR; run_through_step(9); queue 1;
    set_length(0,0,8); run_through_step(12) -> current==0; run_through_step(16) -> current==1
test_queue_right_after_bar_waits_a_whole_bar: BAR; run_through_step(16); queue 1;
    run_through_step(31) -> current==0; run_through_step(32) -> current==1
```

- [ ] **Step 2: Run** `test_playback`. Expected: compile error (`transport_set_launch_quant` undeclared).

- [ ] **Step 3: Implement.** Add the enum, field and API. In the ISR, on every step boundary in Pattern mode, replace the loop-end-only call with this per-track rule (before the step's event is read):

```text
q = g_seq.launch_quant; len = clamp(length of prepared row)
due = queued[tr] != NONE && (q == TRACK_LOOP ? track_step_count[tr] >= len
                                             : global_step % N(q) == 0)      // N: BAR 16, BEAT 4, STEP 1
if due:
    if queued[tr] == current[tr]: clear queued[tr]                            // phase kept
    else: apply (existing apply_track_queue_if_ready), reload prepared, len = new length,
          track_step_count[tr] = (q == TRACK_LOOP) ? 0 : (global_step % 16) % len
if track_step_count[tr] >= len: track_step_count[tr] = 0                     // plain wrap, never applies
```

Song mode keeps its own bar logic and never calls this. `pulse_count` is not reset by launches. In `init_defaults` set `g_seq.launch_quant = LAUNCH_QUANT_BAR`.

- [ ] **Step 4: Run** all host tests. Expected: the new tests and every existing `test_playback`/`test_song`/`test_ui` test pass (they run with the zero-initialized `TRACK_LOOP`).

- [ ] **Step 5: Update `ARCHITECTURE.md` "Playback Rules" and commit.**

```bash
git add include/sequencer.h src/sequencer.c src/main.c test/test_playback.c ARCHITECTURE.md
git commit -m "Quantize pattern launches to bar, beat or step"
```

### Task 2: Clip mask and scene queue

**Files:**
- Modify: `include/sequencer.h`, `src/sequencer.c` (`sequencer_refresh_track_playback`, `fill_prepared_track`, new `transport_queue_scene`)
- Test: `test/test_playback.c`

**Interfaces:**
- Consumes: Task 1's queue semantics.
- Produces:
  ```c
  uint64_t sequencer_clip_mask(uint8_t bank);    // bit (track * 8 + slot): location bank*8+slot has an ACTIVE
                                                 // step within that track's length; ignores tracks[].muted
  bool transport_queue_scene(uint8_t pattern_index); // Pattern mode only, one critical section; playing: tracks not on
                                                 // the location are queued to it, tracks on it lose any queue;
                                                 // stopped: all tracks select it at once (as queue_patterns does)
  ```

- [ ] **Step 1: Write the failing tests.**

```c
test_clip_mask_tracks_edits: CHECK(sequencer_clip_mask(1) == 0);
    set_note(9, 2, 3, 60, 0);  CHECK(sequencer_clip_mask(1) & (1ull << (2 * 8 + 1)));
    set_length(9, 2, 3);       CHECK(!(sequencer_clip_mask(1) & (1ull << (2 * 8 + 1))));
test_clip_mask_ignores_stored_mute: g_seq.patterns[9].tracks[2].muted = true; set_note(9, 2, 0, 60, 0);
    CHECK(sequencer_clip_mask(1) & (1ull << (2 * 8 + 1)));
test_scene_queue_skips_tracks_already_there: transport_start(); queue track 1 -> 9; run 16 steps (TRACK_LOOP);
    CHECK(current_pattern[1]==9); CHECK(transport_queue_track_pattern(3, 2));
    CHECK(transport_queue_scene(9));
    CHECK(queued_pattern[1]==NO_QUEUED_PATTERN && queued_pattern[3]==9 && queued_pattern[0]==9)
test_scene_selects_at_once_when_stopped: CHECK(transport_queue_scene(4)); every current_pattern == 4, no queues
test_scene_rejected_in_song_mode: transport_set_mode(PLAY_MODE_SONG); CHECK(!transport_queue_scene(4))
```

- [ ] **Step 2: Run** `test_playback`. Expected: compile error.
- [ ] **Step 3: Implement.** Keep `static uint64_t clip_mask[PATTERN_BANKS]`; `sequencer_refresh_track_playback` computes the bit from the copied steps and length (before the muted masking in `fill_prepared_track`) and stores it under `seq_lock`. `flash_storage_load` already refreshes every location, so the masks follow a load.
- [ ] **Step 4: Run** all host tests. Expected: pass.
- [ ] **Step 5: Commit** — `git commit -m "Track clip presence per bank and queue scenes atomically"`.

### Task 3: Routing per track

**Files:**
- Modify: `include/sequencer.h`, `src/song.c` (new function), `src/ui.c:621-708` (`handle_track_config`)
- Modify: `ARCHITECTURE.md` ("Playback Rules": routing belongs to the track)
- Test: `test/test_song.c`, `test/test_playback.c`, `test/test_ui.c`

**Interfaces:**
- Produces: `void seq_set_track_config(uint8_t track, const track_config_t *config);` — copies `*config` into `patterns[0..31].tracks[track]` under `seq_lock`, then refreshes the prepared rows of that track for all 32 locations (outside the lock).

- [ ] **Step 1: Write the failing tests.**

```c
// test_song.c
test_track_config_applies_to_every_location: cfg = g_seq.patterns[0].tracks[2]; cfg.midi_channel = 7;
    seq_set_track_config(2, &cfg); for p in 0..31: CHECK(g_seq.patterns[p].tracks[2].midi_channel == 7);
    CHECK(g_seq.patterns[5].tracks[1].midi_channel != 7)          // other tracks untouched
// test_playback.c
test_launch_keeps_track_routing: cfg = patterns[0].tracks[0]; cfg.midi_channel = 5; cfg.output_mode = OUTPUT_MIDI_ONLY;
    seq_set_track_config(0, &cfg); set_note(3, 0, 0, 60, 0); transport_start(); queue track 0 -> 3;
    run_through_step(16); CHECK(a 0x95 note-on for note 60 is in fake_midi)
// test_ui.c
test_track_config_edit_reaches_all_locations: open track config for track 0 (VALUE long -> pattern select,
    long press step button 1), e1d = +1 (MIDI channel), e2d = +1; for p in 0..31: CHECK(patterns[p].tracks[0].midi_channel == old + 1)
```

- [ ] **Step 2: Run** the three executables. Expected: compile errors / failures.
- [ ] **Step 3: Implement** `seq_set_track_config`; rework `handle_track_config` to copy the active location's config into a local, edit the local (no lock held while editing), and call `seq_set_track_config` when it changed. Note in ARCHITECTURE that older saves with differing per-location configs play as before until a track is edited.
- [ ] **Step 4: Run** all host tests. Expected: pass.
- [ ] **Step 5: Commit** — `git commit -m "Keep track routing the same in every location"`.

### Task 4: Session model

**Files:**
- Create: `include/session.h`, `src/session.c`
- Create: `test/test_session.c`; Modify: `test/CMakeLists.txt` (executable `test_session` with `fakes/fake_hardware.c`, `session.c`, `sequencer.c`, `pattern.c`, `song.c`, `shared_state.c`, `midi_out.c`; `add_test(NAME session_tests ...)`), `CMakeLists.txt` (add `src/session.c`)

**Interfaces:**
- Consumes: `transport_queue_track_pattern`, `transport_clear_track_queue`, `transport_clear_queue`, `transport_queue_scene`, `seq_toggle_track_mute`, transport fields.
- Produces:
  ```c
  typedef struct { uint8_t track; uint8_t location; } session_focus_t;
  void session_init(void);                     // bank 0; focus = (0, current location of track 0)
  uint8_t session_bank(void);
  void session_next_bank(void);                // 0->1->2->3->0; focus unchanged
  session_focus_t session_get_focus(void);
  void session_move_focus(int8_t delta);       // track clamped to 0..7; location = queued if pending, else current
  bool session_pad(uint8_t track, uint8_t row, bool move_focus); // false = rejected (Song mode); focus still moves
  bool session_scene(uint8_t row);             // false = rejected (Song mode)
  void session_mute(uint8_t track);
  ```
  Pad rule (L = bank*8+row): queued == L -> clear that track's queue; current == L -> clear any queue (no restart); else queue L. Scene rule: any track queued to L -> `transport_clear_queue()`; else `transport_queue_scene(L)`.

- [ ] **Step 1: Write the failing tests** (`test/test_session.c`, same `CHECK`/`RUN`/`reset` pattern as `test_playback.c`, `reset` also calls `session_init()`).

```c
test_pad_queues_and_focuses: transport_start(); CHECK(session_pad(2, 3, true));
    CHECK(queued_pattern[2] == 3); focus == (2, 3)
test_pad_on_queued_location_cancels: start; session_pad(2,3,true) twice -> queued_pattern[2] == NO_QUEUED_PATTERN
test_pad_on_playing_location_cancels_other_queue: start; session_pad(2,3,true); session_pad(2,0,true)
    -> queued_pattern[2] == NO_QUEUED_PATTERN && current_pattern[2] == 0
test_pad_while_stopped_selects_immediately: session_pad(4,6,true) -> current_pattern[4] == 6
test_empty_pad_launches_silence: start; session_pad(1,7,true) on an empty location -> queued_pattern[1] == 7
test_scene_launches_row_of_shown_bank: session_next_bank(); start; session_scene(3) -> every queued_pattern == 11
test_scene_press_while_pending_cancels_all: start; session_scene(3); session_pad(0,5,false);
    session_scene(3) -> no track has a queue
test_stop_ends_pending_scene: start; session_scene(3); transport_stop(); transport_start();
    session_scene(3) -> every queued_pattern == 3                 // launches again, does not cancel
test_song_mode_rejects_launch_but_moves_focus: transport_set_mode(PLAY_MODE_SONG);
    CHECK(!session_pad(5,2,true)); focus == (5, 2); CHECK(!session_scene(2))
test_move_focus_follows_queue_then_current: start; session_pad(3,6,false); session_move_focus(+3)
    -> focus == (3, 6); session_move_focus(+1) -> focus == (4, current_pattern[4])
test_bank_change_keeps_focus: session_pad(1,2,true); session_next_bank(); focus == (1, 2) && session_bank() == 1
```

- [ ] **Step 2: Run** `ctest` after adding the executable. Expected: link errors for `session_*`.
- [ ] **Step 3: Implement** `src/session.c` (Core 0 state only; reads transport fields under `seq_lock`, calls the transport API outside it).
- [ ] **Step 4: Run** all host tests and the firmware build. Expected: pass, no warnings.
- [ ] **Step 5: Commit** — `git commit -m "Add the session model: pads, scenes, mutes, focus and banks"`.

### Task 5: LED frame builder

**Files:**
- Create: `include/led_frame.h`, `src/led_frame.c`
- Create: `test/test_led_frame.c`; Modify: `test/CMakeLists.txt` (`test_led_frame` from `test_led_frame.c` + `led_frame.c`, `add_test(NAME led_frame_tests ...)`), `CMakeLists.txt` (add `src/led_frame.c`)

**Interfaces:**
- Produces:
  ```c
  #define LED_COUNT 82
  #define LED_PAD(track, row) ((row) * 8 + (track))
  #define LED_SCENE(row)      (64 + (row))
  #define LED_MUTE(track)     (72 + (track))
  #define LED_PLAY            80
  #define LED_BEAT            81
  #define LED_LEVEL_DIM   3      // starting values; retuned on the bench
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
      uint8_t  muted;                   // bit per track: live mute | stored tracks[].muted
      uint8_t  focus_track;
      bool     playing;
      uint32_t pulse;                   // transport pulse_count while playing; a 120 BPM pulse clock when stopped
  } led_state_t;
  void led_frame_build(const led_state_t *s, rgb_t out[LED_COUNT]);
  uint32_t led_frame_estimate_ma(const rgb_t frame[LED_COUNT]);   // idle + dynamic, rounded up
  void led_frame_apply_budget(rgb_t frame[LED_COUNT], uint32_t budget_ma);
  ```
  Hues, scaled by level/255: red {255,0,0}, orange {255,80,0}, yellow {255,200,0}, green {0,255,40}, cyan {0,220,255}, blue {0,60,255}, violet {140,0,255}, pink {255,0,120}; white {255,255,255}; scene green {0,255,0}.
  Blinks: 8th-note blink is "on" when `(pulse / (PPQN / 2)) % 2 == 0`; slow blink "on" when `(pulse / (PPQN * 2)) % 2 == 0`.
  Rules exactly as the spec's light tables (pad priority list, SCENE list, MUTE, PLAY, BEAT); additionally a track whose queue points outside the shown bank has its MUTE blink on 8ths, which takes precedence over the slow "plays elsewhere" blink. A row "plays" when the transport plays and the track's current location is that row; with the transport stopped, a row all 8 tracks sit on is dim green. BEAT while playing: `pulse % (PPQN*4) < PPQN/8` -> white FULL; else `pulse % PPQN < PPQN/8` -> white HALF; else off.
  Budget: `scale = min(1, (budget - idle_total) / dynamic)` applied to channel values only.

- [ ] **Step 1: Write the failing tests** (`test/test_led_frame.c`; a `base_state()` helper: bank 0, all current 0, no queues, no clips, nothing muted, focus 0, stopped, pulse 0; `expect(rgb, hue, level)` compares to a hue scaled by level).

```c
test_empty_pad_is_off                    test_stored_clip_is_dim_hue
test_playing_clip_is_full_hue            test_stopped_current_clip_is_half_hue
test_queued_clip_blinks_on_eighths       // pulse 0 -> full hue, pulse 48 -> off
test_queued_empty_blinks_dim_white       test_current_empty_is_dim_white_while_playing
test_scene_rules                         // pending blink, all on row = full green, some = dim green,
                                         // has clip = dim white, empty = off, stopped all on row = dim green
test_mute_rules                          // audible half hue, muted off, focus audible full hue,
                                         // focus muted dim white, plays other bank: pulse 0 on / 192 off
test_queue_in_other_bank_blinks_mute_fast // pulse 0 on, pulse 48 off
test_play_and_beat                       // PLAY green full while playing, off stopped; BEAT full on bar,
                                         // half on beat 2, off between beats
test_busiest_designed_frame_fits_budget  // all 64 cells have clips, 8 tracks play row 0, focus 0, beat on bar:
                                         // led_frame_estimate_ma(frame) <= LED_RGB_BUDGET_MA before any scaling
test_budget_scales_only_dynamic_current  // all channels 255 -> after apply_budget estimate <= 200 and > idle;
                                         // a frame under budget is returned unchanged
```

- [ ] **Step 2: Run** `ctest`. Expected: link errors.
- [ ] **Step 3: Implement** `src/led_frame.c` (pure: no locks, no hardware).
- [ ] **Step 4: Run** host tests and the firmware build. Expected: pass.
- [ ] **Step 5: Commit** — `git commit -m "Add the pure LED frame builder with the light rules and current budget"`.

### Task 6: TCA8418 driver, PLAY button and panel input

**Files:**
- Create: `include/tca8418.h`, `src/tca8418.c`, `include/panel_input.h`, `src/panel_input.c`
- Create: `test/fakes/hardware/i2c.h` (declarations only); Modify: `test/fakes/pico/stdlib.h` + `test/fakes/fake_hardware.c` (`gpio_get`, `gpio_pull_up`, `bool fake_gpio_level[32]` defaulting high)
- Create: `test/test_panel_input.c` (holds the fake TCA8418: register file, 10-event FIFO, overflow flag, injectable I2C error); Modify: `test/CMakeLists.txt` (`test_panel_input` from it + `tca8418.c` + `panel_input.c` + `fakes/fake_hardware.c`), `CMakeLists.txt` (add both sources)

**Interfaces:**
- Produces:
  ```c
  // tca8418.h
  #define TCA8418_ADDR 0x34
  bool tca8418_init(void);   // rows R0-R7, cols C0-C9 as keypad; debounce disabled; CFG = AI|OVR_FLOW_M|OVR_FLOW_IEN|KE_IEN
  int  tca8418_read_events(uint8_t *events, int max, bool *overflow); // >= 0 events read, -1 on I2C error
  void tca8418_flush(void);  // read the FIFO empty, clear INT_STAT
  // panel_input.h
  typedef enum { PANEL_PAD, PANEL_SCENE, PANEL_MUTE, PANEL_PLAY } panel_kind_t;
  typedef struct { panel_kind_t kind; uint8_t track; uint8_t row; bool down; } panel_event_t;
  #define PANEL_DEBOUNCE_MS 8
  #define PLAY_DEBOUNCE_MS 20
  #define PLAY_BUTTON_PIN 28
  void panel_input_init(void);              // after mcp23017_init (I2C0 already up)
  void panel_input_poll(uint32_t now_ms);   // every ui_update pass
  bool panel_input_next(panel_event_t *out);
  void panel_input_resync(void);            // flush chip, forget held keys, drop queued events
  ```
  Registers (TI datasheet; same values as the Linux `tca8418_keypad` driver — check while writing): CFG 0x01 (AI 0x80, OVR_FLOW_M 0x20, OVR_FLOW_IEN 0x08, KE_IEN 0x01), INT_STAT 0x02 (OVR_FLOW_INT 0x08, K_INT 0x01; write 1 to clear), KEY_LCK_EC 0x03 (count in bits 3:0), KEY_EVENT_A 0x04 (bit 7 press, bits 6:0 key 1..80), KP_GPIO1..3 0x1D..0x1F (0xFF, 0xFF, 0x03), DEBOUNCE_DIS1..3 0x29..0x2B (0xFF, 0xFF, 0x03). Key `k` (1..80): row = (k-1)/10, col = (k-1)%10. A `static const` table maps (row, col) to controls: col 0-7 -> PAD(track = col, row), col 8 -> SCENE(row), col 9 -> MUTE(track = row); a board wired differently edits only this table. I2C uses `i2c_write_timeout_us` / `i2c_read_timeout_us` with 2000 us.
  Debounce: each key keeps a raw state (from events) and an accepted state; an event is emitted once raw differs from accepted and raw has been stable for `PANEL_DEBOUNCE_MS` (taps shorter than that are ignored). Overflow -> flush, all keys released silently, queued events dropped. The ring holds 32 events; when full, new events are dropped.

- [ ] **Step 1: Write the failing tests.**

```c
test_init_configures_keypad: CHECK(tca8418_init()); fake regs CFG == 0xA9, KP_GPIO1..3 == FF,FF,03
test_key_codes_map_to_controls: press keys (r2,c5), (r4,c8), (r6,c9); poll at t=0 and t=8
    -> PAD track 5 row 2 down; SCENE row 4 down; MUTE track 6 down
test_bounce_yields_one_press: press,release,press at t=0,1,2 (polls each ms); nothing until t=10; at t=10 exactly one PAD down
test_release_is_reported: press t=0, release t=20 -> down at t>=8, up at t>=28
test_ten_key_chord_all_reported: 10 presses in one burst -> 10 down events
test_overflow_resyncs_without_stuck_keys: FIFO full + overflow flag -> poll emits nothing,
    INT_STAT cleared; a new press afterwards is reported
test_i2c_error_does_not_block: fake error on next read -> poll returns, no events; next poll reads normally
test_play_button_debounce: fake_gpio_level[28] = false at t=0 -> PLAY down at t>=20 (not before); true at t=50 -> up at t>=70
test_resync_drops_pending_events: press; poll at t=8; panel_input_resync(); CHECK(!panel_input_next(&e))
```

- [ ] **Step 2: Run** `ctest`. Expected: link errors.
- [ ] **Step 3: Implement** both modules.
- [ ] **Step 4: Run** host tests, sanitizers and the firmware build. Expected: pass, no warnings.
- [ ] **Step 5: Commit** — `git commit -m "Add the TCA8418 driver and debounced panel input with PLAY"`.

### Task 7: SK6812 chain driver (PIO + DMA)

**Files:**
- Create: `include/rgb_leds.h`, `src/rgb_leds.c`, `src/ws2812.pio` (the WS2812 program from pico-examples, BSD-3-Clause, header kept)
- Modify: `CMakeLists.txt` (add `src/rgb_leds.c`; `pico_generate_pio_header(rp2040_sequencer ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)`; link `hardware_pio hardware_dma`)

**Interfaces:**
- Consumes: `rgb_t`, `LED_COUNT` (Task 5).
- Produces:
  ```c
  #define RGB_LEDS_PIN 27
  void rgb_leds_init(void);                          // PIO0, 800 kHz, GRB 24-bit; claims a DMA channel; sends an all-off frame
  bool rgb_leds_show(const rgb_t frame[LED_COUNT]);  // false = previous frame still in flight, this one skipped
  ```
  A `static const uint8_t chain_order[LED_COUNT]` maps logical LED index to chain position (identity until the board exists). Frames are packed into a static SRAM buffer before DMA starts; the function never waits for DMA and never disables interrupts.

- [ ] **Step 1: Implement** (no host test: hardware-only module; the frame content is covered by Task 5).
- [ ] **Step 2: Build** the firmware at both gains. Expected: no warnings, `.uf2` produced.
- [ ] **Step 3: Commit** — `git commit -m "Drive the SK6812 chain from PIO with DMA"`.

### Task 8: Session view controls

**Files:**
- Modify: `src/ui.c` (modes, `ui_state_t`, `ui_init`, `ui_update`, `handle_performance_view` -> `handle_session_view`, `handle_pattern_view`, `handle_step_edit`, `handle_pattern_select`, `handle_song_edit`, `handle_panic_combo`)
- Modify: `test/test_ui.c` (stubs for `panel_input_*` with a scripted event queue `push_panel(kind, track, row)`, stub `rgb_leds_init`/`rgb_leds_show`, link `session.c`, `led_frame.c`), `test/CMakeLists.txt`
- Modify: `README.md` ("Live Controls")

**Interfaces:**
- Consumes: Tasks 1-6.
- Produces: `UI_SESSION` replaces `UI_PERFORMANCE`; `ui_state_t` gains `ui_mode_t step_edit_return` and `uint16_t step_short_pending`; `performance_bank` is removed (the bank is `session_bank()`).

Behavior (spec "Session view"): `ui_update` calls `panel_input_poll(now_ms)` first and, after the panic check and before the per-screen switch, drains `panel_input_next` (down events only): PAD -> `session_pad(track, row, ui.mode == UI_SESSION)`; SCENE -> `session_scene(row)` (false -> notify `SONG MODE`); MUTE -> `session_mute(track)`; PLAY -> `toggle_transport()`. No lock is held there. In `UI_SESSION`: TRACK rotate `session_move_focus`, press opens the pattern view on the focus (`selected_track`, `active_pattern[track] = focus.location`), long press `session_next_bank()` + notify `BANK B`; VALUE rotate tempo (as now), press cycles `transport_set_launch_quant` + notify `Q TRACK LOOP` / `Q 1 BAR` / `Q 1 BEAT` / `Q NEXT STEP`, long press opens pattern select on the focus. Step buttons: toggle on release unless long-pressed, only for steps below the focused cell's length, then `sequencer_refresh_track_playback`; long press opens the step editor for that step with `step_edit_return = UI_SESSION`. `step_short_pending` is cleared whenever the mode is not `UI_SESSION`. The step editor returns to `step_edit_return` (set to `UI_PATTERN_VIEW` when opened from there). Every `toggle_transport()` call on an encoder press is removed; song-edit buttons 2/3 only call `transport_set_mode`. Panic returns to `UI_SESSION`. Until Task 9, `ui_render` and `update_step_leds` handle `UI_SESSION` through the old performance-view paths (focus track and `session_bank()` in place of `selected_track` and `performance_bank`).

- [ ] **Step 1: Write the failing tests** (replace `test_performance_mutes_do_not_edit_notes` and `test_editor_follows_performance_selection`; update `test_song_mode_has_a_return_to_pattern_playback` to expect `TRANSPORT_STOPPED` after button 3, then PLAY starts it).

```c
test_session_is_home_and_steps_edit_focus: CHECK(ui.mode == UI_SESSION); push PAD(2,3) down; ui_update();
    step 5 pressed, ui_update(); released, ui_update(); CHECK(patterns[3].steps[2][4].flags & STEP_FLAG_ACTIVE)
test_mute_button_mutes_without_editing: push MUTE(0); ui_update(); track_mutes == 1; no step changed
test_step_long_press_opens_editor_without_toggling: pressed bit 0; long_pressed bit 0; released bit 0
    -> mode == UI_STEP_EDIT, step 0 flags unchanged
test_step_edit_returns_to_session: (from above) press_encoder(1, BTN_PRESS) -> mode == UI_SESSION
test_step_beyond_length_does_nothing: length of focus cell 4; press+release step 6 -> unchanged
test_step_release_after_screen_change_does_not_toggle: pressed bit 2; press_encoder(0, BTN_PRESS) (pattern view);
    released bit 2 -> session cell step 2 unchanged
test_play_is_the_only_transport_toggle: press_encoder(1, BTN_PRESS) in session -> quant advanced, STOPPED;
    in pattern view press_encoder(1, BTN_PRESS) -> STOPPED; push PLAY -> PLAYING
test_grid_works_in_step_editor_without_moving_its_target: open step editor on (track 0, loc 0); transport_start();
    push PAD(0,5) -> queued_pattern[0] == 5, mode == UI_STEP_EDIT, active_pattern[0] == 0, focus unchanged
test_track_long_press_changes_bank: press_encoder(0, BTN_LONG_PRESS); push PAD(0,0) while stopped -> current_pattern[0] == 8
test_value_long_opens_pattern_select_on_focus: push PAD(3,2); press_encoder(1, BTN_LONG_PRESS)
    -> mode == UI_PATTERN_SELECT, select_pattern == 2, selected_track == 3
test_scene_in_song_mode_notifies: transport_set_mode(PLAY_MODE_SONG); push SCENE(1) -> notify text "SONG MODE"
test_panic_returns_to_session: (existing panic test) mode == UI_SESSION afterwards
```

- [ ] **Step 2: Run** `test_ui`. Expected: failures / compile errors.
- [ ] **Step 3: Implement** the `ui.c` changes above.
- [ ] **Step 4: Run** host tests, sanitizers and the firmware build. Expected: pass.
- [ ] **Step 5: Update README "Live Controls" (and the feature list) and commit** — `git commit -m "Add the Session view and global grid, scene, mute and PLAY handling"`.

### Task 9: Session lights and OLED

**Files:**
- Modify: `src/ui.c` (`ui_render`, `update_step_leds`, new `update_panel_lights`), `src/display.c` + `include/sequencer.h` (`display_draw_session_view`; remove `display_draw_performance_view`)
- Test: `test/test_ui.c`

**Interfaces:**
- Consumes: `led_frame_build`, `led_frame_apply_budget`, `rgb_leds_show`, `sequencer_clip_mask`, `session_*`.
- Produces:
  ```c
  void display_draw_session_view(uint8_t bank, uint8_t common_row,   // 0xFF = MIXED
                                 uint8_t focus_track, uint8_t focus_location, uint8_t focus_length,
                                 float bpm, launch_quant_t quant, uint8_t bar_step);
  ```
  `update_panel_lights()` runs from `ui_render` (~30 fps): one short `seq_lock` section copies current/queued locations, transport state and `pulse_count`; mutes = `seq_get_track_mutes()` | stored `tracks[c].muted` of each track's current location; `clip_mask = sequencer_clip_mask(session_bank())`; stopped pulse = `ms * 192 / 1000`; build, apply `LED_RGB_BUDGET_MA`, `rgb_leds_show`. Session step LEDs: active steps of the focused cell within its length; when the focused cell is the playing location of its track, its playhead blinks on 8ths of `pulse_count` (not `led_anim_phase`). OLED lines: `BANK A  ROW 2` or `BANK A  MIXED`; `TRK 1  A2  L16`; `128.0  Q 1 BAR`; a 16-segment bar indicator filled to `bar_step`.

- [ ] **Step 1: Write the failing tests** (the `rgb_leds_show` stub copies the last frame).

```c
test_session_lights_reach_the_chain: set a note on (track 0, loc 0); transport_start(); tick to step 1; ui_render();
    last_frame[LED_PAD(0,0)] != {0,0,0}; last_frame[LED_PAD(1,0)] == {0,0,0}; last_frame[LED_PLAY].g > 0
test_session_step_leds_show_focus_steps: notes on steps 0 and 4 of (track 2, loc 3); push PAD(2,3); ui_update();
    led_mask == 0x0011 (stopped: no playhead)
test_session_oled_renders: ui_render() in UI_SESSION leaves the framebuffer not blank
```

- [ ] **Step 2: Run** `test_ui`. Expected: failures.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** host tests, sanitizers and the firmware build. Expected: pass.
- [ ] **Step 5: Commit** — `git commit -m "Show the session on the RGB lights, step LEDs and OLED"`.

### Task 10: Flash resync, I2C hardening, integration and docs

**Files:**
- Modify: `src/flash_storage.c` (`panel_input_resync()` at the end of a save, after `multicore_lockout_end_blocking`, and after a load)
- Modify: `src/mcp23017.c` (one 2-byte read of GPIOA/GPIOB with `i2c_*_timeout_us`, 2000 us; keep the previous state on error), `src/step_leds.c` (one 3-byte write OLATA/OLATB with timeout)
- Modify: `src/ui.c` (`ui_init`: `session_init`, `panel_input_init` after `mcp23017_init`, `rgb_leds_init`)
- Modify: `test/test_storage.c`, `test/test_ui.c` (count `panel_input_resync` calls)
- Modify: `ARCHITECTURE.md` (hardware table: GPIO 27, GPIO 28, I2C 0x34, external pull-ups, power), `HARDWARE_STATUS.md` (bench list from the spec's "Testing"), `JITTER_MEASUREMENT.md` (load case: scenes launching + LEDs animating + TCA8418 polled + OLED refresh)

- [ ] **Step 1: Write the failing tests.**

```c
test_save_resyncs_panel_input (test_storage.c): stub counts calls; a successful save -> count == 1
test_load_resyncs_panel_input (test_ui.c or test_storage.c, whichever compiles flash_storage.c with the fake flash):
    flash_storage_load() succeeds -> count == 1
```

- [ ] **Step 2: Run.** Expected: count 0.
- [ ] **Step 3: Implement** the resync calls, the I2C changes and the init order; update the three docs.
- [ ] **Step 4: Run** host tests, sanitizers and the firmware build at both gains. Expected: `100% tests passed` twice, no compiler output, two `.uf2` files.
- [ ] **Step 5: Commit** — `git commit -m "Resync panel input after flash access, harden I2C and document the panel"`.
