# Drum-Kit Track Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `TRACK_TYPE_KIT`, a track that plays a 909 over DIN MIDI from one track (an AC lane and 11 voice lanes, off/note/accent/flam steps), edited lane by lane on the session panel.

**Architecture:** Kit steps live in a new `kit_steps` field of `pattern_t` and kit settings per track in `g_seq.kit_config`; pure helpers sit in a new `src/kit.c`. Core 1 plays prepared kit rows (a union with today's event rows) in a fixed pulse order, with flams and kit note-offs sent as their own messages. The MIDI TX path gains running status in the drain, real-time bytes ahead of note messages and a pending-Clock flag. The save format moves to v6 with per-version layouts. On Core 0 the TRACK encoder walks the kit's lanes by detents, and the Session view, Pattern view, a new kit step editor and the track config learn about kits.

**Tech Stack:** C11, Raspberry Pi Pico SDK 2.2.0 (RP2040), CMake; host tests are plain C executables under `test/` run by CTest, with fakes in `test/fakes`.

**Spec:** `docs/superpowers/specs/2026-11-16-drum-kit-track-design.md` (builds on `docs/superpowers/specs/2026-11-16-session-panel-design.md`; this branch is based on `session-panel`).

**Execution:** subagent-driven with a review after each task (chosen by the user on 2026-11-16); starts once `session-panel` is merged, on a branch rebased onto it.

## Global Constraints

- Lanes, in this order: 0 AC, 1 BD, 2 SD, 3 LT, 4 MT, 5 HT, 6 RS, 7 CP, 8 CH, 9 OH, 10 CC, 11 RC. Voice `v` is lane `v + 1`; every per-voice array is indexed by voice in lane order.
- Step values: 0 off, 1 note, 2 accented note, 3 flam. AC is 0 or 1. A step word keeps lane `i` in bits `2i..2i+1`; bits 24-31 are zero.
- Kit settings: `note[]` 0-127, defaults BD 36, SD 38, LT 45, MT 47, HT 50, RS 37, CP 39, CH 42, OH 46, CC 49, RC 51; `vel_normal` 80, `vel_accent` 110, `vel_max` 127 (each 1-127); `flam_pulses` 3 (1-12). Defaults are set by `sequencer_init()`.
- Velocity: note -> `AC ? vel_accent : vel_normal`; accent -> `AC ? vel_max : vel_accent`; flam -> first hit at the note level, second hit one level up `flam_pulses` later.
- Voice order inside a kit step: BD, SD, CP, CH, OH, RS, LT, MT, HT, CC, RC (`kit_voice_order` = voice indices `{0,1,6,7,8,5,2,3,4,9,10}`).
- At most two kit tracks; a track counts as a kit for the limit when its type is KIT in any location. Which data a cell plays, and which editor the panel shows for it, follows that cell's own type.
- A kit is MIDI only: `output_mode`, `cv_channel` and `drum_note` are ignored; a kit never touches CV or gates.
- Kit Note Off is Note On with velocity 0; SYNTH and DRUM tracks keep sending 0x80.
- Pulse order: 1 Clock, 2 live mutes, 3 note-off countdowns, 4 flam countdowns, 5 the step. Kit messages of 2-4 go out with their own send call, never through `midi_buf`.
- Kit note-off countdown: `gate_pulses`, or `min(PULSES_PER_STEP, gate_pulses + flam_pulses)` on a step with a flam.
- MIDI: `MIDI_BUF_SIZE` 160, `MIDI_TX_QUEUE_SIZE` 1024 (1,023 bytes usable; 512 before the multi-model review). The queue keeps whole messages with status; the drain applies running status, and the first message after the queue ran empty (its last byte written) carries its status. Start/Continue/Stop wait in their own small queue and go out at the next message boundary ahead of note messages; then a pending Clock (one flag); Stop, Panic and `midi_clear_output_queue()` clear the flag. The UART hardware FIFO is off; the drain writes one byte per writable check.
- Overload: a flam's second hits are queued only if the TX queue then holds at most 319 bytes (`KIT_FLAM_MAX_QUEUED`, through `midi_try_send_bytes_capped()`; it replaced the 192-byte free-space reserve after the multi-model review); otherwise they are skipped and counted in `sequencer_jitter_stats_t.flam_drops`. The cap limits flams only: mandatory messages may fill the whole queue.
- Save format v6; versions 4, 5 and 6 load. The legacy pattern record is today's `pattern_t` (459 bytes on the RP2040, 532 on the host): no size is hard-coded.
- Core 1: no new locks; lock order playback -> shared state / MIDI. Hardware acceptance (bench, Task 10): GPIO 7 max ISR duration +20 us at most with two dense kits; Core 0 interrupts-off windows under 100 us; wire Clock no worse than today.
- From the session-panel plan: host tests pass under CTest and the CI sanitizer build; unused static functions left by a task are deleted in that task; the firmware builds without warnings with `-DREBIRTH_CV_DAC_GAIN=1` and `=2`.

## Review Focus

- Gate 100 % with a flam on every step: no kit note left sounding, and no voice hit on a step cut by the previous step's countdown — test in Task 4.
- A type change of a playing track (KIT -> SYNTH while a flam is pending; SYNTH with a tie or slide note -> KIT): every Note On paired, no gate left high — tests in Task 5.
- Stop then Start within one step while note bytes are still queued: Start and Clock leave at once, no Clock after the Stop — tests in Task 2.
- A v5 image as the device wrote it (legacy record stride, song at the legacy offset, legacy checksum) — test in Task 6.
- TRACK turned fast across a kit: one stop per detent, no lane skipped — tests in Tasks 7 and 8.

## Working Method (every task)

Write the listed tests first; build the target and see it fail to compile/link or fail its checks; implement; run the task's checks; commit only the task's files with `git add <files> && git commit -m "<message>"`. Tasks marked with cycles (a), (b), (c) run one red-green cycle per letter, with the whole existing suite green after each. Helper names, internal structure and code layout are the implementer's unless an Interfaces block fixes them.

## Commands

Run from the worktree root.

- Host tests: `cmake -S test -B build/host && cmake --build build/host --parallel && ctest --test-dir build/host --output-on-failure` — expected `100% tests passed`. One target: `cmake --build build/host --target <name> && build/host/<name>`; each test prints `PASS <name>`, a failure prints `file:line: condition` and exits 1.
- Sanitizers (as CI): `cmake -S test -B build/san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang '-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' && cmake --build build/san --parallel && ctest --test-dir build/san --output-on-failure`.
- Firmware, for G in 1 and 2: `PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0 cmake -S . -B build/fw$G -G Ninja -DCMAKE_BUILD_TYPE=Release -DREBIRTH_CV_DAC_GAIN=$G && cmake --build build/fw$G > build/fw$G/build.log 2>&1` — both exit 0, `grep -Eic 'warning|error' build/fw$G/build.log` prints `0`, `build/fw$G/rp2040_sequencer.uf2` exists.

---

### Task 1: Kit data model and pure helpers

**Files:**
- Create: `include/kit.h`, `src/kit.c`, `test/test_kit.c`
- Modify: `include/sequencer.h`, `src/pattern.c` (`pattern_init`, `pattern_clear`, `pattern_clear_track`, `pattern_clear_step`, `pattern_shift`, `pattern_shift_track`), `src/sequencer.c` (`sequencer_init`), `CMakeLists.txt` (add `src/kit.c`), `test/CMakeLists.txt` (new `test_kit` = `test_kit.c` + `kit.c` + `pattern.c`, include `${REBIRTH_INC}`; add `${REBIRTH_SRC}/kit.c` to `test_song`, `test_playback`, `test_storage`, `test_ui`, `test_session`), `test/test_playback.c`

**Interfaces:**
- Produces, in `include/sequencer.h`:
  ```c
  #define KIT_LANES       12
  #define KIT_VOICES      11
  #define KIT_LANE_AC     0
  #define KIT_LANE_BD     1
  #define KIT_LANE_CH     8
  #define KIT_LANE_OH     9
  #define MAX_KIT_TRACKS  2
  #define KIT_FLAM_MIN    1
  #define KIT_FLAM_MAX    12
  typedef enum { KIT_OFF = 0, KIT_NOTE = 1, KIT_ACCENT = 2, KIT_FLAM = 3 } kit_value_t;
  typedef enum { TRACK_TYPE_SYNTH, TRACK_TYPE_DRUM, TRACK_TYPE_KIT } track_type_t;
  typedef struct {
      uint8_t note[KIT_VOICES];          // lane order BD..RC
      uint8_t vel_normal, vel_accent, vel_max;
      uint8_t flam_pulses;
  } kit_config_t;
  // pattern_t, new LAST field:  uint32_t kit_steps[NUM_TRACKS][STEPS_PER_PATTERN];
  // sequencer_state_t, new field: kit_config_t kit_config[NUM_TRACKS];
  ```
- Produces, in `include/kit.h` (pure, no locks, no hardware):
  ```c
  uint8_t  kit_get(uint32_t word, uint8_t lane);                 // 0..3
  uint32_t kit_set(uint32_t word, uint8_t lane, uint8_t value);  // AC stores (value != 0)
  uint32_t kit_edit(uint32_t word, uint8_t lane, uint8_t value); // kit_set + CH/OH rule
  uint32_t kit_press(uint32_t word, uint8_t lane);               // AC toggles; voice: 0->1->2->0, 3->0; CH/OH rule
  void kit_config_defaults(kit_config_t *c);
  void kit_config_clamp(kit_config_t *c);                        // note 0..127, vel 1..127, flam 1..12
  bool kit_config_valid(const kit_config_t *c);                  // every field in range
  const char *kit_lane_name(uint8_t lane);                       // "AC","BD",...,"RC"; "??" out of range
  bool kit_cell_has_clip(const uint32_t steps[STEPS_PER_PATTERN], uint8_t length); // any voice lane non-zero within length
  uint8_t kit_track_mask(const pattern_t *patterns, uint8_t count); // bit t: type KIT in any of `count` locations
  void kit_lane_glyphs(const uint32_t steps[STEPS_PER_PATTERN], uint8_t lane, uint8_t length,
                       char out[STEPS_PER_PATTERN + 1]);        // '.', 'o', 'O', 'f'; ' ' beyond length; NUL-terminated
  extern const uint8_t kit_voice_order[KIT_VOICES];              // {0,1,6,7,8,5,2,3,4,9,10}
  ```
  The CH/OH rule: a non-zero value written to CH clears OH on that word and the other way round.

- [ ] **Step 1: Scaffold** `include/kit.h` with the prototypes, an empty `src/kit.c`, the `test_kit` target and an empty `test_kit.c` `main`; the build configures and links.
- [ ] **Step 2: Write `test/test_kit.c`** (style of `test_pattern.c`, `CHECK` + `PASS <name>`):
  - `test_get_set_roundtrip`: for every lane 0..11 and value 0..3, `kit_get(kit_set(0, lane, v), lane) == (lane == 0 ? (v != 0) : v)`; other lanes stay 0; `kit_set(0, 11, 3) == 0x00C00000u`.
  - `test_press_cycles`: on SD `kit_press` gives 1, 2, 0; from 3 gives 0; AC gives 1 then 0.
  - `test_ch_clears_oh`: `w = kit_set(0, KIT_LANE_OH, 1); w = kit_press(w, KIT_LANE_CH)` -> CH 1, OH 0; and the reverse; `kit_edit(w, KIT_LANE_CH, 0)` leaves OH as it was.
  - `test_defaults_and_clamp`: defaults are notes `{36,38,45,47,50,37,39,42,46,49,51}`, 80/110/127, flam 3, and valid; `kit_config_clamp` turns note 200 -> 127, velocity 0 -> 1, flam 0 -> 1 and 13 -> 12; `kit_config_valid` is false for each of those before clamping.
  - `test_has_clip`: AC-only word -> false; SD at step 5 with length 5 -> false, length 6 -> true.
  - `test_track_mask`: KIT at track 2 in location 0 and track 5 in location 31 -> `0x24`.
  - `test_glyphs`: SD values 1,2,3,0 at steps 0-3, length 6 -> `strlen(out) == 16` and `memcmp(out, "oOf...          ", 17) == 0`.
  - `test_pattern_helpers_move_kit_steps`: `pattern_shift_track(p, 1, +1)` moves `kit_steps[1][15]` to `[1][0]`; `pattern_shift(p, -1)` moves every track's word 0 to 15; `pattern_clear_track`, `pattern_clear_step`, `pattern_clear` and `pattern_init` zero the kit words they cover.
- [ ] **Step 3:** in `test_playback.c` add `test_sequencer_init_sets_kit_defaults`: after `reset()` every `g_seq.kit_config[t]` equals `kit_config_defaults`.
- [ ] **Step 4:** the new tests fail.
- [ ] **Step 5: Implement** the types, `src/kit.c`, the `pattern.c` changes (kit words moved, cleared and shifted alongside `steps`), and `kit_config_defaults` for all 8 tracks in `sequencer_init()` before its refresh loop.
- [ ] **Step 6:** host tests, sanitizer build and firmware build pass.
- [ ] **Step 7: Commit** "Add the kit data model and its pure helpers".

---

### Task 2: MIDI TX path: running status, real-time bytes and Clock ahead of notes

**Files:**
- Modify: `src/midi_out.c`, `include/sequencer.h` (midi section), `test/fakes/fake_hardware.c`, `test/fakes/fake_hardware.h`, `test/test_playback.c`, and every test in `test_playback.c`, `test_ui.c`, `test_song.c`, `test_session.c` that reads channel-message bytes from `fake_midi` by position, by length or with `count_byte` — at least `count_note_ons`, `test_panic_clears_all_outputs_and_queues`, `test_concurrent_midi_producers_and_uart_irq`, `test_output_overflow_stops_transport`, `test_launch_keeps_track_routing`, `test_empty_scene_silences_all_tracks`, `test_staging_overflow_never_sends_partial_message`, `test_uart_overflow_never_sends_partial_message`, `test_full_queue_drains_free_uart_before_dropping`; `JITTER_MEASUREMENT.md`

**Interfaces:**
- Produces:
  ```c
  void midi_send_bytes(const uint8_t *data, uint16_t length);     // whole channel messages with status;
                                                                  // sets the overflow flag when they do not fit
  bool midi_try_send_bytes(const uint8_t *data, uint16_t length); // false, flag untouched, unless they fit
                                                                  // with `reserve` bytes free afterwards
  bool midi_try_send_bytes_reserve(const uint8_t *data, uint16_t length, uint16_t reserve);
  uint16_t midi_tx_free(void);                                    // free note-queue bytes, under the TX lock
  // midi_send_clock(): sets the pending-Clock flag and drains (no queue byte).
  // midi_send_start/continue/stop(): real-time queue (depth 4); stop also clears the pending Clock.
  ```
  (`midi_try_send_bytes(d, n)` is `midi_try_send_bytes_reserve(d, n, 0)`.)
- Produces, test fakes (`fake_hardware.h`):
  ```c
  extern unsigned fake_uart_budget;       // bytes the UART takes before it reports not writable; UINT_MAX after reset
  extern bool fake_uart_fifo_enabled;     // last value passed to uart_set_fifo_enabled
  extern uint32_t fake_midi_time_us[FAKE_MIDI_CAPACITY]; // wire start time of each byte (wire-time mode)
  typedef struct { uint8_t status, data1, data2; } fake_midi_msg_t;
  unsigned fake_midi_decode(fake_midi_msg_t *out, unsigned max); // running status expanded; real-time bytes
                                                                 // become {byte, 0, 0}, in place
  unsigned fake_midi_count(uint8_t status);                      // decoded messages with this status byte
  // Wire-time mode: drains the UART through the IRQ handler one byte per 320 us of simulated time
  // from the last byte's end up to `until_us`; unused idle time is not banked.
  void fake_uart_run_until(uint32_t until_us);
  ```

Drain rules (`midi_tx_drain_locked`, one byte per `uart_is_writable` check):

```text
at a message boundary:
    if real-time queue non-empty: write its head (Start/Continue/Stop)
    elif clock_pending: write 0xF8, clock_pending = false
    elif note queue empty: stop
    else: start the next queued message; if it is a channel message whose status == last_status
          and !queue_ran_empty, skip writing its status byte
inside a message: write its next byte; when that byte empties the note queue: queue_ran_empty = true
writing any channel status byte: last_status = it, queue_ran_empty = false
real-time bytes never change last_status
TX IRQ enabled while the note queue or the real-time queue is non-empty or clock_pending
```
Message lengths by status: 0x80-0xBF and 0xE0-0xEF -> 3, 0xC0-0xDF -> 2 (the firmware sends nothing else into the note queue). `midi_clear_output_queue()` also clears the real-time queue, `clock_pending`, the message state and `last_status`; `midi_wait_output_idle()` waits for all three. `midi_init()` calls `uart_set_fifo_enabled(MIDI_UART, false)`. `MIDI_BUF_SIZE` 160, `MIDI_TX_QUEUE_SIZE` 512.

- [ ] **Step 1 (a): Fakes and port.** Add the fake budget, FIFO flag, decoder, byte times and `fake_uart_run_until`; change the tests listed under Files to decode channel messages (Clock/Start/Stop byte checks may stay). All host tests still pass on the old `midi_out.c`.
- [ ] **Step 2 (b): Running status — failing tests:**
  - `test_running_status_within_burst`: UART not writable; note_on(0,36,100), note_on(0,38,100), note_on(1,40,100); writable; IRQ -> bytes `90 24 64 26 64 91 28 64`.
  - `test_status_repeated_after_queue_ran_empty`: two note_on(0,…) sent one after the other with a writable UART -> both carry `0x90`; and with `fake_uart_budget` running out exactly on the last byte of the first message, a second note_on(0,…) queued before the next IRQ still carries `0x90`.
  - Rewrite `test_staging_overflow_never_sends_partial_message`: 54 `midi_buf_note_on` and a flush -> exactly 53 decoded Note Ons, `midi_output_overflowed()`, no partial message.
- [ ] **Step 3 (b):** implement the queue and running status; tests pass.
- [ ] **Step 4 (c): Real-time bytes and Clock — failing tests:**
  - `test_clock_with_empty_queue_goes_at_once`: `midi_send_clock()` -> `fake_midi == {F8}`, `!fake_uart_tx_irq`.
  - `test_clock_at_message_boundary`: not writable; note_on(0,36,100), note_on(0,38,100); budget 1 + writable; IRQ (writes `90`); `midi_send_clock()`; drain byte by byte -> `90 24 64 F8 26 64`.
  - `test_start_and_clock_overtake_old_notes` (Review Focus): not writable; note_on(0,36,100); `midi_send_stop()`; `midi_send_start()`; `midi_send_clock()`; writable; IRQ -> `FC FA F8 90 24 64`.
  - `test_continue_before_clock`: as above with `midi_send_continue()` -> `FB F8` before the note.
  - `test_stop_cancels_pending_clock`: not writable; note_on; `midi_send_clock()`; `midi_send_stop()`; writable; IRQ -> no `F8` at all.
  - `test_panic_cancels_pending_clock`: playing, `midi_send_clock()` pending with the UART blocked, `sequencer_panic()`, unblock -> no `F8` after `FC` or the All Notes Off controllers.
  - `test_clear_queue_cancels_clock`: not writable; `midi_send_clock()`; `midi_clear_output_queue()`; writable; IRQ -> nothing written, `!fake_uart_tx_irq`.
  - `test_wait_idle_sends_pending_clock`: not writable; `midi_send_clock()`; writable; `midi_wait_output_idle()` -> `fake_midi == {F8}`.
  - `test_clock_spacing_on_empty_pattern`: `transport_start()`, 96 pulses with nothing to play, wire-time mode -> exactly 24 `F8`, each `4 * pulse_interval_us` apart (±320 us).
  - `test_clock_waits_one_message_in_long_burst`: wire-time mode; 34 note-ons queued at time 0 (102 bytes); `midi_send_clock()` at 5,000 us -> the `F8` starts no later than 5,960 us.
  - `test_try_send_keeps_reserve`: with 300 bytes queued (UART blocked), `midi_try_send_bytes_reserve(msg, 30, 192)` is false and `!midi_output_overflowed()`; with 100 queued it is true.
  - `test_fifo_disabled`: after `midi_init()`, `!fake_uart_fifo_enabled`.
  - Rewrite `test_uart_overflow_never_sends_partial_message` and `test_full_queue_drains_free_uart_before_dropping` for a 511-byte note queue in which Clock takes no byte: nothing partial is ever written.
- [ ] **Step 5 (c):** implement; all host tests and the sanitizer build pass.
- [ ] **Step 6: Core 0 interrupts-off review.** List every `seq_lock`/playback-lock/MIDI-lock section Core 0 can enter while playing (not the flash save) in `JITTER_MEASUREMENT.md` with what it does; any one that loops over more than one location's data or more than one MIDI message is shortened so each section stays well under 100 us. Host tests still pass.
- [ ] **Step 7: Commit** "Send real-time bytes and Clock ahead of note bytes; running status in the drain".

---

### Task 3: Prepared kit rows and kit steps on Core 1

**Files:**
- Modify: `src/sequencer.c` (prepared row types, `fill_prepared_track`, `load_prepared_track`, the ISR, `stop_playback_outputs`, `panic_playback`, `transport_start`, `transport_set_mode`, `clear_runtime_note_state`, `jitter_reset_now`, `sequencer_get_jitter_stats`), `include/sequencer.h` (`sequencer_jitter_stats_t.flam_drops`)
- Test: `test/test_playback.c`

**Interfaces:**
- Consumes: Task 1 types and `kit_*` helpers; Task 2 `midi_send_bytes`.
- Produces:
  ```c
  typedef struct {
      uint8_t kind;                    // PREPARED_EVENTS or PREPARED_KIT
      uint8_t length;                  // shared, outside the union
      union {
          prepared_track_event_t step[STEPS_PER_PATTERN];
          struct {
              uint32_t lanes[STEPS_PER_PATTERN];
              uint8_t note[KIT_VOICES];
              uint8_t vel[3];          // normal, accent, max
              uint8_t channel, gate_pulses, flam_pulses;
          } kit;
      };
  } prepared_track_pattern_t;
  // sequencer_jitter_stats_t: uint32_t flam_drops; reset by jitter_reset_now, copied by sequencer_get_jitter_stats
  ```
  `sequencer_clip_mask` uses `kit_cell_has_clip` for kit cells, before the stored-mute blanking.

Rules (spec "Playback (Core 1)"): `fill_prepared_track` copies the cell's kit words and `g_seq.kit_config[track]` inside the same `seq_lock` block as today's copy; a kit row zeroes lanes at and beyond the length and all lanes under the stored mute. Per kit track the ISR keeps the sounding mask (11 bits) with the note and channel each voice was sent on, and the note-off countdown. Pulse order steps 2-3 for kits, and Stop / Panic / Start while playing / play-mode change, release every sounding kit voice with one `midi_send_bytes` call of Note On velocity 0 messages, before any Stop byte is queued. At a step boundary: voices still sounding are released first in their own message; then the step's Note Ons go into `midi_buf` in `kit_voice_order` with the velocity rule; the countdown is set to `gate_pulses`. Flam values play their first hit only in this task (the second hit is Task 4). A kit never touches CV or gates.

Test helpers in `test_playback.c`: `make_kit(track, channel)` sets the type KIT and the channel in all 32 locations (track left in `OUTPUT_BOTH` with its CV channel) and refreshes; `set_kit(pattern, track, step, lane, value)` writes a lane with `kit_set` and refreshes; `set_gate(track, pct)`; `msgs()` decodes into a static array.

- [ ] **Step 1: Write the failing tests:**
  - `test_kit_all_voices_order_and_levels`: kit on track 1, channel 9; every voice lane 1 on step 0 -> decoded Note Ons on 0x99, notes `36,38,39,42,46,37,45,47,50,49,51`, all velocity 80; `fake_gates == 0` and no gate rise throughout.
  - `test_kit_levels`: BD 1, SD 2 -> 80, 110; with AC -> 110, 127.
  - `test_kit_note_off_at_gate_is_velocity_zero`: gate 50 %: after 11 more pulses no `99 xx 00`; at pulse 12 one `99 note 00` per sounding voice, in one burst.
  - `test_kit_gate_100_releases_before_new_hits`: gate 100, BD on every step: from step 1 on, each step boundary decodes `99 24 00` then `99 24 50`, and no further `99 24 00` in that pulse.
  - `test_kit_length_wraps`: length 4, BD on step 0 -> `current_track_step[1]` runs 0,1,2,3,0 and BD plays every 4 steps; a hit at step 7 with length 6 never plays.
  - `test_kit_stored_mute_and_clip_mask`: with the stored mute set, the next step plays nothing; `sequencer_clip_mask` still reports the stored-muted cell, and reports no clip for an AC-only cell.
  - `test_kit_live_mute_releases_next_pulse`: sounding BD (gate 100), `seq_toggle_track_mute(1)`, one tick -> `99 24 00`.
  - `test_kit_stop_releases_before_stop_byte`, `test_kit_panic_releases`, `test_kit_restart_releases`, `test_kit_mode_change_releases`: gate 100, sounding voices get `99 note 00` before `FC` (Stop), before the All Notes Off controllers (Panic), and before the new run's first hit (restart).
  - `test_kit_settings_edit_keeps_sounding_note`: CH sounding on 42 (gate 100); change `g_seq.kit_config[1].note[7]` to 44 and refresh; the release is `99 2A 00`, the next hit is 44.
- [ ] **Step 2:** they fail.
- [ ] **Step 3: Implement** per the rules above.
- [ ] **Step 4:** host and sanitizer tests pass; existing playback tests unchanged.
- [ ] **Step 5: Commit** "Play kit tracks from prepared kit rows".

---

### Task 4: Flams and the overload rule

**Files:**
- Modify: `src/sequencer.c`
- Test: `test/test_playback.c`

**Interfaces:**
- Consumes: Task 3 kit runtime state; Task 2 `midi_try_send_bytes_reserve`, wire-time fake.
- Produces: per kit track the flam mask, flam countdown and flam velocity; `flam_drops` counted.

Rules: a flam step plays its first hit at the note level (AC respected) and arms the flam (mask, countdown `flam_pulses`, velocity one level up: `vel_accent`, or `vel_max` with AC). In pulse order step 4, when the countdown reaches 0, one `midi_try_send_bytes_reserve(…, 192)` call carries, per flam voice, Note On velocity 0 then Note On at the flam velocity. If it returns false the second hits are skipped, `flam_drops` goes up and the voices stay in the sounding mask until their note-off. The note-off countdown on a step with a flam is `min(PULSES_PER_STEP, gate_pulses + flam_pulses)`. A live mute, Stop, Panic and a restart cancel a pending flam.

- [ ] **Step 1 (a): Flam behavior — failing tests:**
  - `test_flam_second_hit`: BD flam, flam 3: step pulse -> `99 24 50`; 3 pulses later -> `99 24 00`, `99 24 6E`; with AC -> first `6E`, second `7F`.
  - `test_flam_gate_extends_to_second_hit`: gate 10 % (2 pulses), flam 12 -> the final `99 24 00` comes exactly 14 pulses after the step.
  - `test_flam_gate_100_every_step` (Review Focus): gate 100, flam 12 on BD, SD and CH on every step; run pulses 0-95, then Stop -> per note eight hits and eight releases: first hits at pulses 0/24/48/72 at 80, second hits at 12/36/60/84 at 110, a velocity-0 release right before every hit after the first, and the last release before `FC`.
  - `test_live_mute_cancels_pending_flam`: mute 1 pulse after a flam step -> releases, no second hit.
  - `test_edit_during_flam_keeps_second_hit`: change the CH note 1 pulse after a CH flam step and refresh -> the second hit and its release still use the old note.
- [ ] **Step 2 (a):** implement; tests pass.
- [ ] **Step 3 (b): Overload — failing tests:**
  - `test_flam_overload_drops_second_hits`: 11-voice flam step played with the UART free (first hits sent); then block the UART and queue note-ons until fewer than 192 + 66 bytes are free (the 11-voice flam message is 66 bytes as queued); reach the flam pulse -> no second hits, `sequencer_get_jitter_stats()` reports `flam_drops == 1`, `!midi_output_overflowed()`, still playing; unblock -> every voice still gets one `99 note 00`; after `sequencer_reset_jitter_stats()` and one tick the getter reports 0.
  - `test_kits_at_wire_rate` (wire-time mode, `fake_uart_run_until` before every tick): two kits on different channels with every voice on every step plus six SYNTH tracks with a note every step, gate 100 %:
    - at 150, 200 and 300 BPM without flams, 16 bars -> no overflow, still playing, `flam_drops == 0`, decoded first hits per kit = 11 × 256;
    - at 300 BPM with every kit voice flammed, for gate 10/50/100 % and flam 1 and 12, 8 bars each -> no overflow, still playing, every expected first hit present, `flam_drops > 0`;
    - at 150 BPM with every voice of one kit flammed, flam 3 -> `flam_drops == 0`.
- [ ] **Step 4 (b):** implement; host and sanitizer tests pass.
- [ ] **Step 5: Commit** "Play kit flams and drop second hits on overload".

---

### Task 5: Launches, Song mode, type changes and the two-kit limit

**Files:**
- Modify: `src/song.c` (`seq_set_track_config`, new `seq_set_kit_config`, `seq_kit_tracks`), `include/sequencer.h`, `src/sequencer.c` (`load_prepared_track`)
- Test: `test/test_playback.c`, `test/test_song.c`

**Interfaces:**
- Produces:
  ```c
  // false, nothing changed, when config->type is KIT, the track is not a kit yet and
  // MAX_KIT_TRACKS other tracks are kits (kit_track_mask over all locations)
  bool seq_set_track_config(uint8_t track, const track_config_t *config);
  // stores kit_config_clamp(config) and refreshes the track's 32 prepared rows
  void seq_set_kit_config(uint8_t track, const kit_config_t *config);
  uint8_t seq_kit_tracks(void);   // kit_track_mask under seq_lock
  ```
- Consumes: Task 3 prepared row `kind`.

Rule: when `load_prepared_track` installs a row whose `kind` differs from the row the track played, it first releases what the old kind left sounding: the event note on its stored route (MIDI 0x80 and the CV gate, also for a tie or slide note whose countdown is 0), or every kit voice with a pending flam cancelled.

- [ ] **Step 1: Write the failing tests:**
  - `test_kit_launch_every_quant`: for TRACK_LOOP, BAR, BEAT and STEP: kit cell A1 playing BD on every step at gate 100, A2 queued with SD on every step -> at the launch point `99 24 00` comes before the first `99 26 …`, and A2 joins in the phase the existing `test_launch_phase_matrix` expects.
  - `test_kit_song_mode`: song entries kit location 0 then 1 (gate 100) -> each entry plays its own kit cell, the old voices released at the change; with `SONG_END_STOP` the voices sounding at the end are released before `FC`.
  - `test_kit_to_synth_during_flam` (Review Focus): kit flam pending on BD; `seq_set_track_config(1, synth)` -> the kit releases come before the synth's first Note On and every kit Note On is paired.
  - `test_synth_tie_to_kit_releases_note_and_gate` and `test_synth_slide_to_kit_releases_note_and_gate` (Review Focus): SYNTH tie (then slide) note sounding on CV 0 with the gate high; switch to KIT -> at the next boundary `0x80 note 00` on the synth's channel and gate 0 low, before any kit Note On.
  - `test_kit_to_drum_releases_voices` and `test_drum_to_kit_releases_note`.
  - `test_type_change_keeps_both_step_arrays`: one `step_t` and one kit word set; KIT -> SYNTH -> KIT -> both byte-equal to before.
  - `test_third_kit_refused` (in `test_song.c`): kits on tracks 1 and 4 in all 32 locations accepted; `!seq_set_track_config(6, kit)`, every location of track 6 unchanged; a kit on track 4 in location 31 only also counts.
  - `test_kit_config_clamps_and_refreshes`: `seq_set_kit_config` with note 200, flam 0 / 13 and velocity 0 stores 127, 1 / 12, 1; the next hit uses the new note.
- [ ] **Step 2:** they fail.
- [ ] **Step 3: Implement.** Existing callers of `seq_set_track_config` may ignore the result until Task 9.
- [ ] **Step 4:** host and sanitizer tests pass.
- [ ] **Step 5: Commit** "Launch kits, release notes on type changes, limit the device to two kits".

---

### Task 6: Save format v6 with v4/v5 migration

**Files:**
- Modify: `src/flash_storage.c`, `include/flash_storage.h` (comments)
- Test: `test/test_storage.c`

**Interfaces:**
- Consumes: Task 1 `kit_steps`, `kit_config`, `kit_config_valid`, `kit_config_defaults`, `kit_track_mask`.
- Produces: `VERSION 6`; a private `pattern_v5_t` (today's `pattern_t`, field for field) with `_Static_assert`s that each of its fields has the same offset in `pattern_t`.

Layouts per version (spec "Storage: format version 6"):

| | v4 | v5 | v6 |
|---|---|---|---|
| Patterns at | 256 | 256 | 256 |
| Pattern record | `pattern_v5_t` | `pattern_v5_t` | `pattern_t` |
| Song at | 256 + page-rounded 32 x `sizeof(pattern_v5_t)` | same | 256 + page-rounded 32 x `sizeof(pattern_t)` |
| Kit settings | — | — | after the page-rounded song: `kit_config[8]`, page-padded |
| Checksum | none | legacy patterns + song | new patterns + song + kit settings |

Validation for v6 adds: type in {SYNTH, DRUM, KIT}; `kit_track_mask` has at most `MAX_KIT_TRACKS` bits; every kit word has bits 24-31 zero and AC 0 or 1; `kit_config_valid` for every track. A legacy load copies each `pattern_v5_t` record into the start of a zeroed `pattern_t`, leaves `kit_steps` zero and sets every `kit_config` to the defaults. The `_Static_assert` on the reserved region covers the v6 total.

- [ ] **Step 1: Write the failing tests.** Test helpers: `write_legacy_image(version)` serializes the current patterns as `pattern_v5_t` records and the song at the legacy offset, with the legacy checksum for v5, into `fake_flash` (it replaces today's trick of flipping the header version of a current image); `reseal_v6()` recomputes the v6 checksum of the image in `fake_flash` after a test edits it.
  - v6 round trip keeps `kit_steps[1][3]` = `kit_set(0, 2, 3)` and a changed `kit_config[1]`; kits on tracks 1 and 4 in all 32 locations save and load.
  - v5 and v4 legacy images (Review Focus) load: pattern step data and song equal, all `kit_steps` zero, all `kit_config` at defaults; a flipped byte in a v5 image fails the checksum.
  - Rejected v6 images, each resealed with a correct checksum, leave RAM unchanged: a kit word with bit 24 set; AC value 2; note 200; `vel_normal` 0; `flam_pulses` 13; three kit tracks spread over locations 0, 1 and 2. `flash_storage_save()` refuses the same invalid data in RAM.
  - The existing interrupted-save, playback-guard, late-MIDI and resync checks keep passing.
- [ ] **Step 2:** they fail.
- [ ] **Step 3: Implement.** Keep the static `pattern_buf` copy grown to the new size (simplest); programming from RAM is the implementer's option (spec leaves it open).
- [ ] **Step 4:** host, sanitizer and firmware builds pass.
- [ ] **Step 5: Commit** "Save format v6 with kits; load v4 and v5 saves".

---

### Task 7: Encoder detents

**Files:**
- Create: `test/test_encoder.c` (defines `sequencer_state_t g_seq;` for the fakes)
- Modify: `include/encoder.h`, `src/encoder.c`, `test/CMakeLists.txt` (new `test_encoder` = `test_encoder.c` + `fakes/fake_hardware.c` + `encoder.c`, include dirs `${REBIRTH_INC}` and `${CMAKE_SOURCE_DIR}/fakes`), `test/test_ui.c` (fakes: `encoder_get_delta` returns `deltas[e] * 4`, `encoder_get_detents` returns `deltas[e]`)

**Interfaces:**
- Produces:
  ```c
  #define ENC_EDGES_PER_DETENT 4      // quadrature edges per detent of the chosen part (EC11 type: 4)
  int8_t encoder_get_detents(uint8_t enc); // whole detents since the last call, no acceleration; keeps the remainder
  ```
  `encoder_get_delta` is unchanged; each pass uses one of the two getters per encoder.

- [ ] **Step 1: Write the failing tests** (drive pins 18/19 through `fake_gpio_level`, advance time with `sleep_ms`, call `encoder_poll()` after each change; a CW detent from rest is (A,B) = (0,1), (0,0), (1,0), (1,1)):
  - `test_one_detent_per_four_edges`: one CW cycle -> `encoder_get_detents(0) == 1`; a CCW cycle -> -1.
  - `test_half_detent_is_kept`: two edges -> 0; the other two -> 1.
  - `test_fast_turn_counts_every_detent` (Review Focus): 8 CW cycles with 2 ms per edge -> `encoder_get_detents(0) == 8`.
- [ ] **Step 2:** they fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** host tests pass (the `test_ui` fake change keeps every existing UI test green, since no screen reads detents yet).
- [ ] **Step 5: Commit** "Read whole encoder detents without acceleration".

---

### Task 8: Focus over kit lanes and step editing on kits

**Files:**
- Modify: `include/session.h`, `src/session.c`, `src/ui.c` (`ui_update` encoder reading, `handle_session_view`, `handle_pattern_view`, `build_session_mask`, `update_step_leds`, the end-of-pass pending clear)
- Test: `test/test_session.c`, `test/test_ui.c`

**Interfaces:**
- Consumes: Task 1 `kit_press`, `kit_get`; Task 7 `encoder_get_detents`.
- Produces:
  ```c
  typedef struct { uint8_t track; uint8_t location; uint8_t lane; } session_focus_t; // lane meaningful on a kit cell
  // Walks `detents` stops: a track whose bit is clear in kit_mask is one stop, a kit track KIT_LANES
  // stops (AC..RC). Entering a kit moving right lands on AC, moving left on RC.
  // Clamped at (0, first stop) and (7, last stop).
  void session_walk_stops(uint8_t *track, uint8_t *lane, int8_t detents, uint8_t kit_mask);
  uint8_t session_kit_lane(uint8_t track);        // remembered lane; KIT_LANE_BD after session_init
  void session_set_kit_lane(uint8_t track, uint8_t lane);
  ```
  `session_move_focus(int8_t detents)` builds the kit mask from the type each track would be edited in (the focused track: `focus.location`; others: their queued-or-current location), walks, keeps `focus.location` when the track did not change and otherwise takes the new track's queued-or-current location, and records the lane as remembered. `session_pad(...)` with `move_focus` on a kit cell sets the lane to `session_kit_lane(track)`.

UI rules: `ui_update` reads `e1d` with `encoder_get_detents(0)` in `UI_SESSION` and `UI_PATTERN_VIEW`, `encoder_get_delta(0)` elsewhere. A cell is a kit cell when its own type is KIT. On a kit cell, in the Session view and in the Pattern view, step keys follow the Session rule (short press acts on release through `kit_press` on the focused lane, steps beyond the length do nothing, a long press drops the key; Task 9 makes it open the editor); edits refresh playback. The Pattern view walks the same stops for `ui.selected_track` (kit mask from each track's type at `active_pattern`) with the lane remembered per kit track. A step press acts on the cell **and lane** focused when it was made, as the Session view already does for cells with `ui.step_cell[]` (session-panel commit a86ef4a); the Pattern view on a kit binds its presses the same way. Today's end-of-pass rule (a screen change drops every gesture) stays. Step LEDs in both views show the focused lane (`kit_get != 0` within the length) with the playhead blink as today. Non-kit cells behave exactly as before.

- [ ] **Step 1: Write the failing tests:**
  - `test_session.c`: `test_walk_stops_across_kit` (kit mask `1 << 2`: from (1,0) +1 -> (2, AC); +11 -> (2, RC); +1 -> (3, 0); -1 -> (2, RC); +100 from (0,0) -> (7,0); -100 -> (0,0)); `test_pad_on_kit_uses_remembered_lane` (BD after init; walk to SD, leave, pad on the kit column -> lane SD); `test_lane_walk_keeps_location` (Song mode, pad focuses a non-playing kit cell, one detent -> lane changes, location unchanged).
  - `test_ui.c`:
    - `test_kit_step_cycles_on_release`: focus on SD of a kit cell; press/release step 3 three times -> lane values 1, 2, 0; `step_t` of that cell unchanged.
    - `test_kit_ac_toggle_and_ch_clears_oh`; `test_kit_step_beyond_length_does_nothing`.
    - `test_held_step_stays_on_its_lane`: snapshot every kit word and `step_t` of the cell; press step 2 on SD, one TRACK detent (now LT), release -> SD at step 2 is 1, every other lane and `step_t` unchanged, the focus is on LT; a long press made on SD and firing after the detent opens the kit editor (Task 9) or, until then, changes nothing on LT; a press made on the kit and released after a move to another track still edits the kit's SD.
    - `test_eight_detents_move_eight_stops`: from track 0 with a kit on track 1, `deltas[0] = 8` in the Session view -> focus (1, lane 7 = CP); the same in the Pattern view.
    - `test_pattern_view_on_kit_edits_lane`: TRACK press from the Session view on a kit -> Pattern view; press/release a step -> the lane cycles, `step_t` unchanged; one detent moves the lane.
    - `test_step_leds_show_focused_lane`: Session and Pattern view masks equal the lane's non-zero steps within the length.
    - `test_mixed_type_track_uses_cell_type`: track 1 SYNTH in location 0 and KIT in location 31 -> the Session view on location 0 toggles `step_t` and walks one stop; on location 31 it edits lanes.
    - `test_synth_cell_unchanged`: on a SYNTH cell the Session view toggles `step_t` on release, the Pattern view on press (on release since the merge with hold-step editing), `kit_steps` untouched, LEDs from `step_t`.
- [ ] **Step 2:** they fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** host and sanitizer tests pass.
- [ ] **Step 5: Commit** "Walk kit lanes with TRACK and edit kit steps on the panel".

---

### Task 9: Kit step editor, track config for kits, OLED

**Files:**
- Modify: `src/ui.c` (new `UI_KIT_STEP_EDIT` and its handler, long-press entry, `handle_pattern_view` TRACK press on a kit, `handle_track_config`, `handle_pattern_select` screen, `render_session_view` and the render dispatch), `src/display.c`, `include/sequencer.h` (display prototypes)
- Test: `test/test_ui.c`

**Interfaces:**
- Consumes: Tasks 1, 5, 8.
- Produces (display):
  ```c
  // kit_lane NULL on a non-kit focus; strip from kit_lane_glyphs; kit_playhead 16 = none
  void display_draw_session_view(uint8_t bank, uint8_t common_row,
                                 uint8_t focus_track, uint8_t focus_location, uint8_t focus_length,
                                 float bpm, launch_quant_t quant, uint8_t bar_step,
                                 const char *kit_lane, const char *kit_strip, uint8_t kit_playhead);
  void display_draw_kit_lane_view(uint8_t track, uint8_t location, const char *lane,
                                  const char *strip, uint8_t playhead);           // Pattern view on a kit
  void display_draw_kit_step_edit(uint8_t track, uint8_t step, uint32_t word, uint8_t cursor_lane);
  void display_draw_kit_config(const track_config_t *tc, const kit_config_t *kc,
                               uint8_t track, uint8_t param);
  ```

Kit step editor (spec "Kit step editor"): opened by a long press on a kit lane (Session or Pattern view) or by TRACK press in the Pattern view on a kit cell; `ui.edit_step` = the step, the lane cursor starts on the focused lane; `ui_update` reads `e1d` by detents in `UI_KIT_STEP_EDIT` too; TRACK detents move the cursor (clamped 0..11); each VALUE event moves the value by exactly one step in its sign, whatever its size (0..3, AC 0..1, clamped), through `kit_edit`; step keys select another step within the length; VALUE press returns to the opening screen; nothing touches `step_t`. Track config: TYPE cycles SYNTH -> DRUM -> KIT -> SYNTH, KIT skipped with the notify `MAX 2 KITS` when `seq_set_track_config` refuses. Kit parameter list: TYPE, CHANNEL, GATE, FLAM (`n` pulses and its ms at the current tempo), VEL NORM, VEL ACC, VEL MAX, NOTE BD..NOTE RC; OUTPUT and CV hidden; CHANNEL/GATE/TYPE through `seq_set_track_config`, the rest through `seq_set_kit_config`. The pattern-select screen shows `FLAM DROP <n>` when `flam_drops > 0`.

- [ ] **Step 1 (a): Editor — failing tests:**
  - `test_long_press_on_kit_lane_opens_kit_editor`: mode `UI_KIT_STEP_EDIT`, step and cursor lane set; `deltas[1] = 1` three times -> note, accent, flam; `deltas[1] = 8` once from off -> note (one step); a TRACK detent moves to the next lane; setting CH clears OH; VALUE press -> back to `UI_SESSION`; `step_t` unchanged.
  - `test_pattern_view_track_press_on_kit_opens_kit_editor`: VALUE press returns to `UI_PATTERN_VIEW`; the note editor still opens on a SYNTH cell.
- [ ] **Step 2 (a):** implement; tests pass.
- [ ] **Step 3 (b): Track config — failing tests:**
  - `test_track_config_kit_params`: TYPE to KIT on track 1; VEL NORM, FLAM and NOTE SD edits land in `g_seq.kit_config[1]` with the clamps; OUTPUT/CV not reachable.
  - `test_track_config_third_kit_refused`: kits on tracks 1 and 4; TYPE on track 6 never shows KIT, the notify reads `MAX 2 KITS`.
- [ ] **Step 4 (b):** implement; tests pass.
- [ ] **Step 5 (c): Screens — failing tests:**
  - `test_render_kit_session_screen`: Session view focused on SD, then on CH with different lane contents -> the framebuffers (`ssd1309_get_framebuffer()`) differ; with the transport playing, the frame differs between two playhead positions only inside the strip row.
  - `test_render_kit_editor_and_lane_view`: `ui_render()` in the kit editor and in the Pattern view on a kit draws a non-empty frame that changes when the edited value changes.
  - `test_pattern_select_shows_flam_drops`: with `flam_drops` 0 and 1 the pattern-select frames differ.
- [ ] **Step 6 (c):** implement; host, sanitizer and firmware builds pass.
- [ ] **Step 7: Commit** "Add the kit step editor, kit track config and kit screens".

---

### Task 10: Documentation, final checks and bench acceptance

**Files:**
- Modify: `README.md`, `ARCHITECTURE.md`, `JITTER_MEASUREMENT.md`, `HARDWARE_STATUS.md`

- [ ] **Step 1: Documentation** per the spec's "Documentation" section: README (the kit track, setting up a 909, the kick on its own DRUM track with the BD lane empty, the kick's accents and velocity, drums on the leftmost columns); ARCHITECTURE.md (kit data, prepared kit rows, pulse order, TX queue with running status, real-time bytes and Clock ahead of note bytes, the 192-byte flam reserve, flash layout v6, two-kit limit); JITTER_MEASUREMENT.md (the two-dense-kit load case with flams and the 20 us budget); HARDWARE_STATUS.md (bench items with pass criteria: note map; three velocity levels audible; running status and Note On velocity 0 understood by the 909; flams at 140-150 BPM; GPIO 7 max ISR duration with two dense kits within +20 us of a baseline taken from a build of the base commit of this branch; wire Clock interval no worse than that baseline; longest Core 0 interrupts-off window while playing under 100 us; UI loop time with the per-byte UART interrupt). Every bench item not measured is marked `NOT VERIFIED`.
- [ ] **Step 2: Final checks:** host tests, sanitizer build and both firmware builds pass; record `arm-none-eabi-size -A build/fw1/rp2040_sequencer.elf` `.data` + `.bss` in HARDWARE_STATUS.md (the spec expects about 108 KB with the grown copy buffer).
- [ ] **Step 3: Commit** "Document the kit track".
