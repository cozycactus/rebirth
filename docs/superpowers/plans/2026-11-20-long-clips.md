# Clips up to 512 Steps, Clip Ends and Trig Conditions — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** clips of 1-512 steps on all 64 tracks, shown a bar of 16 at a time,
with loop / stop / go-to ends after N plays, per-step trig conditions and FILL,
in save format v8.

**Architecture:** the dense `pattern_t` step arrays give way to a pool of
2,048 64-byte blocks; a clip's root names its first bar or a table of its 32
bars. The timer reads one step word per track and step under `seq_lock`; the
prepared rows go. v8 is a byte-defined image of the used bars; v7 and v4-v6
migrate through frozen types.

**Tech Stack:** C11, Pico SDK 2.2.0 (RP2040, RP2350), host tests with CMake and
CTest, the WebAssembly simulator (`sim/`), Node for the simulator checks.

**Spec:** `docs/superpowers/specs/2026-11-20-long-clips-design.md`

## Global Constraints

- Base: `master` at f8a8737 or later; fetch and merge `origin/master` before each phase.
- `MAX_CLIP_STEPS` 512, `STEPS_PER_BAR` 16, `CLIP_BARS` 32, `POOL_BLOCKS` 2048 on every build, `BLOCK_NONE` 0xFFFF, `ROOT_TABLE` 0x8000.
- `step_t` is 4 bytes {note, velocity, flags, cond}, flags bits 4-7 zero; a kit word's condition is in bits 24-31.
- `block_t` 64 bytes, `clip_t` 8 bytes, `location_t` 458 bytes on `__arm__`; asserted.
- Condition codes, the list of 49 panel items, `mix32` and its golden values exactly as the spec's "Conditions".
- Gate pulses `(pct * 24 * 5243) >> 19`; no division, no new flash callee on the timer path (relative ELF check, Task 4).
- Positions and lengths are `uint16_t`; "none" is 0xFFFF.
- "Page" means eight tracks; the 16 steps on the step buttons are a "bar" in code, UI text and docs.
- Save v8 byte layout exactly as the spec's table; the fixed part is 51,980 bytes; the CRC is v7's `crc32_update` over bytes 256 to the payload end.
- RAM of the RP2040 Release build (`arm-none-eabi-size -A`, `.data` + `.bss`) at most 236,000 bytes after every task; record it in the task's commit message.
- Host tests: `(cd test && cmake --preset host-tests-debug && cmake --build --preset host-tests-debug && ctest --preset host-tests-debug)`, also configured with `-DCMAKE_C_FLAGS=-fshort-enums` as CI does.
- Firmware: `cmake --preset firmware-release && cmake --build --preset firmware-release`, and `pico2-release`, `revb-rp2354b`.
- Simulator: `sim/build.sh web && sim/check.sh` (needs `sim/emsdk`; in a worktree, link the main checkout's and remove the link after). Every score this firmware takes stays green after every task.
- No board exists: nothing is measured on hardware; do not add bench steps.

## Review Focus

1. **An operation one block short** (double, copy, a new bar, a load) changes nothing at all: lengths, roots, tables, the free count and the playing clip are as before, and the OLED says `POOL FULL`. Tests in Tasks 2, 6 and 5.
2. **A clip shortened below a held step's bar or below the playhead**: the held step keeps its absolute index, nothing reads beyond the length or step 511, the view returns to a bar within the length on release, the timer wraps. Tests in Tasks 6 and 7.
3. **An end action and a due queue at the same boundary**: the queue applies, the end action is dropped, at most one hop happens, `pass` is 0 for the new clip. Test in Task 12.
4. **A v7 save with `track_length` 0 and hidden steps 13-16 of a 12-step clip** migrates note for note: default length resolved, hidden steps stored, LOOP, plays 1. Test in Task 5.
5. **A type change on a 512-step clip** (SYNTH → KIT → SYNTH, track 1) keeps all 32 bars of both layers and their conditions in their own places, neither gating the other. Test in Task 14.

---

## Phase 0 — fixtures

### Task 0: saves written by today's code, as fixtures

**Files:** Create `test/tools/make_fixtures.c`, `test/fixtures/v4.bin`,
`v5.bin`, `v6.bin`, `v7-arm.bin`, `test/fixtures/README.md`. Modify
`test/CMakeLists.txt` (a target `make_fixtures`, not run by CTest).

**Interfaces:** Produces the four images at their flash offsets' contents
(header page first) and `README.md` listing what each holds: per location the
non-empty tracks, lengths (including a `track_length` 0 and a 12-step clip with
hidden steps 13-16 set), kit words (v6, v7), the song, kit settings.

- [ ] Write `make_fixtures.c` from today's types and writers (`flash_storage.c`'s layout and CRC); it asserts `sizeof` 459, 972 and 4,108 for the v5, v6 and v7 records and refuses to run otherwise. Build it with `-fshort-enums` on the host: these records hold only 8- and 32-bit fields and one-byte enums, so the host lays them out as the RP2040 does.
- [ ] Generate the images; write `README.md` from the tool's printout.
- [ ] Test: the existing `test_storage` loads each image through today's loader and gets the README's contents (`fixtures_v4_v5_v6_v7_load_today`).
- [ ] Run the host tests; commit `test: saves v4-v7 written by today's code as fixtures`.

## Phase 1 — the pool, behaviour unchanged

Lengths stay 1-16 through this phase (`UI_LENGTH_MAX` 16 in `ui.c`); every
score stays green.

### Task 1: constants, frozen types, 16-bit positions

**Files:** Modify `include/sequencer.h`, `src/flash_storage.c`, `src/sequencer.c`,
`src/ui.c`, `src/display.c`, `include/session.h`, `src/session.c`,
`sim/core/sim_main.c`, `test/test_storage.c`, `test/test_playback.c`.

**Interfaces:** Produces `MAX_CLIP_STEPS`, `STEPS_PER_BAR`, `CLIP_BARS`,
`POOL_BLOCKS`, `BLOCK_NONE`, `ROOT_TABLE` in `sequencer.h` (`MAX_CLIP_STEPS`
still unused by the store); `step_v3_t`, `track_config_v7_t` and a `uint8_t`
end mode inside `pattern_v5_t`, `pattern_v6_t`, `old_song_t`; frozen
`pattern_v7_t`, `song_v7_t` (copies of today's `pattern_t`, `song_t`) in
`flash_storage.c`; `uint16_t current_track_step[]`, snapshot `track_step[]`,
`ui.edit_step`, and every display function's step and length arguments as
`uint16_t`, with 0xFFFF for "none" (`STEPS_PER_PATTERN` as none goes).

- [ ] Test `legacy_types_frozen` (`__arm__` only): `sizeof(step_v3_t)` 3, `track_config_v7_t` 7, `pattern_v7_t` 4,108, `song_v7_t` 16,652.
- [ ] Test `fixtures_still_load` over Task 0's images.
- [ ] Test `playhead_none_is_ffff`: a stopped track's snapshot step is 0xFFFF.
- [ ] Implement; replace every `STEPS_PER_PATTERN` by `STEPS_PER_BAR` or the literal 16 of a frozen type; run the host tests, the three firmware builds and `sim/check.sh`; commit `refactor: frozen legacy types, 16-bit positions`.

### Task 2: the block pool and clip store

**Files:** Create `include/clip.h`, `src/clip.c`, `test/test_clip.c`. Modify
`CMakeLists.txt`, `test/CMakeLists.txt`.

**Interfaces:** Produces, in `clip.h`, the spec's `step_t` (4 bytes),
`block_t`, `clip_end_t`, `clip_t`, `location_t`, and a store
`typedef struct { location_t locations[32]; clip_t clips[32][NUM_TRACKS]; uint16_t kit_root[32][KIT_TRACK_SLOTS]; block_t pool[POOL_BLOCKS]; uint16_t free_head, free_count; } clip_store_t;`
with:

- `void clip_store_init(clip_store_t *s)` — every clip empty, length 16, LOOP, plays 1, target = next location; all blocks free.
- `step_t clip_get_step(const clip_store_t *s, uint8_t loc, uint8_t track, uint16_t step)`
- `uint32_t clip_get_kit(const clip_store_t *s, uint8_t loc, uint8_t slot, uint16_t step)`
- `bool clip_set_step(clip_store_t *s, uint8_t loc, uint8_t track, uint16_t step, step_t v)` and `bool clip_set_kit(...)` — false (nothing changed) when a block is needed and none is free.
- `bool clip_set_length(clip_store_t *s, uint8_t loc, uint8_t track, uint16_t len)` (1-512, never allocates)
- `bool clip_double(clip_store_t *s, uint8_t loc, uint8_t track)`, `void clip_halve(...)`, `bool clip_copy(clip_store_t *s, uint8_t track, uint8_t from, uint8_t to)`, `void clip_clear(...)`
- `uint16_t clip_blocks_needed_double(...)`, `uint16_t clip_pool_free(const clip_store_t *s)`
- `bool clip_has_notes(const clip_store_t *s, uint8_t loc, uint8_t track)` (an active step, or a kit lane hit, within the length)

Allocation and freeing are internal; operations needing blocks take them all
first and publish last (the publish is a plain write here; Task 3 wraps it in
`seq_lock`).

- [ ] Tests: `sizes_on_arm` (4, 64, 8, 458; store 162,624 + 4); `fresh_store_is_empty` (free 2,048); `first_note_allocates_bar0_root`; `note_in_bar3_promotes_to_table` (2 blocks used; bar 0 kept); `zeroing_a_bar_frees_it`; `table_demotes_when_only_bar0_left`; `kit_layer_independent` (track 1 note and kit roots apart); `double_17_to_34_copies_both_layers_and_conds`; `halve_keeps_hidden_steps`; `copy_takes_end_plays_target`; `pool_exhaustion_changes_nothing` (fill the pool, then `clip_set_step` in a new bar, `clip_double`, `clip_copy`: each false; snapshot of the store byte-equal before and after — Review Focus 1); `free_list_survives_random_ops` (10,000 seeded operations, the free count always equals 2,048 minus reachable blocks, no block reachable twice).
- [ ] Implement in `src/clip.c` (no Pico SDK calls; pure C, built for host and firmware).
- [ ] Run the host tests; commit `feat: block pool and clip store`.

### Task 3: the firmware on the clip store

**Files:** Modify `include/sequencer.h`, `src/shared_state.c`, `src/pattern.c`,
`src/sequencer.c`, `src/ui.c`, `src/session.c`, `src/kit.c`, `src/song.c`,
`src/flash_storage.c`, `src/display.c`, `sim/core/sim_main.c`,
`sim/core/sim_api.h`, `test/*.c`.

**Interfaces:** Consumes Task 2. `sequencer_state_t` holds a `clip_store_t
store` instead of `patterns[]`; `pattern_t` goes; `pattern.c` keeps its
public names as thin wrappers where callers remain (`pattern_get_track_length()`
returns `uint16_t`). Every write path takes `seq_lock` around the publish
only. The v7 save stays: it serializes the store into `pattern_v7_t` records
page by page (bar 0 of each layer, `track_length` = length; lengths are ≤ 16 in
this phase). Prepared rows stay, filled from `clip_get_step()`.
`sim_dbg_step()` and `sim_dbg_kit_step()` read through the store.

- [ ] Test `v7_round_trip_through_store` (save, clear, load: every field equal).
- [ ] Test `ram_budget` is not a test: record `arm-none-eabi-size` in the commit message; it must be ≤ 236,000 (the prepared rows are still here).
- [ ] Implement; run the host tests (both enum sizes), the firmware builds and `sim/check.sh`; commit `refactor: the sequencer stores clips in the block pool`.

### Task 4: the timer reads steps directly

**Files:** Modify `src/sequencer.c`, `include/sequencer.h`,
`test/test_playback.c`, `ARCHITECTURE.md`. Create `tools/isr_callees.sh`.

**Interfaces:** Consumes Task 3. The prepared-row machinery goes (the spec's
list). New, RAM-resident: `static bool read_track_step(uint8_t track, track_read_t *out)`
filling `typedef struct { track_config_t cfg; uint16_t length, pos; uint8_t location, kind; step_t step; uint32_t kit; kit_config_t kitcfg; } track_read_t;`
inside one `seq_lock` section; `static uint8_t gate_pulses_for(uint8_t pct)`.
`tools/isr_callees.sh <elf>` prints the flash functions and division helpers
reachable from `sequencer_timer_isr` (from `arm-none-eabi-objdump -d`).

- [ ] Before any change: run `tools/isr_callees.sh` on the f8a8737 build; commit its output as `test/isr_callees.baseline`.
- [ ] Tests: `gate_formula_matches_division` (10-100); `direct_read_plays_as_before` (the existing playback tests unchanged and green); `shortened_between_sections_never_plays_hidden_step`; `kind_change_releases_old_kind`; `edit_heard_next_occurrence`.
- [ ] Implement; check the ELF: no line in `isr_callees.sh` output that is not in the baseline; RAM ≤ 236,000 (expect about 232,000).
- [ ] Run everything; commit `perf: the timer reads one step per track; prepared rows removed`.

### Task 5: save format v8 and migration

**Files:** Modify `src/flash_storage.c`, `include/flash_storage.h`,
`test/test_storage.c`, `sim/test/device_check.js`, `README.md`.

**Interfaces:** Consumes Tasks 1-4. `#define VERSION 8u`; `flash_load_t` gains
`FLASH_LOAD_POOL_FULL` (RAM untouched; the UI shows `POOL FULL`). Pure
functions for the tests: `size_t v8_serialize(const sequencer_state_t *, void (*sink)(const uint8_t *page, void *ctx), void *ctx)` (whole pages,
header last), `bool v8_validate(const uint8_t *image, size_t len, uint16_t *blocks_needed)`,
`bool v8_load(const uint8_t *image, sequencer_state_t *)`,
`bool v7_migrate(const uint8_t *image, sequencer_state_t *)`,
`bool legacy_migrate(const uint8_t *image, sequencer_state_t *)` (v4-v6).
The probe order is v8, v7, the eight-track slot.

- [ ] Test `v8_layout_matches_spec`: a hand-written image (one location name, one 20-step clip with bars 0 and 1, one kit bar on track 1, a two-entry song) at the spec's offsets loads; serializing it back gives the same bytes; the fixed part is 51,980 bytes.
- [ ] Tests: `v8_round_trip`; one test per validation rule (length 0 and 513, end 3, plays 0 and 100, target 32, reserved byte, mask count mismatch, note 128, flag bit 4, unknown condition, AC 2, kit mask on track 9, three kit tracks, bad CRC) each refused with RAM unchanged.
- [ ] Tests: `v7_arm_fixture_migrates` (Task 0's `v7-arm.bin`; runs where `sizeof(pattern_v7_t) == 4108`, i.e. the `-fshort-enums` host build CI runs; elsewhere skipped with a message); `v7_host_image_migrates` (an image built here with `pattern_v7_t`); `v4_v5_v6_fixtures_migrate`; `v7_default_length_and_hidden_steps` (Review Focus 4); `v7_needing_2049_blocks_refused` (`FLASH_LOAD_POOL_FULL`, RAM unchanged).
- [ ] `device_check.js`: a v8 round trip, the save time under 2.5 s, a v7 image in the simulator's layout and the v6 fixture migrating.
- [ ] Implement; README: the update path (LOAD before SAVE), downgrade falls back to the eight-track save.
- [ ] Run everything; commit `feat: save format v8 with migration from v7 and v4-v6`.

## Phase 2 — long clips

### Task 6: lengths up to 512 in the engine

**Files:** Modify `src/sequencer.c`, `include/sequencer.h`, `src/ui.c`,
`src/song.c`, `test/test_playback.c`, `test/test_song.c`, `test/test_ui.c`.

**Interfaces:** Consumes Tasks 2-5. `UI_LENGTH_MAX` 512 (selector 11/12).
Transport gains `uint32_t pass[NUM_TRACKS]`, `uint8_t pass_mod[NUM_TRACKS][7]`
(B = 2..8), `uint32_t bar_count`; `sequencer_set_seed(uint32_t)` (test hook;
the seed itself is used from Task 14). `locate_locked()`/`locate_song()`
compute in `uint32_t` and store `uint16_t`.

- [ ] Tests: `clip_200_steps_plays_through` (steps 1-200 on the wire in order, then step 1); `phase_join_long_loop_on_bar_and_beat`; `song_entries_run_long_clip_on` (64-step clip across four one-bar entries keeping its location); `shorten_below_playhead_wraps` (Review Focus 2); `pass_counts_wraps_and_resets_on_change`; `pass_mod_tracks_pass`; `bar_count_from_start`; `spp_places_long_clips` (Pattern and Song mode); `selector_length_buttons_1_to_512`; `double_refused_when_one_block_short` (Review Focus 1, through the UI path: notice `POOL FULL`).
- [ ] Implement; run everything; commit `feat: clips of up to 512 steps in the engine`.

### Task 7: bars on the step buttons

**Files:** Modify `src/ui.c`, `include/session.h`, `src/session.c`,
`test/test_ui.c`, `test/test_session.c`.

**Interfaces:** Consumes Task 6. `ui.bar_pin[NUM_TRACKS]` (0xFF = FOLLOW, else
0-31); `static uint8_t shown_bar(session_focus_t cell)`; the hold bar per
screen; `grid_enc_target`/`grid_enc_long_done` gain `uint8_t ..._bar[2]`;
`ui.step_bar[16]` binds each press's bar.

- [ ] Tests: `follow_shows_playhead_bar`; `value_short_cycles_bars_then_follow` (`BAR 2/32`, `FOLLOW`; `BAR 1/1` on 16 steps); `hold_pins_bar_until_release`; `press_binds_absolute_step` (button 3 on bar 5 is step 67 after the view moves); `encoder_targets_keep_their_bar`; `beyond_length_per_screen_as_today` (length 40: bar 3 buttons 9-16 dark; Session ignores, Pattern view edits hidden); `held_step_survives_shortening` (Review Focus 2); `editor_opens_on_shown_bar`.
- [ ] Implement; run everything; commit `feat: the step buttons show one bar of a long clip`.

### Task 8: the OLED bar strip

**Files:** Modify `include/sequencer.h` (display API), `src/display.c`,
`src/ui.c`, `test/test_ui.c`.

**Interfaces:** `typedef struct { uint8_t count, shown, playing; bool follow, blink; } bar_view_t;`
(`playing` 0xFF when not playing). `display_draw_pattern_view()`,
`display_draw_kit_lane_view()`, `display_draw_session_view()`,
`display_draw_step_edit()`, `display_draw_kit_step_edit()` take a
`const bar_view_t *` and draw the shown bar's 16 steps with absolute numbers.

- [ ] Tests (fake OLED): 32 cells of 4 px for 512 steps, 1 for 16; shown solid; playing present on one blink phase only; FOLLOW and pinned marks differ; bar 2 of the roll shows steps 17-32; a held step 300 reads `S300`.
- [ ] Implement; run everything; commit `feat: bar strip and absolute step numbers on the OLED`.

### Task 9: the pad overview

**Files:** Modify `include/led_frame.h`, `src/led_frame.c`, `src/ui.c`,
`test/test_led_frame.c`, `test/test_ui.c`, `ARCHITECTURE.md`.

**Interfaces:** `led_state_t` gains `bool overview, ov_bars; uint8_t ov_track, ov_count, ov_shown, ov_playing, ov_edit; bool ov_edit_lit; uint64_t ov_active, ov_strong;`
(`ov_bars`: one pad per bar; `ov_count` cells in use; 0xFF = none).
`led_frame_build()` draws it when `overview && !keys`. `handle_pad_press()`
routes keyboard, overview (Pattern view, both step editors: no
`find_copy_source()`, no `pad_seen`), launch or copy.

- [ ] Tests (`test_led_frame`): step mapping (pad column 1, row 1 = step 10; length 40: rows 5-7 off); bar mapping (length 200: 13 bars lit, pads 13-63 off; the playing bar white full; a bar with an accent full); `ov_edit` blinks; `keys` wins.
- [ ] Tests (`test_ui`): `overview_press_pins_bar`; `overview_in_note_editor_picks_step` (64-step clip: pad of step 20 → `edit_step` 19); `overview_in_note_editor_picks_bar_keeps_button` (300-step clip, editing step 35 (button 3), pad of bar 10 → step 163); `overview_in_kit_editor_keeps_lane`; `overview_never_launches_or_copies` (Pattern view and both editors); `keyboard_precedes_overview`; `copy_still_works_in_selector_and_track_config`; `pads_still_launch_in_session`.
- [ ] Implement; run everything; commit `feat: the pads show the clip in the Pattern view and the step editors`.

### Task 10: the CLIP screen

**Files:** Modify `src/ui.c`, `src/display.c`, `include/sequencer.h` (display
API), `test/test_ui.c`, `README.md`.

**Interfaces:** `UI_CLIP_SETTINGS` mode; `ui.clip_row`; rows enum `CLIP_ROW_LENGTH, CLIP_ROW_END, CLIP_ROW_PLAYS, CLIP_ROW_TARGET, CLIP_ROW_FILL`;
`display_draw_clip_settings(const clip_t *, uint8_t track, uint8_t location, uint8_t row, uint16_t pool_used, uint8_t fill_mode)`.
In this task END, PLAYS, TARGET and FILL rows are drawn and stored but END
has no effect on playback yet (Task 12) and FILL none (Task 15).

- [ ] Tests: `selector_16_opens_clip_screen` (for the selector's track and location); `length_row_accelerates`; `button_15_halves_16_doubles` (notice `LEN 32`; `POOL FULL` when short); `plays_and_target_inactive_while_loop`; `next_sets_target_to_next_location` (A8 → B1, D8 → A1); `track_press_next_track_value_press_back`; `pool_use_shown`.
- [ ] Implement; run everything; commit `feat: CLIP screen on selector button 16`.

### Task 11: the simulator plays long clips

**Files:** Modify `sim/build.sh`, `sim/check.sh`, `sim/core/sim_api.h`,
`sim/core/sim_main.c`, `sim/web/app.js`, `sim/web/artist.js`,
`sim/test/artist_check.js`, `sim/README.md`.

**Interfaces:** feature bit 32 (`grep -q UI_CLIP_SETTINGS src/ui.c` — true
from Task 10, so no firmware before the CLIP screen is offered the score);
`sim_dbg_pool_free()`; score `long` (env `LONG=1`, on top
of `pads`); the artist's `Model` holds 512 steps and the clip ends.

- [ ] Score part 1: one song's lead row 3 clip doubled to 32 on the CLIP screen; bar 2 opened with VALUE short, steps 17-32 edited with held steps and pad keys; one overview press opening bar 2 of another clip; the note editor on the doubled clip, step 20 picked with a pad; a 128-step clip made with three doublings.
- [ ] Checks: memory up to each clip's length; steps 17-32 in the second bar of each pass; the 128-step clip's eight bars in order. The other scores keep reading 16 steps.
- [ ] Run `sim/check.sh`; commit `sim: the long score programs and plays 32- and 128-step clips`.

## Phase 3 — clip ends

### Task 12: end actions

**Files:** Modify `src/sequencer.c`, `include/sequencer.h`, `src/session.c`,
`src/ui.c`, `src/led_frame.c`, `test/test_playback.c`, `test/test_song.c`,
`test/test_session.c`, `test/test_ui.c`.

**Interfaces:** Consumes Tasks 6 and 10. Transport `track_mask_t idle`;
`transport_snapshot_t.idle`; the clip-end rule at the wrap, after the queue
check and before the next step is read. `sim_dbg_clip_end()`, `sim_dbg_pass()`,
`sim_dbg_idle()`.

- [ ] Tests: `stop_after_three_plays_goes_idle` (silent from the fourth pass, notes released, kit voices released, flam cancelled); `next_after_one_play_starts_target_at_step1` (no quantization wait, `pass` 0); `next_to_empty_target_plays_silence`; `next_to_itself_restarts`; `chain_a_b_c_a_in_time`; `lowering_plays_ends_at_next_wrap`; `queue_beats_end_action_same_boundary` (Review Focus 3); `scene_beats_end_action`; `mute_keeps_counting`; `once_clip_starts_at_step1_under_bar_and_beat`; `loop_clip_keeps_phase_join`; `idle_track_launch_point_is_next_step`; `own_pad_relaunches_idle_clip`; `song_mode_end_makes_idle_until_location_changes`; `song_mode_next_never_changes_location`; `start_clears_idle_and_pass`; `spp_clears_idle`; `copy_carries_end_fields`; `session_shows_idle_as_present_not_playing`.
- [ ] Implement; run everything; commit `feat: clips end by looping, stopping or starting another clip after N plays`.

### Task 13: the simulator plays clip ends

**Files:** Modify `sim/web/artist.js`, `sim/test/artist_check.js`.

- [ ] Score part 2: a fill clip set to NEXT after 1 play back to its loop, the loop set to NEXT after 3 plays to the fill; a crash clip set to STOP after 1 play.
- [ ] Checks: the fill every fourth bar for the whole stretch; the crash once, then silence on that track until the next launch.
- [ ] Run `sim/check.sh`; commit `sim: the long score chains clips and ends a crash`.

## Phase 4 — conditions

### Task 14: conditions in the engine

**Files:** Create `include/condition.h`, `test/test_condition.c`. Modify
`include/kit.h`, `src/kit.c`, `src/sequencer.c`, `src/song.c`,
`src/flash_storage.c`, `test/CMakeLists.txt`, `test/test_playback.c`,
`test/test_song.c`, `test/test_kit.c`.

**Interfaces:** `condition.h`: the code constants, `static inline uint32_t mix32(uint32_t)`,
`static inline bool cond_plays(uint8_t code, const cond_ctx_t *c)` with
`typedef struct { uint32_t pass; const uint8_t *pass_mod; bool fill, last; uint32_t seed; uint8_t track, location; uint16_t step; } cond_ctx_t;`,
`bool cond_code_valid(uint8_t)`. `kit.h`: `kit_word_cond(uint32_t)`,
`kit_word_with_cond(uint32_t, uint8_t)`; every lane mask uses `0x00FFFFFF`.
Transport `uint8_t song_change_bars[NUM_TRACKS]`, computed in
`advance_song_position()` and by song edits. The validator (Task 5) accepts
known codes from here on.

- [ ] Tests (`test_condition`): every code; the six golden hash values and the 4,962-of-10,000 count; `a_b_by_pass_mod`; `fill_every_4_bars_4_8_12`; `fill_phase_is_global_not_clip`.
- [ ] Tests (`test_playback`): `false_condition_is_rest_after_slide`, `_after_tie`, `_after_flam`; `lst_under_bar_beat_step_track_loop`; `lst_at_end_action` (STOP after 2: LST steps play in the second pass only); `lst_song_mode_32_bar_horizon` (a 512-step clip whose location changes after 32 bars; a run of 0-repeat entries); `lst_queue_to_playing_location_is_no_change`; `lst_never_under_step`; `kit_condition_in_word_bits`; `type_change_keeps_both_layers_and_conds` (Review Focus 5).
- [ ] Implement; check the ELF against the baseline; run everything; commit `feat: per-step trig conditions`.

### Task 15: FILL from the pedal and the FILL mode

**Files:** Modify `src/midi_in.c`, `include/midi_in.h`, `src/midi_out.c`,
`src/sequencer.c`, `src/ui.c`, `src/display.c`, `test/test_midi_in.c`,
`test/test_playback.c`, `test/test_ui.c`, `sim/core/sim_api.h`, `sim/core/sim_main.c`.

**Interfaces:** `midi_in_event_t` gains `MIDI_IN_PEDAL` (down/up);
`bool sequencer_fill_active(void)`; `fill_mode` (OFF, ON, EVERY 2/4/8/16) set
from the CLIP screen's FILL row; the receive interrupt reads `uart_get_hw()->dr`
and drops a byte with OE, BE, PE or FE, counting it in `midi_in_dropped()`;
`sim_dbg_fill()`.

- [ ] Tests: `cc64_down_at_64_up_at_31_hysteresis`; `pedal_any_channel_not_passed_through`; `gap_lifts_pedal_and_notifies_fill_off`; `pedal_held_through_gap_needs_new_press`; `uart_error_byte_dropped_counts_as_gap`; `fill_row_cycles_modes`; `transport_symbol_inverted_while_fill`.
- [ ] Implement; run everything; commit `feat: FILL from a sustain pedal and the FILL mode`.

### Task 16: editing conditions on the panel

**Files:** Modify `src/ui.c`, `src/display.c`, `include/condition.h`
(`const char *cond_name(uint8_t code, char out[8])`), `test/test_ui.c`,
`README.md`.

**Interfaces:** `ui.value_layer` (`VALUE_LAYER_VEL`, `VALUE_LAYER_COND`); the
49-item list as `static const uint8_t cond_menu[]`.

- [ ] Tests: `hold_value_short_switches_to_cond` (hold step 5, VALUE short, VALUE +1: 0x5A; +7: 0x70), in both release orders; `cond_layer_ends_with_the_hold`; `cond_on_several_held_steps`; `cond_turn_activates_step`; `track_still_turns_note_in_cond_layer`; note editor: `tap_edited_step_switches_layer`, `tap_with_a_turn_does_not_switch`, `tap_after_a_key_does_not_switch`, `tap_after_an_overview_pad_does_not_switch` (DRUM track: hold the edited step, pad of step 20, release: step 20 edited, on velocity), `tap_other_step_only_selects`, `opening_and_leaving_reset_layer`, `hold_edited_step_is_keyboard`, `long_press_edited_step_toggles`; kit editor: `kit_cursor_reaches_cond_stop` (after RC; VALUE sets bits 24-31 and leaves `step_t.cond`; LEDs show the bar's conditioned steps); `session_long_press_unchanged`; screens show `COND 1:4`.
- [ ] Implement; run everything; commit `feat: edit trig conditions from held steps and the step editors`.

### Task 17: the simulator plays conditions and fills

**Files:** Modify `sim/web/artist.js`, `sim/test/artist_check.js`, `sim/README.md`.

- [ ] Score part 3: closing hats on FILL on the last two steps of their clip; a lead step on 1:2; a crash on 1ST; the pedal pressed for one bar (CC64 as the `keys` score sends notes); FILL mode EVERY 8 set once on the CLIP screen.
- [ ] Checks: 1:2 steps every other pass only; FILL steps only in the pedal bar and the auto-fill bars; 1ST only in the first pass after the launch. Probability is not checked on the wire.
- [ ] Run `sim/check.sh`; commit `sim: the long score plays conditions and fills`.

## Phase 5 — documentation

### Task 18: documentation

**Files:** Modify `README.md`, `ARCHITECTURE.md`, `HARDWARE_STATUS.md`.

- [ ] `README.md`: bars, the pad overview, the CLIP screen, clip ends, conditions, FILL, the pool and `POOL FULL`, the update path and downgrade.
- [ ] `ARCHITECTURE.md`: the clip store and pool, the direct step read, the lock rules for allocating and freeing, v8.
- [ ] `HARDWARE_STATUS.md`: RAM after the work (`arm-none-eabi-size`), the ISR callee check, the existing bench items unchanged.
- [ ] The pull request lists the changes for `docs/manual-ru.md` (not in git) for the owner.
- [ ] Commit `docs: long clips, clip ends and conditions`.
