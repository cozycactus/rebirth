# Clips up to 64 Steps and Trig Conditions — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** clips of 1-64 steps shown a page of 16 at a time, with an overview on
the pads, and a trig condition on every step, in save format v7.

**Architecture:** the location keeps today's two step arrays, 64 long, with a
6-byte step (condition plus two reserved bytes). The save buffer and the
prepared playback rows go; the timer reads the step it plays under `seq_lock`.
The save region grows to the last 256 KiB; v4-v6 saves still load from the old
offset through layout-independent readers. Conditions are evaluated in the
timer from per-track pass counters, a global FILL state and a hash-based draw.

**Tech Stack:** C11, Pico SDK 2.2.0, RP2040; host tests with CMake/CTest and fake
peripherals; the WebAssembly simulator (`sim/`) with Node checks.

**Spec:** `docs/superpowers/specs/2026-11-17-long-clips-conditions-design.md`.
Read it first; this plan does not repeat its tables (condition codes, legacy
offsets, gesture map, golden hash values).

## Global Constraints

- Branch from `origin/master` (fetch first; it moves several times a day). Base
  at writing: bcf6871.
- Tests first, seen failing, then the code. One commit per task. Nothing outside
  the files a task names, except docs the task's change makes wrong.
- Every task ends green on:
  - the host suite: `(cd test && cmake --preset host-tests-debug && cmake --build --preset host-tests-debug && ctest --preset host-tests-debug)`;
  - the same suite with `-fshort-enums` and the sanitizers, as CI runs it:
    `cmake -S test -B test/build/short -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang '-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -fshort-enums -fsanitize=address,undefined -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' && cmake --build test/build/short && ctest --test-dir test/build/short --output-on-failure`;
  - the firmware build: `cmake --preset firmware-debug && cmake --build --preset firmware-debug`;
  - the simulator: `sim/build.sh native && sim/build.sh web && sim/check.sh`, every
    score the firmware takes (today `fw2`, `keys`, `pads`; `long` from Task 9).
    The existing scores never change and never fail.
- Step record: `note, velocity, flags, cond, ratchet, micro`, 6 bytes; `ratchet`
  and `micro` are 0 in v7. `MAX_CLIP_STEPS` 64, `STEPS_PER_PAGE` 16,
  `STEPS_PER_BAR` 16, `CLIP_PAGES` 4, `DEFAULT_CLIP_STEPS` 16; legacy code uses
  the literal 16.
- Location 5,196 bytes on the RP2040 (5,268 in host-enum builds); v7 payload
  169,472 bytes on the RP2040; region the last 256 KiB (0x1C0000); legacy header
  at 0x1E0000; erase 42 sectors, the old header's sector first.
- Condition codes, legacy offsets and hash exactly as the spec; 0 = ALWAYS;
  unknown codes reject a save.
- The timer's new path (step read, length and kind tests, condition evaluation,
  LST distance) runs from RAM, copies fields one by one, and calls no
  `__aeabi_uidiv`, `__aeabi_uidivmod` or `__aeabi_idiv*`. Checked in the ELF
  (Debug and Release) whenever a task touches that path.
- No gesture beyond the spec's gesture map.
- After each task that changes RAM use, record `arm-none-eabi-size -A` of the
  firmware ELF in `HARDWARE_STATUS.md`.
- `docs/manual-ru.md` is not in git: tasks update `README.md`; Task 14 lists the
  manual's changes in the pull request for the owner.

## Review Focus

1. **A clip shortened below the page of a held step.** The held step keeps its
   absolute index (edits go to the hidden step), nothing reads beyond 64, and on
   release the view returns to a page within the length. Test in Task 6.
2. **A clip of length 1.** Every boundary is a wrap: `pass` grows every step, 1:2
   plays every other step, and LST never fires (a TRACK LOOP queue applies
   before the next step is evaluated). Test in Task 10.
3. **A type change on a 64-step clip** (SYNTH → KIT → SYNTH) keeps the notes,
   the kit words and the conditions of all 64 steps. Test in Task 4.
4. **A last page that is partial** (length 40: page 3 has 8 steps). Buttons
   9-16 there are dark; in the Session view and on a kit they do nothing, in the
   SYNTH/DRUM Pattern view they edit hidden steps as today; the page cycle stops
   at page 3; the overview rows beyond step 40 are off. Tests in Tasks 6 and 8.
5. **A queue cancelled and set again within one pass**, and a queue to the
   playing location. LST steps after the cancel are silent, after the new queue
   they play again, a queue to the playing location never makes them play;
   nothing already played is replayed. Test in Task 10.

---

## Phase 0 — fixtures

### Task 0: legacy images as committed fixtures

**Files:** Create `test/tools/make_fixtures.c`, `test/fixtures/v4.bin`,
`test/fixtures/v5.bin`, `test/fixtures/v6.bin`, `test/fixtures/README.md`.
Modify `test/CMakeLists.txt` (a `REBIRTH_FIXTURES` path define for the tests),
`test/test_storage.c`.

**Interfaces:** Produces the three files (each the full old 128 KiB region as
the RP2040 firmware writes it, header at offset 0) and `README.md` listing
exactly what they hold and the command that made them.

- [ ] Write `make_fixtures.c` against today's `flash_storage.c` and fakes, with
  `_Static_assert`s that `sizeof(pattern_v5_t) == 459`, `sizeof(pattern_t) ==
  972` and `sizeof(song_t) == 2316` (so it only builds with `-fshort-enums`). It
  fills RAM with known content and writes v6 through `flash_storage_save()`, v5
  and v4 with the legacy writer the storage test has (`write_legacy_image`, moved
  or copied into the tool). Content: location A1 track 1 SYNTH steps 1, 5, 9
  notes 48, 50, 52 velocities 100, 90, 80, step 5 accent and slide, length 12;
  A1 track 2 KIT (v6 only) word `0x000105` at step 1 and kit settings with
  `flam_pulses` 7; B3 track 5 DRUM, length 16, every step active; track 7 stored
  mute at C2; a song of 3 entries (repeats 2, 0, 4) ending in STOP, named
  `FIXTURE`.
- [ ] Build it once with `clang -fshort-enums` from today's sources, run it,
  commit the outputs and the README.
- [ ] Add `test_legacy_fixtures()` to `test_storage.c`: each file copied to the
  fake flash's legacy region loads, and RAM holds the README's content. Today's
  struct-casting loader reads them only where the host layout is the RP2040's,
  so it is skipped with a printed note unless `sizeof(pattern_v5_t) == 459`
  (Task 4 removes the skip).
- [ ] Run the suites; commit `test: v4-v6 save images as fixtures`.

## Phase 1 — no behaviour changes

### Task 1: split STEPS_PER_PATTERN, freeze the legacy types, add the offset readers

**Files:** Modify `include/sequencer.h`, `include/kit.h`, the `src/*.c` that use
`STEPS_PER_PATTERN` (display, flash_storage, kit, pattern, sequencer, ui),
`sim/core/sim_main.c`, `test/test_storage.c`, other tests that use the constant.

**Interfaces:** Produces in `sequencer.h` `MAX_CLIP_STEPS` (still 16 in this
task), `STEPS_PER_PAGE`, `CLIP_PAGES`, `STEPS_PER_BAR`, `DEFAULT_CLIP_STEPS`;
`STEPS_PER_PATTERN` no longer exists. In `flash_storage.c`: frozen
`legacy_step_t`, `legacy_track_config_t`, `pattern_v5_t`, `pattern_v6_t`,
`song_v6_t` used only for `__arm__` assertions of the offsets; the offset
constants of the spec's legacy table; and
`static bool legacy_pattern_read(const uint8_t *record, uint8_t version, pattern_t *out)`,
`static bool legacy_song_read(const uint8_t *bytes, song_t *out)`,
`static bool legacy_kits_read(const uint8_t *bytes, kit_config_t out[NUM_TRACKS])`,
each range-checking raw bytes and returning false for a record the legacy rules
reject. **The load path keeps using today's code in this task**: today's native
v6 writer and reader still round-trip in host-enum builds, and Task 4 switches
the legacy path over when the writer moves to v7.

- [ ] Tests: `legacy_pattern_read_offsets`, `legacy_song_read_offsets` and
  `legacy_kits_read_offsets` on records built byte by byte (not from a struct),
  each giving the expected `pattern_t`, `song_t` or kit settings in every build;
  rejections for an out-of-range end mode, a `track_length` of 17, a KIT type in
  a v5 record and flag bits 4-7. Run: they fail.
- [ ] Replace each use of `STEPS_PER_PATTERN` by its meaning: clip storage and
  length clamps → `MAX_CLIP_STEPS`; step buttons, LED masks, OLED step strips →
  `STEPS_PER_PAGE`; launch quantization, phase join, Song bar, the Session
  screen's bar step → `STEPS_PER_BAR`; `pattern_init` length →
  `DEFAULT_CLIP_STEPS`; legacy records → the literal 16 via the frozen types.
- [ ] Implement the readers.
- [ ] Run everything; commit `refactor: step constants by meaning; frozen legacy readers`.

### Task 2: the timer reads the step it plays

**Files:** Modify `src/sequencer.c`, `include/sequencer.h` (comments),
`test/test_playback.c`, `ARCHITECTURE.md` ("Prepared Patterns" becomes "Reading
a step"; the kit section's prepared-row bullet).

**Interfaces:** Removes `prepared_patterns`, `prepared_playback`,
`prepared_revision`, `playback_revision`, `playback_pattern`,
`fill_prepared_track()`, `load_prepared_track()`. Produces, internal to
`sequencer.c`:
`typedef struct { uint8_t kind, length; track_config_t config; step_t step; uint32_t kit_word; kit_config_t kit; } track_read_t;`,
`static void read_track_step(uint8_t track, uint8_t location, uint8_t step, track_read_t *out)`
(one `seq_lock` section, in RAM, length and kind computed from fields, fields
copied one by one) and `static inline uint8_t gate_pulses_for(uint8_t pct)` =
`(pct * 24 * 5243) >> 19`. `sequencer_refresh_track_playback(track, location)`
keeps its signature and now only updates the clip mask bit.

- [ ] Tests: `gate_pulses_exact` (equals `pct * 24 / 100` for 10-100);
  `edit_heard_at_next_occurrence` (a note changed at step 3 while step 2 plays
  sounds at step 3 of this pass); `kind_change_releases_first` (SYNTH note held
  by tie, track switched to KIT: Note Off precedes the kit hits at the next
  boundary); `stored_mute_silences`; `length_read_live` (shortening to 4 at
  step 6 wraps at the next boundary, as today); `clip_mask_follows_refresh`.
  The existing playback tests pass unchanged, except ones that inspect prepared
  rows, which move to the public behaviour.
- [ ] Implement.
- [ ] Check the ELF (`arm-none-eabi-objdump -d` from the ISR through every new
  callee: RAM addresses only, no division helpers) in Debug and Release.
- [ ] Run everything; record RAM; commit `perf: timer reads the step it plays; prepared rows removed`.

### Task 3: save straight from RAM

**Files:** Modify `src/flash_storage.c`, `test/test_storage.c`,
`ARCHITECTURE.md` (the Storage paragraph on the copy buffer).

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
`test/fakes/fake_hardware.c` and `test/fakes/hardware/flash.h` (an erase cut),
`sim/core/hal.c`, `sim/core/sim_main.c`, `sim/core/sim_api.h`,
`sim/web/device.js`, `sim/test/device_check.js`, `test/test_storage.c`,
`test/test_pattern.c`, `ARCHITECTURE.md` (Storage), `HARDWARE_STATUS.md`,
`README.md` (update note).

**Interfaces:** `step_t` and `pattern_t` as the spec; `MAX_CLIP_STEPS` 64.
`STORE_RESERVED` 256 KiB, `LEGACY_STORE_OFFSET` (2 MiB − 128 KiB), `VERSION` 7,
`STORE_ERASE` the spec's expression. `flash_storage_error_t` and
`flash_storage_last_error()` as the spec; `bool flash_storage_layout_ok(void)`
(target: `__flash_binary_end` below the region; host: true), checked at boot
(`ui_init()` shows `FLASH LAYOUT`) and before every save. The legacy load path
now uses Task 1's readers only. `pattern_copy_track()` copies 64 steps of both
kinds; `pattern_shift()` and `pattern_shift_track()` rotate within the clip's
length. In `ui.c` a temporary `UI_LENGTH_MAX 16` keeps the selector at 16 until
Task 6. `fake_flash_erase_limit` (sectors) cuts an erase in the fakes.
`sim_dbg_step()` takes steps 0-63 and returns
`note | velocity << 8 | flags << 16 | cond << 24`; `sim_dbg_kit_step()` takes
0-63; `Device.stepData()` returns `cond`; `sim/core/hal.c` `STORE_BYTES` 256 KiB.

- [ ] Tests (`test_storage`): `v7_round_trip` (content in steps 0-63, hidden
  steps beyond the length, kit words at step 64); `v7_rejections` (length 65,
  `track_length` 65, a non-zero `cond` (until Task 10), `ratchet` 1, `micro` 1,
  flag bit 4, kit bit 24, AC 2, three kit tracks, a bad CRC);
  `legacy_fixtures` without the skip, in every build (steps 17-64 zero, `cond`
  0, lengths, kit words, song and its STOP end as the README);
  `erase_order_and_span` (the 0x1E0000 sector first, then 42 sectors from
  0x1C0000); `torn_v7_save_restores_nothing` (v6 fixture at the old offset; the
  erase cut after 0, 1, 31, 32 and 41 sectors and the programming cut before the
  header: neither v7 nor v6 loads); `layout_error_refuses_save`; `pattern_shift`
  of a 16-step clip unchanged with 64 slots; `type_change_keeps_everything`
  (Review Focus 3: 64 notes, 64 kit words and 64 non-zero conds set in memory
  survive SYNTH → KIT → SYNTH through `seq_set_track_config()`). On `__arm__`:
  `sizeof(step_t) == 6`, `sizeof(pattern_t) == 5196`,
  `offsetof(pattern_t, kit_steps) == 3148`, `sizeof(song_t) == 2316`.
- [ ] `device_check.js`: the header at `sim_flash_store()` has version 7; the
  save stall (671 pages, 42 sectors) ends before 2.5 s; `test/fixtures/v6.bin`
  written at `sim_flash_store() + 128 KiB` loads with LOAD and A1 track 1 step 1
  is note 48.
- [ ] Implement.
- [ ] Run everything; record RAM (expect about 190,000 bytes) and the v7 sizes
  in `HARDWARE_STATUS.md`; commit `feat: 64-step clips in memory, save format v7`.

## Phase 2 — long clips

### Task 5: the engine plays long clips and counts passes

**Files:** Modify `include/sequencer.h`, `src/sequencer.c`, `src/song.c` (if a
reset lives there), `sim/core/sim_main.c`, `sim/core/sim_api.h`,
`test/test_playback.c`, `test/test_song.c`, `ARCHITECTURE.md` (Playback Rules).

**Interfaces:** `transport_t` gains `uint32_t track_pass[NUM_TRACKS]`,
`uint8_t pass_mod[NUM_TRACKS][7]` (index B − 2) and `uint32_t bar_count`, with
the spec's rules ("Counters"). `int sim_dbg_pass(int track)`.

- [ ] Tests: `long_clip_plays_64` (notes on steps 1, 17, 33, 49 sound on the
  first step of bars 1-4); `pass_counts_wraps` (length 12: pass 3 after three
  wraps, `pass_mod` 1, 0, 3 for B = 2, 3, 4); `pass_resets_on_launch`;
  `pass_kept_on_relaunch_same_location`; `partial_join_is_pass_0` (BEAT launch
  at global step 4 of a 64-step clip: starts at step 5, pass 0 until its wrap);
  `shorten_below_playhead_counts_wrap`; `song_long_clip_runs_on` (two entries of
  2 bars with the same 64-step location: steps 1-64 continuously);
  `song_changed_track_pass_0`; `bar_count_completed_bars` (0 in bar 1, 1 in
  bar 2) and `bar_count_monotonic_in_song`.
- [ ] Implement; ELF check; run everything; commit `feat: long clips play; pass and bar counters`.

### Task 6: pages on the step buttons, length to 64

**Files:** Modify `src/ui.c`, `src/pattern.c`, `include/sequencer.h`,
`test/test_ui.c`, `test/test_pattern.c`, `README.md`.

**Interfaces:** `void pattern_double_track(pattern_t *p, uint8_t track)` and
`void pattern_halve_track(pattern_t *p, uint8_t track)` (spec "Length"). In
`ui.c`: `ui.page_pin[NUM_TRACKS]` (`PAGE_FOLLOW` 0xFF or 0-3);
`static uint8_t shown_page(uint8_t track, uint8_t location, const transport_snapshot_t *snap)`;
one frozen `ui.hold_page` per screen while a step pressed on it or an encoder
that joined a hold is down; per press, the bound cell and absolute step
(`ui.step_cell[]` for every cell kind, plus `ui.step_abs[STEPS_PER_PAGE]`);
`ui.grid_enc_target[]` and `ui.grid_enc_long_done[]` as `uint64_t` masks of
absolute steps with their cell. `ui.edit_step` is 0-63. `UI_LENGTH_MAX` goes.

- [ ] Tests (`test_ui`): `value_short_cycles_pages` (64 steps: FOLLOW, PAGE 1/4
  ... PAGE 4/4, FOLLOW notices); `value_short_on_16_steps` (`PAGE 1/1`);
  `partial_page` (length 40: three pages; on page 3 buttons 9-16 dark; a tap
  there does nothing in the Session view and on a kit and toggles hidden step
  41-48 in the SYNTH Pattern view — Review Focus 4); `follow_shows_playhead_page`
  (playhead at step 38: LEDs show steps 33-48 with the playhead on button 6);
  `hold_freezes_page`; `press_binds_absolute_step` (page 2 pinned, tap button 1:
  step 17 toggles); `long_press_opens_absolute_step` (button 1 on page 2 in the
  Session view: the editor opens on step 17); `encoder_target_survives_follow`
  (hold button 5 on page 1, press TRACK, release the step, let FOLLOW move to
  page 2, press button 5 again, release TRACK late: only step 5 gets the long
  press, in both release orders); `shorten_below_held_page` (Review Focus 1);
  `session_shows_pinned_or_followed_page`; `track_press_opens_first_step_of_page`;
  `midi_key_and_pad_key_write_absolute_steps`; `selector_length_to_64`;
  `selector_halve_and_double` (16 → 32 copies steps 1-16 into 17-32 with conds
  and kit words; 32 → 16 keeps 17-32; 17 halves to 8; 40 doubles to 64 with
  steps 41-64 copies of 1-24); `copy_clip_copies_64_steps_and_conds`.
- [ ] Implement; update `README.md`'s control tables.
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
keyboard" gains the routing order), `README.md`.

**Interfaces:** `led_state_t` gains `bool overview; uint8_t ov_track, ov_length,
ov_shown_page, ov_playhead; uint64_t ov_active, ov_strong;` (playhead 0xFF =
none; `ov_strong` = an accent, or a kit accent or flam on the shown lane).
`led_frame_build()` draws the overview when `overview && !keys`.
`handle_pad_press()` routes: keyboard, then the overview (Pattern view and step
editors: no `find_copy_source()`, no `pad_seen`), then launch or copy.

- [ ] Tests (`test_led_frame`): pad (column 1, row 1) is step 10; beyond the
  length off (length 40: rows 5-7 off — Review Focus 4); empty steps dim hue,
  dim white on the shown page; active half; strong full; playhead white full; a
  kit lane's values; `keys` wins over `overview`; SCENE, MUTE, PLAY and BEAT
  unchanged.
- [ ] Tests (`test_ui`): `overview_press_pins_page`;
  `overview_press_in_step_editor_selects_step` (beyond the length nothing);
  `overview_never_launches_or_copies` (a pad held 600 ms then another pad of the
  track in the Pattern view: no queue, no copy, no `pad_seen`);
  `keyboard_precedes_overview`; `copy_still_works_in_selector_and_track_config`;
  `pads_still_launch_in_selector_and_session`.
- [ ] Implement; run everything; commit `feat: the pads show the clip in the Pattern view`.

### Task 9: the simulator plays a long clip

**Files:** Modify `sim/build.sh`, `sim/check.sh`, `sim/web/app.js`,
`sim/web/artist.js`, `sim/test/artist_check.js`, `sim/README.md`.

**Interfaces:** feature bit 8 (`grep -q pattern_double_track include/sequencer.h`);
score `long` (env `LONG=1`, with the `pads` score's options); artist option
`long`; the artist's `Model` (steps, kit words, `copy()`) holds 64 steps.

- [ ] In the `long` score: one song's LEAD row 3 clip is doubled to 32 with
  button 15, page 2 is opened with VALUE short and steps 17-32 are edited with
  held steps and pad keys; one overview pad press opens page 2 of another clip;
  a pad-hold copy of the 32-step clip in the Session view.
- [ ] `artist_check.js` (`long` only): `verifyPatterns` checks each clip up to
  its length; the 32-step clip's steps 17-32 are on the wire in the second bar
  of each of its passes; the copy matches its source in all 32 steps. The other
  scores keep checking 16 steps.
- [ ] Run `sim/check.sh` (every score); commit `sim: the long score programs and plays a 32-step clip`.

## Phase 3 — conditions

### Task 10: conditions in the engine

**Files:** Create `include/condition.h`, `test/test_condition.c`. Modify
`include/sequencer.h`, `src/sequencer.c`, `src/flash_storage.c`,
`test/CMakeLists.txt`, `test/test_playback.c`, `test/test_storage.c`,
`ARCHITECTURE.md` (a Conditions section).

**Interfaces:** `condition.h`, pure: the `COND_*` codes of the spec;
`bool cond_valid(uint8_t code)`; `uint32_t cond_mix32(uint32_t x)`;
`uint32_t cond_hash(uint32_t seed, uint8_t track, uint8_t location, uint32_t pass, uint8_t step)`;
`typedef struct { uint32_t pass; const uint8_t *pass_mod; uint32_t hash; bool fill; bool last; } cond_ctx_t;`
`bool cond_eval(uint8_t code, const cond_ctx_t *ctx)`; the ISR's copies are
placed in RAM (`__not_in_flash_func` definitions in `sequencer.c` if the
inline ones are not inlined). `sequencer.h`:
`typedef enum { FILL_OFF, FILL_ON, FILL_EVERY_2, FILL_EVERY_4, FILL_EVERY_8, FILL_EVERY_16, FILL_MODE_COUNT } fill_mode_t;`
`void seq_set_fill_pedal(bool down); void seq_set_fill_mode(fill_mode_t mode); fill_mode_t seq_get_fill_mode(void);`
`transport_snapshot_t` gains `bool fill_active`;
`void sequencer_set_seed(uint32_t seed)` (tests; Start otherwise takes the
timer).

- [ ] Tests (`test_condition`): validity of 0x00, 0x01, 0x63, 0x70-0x75, 0x88
  (1:2), 0x89 (2:2), 0xBF (8:8) and invalidity of 0x64, 0x6F, 0x76, 0x80, 0x8A
  (3:2), 0xC0, 0xFF; `cond_eval` for each kind; `cond_hash` equal to the spec's
  six golden values (literals in the test, not recomputed); X = 50 over the
  spec's 10,000 inputs plays 4,962 times.
- [ ] Tests (`test_playback`): `cond_1_2_alternates`; `cond_1st_first_pass_only`
  and again after a launch of another location; `cond_fill_pedal`;
  `cond_fill_every_4` (bars 4, 8, 12); `cond_fill_on`; `cond_lst_track_loop`;
  `cond_lst_bar_misaligned` (length 12 under BAR: each LST step plays once more
  before the change); `cond_lst_step_quant_never`; `cond_lst_song` (entry end,
  a 0-repeat entry skipped, LOOP wrap, STOP end, ten 0-repeat entries in a row,
  the playing entry removed); `cond_lst_queue_cancel_requeue_and_same_location`
  (Review Focus 5); `cond_length_1` (Review Focus 2);
  `cond_false_after_slide`, `cond_false_after_tie` (Note Off and gate off at the
  false step, the next true step retriggers, also across a wrap);
  `cond_false_after_kit_flam` (voices released, no new flam);
  `cond_kit_step_all_lanes`; `cond_probability_fixed_seed` (seed 1234; plays
  predicted from the golden table's inputs); `cond_muted_track_keeps_counting`.
- [ ] Tests (`test_storage`): the validator accepts every valid code and
  rejects the invalid ones above.
- [ ] Implement; ELF check (RAM, no division helpers) in Debug and Release.
- [ ] Run everything; commit `feat: trig conditions and FILL in the engine`.

### Task 11: FILL from the pedal and the selector

**Files:** Modify `include/midi_in.h`, `src/midi_in.c`, `src/ui.c`,
`src/display.c`, `test/test_midi_in.c`, `test/test_ui.c`, `README.md`.

**Interfaces:** `midi_in_kind_t` gains `MIDI_IN_SUSTAIN` (`velocity` = the CC
value). Pattern selector button 16 calls `seq_set_fill_mode()`.

- [ ] Tests (`test_midi_in`): `B0 40 7F` gives SUSTAIN 127 on any channel, with
  running status; CC 65 gives nothing; All Notes Off unchanged.
- [ ] Tests (`test_ui`): `pedal_down_and_up`; the pedal is not passed through
  and writes no step even with a step held; it goes up on an input gap, All
  Notes Off, a panic, a save and a load, and stays through Stop and Start;
  `selector_16_cycles_fill_mode` (OFF → ON → EVERY 2 → 4 → 8 → 16 → OFF, with
  notices); the transport symbol is inverted while `fill_active`; the selector
  shows the mode.
- [ ] Implement; run everything; commit `feat: FILL from a sustain pedal and the selector`.

### Task 12: editing conditions on the panel

**Files:** Modify `src/ui.c`, `src/display.c`, `include/condition.h`
(`const char *cond_name(uint8_t code, char out[8])`), `test/test_ui.c`,
`README.md`.

**Interfaces:** `ui.value_layer` (`VALUE_LAYER_VEL`, `VALUE_LAYER_COND`); the
spec's 49-item list as `static const uint8_t cond_menu[]`; a code not in the
list moves to the list's first item on the first detent.

- [ ] Tests: `hold_value_short_switches_to_cond` (hold step 5, VALUE short,
  VALUE +1: cond 0x5A; +7 more: 0x70), in both release orders (step first,
  VALUE first: the layer switches, no page cycle, no tap);
  `cond_layer_ends_with_the_hold`; `cond_on_several_held_steps`;
  `cond_turn_activates_step`; `track_still_turns_note_in_cond_layer`. Step
  editor: `tap_edited_step_switches_layer`; `tap_with_a_turn_does_not_switch`;
  `tap_after_a_key_does_not_switch`; `tap_other_step_only_selects` (and resets
  to velocity); `opening_and_leaving_reset_layer`; `hold_edited_step_is_keyboard`;
  `long_press_edited_step_toggles`. Kit editor: `kit_cursor_reaches_cond_stop`
  (after RC; VALUE sets the step's cond; LEDs show the page's conditioned
  steps). `session_long_press_unchanged`; the screens show `COND 1:4` and which
  value VALUE turns.
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

### Task 14: docs, status and bench list

**Files:** Modify `README.md`, `ARCHITECTURE.md`, `HARDWARE_STATUS.md`,
`sim/README.md`.

- [ ] `README.md`: pages, the overview, length buttons 11-16, conditions and
  FILL, the pedal; "after updating, LOAD before the first SAVE"; older firmware
  cannot read a v7 save.
- [ ] The pull request lists the changes `docs/manual-ru.md` (not in git) needs.
- [ ] `HARDWARE_STATUS.md`: final RAM and flash figures; bench items: ISR time
  and Clock interval with direct reads and conditions (GPIO 7), with a song edit
  near the start of a full song, a clip copy and a doubling while playing; the
  erase time of 168 KiB on the real chip; a real pedal on the MIDI input.
- [ ] Check every doc claim against the code; commit `docs: long clips and conditions`.
