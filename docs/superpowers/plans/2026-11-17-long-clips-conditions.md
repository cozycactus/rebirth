# Clips up to 64 Steps and Trig Conditions — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** clips of 1-64 steps shown a page of 16 at a time, with an overview on
the pads, and a trig condition on every step, in save format v7.

**Architecture:** the location keeps today's two step arrays, 64 long, with a
6-byte step (condition plus two reserved bytes). The save buffer and the
prepared playback rows go; the timer reads the step it plays under `seq_lock`.
The save region grows to the last 256 KiB; v4-v6 saves still load from the old
offset. Conditions are evaluated in the timer from a per-track pass counter, a
global FILL state and a hash-based draw.

**Tech Stack:** C11, Pico SDK 2.2.0, RP2040; host tests with CMake/CTest and fake
peripherals; the WebAssembly simulator (`sim/`) with Node checks.

**Spec:** `docs/superpowers/specs/2026-11-17-long-clips-conditions-design.md`.
Read it first; this plan does not repeat its tables.

## Global Constraints

- Branch from `origin/master` (fetch first; it moves several times a day). Base
  at writing: bcf6871.
- Tests first, seen failing, then the code. One commit per task. Nothing outside
  the files a task names, except docs the task's change makes wrong.
- Every task ends green on: the host suite
  (`cmake --preset host-tests-debug && cmake --build --preset host-tests-debug && ctest --preset host-tests-debug`),
  the same suite with `-fshort-enums` and the sanitizers (the CI job's command:
  `cmake -S test -B test/build/short -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang '-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -fshort-enums -fsanitize=address,undefined -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' && cmake --build test/build/short && ctest --test-dir test/build/short --output-on-failure`),
  the firmware build (`cmake --preset firmware-debug && cmake --build --preset firmware-debug`),
  and `sim/build.sh native && sim/build.sh web && sim/check.sh`. The four existing
  scores (`base`, `fw2`, `keys`, `pads`) never change and never fail.
- Step record: `note, velocity, flags, cond, ratchet, micro`, 6 bytes; `ratchet`
  and `micro` are 0 in v7. `MAX_CLIP_STEPS` 64, `STEPS_PER_PAGE` 16,
  `STEPS_PER_BAR` 16, `CLIP_PAGES` 4, `DEFAULT_CLIP_STEPS` 16, legacy literal 16.
- Location 5,196 bytes on the RP2040; v7 payload 169,472 bytes; region the last
  256 KiB (offset 0x1C0000); legacy header at 0x1E0000.
- Condition codes exactly as the spec's table; 0 = ALWAYS; unknown codes reject
  a save.
- The timer's new code (step read, condition evaluation, LST distance) runs from
  RAM and does no division.
- No gesture beyond the spec's gesture map.
- After each task that changes RAM use, record `arm-none-eabi-size -A` of the
  firmware ELF in `HARDWARE_STATUS.md`.

## Review Focus

1. **A clip shortened below the page of a held step.** The held step keeps its
   absolute index (edits go to the hidden step), nothing reads beyond 64, and on
   release the view returns to a page within the length. Test in Task 6.
2. **A clip of length 1.** Every step boundary is a wrap: `pass` grows every
   step, 1:2 plays every other step, LST with a TRACK LOOP queue fires once.
   Test in Task 10.
3. **A type change on a 64-step clip** (SYNTH → KIT → SYNTH) keeps the notes,
   the kit words and the conditions of all 64 steps. Test in Task 4.
4. **A last page that is partial** (length 40: page 3 has 8 steps). Buttons
   9-16 there are dark and do nothing, the page cycle stops at page 3, the
   overview rows beyond step 40 are off. Tests in Tasks 6 and 8.
5. **A queue cancelled and set again within one pass.** LST steps after the
   cancel are silent, after the new queue they play again; nothing that already
   played is replayed. Test in Task 10.

---

## Phase 0 — fixtures

### Task 0: legacy images as committed fixtures

**Files:** Create `test/tools/make_fixtures.c`, `test/fixtures/v4.bin`,
`test/fixtures/v5.bin`, `test/fixtures/v6.bin`, `test/fixtures/README.md`.
Modify `test/CMakeLists.txt` (a `REBIRTH_FIXTURES` path define for the tests),
`test/test_storage.c`.

**Interfaces:** Produces the three files (each the full 128 KiB region as the
old firmware wrote it, header at offset 0) and `README.md` listing exactly what
they hold.

- [ ] Write `make_fixtures.c` against today's `flash_storage.c` and fakes:
  `_Static_assert`s that `sizeof(pattern_v5_t) == 459` and
  `sizeof(pattern_t) == 972` (so it only builds with `-fshort-enums`); it fills
  RAM with known content and writes the images: v6 through
  `flash_storage_save()`; v5 and v4 through the same writer the storage test
  uses (`write_legacy_image`). Content: location A1 track 1 SYNTH steps 1, 5,
  9 notes 48, 50, 52 velocities 100, 90, 80, step 5 accent and slide, length
  12; A1 track 2 KIT (v6 only) word `0x000105` at step 1 and kit settings with
  `flam_pulses` 7; B3 track 5 DRUM length 16 all steps active; track 7 stored
  mute at location C2; a song of 3 entries (repeats 2, 0, 4) ending in STOP;
  name `FIXTURE`.
- [ ] Build it once with `clang -fshort-enums` from today's sources, run it,
  commit the outputs and the README (with the build command).
- [ ] Add `test_legacy_fixtures()` to `test_storage.c`: copy each file to the
  fake flash at the legacy region, `flash_storage_load()` returns true and RAM
  holds exactly the README's content. With today's struct-casting loader this
  only holds where the host layout is the RP2040's: skip with a printed note
  unless `sizeof(pattern_v5_t) == 459`. (Task 1 removes the skip.)
- [ ] Run the suites; commit `test: v4-v6 save images as fixtures`.

## Phase 1 — no behaviour changes

### Task 1: split STEPS_PER_PATTERN, freeze the legacy readers

**Files:** Modify `include/sequencer.h`, `include/kit.h`, `src/*.c` that use
`STEPS_PER_PATTERN` (display, flash_storage, kit, pattern, sequencer, ui),
`sim/core/sim_main.c`, `test/test_storage.c`, other tests that use the
constant.

**Interfaces:** Produces `MAX_CLIP_STEPS` (still 16 in this task),
`STEPS_PER_PAGE`, `CLIP_PAGES`, `STEPS_PER_BAR`, `DEFAULT_CLIP_STEPS` in
`sequencer.h`; `STEPS_PER_PATTERN` no longer exists. In `flash_storage.c`:
`static bool legacy_record_to_pattern(const uint8_t *record, uint8_t version, pattern_t *out)`
reading by the RP2040 offsets (v5 record 459 bytes, v6 972, kit words at 460,
`track_config_t` 7 bytes), returning false for a record that fails the legacy
rules (lengths ≤ 16, no KIT in v4/v5, today's field checks).

- [ ] Remove the skip in `test_legacy_fixtures()`; add
  `test_legacy_reader_offsets()`: a record built byte by byte (not from a
  struct) converts to the expected `pattern_t`. Run: both fail in the host-enum
  build.
- [ ] Replace each use of `STEPS_PER_PATTERN` by its meaning: clip storage and
  length clamp → `MAX_CLIP_STEPS`; step buttons, LED masks, OLED step strips →
  `STEPS_PER_PAGE`; launch quantization, phase join, Song bar, bar step on the
  Session screen → `STEPS_PER_BAR`; `pattern_init` length →
  `DEFAULT_CLIP_STEPS`; legacy records → the literal 16.
- [ ] Rewrite the v4/v5/v6 load path on `legacy_record_to_pattern()`; frozen
  copies of the old structs remain only for `__arm__` assertions of the offsets.
- [ ] Run everything (all suites, firmware, `sim/check.sh`); commit
  `refactor: step constants by meaning; legacy saves read by offset`.

### Task 2: the timer reads the step it plays

**Files:** Modify `src/sequencer.c`, `include/sequencer.h` (comments),
`test/test_playback.c`, `ARCHITECTURE.md` ("Prepared Patterns" becomes
"Reading a step"; "Kit Tracks" prepared-row bullet).

**Interfaces:** Removes `prepared_patterns`, `prepared_playback`,
`prepared_revision`, `playback_revision`, `playback_pattern`,
`fill_prepared_track()`, `load_prepared_track()`. Produces, internal to
`sequencer.c`:
`typedef struct { uint8_t kind, length; track_config_t config; step_t step; uint32_t kit_word; kit_config_t kit; } track_read_t;`
and `static void read_track_step(uint8_t track, uint8_t location, uint8_t step, track_read_t *out)`
(one `seq_lock` section, in RAM). `sequencer_refresh_track_playback(track, location)`
keeps its signature and now only updates the clip mask bit.

- [ ] Tests: `edit_heard_at_next_occurrence` (a note changed at step 3 while
  step 2 plays sounds at step 3 of this pass); `kind_change_releases_first`
  (SYNTH note held by tie, track switched to KIT: Note Off precedes the kit hits
  at the next boundary); `stored_mute_silences`; `length_read_live` (shortening
  to 4 at step 6 wraps at the next boundary, as today); `clip_mask_follows_refresh`.
  The existing playback tests must pass unchanged, except ones that inspect
  prepared rows, which move to the public behaviour.
- [ ] Implement; inline or `__not_in_flash_func` the read.
- [ ] Run everything; record RAM; commit `perf: timer reads the step it plays; prepared rows removed`.

### Task 3: save straight from RAM

**Files:** Modify `src/flash_storage.c`, `test/test_storage.c`,
`ARCHITECTURE.md` (Storage paragraph on the copy buffer).

**Interfaces:** Removes `pattern_buf` and the per-location copy. Produces
`static void program_padded(uint32_t offset, const void *data, size_t bytes)`
using one static `uint8_t page_tail[FLASH_PAGE]` padded with 0xFF.

- [ ] Tests: `save_pads_tails_with_ff` (bytes after each payload to its page end
  are 0xFF); `save_programs_header_last` (with `fake_flash_program_limit`, a save
  cut before the header leaves no valid save); the v6 round trip unchanged.
- [ ] Implement: CRC over `g_seq` in place before the lockout; erase; program
  from `g_seq`; header last.
- [ ] Run everything; record RAM; commit `perf: save programs flash from RAM, no copy buffer`.

### Task 4: 64-step data and format v7

**Files:** Modify `include/sequencer.h`, `include/flash_storage.h`,
`src/pattern.c`, `src/flash_storage.c`, `src/kit.c`, `src/ui.c`, `src/main.c`,
`sim/core/hal.c`, `sim/core/sim_main.c`, `sim/core/sim_api.h`,
`sim/web/device.js`, `sim/test/device_check.js`, `test/test_storage.c`,
`test/test_pattern.c`, `ARCHITECTURE.md` (Storage), `HARDWARE_STATUS.md`.

**Interfaces:** `step_t` and `pattern_t` as the spec; `MAX_CLIP_STEPS` 64.
`STORE_RESERVED` 256 KiB, `LEGACY_STORE_OFFSET` (2 MiB − 128 KiB), `VERSION` 7.
`bool flash_storage_layout_ok(void)` (target: `__flash_binary_end` below the
region; host: true); a save with it false fails. `pattern_copy_track()` copies
64 steps of both kinds. In `ui.c` a temporary `UI_LENGTH_MAX 16` keeps the
selector at 16 until Task 6. `sim_dbg_step()` takes steps 0-63 and returns
`note | velocity << 8 | flags << 16 | cond << 24`; `sim_dbg_kit_step()` takes
0-63; `Device.stepData()` returns `cond`.

- [ ] Tests (`test_storage`): `v7_round_trip` (content in steps 0-63, hidden
  steps beyond the length, kit words at 63); `v7_rejections` (length 65,
  `track_length` 65, a non-zero `cond` (until Task 10), `ratchet` 1, `micro` 1,
  flag bit 4, kit bit 24, AC 2, three kit tracks, a bad CRC);
  `v6_fixture_migrates` and the v4/v5 ones (steps 16-63 zero, `cond` 0, lengths
  and kit words as the README); `torn_v7_save_leaves_nothing` (v6 fixture at the
  old offset, a save cut after the erase: neither v7 nor v6 loads);
  `save_erase_span` (42 sectors from 0x1C0000); `type_change_keeps_everything`
  (Review Focus 3, in `test_pattern` or `test_ui`: 64 notes, 64 kit words and
  64 non-zero conds set in memory survive SYNTH → KIT → SYNTH through
  `seq_set_track_config()`). On `__arm__` only: `sizeof(step_t) == 6`,
  `sizeof(pattern_t) == 5196`, `offsetof(pattern_t, kit_steps) == 3148`.
- [ ] `device_check.js`: the header at `sim_flash_store()` has version 7; the
  save stall ends before the artist's 2.6 s; `test/fixtures/v6.bin` written at
  `sim_flash_store() + 128 KiB` loads with LOAD and step 1 of A1 track 1 is
  note 48.
- [ ] Implement.
- [ ] Run everything; record RAM (expect about 190,000 bytes) and the v7 sizes
  in `HARDWARE_STATUS.md`; commit `feat: 64-step clips in memory, save format v7`.

## Phase 2 — long clips

### Task 5: the engine plays long clips and counts passes

**Files:** Modify `include/sequencer.h`, `src/sequencer.c`, `src/song.c` (if
the reset lives there), `sim/core/sim_main.c`, `sim/core/sim_api.h`,
`test/test_playback.c`, `test/test_song.c`, `ARCHITECTURE.md` (Playback
Rules).

**Interfaces:** `transport_t` gains `uint32_t track_pass[NUM_TRACKS]` and
`uint32_t bar_count` (rules in the spec, "Counters"). `int sim_dbg_pass(int track)`.

- [ ] Tests: `long_clip_plays_64` (notes on steps 1, 17, 33, 49 sound on the
  first step of bars 1-4); `pass_counts_wraps` (length 12: pass 3 after three
  wraps); `pass_resets_on_launch`; `pass_kept_on_relaunch_same_location`;
  `partial_join_is_pass_0` (BEAT launch at global step 4 of a 64-step clip:
  starts at step 5, pass 0 until its wrap); `shorten_below_playhead_counts_wrap`;
  `song_long_clip_runs_on` (two entries of 2 bars with the same 64-step
  location: steps 1-64 continuously); `song_changed_track_pass_0`;
  `bar_count_monotonic_in_song`.
- [ ] Implement; run everything; commit `feat: long clips play; pass and bar counters`.

### Task 6: pages on the step buttons, length to 64

**Files:** Modify `src/ui.c`, `src/pattern.c`, `include/sequencer.h`,
`test/test_ui.c`, `test/test_pattern.c`, `README.md`, `docs/manual-ru.md`.

**Interfaces:** `void pattern_double_track(pattern_t *p, uint8_t track)` and
`void pattern_halve_track(pattern_t *p, uint8_t track)` (spec "Length").
In `ui.c`: `ui.page_pin[NUM_TRACKS]` (`PAGE_FOLLOW` 0xFF or 0-3),
`static uint8_t shown_page(uint8_t track, uint8_t location, const transport_snapshot_t *snap)`,
a page frozen while a step pressed on the screen is down, and the absolute step
recorded per press (`ui.step_abs[STEPS_PER_PAGE]` beside `ui.step_cell`).
`ui.edit_step` is 0-63. `UI_LENGTH_MAX` goes.

- [ ] Tests (`test_ui`): `value_short_cycles_pages` (64 steps: FOLLOW, PAGE 1/4
  ... PAGE 4/4, FOLLOW notices); `value_short_on_16_steps` (`PAGE 1/1`);
  `page_cycle_stops_at_partial_page` and `partial_page_buttons_inert` (length
  40: three pages; on page 3 buttons 9-16 dark and a tap changes nothing —
  Review Focus 4); `follow_shows_playhead_page` (playhead at step 38: LEDs show
  steps 33-48); `hold_freezes_page`; `press_binds_absolute_step` (page 2 pinned,
  tap button 1: step 17 toggles); `shorten_below_held_page` (Review Focus 1);
  `session_shows_pinned_or_followed_page`; `track_press_opens_first_step_of_page`;
  `midi_key_and_pad_key_write_absolute_steps`; `selector_length_to_64`;
  `selector_halve_and_double` (16 → 32 copies steps 1-16 into 17-32 with conds
  and kit words; 32 → 16 keeps 17-32; 17 halves to 8; 40 doubles to 64 with
  steps 41-64 copies of 1-24); `copy_clip_copies_64_steps_and_conds`.
- [ ] Implement; update `README.md` and the manual's control tables.
- [ ] Run everything; commit `feat: pages on the step buttons; clip length 1-64`.

### Task 7: pages on the OLED

**Files:** Modify `src/display.c`, `include/sequencer.h` (display API),
`src/ui.c` (callers), `test/test_ui.c`.

**Interfaces:**
`typedef struct { uint8_t count, shown, playing; bool follow, blink; } page_view_t;`
(`playing` 0xFF when the cell does not play). `display_draw_pattern_view()`,
`display_draw_kit_lane_view()`, `display_draw_session_view()`,
`display_draw_step_edit()` and `display_draw_kit_step_edit()` take a
`const page_view_t *` and draw the shown page's 16 steps with absolute step
numbers.

- [ ] Tests (render to the fake OLED): the strip has `count` cells, the shown
  one solid, the playing one present on one blink phase and absent on the other,
  FOLLOW and pinned drawn differently; the roll and the kit strip of page 2 show
  steps 17-32; a held step 17 reads `S17`.
- [ ] Implement; run everything; commit `feat: page strip and paged clip views on the OLED`.

### Task 8: the pad overview

**Files:** Modify `include/led_frame.h`, `src/led_frame.c`, `src/ui.c`,
`test/test_led_frame.c`, `test/test_ui.c`, `ARCHITECTURE.md` ("Pads as a
keyboard" gains the routing order), `docs/manual-ru.md`.

**Interfaces:** `led_state_t` gains `bool overview; uint8_t ov_track, ov_length,
ov_shown_page, ov_playhead; uint64_t ov_active, ov_strong;` (playhead 0xFF =
none; `ov_strong` = accent, or a kit accent or flam). `led_frame_build()` draws
the overview when `overview && !keys`.

- [ ] Tests (`test_led_frame`): pad (column 1, row 1) is step 10; beyond the
  length off (length 40: rows 5-7 off — Review Focus 4); empty steps dim hue,
  dim white on the shown page; active half; strong full; playhead white full;
  `keys` wins over `overview`; SCENE, MUTE, PLAY and BEAT unchanged.
- [ ] Tests (`test_ui`): `overview_press_pins_page`;
  `overview_press_in_step_editor_selects_step` (and beyond the length does
  nothing); `overview_never_launches_or_copies` (a pad held 600 ms then another
  pad of the track in the Pattern view: no queue, no copy, no `pad_seen`);
  `keyboard_precedes_overview`; `pads_still_launch_in_selector_and_session`.
- [ ] Implement; run everything; commit `feat: the pads show the clip in the Pattern view`.

### Task 9: the simulator plays a long clip

**Files:** Modify `sim/build.sh`, `sim/check.sh`, `sim/web/app.js`,
`sim/web/artist.js`, `sim/test/artist_check.js`, `sim/README.md`.

**Interfaces:** feature bit 8 (`grep -q pattern_double_track include/sequencer.h`);
score `long` (env `LONG=1`, implies the `pads` score's options); artist option
`long`.

- [ ] In the `long` score: one song's LEAD row 3 clip is doubled to 32 with
  button 15, page 2 is opened with VALUE short and steps 17-32 are edited with
  held steps and pad keys; one overview pad press opens page 2 of another clip.
  The model keeps lengths and steps up to 64.
- [ ] `artist_check.js` (`long` only): `verifyPatterns` checks each clip up to
  its length; the 32-step clip's notes 17-32 are on the wire in the second bar
  of each of its passes. The other scores keep checking 16 steps.
- [ ] Run `sim/check.sh` (five scores); commit `sim: the long score programs and plays a 32-step clip`.

## Phase 3 — conditions

### Task 10: conditions in the engine

**Files:** Create `include/condition.h`, `test/test_condition.c`. Modify
`include/sequencer.h`, `src/sequencer.c`, `src/flash_storage.c`,
`test/CMakeLists.txt`, `test/test_playback.c`, `test/test_storage.c`,
`ARCHITECTURE.md` (a Conditions section).

**Interfaces:** `condition.h`, pure, `static inline` (so the timer gets them in
RAM): the `COND_*` codes of the spec; `bool cond_valid(uint8_t code)`;
`uint32_t cond_hash(uint32_t seed, uint8_t track, uint8_t location, uint32_t pass, uint8_t step)`;
`typedef struct { uint32_t pass; uint32_t hash; bool fill; bool last; } cond_ctx_t;`
`bool cond_eval(uint8_t code, const cond_ctx_t *ctx)`.
`sequencer.h`: `typedef enum { FILL_OFF, FILL_ON, FILL_EVERY_2, FILL_EVERY_4, FILL_EVERY_8, FILL_EVERY_16, FILL_MODE_COUNT } fill_mode_t;`
`void seq_set_fill_pedal(bool down); void seq_set_fill_mode(fill_mode_t mode); fill_mode_t seq_get_fill_mode(void);`
`transport_snapshot_t` gains `bool fill_active`;
`void sequencer_set_seed(uint32_t seed)` (tests; Start otherwise takes the
timer).

- [ ] Tests (`test_condition`): validity of 0x00, 0x01, 0x63, 0x70-0x75, 0x88
  (1:2), 0x89 (2:2), 0xBF (8:8) and invalidity of 0x64, 0x6F, 0x76, 0x80, 0x8A
  (3:2), 0xC0, 0xFF; `cond_eval` for each kind; `cond_hash` is a pure function
  and, with seed 1 over 10,000 inputs, X = 50 plays 4,700-5,300 times.
- [ ] Tests (`test_playback`): `cond_1_2_alternates`; `cond_1st_first_pass_only`
  and again after a relaunch of another location; `cond_fill_pedal`;
  `cond_fill_every_4` (bars 4, 8, 12); `cond_fill_on`; `cond_lst_track_loop`;
  `cond_lst_bar_misaligned` (length 12 under BAR: each LST step plays once more
  before the change); `cond_lst_song` (entry end, a 0-repeat entry skipped,
  LOOP wrap, STOP end); `cond_lst_queue_cancel_and_requeue` (Review Focus 5);
  `cond_length_1` (Review Focus 2); `cond_false_is_rest_with_slide` (step 2
  slides into a false step 3: Note Off at step 3, step 4 retriggers);
  `cond_kit_step_all_lanes`; `cond_probability_fixed_seed` (expected plays
  computed with `cond_hash` and seed 1234); `cond_muted_track_keeps_counting`.
- [ ] Tests (`test_storage`): the validator accepts every valid code and
  rejects the invalid ones above.
- [ ] Implement; check with `arm-none-eabi-nm` that the ISR's new code is in RAM
  and that no `__aeabi_uidiv` call was added to it.
- [ ] Run everything; commit `feat: trig conditions and FILL in the engine`.

### Task 11: FILL from the pedal and the selector

**Files:** Modify `include/midi_in.h`, `src/midi_in.c`, `src/ui.c`,
`src/display.c`, `test/test_midi_in.c`, `test/test_ui.c`, `README.md`,
`docs/manual-ru.md`.

**Interfaces:** `midi_in_kind_t` gains `MIDI_IN_SUSTAIN` (`velocity` = the CC
value). Pattern selector button 16 calls `seq_set_fill_mode()`.

- [ ] Tests (`test_midi_in`): `B0 40 7F` gives SUSTAIN 127 on any channel, with
  running status; CC 65 gives nothing; All Notes Off unchanged.
- [ ] Tests (`test_ui`): `pedal_down_and_up`; the pedal goes up on an input gap,
  All Notes Off, a panic, a save and a load; `selector_16_cycles_fill_mode`
  (OFF → ON → EVERY 2 → 4 → 8 → 16 → OFF, with notices); the transport symbol is
  inverted while `fill_active`; the selector shows the mode.
- [ ] Implement; run everything; commit `feat: FILL from a sustain pedal and the selector`.

### Task 12: editing conditions on the panel

**Files:** Modify `src/ui.c`, `src/display.c`, `include/condition.h`
(`const char *cond_name(uint8_t code, char out[8])`), `test/test_ui.c`,
`README.md`, `docs/manual-ru.md`.

**Interfaces:** `ui.value_layer` (`VALUE_LAYER_VEL`, `VALUE_LAYER_COND`); the
49-item list `static const uint8_t cond_menu[]` of the spec; a code not in the
list moves to the list's first item on the first detent.

- [ ] Tests: `hold_value_short_switches_to_cond` (hold step 5, VALUE short,
  VALUE +1: cond 0x5A; +7 more: 0x70); `cond_layer_ends_with_the_hold`;
  `cond_on_several_held_steps`; `cond_turn_activates_step`;
  `track_still_turns_note_in_cond_layer`; step editor:
  `tap_edited_step_switches_layer`, `tap_other_step_only_selects`,
  `hold_edited_step_is_keyboard`, `long_press_edited_step_toggles`; kit editor:
  `kit_cursor_reaches_cond_stop` (after RC; VALUE sets the step's cond; LEDs
  show the page's conditioned steps); `session_long_press_unchanged`; screens
  show `COND 1:4` and which value VALUE turns.
- [ ] Implement; run everything; commit `feat: edit trig conditions from held steps and the step editors`.

### Task 13: the simulator plays conditions and fills

**Files:** Modify `sim/core/sim_main.c`, `sim/core/sim_api.h`,
`sim/web/device.js`, `sim/web/artist.js`, `sim/test/artist_check.js`,
`sim/README.md`.

**Interfaces:** `int sim_dbg_fill(void)`; the `long` score sends CC64 bytes on
the MIDI input the way the `keys` score sends notes.

- [ ] In the `long` score: hats with FILL on their last two steps, a lead step
  on 1:2, a crash on 1ST, the pedal held through one bar of the set, FILL mode
  EVERY 8 set once and back to OFF.
- [ ] Checks: conds in memory; on the wire the 1:2 step in every other pass
  only, the FILL hats only in the pedal bar and the auto bars, the crash only in
  the first pass after its launch.
- [ ] Run `sim/check.sh`; commit `sim: the long score uses conditions and the FILL pedal`.

## Phase 4 — wrap-up

### Task 14: manual, status and bench list

**Files:** Modify `docs/manual-ru.md`, `README.md`, `ARCHITECTURE.md`,
`HARDWARE_STATUS.md`, `sim/README.md`.

- [ ] Manual: pages, the overview, length buttons 11-16, conditions and FILL,
  the pedal; "after updating, LOAD before the first SAVE"; older firmware cannot
  read a v7 save.
- [ ] `HARDWARE_STATUS.md`: final RAM and flash figures; bench items: ISR time
  and Clock interval with direct reads and conditions (GPIO 7), erase time of
  168 KiB on the real chip, a real pedal through the MIDI input.
- [ ] Check every doc claim against the code; commit `docs: long clips and conditions`.
