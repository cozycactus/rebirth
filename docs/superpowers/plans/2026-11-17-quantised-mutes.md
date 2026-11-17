# Quantised Mutes and Mute Groups — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** MUTE keys wait for the launch point of the launch quantisation (at once when stopped or under NEXT STEP), a second press cancels, and a MUTE held for half a second collects others into a group that changes together after its release.

**Architecture:** The engine keeps two masks, `mute_pending` and `mute_target`, written under the playback lock; the timer applies due changes at a step boundary before it releases muted tracks' notes. Core 0 decides and publishes a change in one critical section, then clears stored mutes. The UI tracks one held MUTE key and the group it collects; the LED frame shows waiting and collected keys in white.

**Tech Stack:** C11 firmware (RP2040), host tests with CMake/CTest (`test/`), simulator in JavaScript over the firmware compiled with Emscripten (`sim/`).

**Spec:** `docs/superpowers/specs/2026-11-17-quantised-mutes-design.md`. Read it first; this plan does not repeat its reasons.

## Global Constraints

- No save-format change: the new fields are never written to flash.
- Lock order is playback lock, then shared lock (`seq_lock`). Never call a function that takes `seq_lock` (`seq_get_track_mutes()`, `sequencer_refresh_track_playback()`) while holding it.
- Immediate means: `g_seq.transport.state != TRANSPORT_PLAYING || g_seq.launch_quant == LAUNCH_QUANT_STEP`.
- The group hold is `PAD_COPY_HOLD_MS` (500 ms), measured from the UI's handling of the held key's press.
- Waiting light: white at `LED_LEVEL_FULL` when `(pulse / (PPQN / 4)) % 2 == 1`, the track's normal light otherwise; group: white at `LED_LEVEL_FULL`, steady.
- Tests first, seen failing, then the code; the whole host suite passes before a task ends (`cd test && cmake --build --preset host-tests-debug && ctest --preset host-tests-debug`).
- Touch only the files a task names. One commit per task; messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never stage `sim/emsdk` (a local symlink).
- Docs and code comments in English, in the plain style of the existing files.

## Review Focus

1. A MUTE press whose decision section runs right after the timer applied a boundary: the change waits for the following boundary and is never lost (Task 1 test `test_press_after_boundary_waits_for_next`).
2. The held key released in the same UI pass as a member's press: the member is in the group that is applied (Task 3 test `test_mute_join_and_release_in_one_pass`).
3. The quantisation changed while a change waits (1 BAR to NEXT STEP): it lands on the next step (Task 1 test `test_waiting_mute_follows_quant_change`).
4. A group released after PLAY stopped the transport during the hold: it applies at once (Task 3 test `test_mute_group_released_after_stop_applies_now`).
5. The power budget scaling a frame full of white MUTE lights still keeps them white (equal channels) (Task 2 test `test_mute_white_survives_budget`).

---

### Task 1: The engine

**Files:**
- Modify: `include/sequencer.h`, `src/sequencer.c`, `src/song.c`
- Test: `test/test_playback.c`
- Docs: `ARCHITECTURE.md` (pulse order in "Kit Tracks", "Playback Rules", "Execution and Locking")

**Interfaces:**
- Produces, in `include/sequencer.h`:
  - `sequencer_state_t`: `volatile uint8_t mute_pending; volatile uint8_t mute_target;` beside `track_mutes`, commented "not saved; written under the playback lock".
  - `transport_snapshot_t`: `uint8_t mute_pending;`
  - `void transport_toggle_mutes(uint8_t mask);` — a MUTE press: per track in `mask`, a waiting change is cancelled; each track in `mask` with none gets a change to the opposite of its state (live or stored mute at the location it plays), at once when immediate, else waiting. Tracks outside `mask` untouched.
  - `void transport_set_mutes(uint8_t mask, uint8_t target);` — a group's release: each track in `mask` gets a change to its bit of `target` (1 = muted), replacing a waiting one; at once when immediate, else waiting.
  - `uint8_t transport_cancel_mutes(uint8_t mask, uint8_t *target);` — cancels the waiting changes in `mask`; returns the tracks that had one; when `target` is not NULL, `*target` gets their target bits.
  - `void transport_toggle_mutes_now(uint8_t mask);` — the selector: cancels waiting changes in `mask` and toggles those tracks at once, in every mode.
- Produces, in `src/song.c` (declared in `sequencer.h`):
  - `uint8_t seq_track_mutes_locked(void);` — the mute rule (`track_mutes` | stored mute of the location each track plays); caller holds `seq_lock`. `seq_get_track_mutes()` becomes lock + this + unlock.
  - `void seq_clear_stored_mutes(uint8_t mask);` — clears the stored mute of every location for the tracks in `mask`, one location per lock section, and refreshes the rows of each track that had one. Takes `seq_lock`: call with none held. `seq_toggle_track_mute()` uses it.

- [ ] **Step 1: Write the failing tests in `test/test_playback.c`**

Use the file's `reset()` (it sets TRACK LOOP; set the quantisation each test needs), `set_note`, `set_length`, `pulses`, `next_step`, `count_note_on`, `decode_midi`, and `fake_before_lock` for interleaving. "The bar" is pulse `16 * PULSES_PER_STEP`. Each test name and what it asserts:

- `test_mute_waits_for_the_bar`: BAR; track 0 has a note on every step; start, play 5 steps; `transport_toggle_mutes(1)`: `track_mutes == 0`, `mute_pending == 1`; play to the pulse before the bar: still 0 and step 15's note was sent; tick the bar's pulse: `track_mutes == 1`, `mute_pending == 0`, and no Note On for step 0 of bar 2.
- `test_unmute_lands_with_the_downbeat_note`: BAR; track 0 live-muted, notes on every step; toggle at step 5: no Note On until the bar; the bar's pulse sends step 0's Note On.
- `test_mute_cuts_sounding_note_on_the_boundary`: BAR; a tied note sounding across the bar; the Note Off goes out in the bar's pulse, not before.
- `test_mute_waits_for_the_beat`: BEAT; toggle at step 5: lands on the pulse of step 8, not 6 or 7.
- `test_mute_track_loop_lands_at_loop_end`: TRACK LOOP; track 0 length 12, track 1 length 16; toggle both at step 3: track 0 lands at global step 12, track 1 at 16.
- `test_mute_track_loop_after_shortening`: TRACK LOOP; length 16, toggle at step 9, shorten to 8 during step 9: lands on the next boundary (the shortening is the loop end, as for a queued pad).
- `test_mute_and_pad_same_loop_end`: TRACK LOOP; track 0 at location 0 length 12, queued to location 1 (length 16, note at step 0) and its unmute queued: at global step 12 location 1 plays and its step 0 Note On is sent; with a queued mute instead, no Note On.
- `test_mute_now_when_stopped`: stopped; toggle: `track_mutes` changes at once, `mute_pending == 0`.
- `test_mute_now_under_next_step`: STEP, playing; toggle: at once, nothing waits.
- `test_second_toggle_cancels`: BAR; toggle twice before the bar: `mute_pending == 0`, nothing changes at the bar.
- `test_toggle_after_landing_is_a_new_change`: BAR; toggle, pass the bar (muted), toggle: waits again, unmutes at the next bar.
- `test_toggles_in_one_call_land_on_one_pulse`: BAR; `transport_toggle_mutes(0x06)`: both bits change on the same pulse.
- `test_set_mutes_writes_targets`: BAR; track 2 live-muted, track 3 audible; `transport_set_mutes(0x0C, 0x04)`: at the bar track 2 stays muted, track 3 stays audible; then `transport_set_mutes(0x08, 0x08)` replaces nothing waiting and mutes 3 at the next bar; a `set` on a track with a waiting change replaces its target.
- `test_cancel_mutes_returns_waiting_and_targets`: two waiting changes with targets 1 and 0; `transport_cancel_mutes(mask, &t)` returns both bits and their targets; a second call returns 0; NULL target is accepted.
- `test_toggle_mutes_now_cancels_and_toggles`: BAR, playing; track 4 audible with a waiting mute; `transport_toggle_mutes_now(1u << 4)`: nothing waits and track 4 is muted at once (the toggle acts on the state now); a second call unmutes it at once.
- `test_waiting_mute_follows_quant_change`: BAR; toggle at step 2; `transport_set_launch_quant(LAUNCH_QUANT_STEP)`: lands on step 3's pulse.
- `test_press_after_boundary_waits_for_next`: BAR; `fake_before_lock` runs the timer through the bar just before the toggle takes the playback lock: the change waits for the next bar and lands there (never lost, never applied on the bar it missed).
- `test_stop_start_mode_panic_cancel_waiting_mutes`: for each of `transport_stop()`, `transport_start()` while playing, `transport_set_mode(PLAY_MODE_SONG)`, `sequencer_panic()`: a waiting change is gone (`mute_pending == 0`) and nothing changes on the next run's first bar.
- `test_empty_song_start_clears_waiting_mutes`: Song mode playing a song, a mute waits, `song_clear()`, `transport_start()` (returns early): `mute_pending == 0`.
- `test_song_end_boundary_lands_due_mute`: Song mode, one entry, repeats 1, end STOP; toggle during the bar under BAR: at the boundary the transport stops and `track_mutes` has the bit, `mute_pending == 0`.
- `test_song_mode_track_loop_uses_bar`: Song mode, TRACK LOOP, track 0 length 12: a toggle lands at global step 16, not 12.
- `test_queued_mute_releases_kit_voices`: track 1 a kit with voices sounding (gate 100 %); BAR; toggle: on the bar's pulse the voices are released (Note On velocity 0) and no kit hit is sent for that step.
- `test_stored_unmute_waits_silently`: track 2 has a stored mute at the location it plays and at location 5; BAR, playing; toggle: at once `track_mutes` has bit 2, no location has a stored mute for track 2, no Note On on its channel before the bar; at the bar step 0's Note On is sent.
- `test_cancelled_stored_unmute_stays_muted`: as above, toggled twice: `mute_pending == 0`, `track_mutes` bit 2 set, no stored mutes left; `seq_get_track_mutes()` has bit 2.
- `test_bar_inside_stored_refresh`: as `test_stored_unmute_waits_silently`, with `fake_before_lock` running the timer through the bar during the stored-mute clearing: the change landed at that bar (`track_mutes` bit clear, nothing waits), and from the next step on the track's notes are sent.
- `test_snapshot_reports_waiting_mutes`: `sequencer_snapshot()` has `mute_pending` equal to the waiting mask.
- `test_init_clears_mute_masks`: set both masks, `sequencer_init()`: both 0.

Register each in `main()` in the file's order.

- [ ] **Step 2: Build and run; the new tests fail to compile or fail**

Run: `cd test && cmake --build --preset host-tests-debug 2>&1 | tail -5`
Expected: errors for the undeclared names (or failures once declared).

- [ ] **Step 3: Implement**

`song.c`: `seq_track_mutes_locked()`, `seq_clear_stored_mutes()`, and `seq_toggle_track_mute()`/`seq_get_track_mutes()` rewritten on them with unchanged behaviour.

`sequencer.c`: one static helper serves the four calls. Its shape:

```c
typedef enum { MUTES_TOGGLE, MUTES_SET, MUTES_NOW } mutes_op_t;
// One decision under both locks, published at once; stored mutes cleared after.
static void change_mutes(uint8_t mask, uint8_t target, mutes_op_t op) {
    playback lock, then seq_lock;
    uint8_t muted = seq_track_mutes_locked();
    uint8_t change = mask;
    if (op == MUTES_TOGGLE) change &= ~g_seq.mute_pending;     // waiting ones are cancelled only
    g_seq.mute_pending &= ~mask;
    uint8_t want = (op == MUTES_SET ? target : (uint8_t)~muted) & change;
    uint8_t unmute = change & ~want;
    g_seq.track_mutes |= unmute & muted;   // a stored-muted track keeps silent by its live bit
    if (op == MUTES_NOW || immediate)  g_seq.track_mutes = (g_seq.track_mutes & ~change) | want;
    else { g_seq.mute_target = (g_seq.mute_target & ~change) | want; g_seq.mute_pending |= change; }
    unlock both;
    if (unmute) seq_clear_stored_mutes(unmute);
}
```

The timer: right after the MIDI Clock send and before `uint8_t track_mutes = g_seq.track_mutes;`, when `g_seq.mute_pending` and `t->pulse_count % PULSES_PER_STEP == 0`, call `static void apply_due_mutes(transport_t *t)`: quantisation from `g_seq.launch_quant` (TRACK LOOP read as BAR in Song mode); global step `t->pulse_count / PULSES_PER_STEP`; BAR `% 16 == 0`, BEAT `% 4 == 0`, STEP always; TRACK LOOP: `load_prepared_track(tr)` then due when `t->track_step_count[tr] >= clamp_pattern_steps(prepared_playback.track[tr].length)`. Write the due bits of `mute_target` into `track_mutes` in one `seq_lock` section and clear them from `mute_pending`.

Clear `mute_pending` in `stop_playback_outputs()`, in `transport_start()` where the queues are reset (before the empty-song return), and both masks in `sequencer_init()`. `sequencer_snapshot()` copies `mute_pending`.

ARCHITECTURE.md: the pulse order gains "0, on a step boundary, due MUTE changes"; Playback Rules describe waiting changes, cancel, immediate cases, the stored-mute conversion and which paths cancel; Execution and Locking says the timer writes `track_mutes` under the shared lock and Core 0 writes the two masks under the playback lock.

- [ ] **Step 4: Run the whole suite**

Run: `cd test && cmake --build --preset host-tests-debug && ctest --preset host-tests-debug`
Expected: `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add include/sequencer.h src/sequencer.c src/song.c test/test_playback.c ARCHITECTURE.md
git commit -m "Engine: MUTE changes wait for the launch point"
```

### Task 2: The panel accessor and the lights

**Files:**
- Modify: `include/panel_input.h`, `src/panel_input.c`, `include/led_frame.h`, `src/led_frame.c`
- Test: `test/test_panel_input.c`, `test/test_led_frame.c`

**Interfaces:**
- Produces: `bool panel_input_mute_down(uint8_t track);` — true while the MUTE key of `track` is reported down, false for `track >= 8`; same semantics and comment style as `panel_input_pad_down()`.
- Produces: `led_state_t` fields `uint8_t mute_pending;` (bit per track: a change waits) and `uint8_t mute_group;` (bit per track: collected in a group).

- [ ] **Step 1: Write the failing tests**

`test_panel_input.c`, mirroring the `test_pad_down_*` tests:
- `test_mute_down_follows_the_accepted_press`: false before, false while debouncing, true from the sample that queued the press, false after the release is queued; track 5's MUTE does not make track 2 down.
- `test_mute_down_is_only_mutes`: a pad, a SCENE key and PLAY down leave every `panel_input_mute_down()` false.
- `test_mute_down_refuses_arguments_out_of_range`: track 8 and 255 are false.
- `test_mute_down_is_false_after_a_resync`.

`test_led_frame.c`:
- `test_mute_group_is_steady_white`: for focused/unfocused, muted/audible, every pulse of a bar: `LED_MUTE(t)` is white at full.
- `test_mute_pending_flashes_white_on_odd_sixteenths`: pulses 0-23 show the light the track has without `mute_pending`; pulses 24-47 white at full; pulse `PPQN * 4 - 1` (the bar's last sixteenth) white.
- `test_mute_pending_shows_on_dark_track`: unfocused muted track (normally off) with `mute_pending`: white on odd sixteenths, off on even ones.
- `test_mute_pending_outranks_bank_blinks`: a track whose current and queued locations are outside the shown bank: with `mute_pending` the light follows only the sixteenth rule.
- `test_mute_group_outranks_pending`: both bits set: steady white.
- `test_mute_white_survives_budget`: all 8 MUTE lights white plus a full pad grid, after `led_frame_apply_budget()`: each MUTE light has equal non-zero channels.
- The existing tests stay unchanged and pass (both masks 0 reproduce today's frames).

- [ ] **Step 2: Run them; they fail**

Run: `cd test && cmake --build --preset host-tests-debug 2>&1 | tail -3; ctest --preset host-tests-debug -R "panel|led"`
Expected: build errors or failures for the new names.

- [ ] **Step 3: Implement** `panel_input_mute_down()` (the key map entry `PANEL_MUTE` with that track) and the two cases at the top of `mute_light()` (group, then pending), the normal light being today's `base`.

- [ ] **Step 4: Whole suite passes** (command as in Global Constraints).

- [ ] **Step 5: Commit**

```bash
git add include/panel_input.h src/panel_input.c include/led_frame.h src/led_frame.c test/test_panel_input.c test/test_led_frame.c
git commit -m "Panel: MUTE down state; lights for waiting and grouped mutes"
```

### Task 3: The UI — presses, groups, selector, lights

**Files:**
- Modify: `src/ui.c`, `src/session.c`
- Test: `test/test_ui.c` (and `test/test_session.c` only if a test there expects an instant `session_mute()` while playing)
- Docs: `README.md` (Features "Track mutes" line, Live Controls MUTE entry, the selector line, a short "Mute groups" entry)

**Interfaces:**
- Consumes: Task 1's four calls and `transport_snapshot_t.mute_pending`; Task 2's `panel_input_mute_down()` and `led_state_t.mute_pending/mute_group`.
- Produces (static in `ui.c`, names free): the held key (track or none), its press time (`to_ms_since_boot`) and `panel_input_generation()` at the press, `mute_group`, `mute_group_target`.

- [ ] **Step 1: Extend the `test_ui.c` panel fake**

`panel_input_mute_down()` from a `mutes_down[NUM_TRACKS]` array that `push_panel_event()` sets for `PANEL_MUTE` and `panel_input_resync()` clears; helpers `mute_down(track)` and `mute_up(track)` that push one event and run `ui_update()`.

- [ ] **Step 2: Write the failing tests**

Change `test_grid_panel_keys_during_a_hold_leave_the_tap`: after the MUTE press while playing (TRACK LOOP, not at the loop end), `g_seq.mute_pending == 2` and `g_seq.track_mutes == 0`.

New tests (BAR and playing unless said otherwise; "lands" = after `pulses()` to the next bar):
- `test_mute_press_while_playing_waits`: MUTE 3: pending bit 3, `track_mutes` 0; lands at the bar.
- `test_mute_second_press_cancels`.
- `test_two_quick_mute_presses_are_two_presses`: `mute_down(0)`, `sleep_ms(100)`, `mute_down(1)`, both up: both bits pending, no group.
- `test_mute_group_collects_after_half_a_second`: `mute_down(0)` (pending 0), `sleep_ms(PAD_COPY_HOLD_MS)`, `mute_down(1)`, `mute_up(1)`: pending empty, both MUTE lights steady white in the shown frame; `mute_up(0)`: pending 0 and 1; both land on the same pulse.
- `test_mute_join_and_release_in_one_pass`: after the hold, push MUTE 1 down and MUTE 0 up, then one `ui_update()`: the group (0 and 1) is pending.
- `test_mute_group_target_fixed_at_join`: track 1 audible joins; before the release `seq_set_track_mute(1, true)`; release; at the bar track 1 is muted (the target), not unmuted.
- `test_mute_group_member_toggles_out`: join 1, press 1 again: not in the group; release 0: only 0's change is pending.
- `test_mute_group_waiting_key_press_cancels_it`: tap MUTE 2 (pending); hold 0 for the hold time; press 2: its change is cancelled and it does not join; 0's change still waits (no group formed).
- `test_mute_empty_group_release_keeps_held_change`: hold 0 past the hold time, release: 0's change still pending.
- `test_mute_group_without_landed_held_key`: hold 0, play through the bar (0 lands), press 1: the group is 1 only; release: 1 pending, 0 unchanged.
- `test_mute_group_immediate_mode`: NEXT STEP: hold 0 (toggles at once), after the hold time press 1: unchanged; release 0: 1 toggled at once.
- `test_mute_group_released_after_stop_applies_now`: group formed while playing, PLAY stops, release: the members change at once.
- `test_mute_lost_hold_drops_group`: group formed; `poll_loses_sync = true` and a pass: the group is gone (no white light), and the later release of 0 changes nothing.
- `test_mute_join_in_a_pass_that_lost_the_hold`: 0 held past the hold time; `poll_loses_sync = true` and MUTE 1 pushed down in that same pass: 1 is an ordinary press (its change waits), no group forms, and 0's own change still waits.
- `test_mute_hold_dropped_by_panic` and `test_mute_hold_dropped_by_save`: the group is gone and the release changes nothing.
- `test_selector_mute_acts_now_and_leaves_group`: MUTE 2 waiting; selector short release of button 3 (track 2): pending bit clear, `track_mutes` toggled at once; with track 2 collected in a group, the selector press also takes it out of the group.
- `test_pending_mute_light_in_frame`: MUTE 4 waiting: the shown frame's `LED_MUTE(4)` is white on an odd sixteenth.

- [ ] **Step 3: Run; the new tests fail.**

- [ ] **Step 4: Implement**

- `session_mute()` → `transport_toggle_mutes(1u << track)`.
- `handle_panel_events()`: MUTE press and release per the spec's "Groups": no held key → ordinary press, becomes held; held key down for less than `PAD_COPY_HOLD_MS`, or no longer trusted (`!panel_input_mute_down(held)` or generation changed) → ordinary press; otherwise join (in group → out; waiting change → `transport_cancel_mutes()` only; else the first joiner withdraws the held key's waiting change with its target, and the key joins with target `!(seq_get_track_mutes() & bit)`). Release of the held key: if the group is non-empty and the generation is unchanged, `transport_set_mutes(group, target)`; then no held key and an empty group. Other MUTE releases do nothing.
- End-of-pass sweep beside `release_lost_pad_keys()`: a held key not reported down or whose generation changed is dropped with its group. The panic combo and a flash save or load drop it where they drop pad holds.
- Selector buttons 1-8: take the track out of the group, then `transport_toggle_mutes_now(1u << t)`.
- `update_panel_lights()`: `state.mute_pending = snap->mute_pending; state.mute_group = <group>;`.
- README: MUTE waits for the launch point (at once when stopped or under NEXT STEP), flashes white while waiting, a second press cancels; mute groups (hold half a second, press others, release; the held key goes with them unless it already changed); the selector's buttons still act at once; the Features line.

- [ ] **Step 5: Whole suite passes.**

- [ ] **Step 6: Commit**

```bash
git add src/ui.c src/session.c test/test_ui.c README.md
git commit -m "UI: quantised MUTE presses and mute groups"
```

### Task 4: The simulator

**Files:**
- Modify: `sim/build.sh`, `sim/check.sh`, `sim/core/sim_api.h` (comment of `sim_features`), `sim/web/device.js`, `sim/web/app.js`, `sim/web/artist.js`, `sim/test/artist_check.js`, `sim/test/device_check.js`, `sim/README.md`

**Interfaces:**
- Consumes: the firmware of Tasks 1-3; `transport_toggle_mutes` in `include/sequencer.h` as the detection mark.
- Produces: feature bit 8; `Device.features().quantMutes`; `Artist.buildShow(seed, { ..., quantMutes })`; env `QMUTE=1` for the checks.

- [ ] **Step 1: Wiring**

- `sim/build.sh`: `grep -q 'transport_toggle_mutes' include/sequencer.h && FEATURES=$((FEATURES | 8))`; the comment and the echo name bit 8 "MUTE waits for the launch point".
- `device.js` `features()`: `quantMutes: !!(bits & 8)`.
- `check.sh`: the node snippet also reports bit 8; when set, every score's `VARS` gets `QMUTE=1`.
- `artist_check.js`, `device_check.js`: `quantMutes: process.env.QMUTE === '1'` into `buildShow`.
- `app.js` `startShow()`: `quantMutes: device.features().quantMutes`.

- [ ] **Step 2: `device_check.js`**: the "MUTE does not work after the resets" tap: with `QMUTE`, check the mutes unchanged right after the tap, then run one bar (`show.barUs`) plus 50 ms and check the change.

- [ ] **Step 3: The `QMUTE` score in `artist.js`** (`Script` keeps `this.quantMutes`; only `stage()`, `perform()`, `prepare()` captions and `mutePair` change):

- Captions: `perform()` says MUTE also waits for the bar and flashes white until then; `mute`/`mutePair` say the change lands on the downbeat.
- `mute()` and `mutePair()` (as two fingers) keep their moments. With `quantMutes` each records two mutes-only points: `{ t: downbeat - 40 ms, mutesOnly: true, map: { muted: before } }` and `{ t: downbeat + 40 ms, ..., muted: after }`, the downbeat being that of the bar passed to `onDownbeat` (remember it in `st`).
- With `quantMutes`, `onDownbeat(bar)` only records the target bar and `mute`/`mutePair` move the cursor themselves (`st.at(bar, 1, -70 - 20 * s.rnd())` as today), because the cursor may only move forward and `mutePair` must see where it was. `stop()` keeps its moment by calling `st.at` itself.
- `mutePair()` with `quantMutes`, when the cursor is at or before beat 2 of bar `N - 2` (N = the target bar): the group gesture instead — down(a) at bar `N-2` beat 2; wait 650 ms; tap b; hold a across bar `N-1`'s downbeat; up(a) at bar `N` beat 1 minus 120 ms. Points: mutes-only at bar `N-1` downbeat + 40 ms (unchanged), a lights point 100 ms after b's press (`{ t, lights: [a, b] }`: both MUTE lights white), and the two points around bar N. Where there is no room, two fingers at the old moment.
- `artist_check.js`: for a `mutesOnly` point check only the mutes; for a `lights` point check `device.leds()` at `LED_MUTE(t) = 72 + t`: `r === g && g === b && r > 0` for each track listed.
- `sim/README.md`: the variants list gains the `QMUTE` behaviour (what the artist does differently and what the check adds); "MUTE" wording where it describes the score.

- [ ] **Step 4: Build and check**

Run: `sim/build.sh && sim/check.sh` (both from the repository root; `sim/emsdk` must exist)
Expected: `firmware features: 15`, then for every score `PASS device` and `PASS artist (44 shows)`.
Also run the unchanged score once on the new firmware: `FW2=1 node sim/test/artist_check.js | tail -1` → `PASS artist (44 shows)` (the compatibility claim of the spec).
Count the shows that played a group gesture (grep the say() captions in `sim/build/artist-*.log`); at least one per form, or say in the commit message which forms had no room.

- [ ] **Step 5: Commit**

```bash
git add sim/build.sh sim/check.sh sim/core/sim_api.h sim/web/device.js sim/web/app.js sim/web/artist.js sim/test/artist_check.js sim/test/device_check.js sim/README.md
git commit -m "Simulator: quantised mutes and a mute group in the score"
```
