# Clips up to 512 Steps, Clip Ends and Per-Step Trig Conditions — Design

Date: 2026-11-20. Base: `master` at f8a8737 (64 tracks, save format v7, the
Pico 2 and Rev B builds). This document replaces the design of 2026-11-17
(64-step clips on the eight-track firmware), which no longer fits `master`; see
"Why the earlier design is replaced".

Decided by the project owner:

1. Clips of **1 to 512 steps** (32 bars), adjustable, on all 64 tracks.
2. A long clip holds both **repeating loops and through-composed passages**
   (different bars in a row, played once).
3. A clip has an **end behaviour with a repeat count**: loop, or play N times
   and stop, or play N times and go to another clip of the same track.
4. The clip's settings live on a **CLIP screen opened by selector button 16**;
   the FILL mode is that screen's last row.
5. Pool space comes back by **explicit clearing** (halving drops the bars
   beyond, a CLEAR action on the CLIP screen); switched-off steps keep their
   notes as today.
6. Carried over from 2026-11-17: per-step **trig conditions** (X%, A:B, FILL,
   !FILL, 1ST, !1ST, LST, !LST); manual FILL from a **sustain pedal (CC64)** on
   the MIDI input; the pads show the clip in the **Pattern view and both step
   editors**, where a pad press picks the step.

The end-behaviour rules below were played back to the owner and not
corrected. The owner approved the design and its plan on 2026-11-20
(`docs/superpowers/plans/2026-11-20-long-clips.md`). Everything marked "my
call" stays open to review in the pull request.

Builds on: `2026-11-16-session-panel-design.md`, `2026-11-16-drum-kit-track-design.md`,
`2026-11-16-hold-step-edit-design.md`, `2026-11-17-pad-keyboard-design.md`,
`2026-11-17-midi-keyboard-entry-design.md`.

## Why the earlier design is replaced

The 2026-11-17 design stored 64 steps of both kinds for every clip ("dense")
on the eight-track firmware. Since then `master` grew to 64 tracks:

- Dense storage is impossible: 64 steps x 64 tracks x 32 locations is about
  0.8 MB, 512 steps several MB; the RP2040 has 256 KiB, the RP2350 520 KB.
- The RP2040 build of f8a8737 already uses 241,336 bytes of RAM (`.data` +
  `.bss`); about 19 KB are free.
- Its save is format v7 in a 192 KiB slot at 0x0B0000, below the 1 MiB MIDI
  Song slot; the earlier design's region at 0x1C0000 now lies inside that slot.

So steps are now allocated per clip from a shared pool. With a pool a 512-step
clip costs little more than a 64-step one: the limit is the material of all
clips together, not the length of one. This reverses the owner's earlier
choice of dense storage, as forced by the 64 tracks, and with it one earlier
anti-goal: an edit can now be refused when the pool is full.

## Intent

A clip is 16 steps today. Played live, a Goa trance part changes its clip
almost every bar. The owner's test case is the eight MIDI songs of a
commercial pack laid out on the pads (session "hallucinogen-pads"): with
16-step clips a player needs about 4.6 pad presses per bar to follow a song.
With clips that hold whole loops and whole passages, about 1.2.

The second half is variation inside a clip: a step that plays every second
pass, only in a fill, only the first or the last time, or by chance.

## Clip demand of the test songs

Measured with the draft importer (`sim/web/midi_clips.js` on the spike branch
`claude/hallucinogen-pads`, not on `master`): each part becomes device tracks,
then each track is cut into clips. "Loops" are blocks of 1-32 bars repeated two
or more times in a row; "passages" are runs of non-repeating bars. Identical
clips on a track count once. Script and output:
`~/.claude/polls/20261120-022653-longclips512/` (not in git).

| Length cap | Pad presses per bar, loops only | Loops and passages | Most clips on one track | Bars, heaviest song |
|---:|---:|---:|---:|---:|
| 16 | 4.47 | 4.60 | 86 | 409 |
| 64 | 1.99 | 1.56 | 33 | 569 |
| 128 | 1.77 | 1.28 | 22 | 597 |
| 512 | 1.73 | 1.20 | 21 | 613 |

At 512 steps every track of every song fits the 32 locations it has, and the
heaviest song needs 613 bars of 16 steps.

## Goals

1. A clip (one track at one location) is 1 to 512 steps long. The 16 step
   buttons show one bar of 16 steps.
2. The view follows the playhead by default; choosing a bar, or holding a step,
   pins it. The OLED shows the bar being edited and the bar being played.
3. Lengthening can duplicate the clip into the new steps (double), and the
   length can be halved.
4. A clip ends by looping, or after N plays (1-99) by stopping its track or by
   starting another location of its track.
5. In the Pattern view and both step editors the 8x8 pads show the whole clip:
   one pad per step up to 64 steps, one pad per bar above.
6. Every step has a trig condition (codes of 2026-11-17, unchanged). FILL comes
   from a sustain pedal, a latched setting, or automatically every N bars.
7. Room is kept for ratchets and micro-timing: a clip may later get a third
   layer (a 4-byte record per step, bars from the same pool); the v8 clip
   record already carries that layer's bar mask, zero for now.
8. Saves v7 (64 tracks) and v4, v5, v6 (eight tracks) still load.
9. The RP2040 and RP2350 builds have the same capacity, so a save made on one
   loads on the other.

## Non-goals

- Ratchets and micro-timing themselves.
- Follow actions beyond the three end behaviours: no chance, no "any/other"
  targets, no targets on another track.
- More than 32 locations per track, or more than 256 song entries.
- Probability combined with another condition on one step; per-lane kit
  conditions; PRE, NEI and other Elektron conditions (codes reserved).
- A save that survives power loss during the save (one slot, as now).
- Shorter `seq_lock` sections for song edits (they move up to 16.5 KB today; a
  separate follow-up).
- Saves on the Rev B's microSD.

## Anti-goals

- A clip that plays differently after the update: a migrated save, at lengths
  up to 16, with LOOP and no conditions, plays note for note as before.
- A refused edit that changes anything: when the pool is full the whole
  operation is refused and the clip stays as it was.
- A type change that erases steps: both step kinds stay, as now.
- A held step that changes bar under the finger.
- A bar-boundary spike in the timer interrupt: playing a long clip costs the
  same at every step.
- A save that cannot be written back: every state the pool can hold fits the
  save slot.

## Constraints and measurements

- **RAM**, f8a8737 RP2040 Release build (`arm-none-eabi-size -A`): `.data`
  15,180 + `.bss` 226,156 = 241,336 bytes of 256 KiB main SRAM; `.heap` 2,048
  and two 2 KiB stacks (scratch banks) separate. Largest symbols: `g_seq`
  149,640 (32 `pattern_t` of 4,108 bytes = 131,456), `prepared_patterns` 29,696,
  `held` 16,384 (MIDI Song player), `prepared_revision` 8,192,
  `prepared_playback` 7,424.
- **Struct sizes** with `arm-none-eabi-gcc -mcpu=cortex-m0plus` (enums are one
  byte on this target by default): today's `pattern_t` 4,108 (calibrated); the
  proposed `step_t` 4, `block_t` 64, `clip_t` 8, `location_t` 458, the whole
  clip store with 2,048 blocks 162,624.
- **Host and simulator builds** use four-byte enums: `pattern_t` is 4,684 bytes
  there. A v7 image is therefore laid out differently in the simulator than on
  the board. The simulator page keeps no flash across reloads.
- **Flash map** (`midi_song_flash.ld`, `flash_storage.c`, `midi_song.h`):
  firmware below 0x0B0000; the save slot 0x0B0000-0x0E0000 (192 KiB,
  `STORE_ERASE` = the whole slot); MIDI Song 0x0E0000-0x1E0000; the
  eight-track save 0x1E0000-0x200000. The load takes v7 when valid, else the
  eight-track save (`flash_load_t`, `FLASH_LOAD_OLD_SAVE`).
- **Playback** today: each of the 64 tracks plays a prepared 16-step row.
  Tracks 1-8 copy theirs from `prepared_patterns`; tracks 9-64 snapshot and
  prepare theirs inside the timer interrupt whenever the location or its
  revision changes (`load_prepared_track()`), so a SCENE prepares up to 56 rows
  in one pulse.
- **Song mode**: an entry is `repeats` bars of 16 steps; a track whose location
  does not change keeps playing through entries.
- **Song edits** (`song_insert_patterns()`, `song_remove()`, `song_move()`)
  `memmove` up to 255 x 65 bytes under `seq_lock`.
- **Narrow types**: `transport_t.current_track_step`,
  `transport_snapshot_t.track_step`, `ui.edit_step`, lengths and playheads in
  the UI and display API are `uint8_t`; `locate_song()` narrows a position to
  `uint8_t`.
- **Legacy readers** `pattern_v5_t`, `pattern_v6_t` and `old_song_t` embed the
  live `step_t`, `track_config_t` and `song_end_mode_t`; a 4-byte step would
  break v4-v6 loads unless they get frozen copies.
- **Selector buttons**: 9 save, 10 load, 11 and 12 length -/+, 13 reset the
  jitter counters, 14 MIDI Song player, 15 clock source; 16 is free.
- **"Page"** on `master` means eight columns of tracks (`sequencer_page_clips()`,
  `panel_first_track()`, `ui.midi_page`).
- The MIDI receive interrupt reads with `uart_getc()` (`midi_out.c`), which
  drops the UART error bits.
- The simulator erases a 4 KiB sector in 40 ms and programs a page in 0.4 ms
  (`sim/core/hal.c`); the artist waits 2.6 s after SAVE. No board has been
  assembled: verification is host tests, the firmware builds and `sim/check.sh`.

## Approaches considered

Codex, DeepSeek and Grok were polled independently on 2026-11-20 (Gemini failed:
no credits). The brief listed the options without a preference; the reviewers
could read the 2026-11-17 design.

| | Storage | Verdict |
|---|---|---|
| S1 | pool of 16-step bars, linked per clip | DeepSeek's and Grok's first choice |
| **S2** | **pool of 64-byte blocks; a clip's root is its first bar or a table of its 32 bars** | **Codex's first choice; chosen** |
| S3 | one contiguous run of steps per clip | moves clips while playing; rejected by all |
| S4 | dense first bar per clip, pool for the rest | 131 KB before the first note; rejected |
| S5 | a few 512-step slots | too few for 180 clips a song; rejected |
| S6 | 16-step clips chained by end actions | 86 clips on one track, 32 locations; rejected |
| S7 | long clips in flash, read in place | flash cannot be edited while playing; rejected |

S2 over S1 (my call): any bar is reached in two reads, so the timer, a jump to
a song position and the UI never walk a chain; an empty bar inside a long clip
costs nothing (21 % of the songs' bars are empty); one block type serves bars,
kit bars, bar tables and later a ratchet layer. The price is one table block
per clip longer than 16 steps.

Playback: Codex and DeepSeek read one step word per track and step directly;
Grok kept a prepared window per bar, prepared ahead by core 0. Direct reads
(chosen) remove the prepared rows (45,632 bytes) and the bar-boundary work.

End behaviour: all three proposed loop / stop / go-to; DeepSeek and Grok with a
play count, Codex without. The owner chose the count.

## Data model

### Constants

| Constant | Value | Meaning |
|---|---:|---|
| `MAX_CLIP_STEPS` | 512 | the length range is 1..512 |
| `STEPS_PER_BAR` | 16 | one bar on the step buttons; also the Song entry and launch bar |
| `CLIP_BARS` | 32 | `MAX_CLIP_STEPS / STEPS_PER_BAR` |
| `POOL_BLOCKS` | 2048 | blocks in the pool, on both chips |
| `BLOCK_NONE` | 0xFFFF | no block |
| `ROOT_TABLE` | 0x8000 | tag bit of a root that names a bar table |

`STEPS_PER_PATTERN` goes; every use becomes one of these, or the literal 16 in
the frozen legacy types.

### The step and the block

```c
typedef struct {
    uint8_t note;       // 0-127
    uint8_t velocity;   // 0-127
    uint8_t flags;      // ACTIVE, ACCENT, SLIDE, TIE; bits 4-7 zero (reserved)
    uint8_t cond;       // trig condition code, 0 = always
} step_t;               // 4 bytes, read and written as one aligned word

typedef union {
    step_t   step[16];  // a bar of SYNTH/DRUM steps
    uint32_t kit[16];   // a bar of kit words: lanes in bits 0-23, condition in 24-31
    uint16_t bar[32];   // a bar table: block of each bar, or BLOCK_NONE
} block_t;              // 64 bytes
```

A kit word keeps its layout; its free bits 24-31 hold the kit step's condition
(today's validator requires them to be zero, so no v7 save uses them).

### The clip and the location

```c
typedef enum { CLIP_END_LOOP, CLIP_END_STOP, CLIP_END_NEXT } clip_end_t;

typedef struct {
    uint16_t root;      // BLOCK_NONE, bar 0's block, or ROOT_TABLE | table block
    uint16_t length;    // 1-512
    uint8_t  end;       // clip_end_t
    uint8_t  plays;     // 1-99; used when end != LOOP
    uint8_t  target;    // location 0-31; used when end == NEXT
    uint8_t  reserved;  // zero
} clip_t;               // 8 bytes

typedef struct {
    track_config_t tracks[NUM_TRACKS];   // as today, per location
    uint8_t        swing;
    char           name[9];
} location_t;           // 458 bytes on the RP2040
```

`sequencer_state_t` replaces `patterns[MAX_PATTERNS]` with:

```c
location_t locations[MAX_PATTERNS];
clip_t     clips[MAX_PATTERNS][NUM_TRACKS];
uint16_t   kit_root[MAX_PATTERNS][KIT_TRACK_SLOTS];  // kit layer of tracks 1-8
block_t    pool[POOL_BLOCKS];
```

- A root is `BLOCK_NONE` (the clip stores nothing), a data block (the clip
  stores only bar 0), or `ROOT_TABLE | t` (block `t` is a table; `bar[b]` is
  bar `b`'s block or `BLOCK_NONE`). The note layer and the kit layer of tracks
  1-8 each have a root; both share the clip's length, end and plays.
- **Bar `b` of a layer** is: nothing when the root is `BLOCK_NONE`; the root's
  block for `b == 0` when the root is a data block; `table.bar[b]` otherwise.
  A missing bar reads as 16 zero steps (inactive, note 0, ALWAYS).
- **Writing** a step that makes a missing bar non-zero allocates its block; a
  write into bar `b > 0` of a layer with a data-block root first allocates a
  table (entry 0 = the old root). A block whose 64 bytes become zero is freed;
  a table whose entries are all `BLOCK_NONE` but entry 0 turns back into a
  data-block root (my call: no table is kept for nothing).
- **Hidden bars**: steps beyond the length stay stored, as today's hidden
  steps 13-16 of a 12-step clip do; they come back when the clip is
  lengthened. They occupy the pool.
- **How note bars are freed: explicitly** (owner's choice). A switched-off
  step keeps its note and velocity, and switching it on again brings them back
  (`pattern_toggle_step()`); no panel action zeroes a step, so a note bar, once
  used, keeps its block. That stays as it is. Blocks come back by:
  - **halving** on the CLIP screen, which frees every bar that lies wholly
    beyond the new length (steps beyond the length inside the last kept bar
    stay, as today's hidden steps);
  - **CLEAR** on the CLIP screen, which frees both layers of the clip and keeps
    its length, end, plays and target;
  - a kit bar returning to all zero (a lane cycled to OFF), and copying an
    empty clip over a used one.
  Shortening with selector buttons 11 and 12 frees nothing, as today's
  shortening loses nothing. The rejected alternative freed a bar automatically
  once no step in it was active, at the price of switched-off steps forgetting
  their notes.
- The location's default `length` and `track_length[]` go: every clip has its
  own length. Migration resolves "0 = the location's default".
- The pool's free blocks form a list threaded through the blocks themselves
  (first `uint16_t` = next free); no extra RAM. Core 0 alone allocates and
  frees, outside `seq_lock`. An operation that needs several blocks (double, a
  clip copy, a migration) counts them first, takes them all, fills them while
  they are still unreachable, then publishes roots, tables and the length in
  one short `seq_lock` section; the blocks it replaced are freed after that
  section.
- **Copying a clip** (a pad held, then another pad of the track) copies both
  layers, the length, end, plays and target, not the routing or the stored
  mute, as today; it is refused with `POOL FULL` when the copy needs more
  blocks than are free.

### Budget

| RP2040, bytes | Today | After |
|---|---:|---:|
| `.data` + `.bss` | 241,336 | about 232,000 |
| of which clip storage | 131,456 (32 `pattern_t`) | 162,624 (locations, clips, kit roots, pool) |
| of which prepared rows, revisions, ids | 45,632 | 0 |
| new counters (pass, residues, idle, song lookahead, FILL, seed) | — | about 1,600 |
| new code in RAM (estimate) | — | about 3,000 |
| free main SRAM (after the 2 KiB heap) | about 18,800 | about 28,000 |

The RP2350 build has the same layout and about 290 KB free. The pool has the
same 2,048 blocks there (my call): the save slot bounds what can be saved
(below), and one size means every save loads on both chips. The heaviest test
song needs 613 bars plus at most 180 tables, under 800 of the 2,048 blocks.

## Playback

### Reading a step

The prepared rows (`prepared_track_pattern_t`, `prepared_patterns`,
`prepared_playback`, `prepared_revision`, `playback_revision`,
`playback_pattern`, `snapshot_row_locked()`, `prepare_row()`,
`load_prepared_track()`) go. At each step boundary, for each track, under the
playback lock as today and inside one `seq_lock` section per track, the timer
copies:

- the playing location's `tracks[track]` (7 bytes) and the clip's `clip_t`
  (8 bytes);
- the step at the track's position: the root, at most one table entry and one
  4-byte word; for a kit cell the kit word through `kit_root` and
  `kit_config[track]` (15 bytes).

A position at or beyond the length read in that same section reads as an
inactive step, so a clip shortened between two sections never plays a hidden
step. The timer then releases `seq_lock` and decides from the copy, exactly as
the prepared row did: the drum note for a DRUM cell, velocity 127 for an
accent, nothing for a stored mute, the kit's note and velocity tables for a
kit. Gate pulses are `(pct * 24 * 5243) >> 19` (equal to `pct * 24 / 100` for
10-100, checked in a test), with no division.

- **Kind change**: the timer remembers per track the kind it last played; a
  copy of the other kind first releases what the old kind left sounding, as
  `load_prepared_track()` does now.
- **Edits** under `seq_lock` are complete before the timer can copy them; an
  edit is heard the next time its step comes up. Nothing needs a revision.
- **Freeing**: the timer holds no block index across sections, so a block that
  core 0 unlinks under `seq_lock` can be freed at once after that section.
- **`sequencer_refresh_track_playback()`** keeps its callers and only
  recomputes the cell's bit in `clip_mask` (an active step, or a kit lane hit,
  within the length), scanning the clip's blocks on core 0.
- **Placement**: everything the timer calls for this is in RAM (the step read,
  the length and kind tests, condition evaluation, the LST distance); fields
  are copied one by one, not by struct assignment that may call `memcpy` in
  flash. The check is relative and done on the ELF: against a baseline taken
  before the change, the list of flash functions and division helpers the
  timer reaches grows by none.
- **Cost**: 64 short lock sections per step (today `load_prepared_track()`
  already takes 64), about ten loads each; no extra work at a bar boundary.

### Counters and state

Per track, in the transport, reset by Start:

- **`pass[track]`** (`uint32_t`): completed passes of the playing clip; 0 in
  the first. It grows when the position wraps to step 0, including the wrap
  forced when the clip is shortened below the playhead. It is reset when the
  track's location changes (a queue applied, an end action, a Song entry with
  another location), and by Start and a song position jump. A launch of the
  location already playing only clears the queue and leaves `pass` and the
  playhead. With it the residues `pass_mod[B]` = `pass % B` for B = 2..8, stepped
  with `pass`, so A:B needs no division.
- **`idle`** (`track_mask_t`): tracks whose clip has ended by STOP. An idle
  track plays nothing and keeps its location; its playhead is "none" (0xFFFF).
- **`bar_count`**: completed 16-step bars since Start, for EVERY N fills.

Positions become `uint16_t` (`current_track_step`, the snapshot's
`track_step`, the UI's edited step and playhead); "none" is 0xFFFF.

### Launches

Unchanged: a pad or SCENE queues a location; it applies at the launch point of
the launch quantization (BAR, BEAT, STEP, TRACK LOOP = when the track has
played its clip's length). Under BAR, BEAT and STEP a **looping** clip starts
at `(global step % 16) % length`, so a long clip launched on a bar starts at
its step 1 and one launched on a beat starts within its first bar.

New:

- A clip whose end is STOP or NEXT **always starts at step 1** (my call,
  played back to the owner: a passage plays whole). Its pass is 0.
- An idle track's TRACK LOOP launch point is the next step.
- The pad of an idle track's own location launches it again (a playing clip's
  own pad still only cancels a queue to another slot, as today). For an idle
  track a queue to its own location is a change: it restarts at step 1 with
  `pass` 0.
- SCENE treats an idle track on its row as not playing that row (my call,
  following the pad rule): the track is queued and restarts. Today's rules stay
  otherwise: tracks playing the row keep playing, and a SCENE pressed while any
  track is queued to its row cancels every queue.

### Clip ends

At the boundary where a clip would wrap to step 0, if its end is STOP or NEXT
and this wrap completes its `plays`-th pass or a later one (`pass + 1 ≥ plays`,
so lowering PLAYS during play takes effect at the next wrap):

1. **A queue applies first.** If the track has a queued launch due at this
   boundary, the queue applies and the end action is dropped.
2. **STOP**: the track goes idle at this boundary. Notes are released as for a
   rest; a kit's sounding voices are released, its pending flam cancelled.
3. **NEXT**: the track starts its `target` location at step 1 at this same
   boundary, with `pass` 0, without waiting for the launch quantization. An
   empty target plays as an empty clip does today (silence, its own length,
   end and plays apply). A target equal to the clip itself restarts it.

At most one end action per track per boundary. Chains (A → B → C → A) are
allowed; each hop needs a whole clip to play, so a chain cannot spin within
one step.

- **MUTE** silences the output only: positions, passes and end actions go on,
  so a muted chain stays in time.
- **SCENE** queues every track; a queue beats an end action as above.
- **Song mode**: the song decides where a track goes. An end action never
  changes a location in Song mode: STOP and NEXT both make the track idle at
  the end of the clip's `plays`-th pass, until the song next changes the
  track's location. Song entries still last whole bars, so a passage the song
  places for exactly its length is replaced at the same boundary and its end
  action never runs.
- **Stop and Start**: Stop keeps locations; Start clears `idle` and `pass` and
  starts every track at step 1 of its location, as today.
- **Song Position Pointer** (external clock): tracks are placed as today
  (Pattern mode: `spp % length` on the current location; Song mode: steps since
  the location last changed, modulo its length), with `pass` 0 and no track
  idle (my call: end actions are not replayed through the skipped time).

### Song mode

Unchanged otherwise: entries of `repeats` bars; a track whose location does not
change keeps playing, so a 512-step clip runs on through 32 bars of entries
that keep its location; a track whose location changes restarts at step 1.

## Conditions

### Codes

As 2026-11-17, unchanged. The code is in `step_t.cond`, or in bits 24-31 of a
kit word. Codes never move; new values take reserved codes; a save with an
unknown code is rejected.

| Code | Condition | Plays when |
|---|---|---|
| `0x00` | ALWAYS | always (every migrated and new step) |
| `0x01`-`0x63` | X% (code = X, 1-99) | a random draw is below X |
| `0x64`-`0x6F` | reserved | |
| `0x70` / `0x71` | FILL / !FILL | FILL is / is not active |
| `0x72` / `0x73` | 1ST / !1ST | `pass == 0` / `pass != 0` |
| `0x74` / `0x75` | LST / !LST | this is / is not the step's last pass |
| `0x76`-`0x7F` | reserved (PRE, !PRE, ...) | |
| `0x80 \| (B−1)<<3 \| (A−1)` | A:B, 2 ≤ B ≤ 8, 1 ≤ A ≤ B | `pass % B == A − 1` |
| other `0x80`-`0xFF` | reserved | |

The panel's list, one item per VALUE detent, clamped at both ends (a table in
`ui.c`): ALWAYS, 90%, 75%, 66%, 50%, 33%, 25%, 10%, FILL, !FILL, 1ST, !1ST,
LST, !LST, 1:2, 2:2, 1:3, 2:3, 3:3, 1:4 ... 4:4, ... 1:8 ... 8:8 (49 items). A
code not in the list moves to the list's first item on the first detent.

### Evaluation

Once per step occurrence, at the step boundary, for an active step of an
audible track (a kit step with any lane set). One evaluation covers the whole
step; for a kit, all 12 lanes.

- **False is a rest**: the previous output is released as for an inactive step
  (a slide or tie from the step before ends here), no kit voice is hit and no
  flam is scheduled.
- **Probability** is a hash, not a stateful generator:

  ```c
  static inline uint32_t mix32(uint32_t x) {   // "lowbias32"
      x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
      return x;
  }
  h = mix32(mix32(seed ^ pass) ^ (track << 24) ^ (location << 16) ^ step);
  roll = ((h & 0xFFFF) * 100) >> 16;            // 0-99; the step plays when roll < X
  ```

  `track` 0-63, `location` 0-31, `step` 0-511. Golden values (seed, track,
  location, pass, step → h, roll): (1234, 0, 0, 0, 0) → 0x707A0DC2, 5;
  (1234, 0, 0, 0, 1) → 0x67FC9E59, 61; (1234, 0, 0, 1, 0) → 0x659059AB, 35;
  (1234, 3, 5, 2, 17) → 0x850D1A07, 10; (1234, 7, 31, 1000, 63) → 0xBDBCE268,
  88; (1, 0, 0, 0, 0) → 0x58F54975, 28. Over `i` = 0..9,999 with seed 1, track
  `i % 8`, location `(i / 8) % 32`, pass `i / 256`, step `i % 64`, X = 50 plays
  4,962 times. The seed comes from the timer at Start; host tests set it with
  `sequencer_set_seed()`, kept by every later Start until cleared.
- **A:B** reads `pass_mod[B] == A − 1`; **1ST** reads `pass == 0`.
- **FILL** is one value for all tracks, taken once per step boundary:
  `pedal_down || fill_mode == ON || (fill_mode == EVERY_N && (bar_count & (N − 1)) == N − 1)`,
  N = 2, 4, 8 or 16; EVERY 4 fills bars 4, 8, 12, counted from Start, not from
  a clip's launch (a fill lines up with the set's phrases).
- **LST** is true when the step will not come round again before the clip
  changes. With `L` the clip's length and `D` the number of boundaries until
  the track's clip changes, if a change is known (`D = 1`: the next boundary
  plays something else): LST is true when `D ≤ L`.
  - A queue to **another** location (a queue to the playing one is no change):
    BAR, the next multiple of 16 global steps; BEAT, of 4; STEP, the next step;
    TRACK LOOP, the wrap (`D = L − s` at position `s`).
  - **An end action**: a clip in its last pass (`pass + 1 ≥ plays`, end STOP
    or NEXT) changes at its wrap, `D = L − s`. In Song mode the same holds, as
    the track goes idle there.
  - **Song mode**: `song_change_bars[track]` (1-32 = the location changes at the
    end of the current bar or of the 2nd ... 32nd bar from now; 0 = not within
    32 bars) is computed once per bar inside `advance_song_position()` and again
    by every song edit, with the same rules as the song walk (0-repeat entries
    skipped, LOOP wrapping, the end in STOP counting as a change). Then
    `D = (song_change_bars − 1) * 16 + 16 − global % 16`. Thirty-two bars cover
    the longest clip. The walk visits at most 32 bars of entries and compares
    64 locations per entry: at most 2,048 byte compares, once per bar.
  - No change known: LST false, !LST true. A queue is applied at a boundary
    before that boundary's step is evaluated, so under STEP, and with a 1-step
    clip under TRACK LOOP, LST never fires. Stop and Panic cannot be foreseen.

### FILL sources

- **Pedal**: the MIDI input parser reports Control Change 64 on any channel.
  Down at 64 or more, up at 31 or less; values between keep the state. Not
  passed to the MIDI output; never writes a step. An input gap, All Notes Off,
  All Sound Off and a panic put the pedal up; if that lifts a pedal that was
  down, the OLED shows `FILL OFF`. A pedal held through a gap stays up until it
  is released and pressed again. Stop and Start leave it as it is.
- **UART errors count as a gap**: the receive interrupt reads
  `uart_get_hw()->dr`; with OE, BE, PE or FE set it counts a lost byte and drops
  it, so a lost "pedal up" can end a fill early but never leave one on. This
  also protects passed-through notes from a lost Note Off.
- **FILL mode**: the last row of the CLIP screen, OFF → ON → EVERY 2 → EVERY 4 →
  EVERY 8 → EVERY 16, the same for all tracks; not saved, like the launch
  quantization. Every screen with the transport symbol draws it inverted while
  FILL is active.

## The panel

### Bars

"Page" stays the word for eight tracks. The 16 steps on the step buttons are a
**bar**.

Each track has a bar pin in UI RAM: FOLLOW (the default) or a bar 0-31. The
**shown bar** of a cell is computed once per UI pass:

1. while a step button pressed on this screen is down, the bar it was pressed
   on (one hold bar per screen; it also holds while an encoder button that
   joined the hold is down);
2. else a pinned bar, clamped to the clip's last bar `(length − 1) / 16`;
3. else, if the transport plays this location on this track, the playhead's
   bar;
4. else bar 0.

Every step press binds, at the press, to its cell (track, location, lane) and
to the absolute step `bar * 16 + button`, on every kind of cell. Taps, holds,
long presses, encoder edits and keys from the MIDI keyboard and the pad
keyboard all address absolute steps. The encoder targets that outlive a step's
release (`grid_enc_target`, `grid_enc_long_done`) keep their 16-bit masks and
gain the bar they were bound on (a hold never spans two bars), so pressing the
same button on another bar never retargets them. Buttons beyond the length
behave as today on every bar: ignored in the Session view and on kit cells;
the SYNTH/DRUM Pattern view and the note step editor edit the hidden step;
their LEDs stay dark. LED masks stay 16 bits, relative to the shown bar.

- **Pattern view, VALUE short with no step held** (no action today): FOLLOW →
  bar 1 → ... → last bar → FOLLOW, notice `BAR 2/32` or `FOLLOW`. On a clip of
  16 steps or less it shows `BAR 1/1`. The pads are the fast way on long clips.
- **Session view**: the step buttons show the focused cell's shown bar; the
  track's pin applies; VALUE short still cycles the launch quantization.
- **Step editors**: the shown bar is the edited step's bar. TRACK press in the
  Pattern view opens the editor on the first step of the shown bar.
- **Step LEDs**: the shown bar's active steps (on a kit, the lane's), dark
  beyond the length; the playhead blinks on its button only on the shown bar.
- **OLED**: every clip view (piano roll, drum grid, kit lane strip, Session
  focus line, step editors) shows a bar strip: one cell per bar of the length
  (up to 32 cells of 4 pixels), the shown bar solid, the playing bar blinking
  on 8th notes, a mark for FOLLOW or pinned. Step numbers are absolute (`S17`,
  `S512`).

### The pad overview

In the Pattern view and both step editors, while the pad keyboard is off (no
SYNTH step held), the pads show the clip:

- **Clips of up to 64 steps**: pad (column `c`, row `r`, row 0 at the top) is
  step `r * 8 + c`; bar `b` is rows `2b` and `2b + 1`. Beyond the length: off;
  an empty step dim in the track's hue, dim white on the shown bar; an active
  step half in the hue, an accent full; the playhead white, full. On a kit cell
  the overview shows what the step LEDs show (the remembered lane; in the kit
  editor the cursor lane; at its COND stop the steps with a condition).
- **Clips of 65 to 512 steps** (my call, played back to the owner): pad
  `r * 8 + c` is bar `r * 8 + c`, so 32 bars fill four rows. Beyond the last bar:
  off; a bar with no active step dim in the hue, dim white when shown; a bar
  with an active step half, with an accent (kit: an accent or flam on the shown
  lane) full; the playing bar white, full.
- **In a step editor** the edited step blinks white on its pad (bar pad on a
  long clip) in time with the step LED.
- **A press** within the length pins the track's bar to the pad's bar. In a
  step editor it also selects a step: the pad's step (up to 64 steps), or the
  edited step's button position in the pad's bar (above 64), clamped to the
  length. Selecting another step puts VALUE back on velocity; the kit editor
  keeps its cursor lane. A press beyond the length does nothing. It never
  launches, queues or copies a clip and is never a copy source; its release
  does nothing.

Routing of a pad press: the pad keyboard (a SYNTH step held), then the overview
(Pattern view and step editors), else launch or copy as today. SCENE, MUTE and
PLAY are unchanged everywhere. The artist's scores press pads only in the
Session view and as keys with a SYNTH step held, and never open a step editor.

### The CLIP screen

Selector button 16 opens it for the selector's track and location (owner's
choice). It is a list like the track settings: TRACK turns choose the row,
VALUE turns change it, TRACK press moves to the next track at the same
location, VALUE press returns to the selector.

| Row | Values | Notes |
|---|---|---|
| LENGTH | 1-512 | a number: fast turns accelerate as for the tempo |
| END | LOOP, STOP, NEXT | a choice |
| PLAYS | 1-99 | `—` and inactive while END is LOOP |
| TARGET | A1-D8 | `—` and inactive unless END is NEXT; choosing NEXT sets it to the next location (A8 → B1, D8 → A1) |
| FILL (ALL) | OFF, ON, EVERY 2, 4, 8, 16 | for all tracks, not saved |

On this screen step button 15 halves the length (`max(1, length / 2)`; the
bars wholly beyond the new length are freed), step button 14 held for half a
second clears the clip (notice `CLEARED`; a short press only shows `HOLD TO
CLEAR`, my call, as nothing else on the panel destroys a whole clip at one
touch) and step button 16 doubles it with copy (`min(512, 2 *
length)`; each new step `s` becomes a copy of step `s − length`, both layers,
conditions included). Their LEDs are lit; the OLED labels them (my call: the
step buttons are free on this screen). Selector buttons 11 and 12 keep
changing the length by one, now within 1-512. The screen shows the pool's use
(`POOL 812/2048`).

When an operation needs more blocks than are free (double, copy, the first note
in a new bar, a migration), it is refused as a whole and the OLED shows
`POOL FULL`.

### Editing a condition

- **Pattern view, SYNTH or DRUM cell, steps held**: VALUE short press switches
  what VALUE turns between velocity and condition (it counts as "with steps
  held" when the VALUE button joined a hold, in whichever order the step and
  VALUE are let go; the bar cycle runs only with no hold and no encoder
  targets). In condition mode a VALUE detent moves every held step one item
  along the list (an inactive held step is switched on first). TRACK still
  turns the note; TRACK press and long, VALUE long still toggle accent, slide
  and tie. The mode returns to velocity when every held step is released. The
  info line shows the lowest held step's condition (`S17 COND 1:4`).
- **Note step editor**: a tap on the button of the edited step switches VALUE
  between velocity and condition (today that press selects the same step). It
  is a tap when, at its press, the button was the edited step and it is
  released before the long press with no encoder turned or pressed, no key
  played and no overview pad pressed meanwhile. Holding that button still makes
  the pads a keyboard; its long press still toggles the step. Opening, leaving
  and selecting another step put VALUE back on velocity.
- **Kit step editor**: TRACK walks the lane cursor AC ... RC and one more stop,
  COND, where VALUE turns the kit step's condition (bits 24-31); there the step
  LEDs show the bar's steps that have a condition.
- The Session view does not edit conditions.

### Gesture map

Against `src/ui.c` at f8a8737:

| Where | Control | Today | Now | Kind of change |
|---|---|---|---|---|
| Pattern view, no hold | VALUE short | nothing | bar cycle | free gesture |
| Pattern view, steps held | VALUE short | nothing (cancels the taps) | velocity ↔ condition | free gesture |
| Pattern view | pad (no keyboard) | launch, or copy with a pad held | overview: pin bar | owner-approved 2026-11-17 |
| Note and kit step editors | pad (no keyboard) | launch, or copy with a pad held | overview: pick the step, pin bar | owner-approved 2026-11-17 |
| Note step editor | tap on the edited step | selects it again | velocity ↔ condition | no-op becomes an action |
| Kit step editor | TRACK past RC | clamped at RC | COND stop | new stop |
| Pattern selector | 16 | nothing | CLIP screen | owner-approved 2026-11-20 |
| Pattern selector | 11, 12 | ±1 within 1-16 | ±1 within 1-512 | range only |
| CLIP screen | all | — | rows; button 14 held clears, 15 halves, 16 doubles | new screen (CLEAR owner-approved 2026-11-20) |
| Any view | pad of an idle track's location, or SCENE of its row | (no idle state) | launches it again | new state |
| MIDI input | CC64 | ignored | FILL pedal | new input |

Unchanged on purpose: PLAY, SCENE and MUTE act on press; TRACK long (bank in
Session, back to Session in the Pattern view); VALUE long (selector; tie on
held steps); VALUE short in Session (launch quantization); both encoders long
(panic); a pad held 500 ms then another pad of the track (copy, on every screen
but the Pattern view and the step editors); held SYNTH step plus pad
(keyboard); selector buttons 9-15; TRACK LOOP's meaning.

### Session view lights

An idle track's location shows as a clip that is present but not playing. The
snapshot (`transport_snapshot_t`) gains the `idle` mask.

## Save format v8

### Layout

Same slot (0x0B0000, 192 KiB), same erase of the whole slot. Unlike v7 the
image is defined byte by byte, little-endian, so the board, the host tests, the
simulator and an importer on a computer write and read the same bytes.

| Offset | Content | Bytes |
|---|---|---:|
| 0 | header: `u32` magic `RBTH` at 0, `u8` version 8 at 4, three zero bytes, `u32` CRC32 at 8, `u32` payload length at 12, `u16` data blocks at 16, `u16` kit blocks at 18, zeros to 256 | 256 |
| 256 | 32 locations: for each of 64 tracks 7 bytes (type, MIDI channel, drum note, output mode, CV pair, gate %, muted), then swing, then name (9 bytes, zero-padded) | 14,656 |
| then | 2,048 clips, location-major: `u16` length, end, plays, target, `u32` note bar mask (bit `b`: bar `b` of the note layer is stored), `u32` extension bar mask (zero in v8) | 26,624 |
| then | 256 kit masks, location-major, slots 1-8: `u32` bar mask of the kit layer | 1,024 |
| then | song: 256 entries of 64 locations and repeats, `u16` length, end mode, name (9) | 16,652 |
| then | kit settings: 64 x (11 notes, normal, accent, max, flam) | 960 |
| then | note bars in clip order, each clip's set mask bits in rising order: 16 x (note, velocity, flags, condition) | 64 each |
| then | kit bars in the same order: 16 x `u32` | 64 each |

The fixed part is 60,172 bytes. Tables are not saved; the load rebuilds them
from the masks. With all 2,048 blocks as data the image is 191,244 bytes, below
the slot's 196,608. The extension mask is room for the ratchet and micro-timing
layer (Goal 7): the firmware that first fills it writes version 9 with this
layout. The CRC32 (as v7's `crc32_update`) covers bytes 256 to the
payload end.

### Validation

Before RAM is touched: magic, version, payload length; every length 1-512, end
0-2, plays 1-99, target 0-31, reserved bytes and every extension mask zero; the masks' bit count equals
the block counts; blocks plus the tables the load will need (one per layer with
a bar above 0) at most `POOL_BLOCKS`, else the load is refused with
`POOL FULL`; every step: note and velocity ≤ 127, flag bits 4-7 zero, a known
condition; every kit word: AC 0 or 1, a known condition in bits 24-31; kit
masks only on tracks 1-8 and at most two kit tracks across all locations; the
song and kit settings as v7 checks them; the CRC.

### Save

Transport stopped, MIDI output idle, as now. Core 0 serializes the image on the
fly through one 256-byte page buffer, twice: a pass computing the CRC and the
length, then (core 1 parked, interrupts off) the erase of the slot and a pass
programming the pages; the header last. No other buffer. In the simulator:
48 sectors at 40 ms and at most 748 pages at 0.4 ms, about 2.2 s against the
artist's 2.6 s wait (v7 today: about 2.15 s).

### Load and migration

The load probes the slot: version 8, else version 7, else the eight-track save
at 0x1E0000 (v4, v5, v6), as `master` does today with its fallback result.

- **v7** is read through frozen copies of today's types (`pattern_v7_t`,
  `song_v7_t`), compiled in the build's own ABI, so the board reads the board's
  v7 (4,108 bytes a location) and the simulator its own (4,684). Each clip gets
  its length (`track_length`, or the location's default when 0), its 16 steps
  as bar 0 when any byte is non-zero, its kit words likewise for tracks 1-8,
  end LOOP, plays 1, target the next location. Hidden steps are kept.
- **v4-v6** go through today's readers, with their step, track settings and
  song end mode frozen as `step_v3_t`, `track_config_v7_t` and a one-byte end
  mode, then the same conversion for eight tracks; tracks 9-64 stay empty.
- **Capacity**: a v7 image needs one block per non-empty bar, at most 2,304
  (2,048 note layers and 256 kit layers). An image needing more than 2,048 is
  refused with `POOL FULL` before RAM is touched (my call: a full v7 save would
  have nearly every clip of every track in use). No eight-track save comes near.

After an update, LOAD migrates; the first SAVE writes v8 over the v7. Firmware
without this change finds no valid v7 in the slot and falls back to the
eight-track save, if one is still there. Both go into `README.md` and the
manual's change list in the pull request.

### Importers

The importer on the spike branch writes v7 in the simulator's layout today. It
moves to v8, which needs no knowledge of the compiler's layout: a loop becomes
a LOOP clip, a passage a STOP clip with plays 1 (or NEXT to the passage that
follows on the same track), Song entries keep a long clip's location for all
its bars. That change belongs to the importer's own work.

## Simulator and CI

- `sim/core/hal.c` keeps the 192 KiB slot.
- `sim_dbg_step()` and `sim_dbg_kit_step()` take steps 0-511; `sim_dbg_step()`
  returns the condition in bits 24-31. New: `sim_dbg_clip_end(location, track)`
  (end | plays << 8 | target << 16), `sim_dbg_pass(track)`, `sim_dbg_idle()`,
  `sim_dbg_fill()`, `sim_dbg_pool_free()`.
- `sim/build.sh`: feature bit 32 when the firmware has the CLIP screen
  (`grep -q UI_CLIP_SETTINGS src/ui.c`); `sim/check.sh` and the page offer a
  `long` score on such firmware.
- The scores this firmware takes today stay green after every phase: with
  lengths up to 16, LOOP and no conditions the device behaves as today. Their
  checks keep reading 16 steps. The artist's `Model` grows to 512 steps and the
  clip ends for the new score.
- **The `long` score** (built on `pads`) adds to one song's preparation and set:
  - a lead clip doubled 16 → 32 on the CLIP screen, bar 2 opened with VALUE
    short and steps 17-32 edited with held steps and pad keys;
  - one overview pad press that opens bar 2 of another clip; the note step
    editor opened on the doubled clip and step 20 picked with a pad;
  - a 128-step clip made with three doublings, played across eight bars;
  - a two-clip chain: a fill clip set to NEXT after 1 play back to its loop,
    the loop set to NEXT after 3 plays to the fill; and a crash clip set to
    STOP after 1 play;
  - conditions: closing hats on FILL, a lead step on 1:2, a crash on 1ST; the
    pedal for one bar (CC64, as the `keys` score sends notes); FILL mode EVERY 8
    once.
- **Its checks**: memory up to each clip's length (conditions and ends
  included); on the wire, the 32-step clip's second half in the second bar, the
  128-step clip's eight bars in order, the chain's fill every fourth bar, the
  crash once and then silence, 1:2 steps every other pass, FILL steps only in
  the pedal and auto-fill bars, 1ST only in the first pass.
- `device_check.js`: a v8 round trip; the save time below the artist's wait; a
  v7 image (this build's layout) and a v6 image migrating.

## Tests on the host

Fixtures first: before anything changes, images written by today's code are
committed under `test/fixtures/`: v4, v5 and v6 (made with `-fshort-enums`, as
the legacy readers expect the RP2040's 459- and 972-byte records) and v7 in the
RP2040 layout (4,108 bytes a location), each with a note of what it holds. A v7
image in the host layout is made by the test itself from today's types before
the change lands, for the frozen-type reader.

Then, among others: struct sizes on `__arm__`; the pool (allocation, table
promotion and demotion, freeing on zero, exhaustion leaving everything as it
was, double and copy refused when short); v8 byte layout against a hand-written
image, round trip, every validation rule, the capacity refusal; the four
migrations; playback of 17-512 step clips, a length change while playing, phase
join of long loops, a STOP/NEXT clip starting at step 1, Song entries running a
long clip on; every end action with each quantization, a queue at the same
boundary, MUTE, SCENE, Song mode and SPP; kind changes; every condition code,
FILL, LST under each quantization, at an end action and in Song mode (32-bar
horizon, 0-repeat runs); false-condition rests after a slide, a tie and a kit
flam; the hash against the golden values; the gate formula over 10-100; the
pedal events and gaps; bar pinning, hold pinning, absolute binding; the
overview in both mappings; the CLIP screen; the condition gestures in both
release orders; the ELF check of the timer's flash callees against the
baseline.

## Risks

- **RAM** about 232 KB of 256 KiB on the RP2040. `arm-none-eabi-size` after
  each phase, recorded in HARDWARE_STATUS.md.
- **Timer time** with direct reads and conditions: estimated, not measured (no
  board). The relative ELF check guards the code path; the bench measurement
  stays the existing HARDWARE_STATUS item.
- **Pool use is invisible until full**: hidden bars beyond the length occupy
  blocks. The CLIP screen shows the count; a refused edit says `POOL FULL`.
- **Downgrade**: older firmware cannot read v8 and falls back to an eight-track
  save if one exists; documented.
- **Probability** draws depend on the seed taken at Start: two runs of a set
  differ on the device, not in the simulator.

## Left to the builder

The exact LED levels of both overview mappings, the OLED layout of the bar strip
and of the CLIP screen, the accessor names around the pool, the order of the
CLIP rows beyond the table above, and where the song lookahead is computed
within `advance_song_position()`, within the bounds stated here.

## Review history

- 2026-11-17: design for 64-step clips on eight tracks (dense storage),
  conditions and the pad overview; two model reviews and a critique (PR #18).
- 2026-11-20: the owner asked for up to 512 steps on the 64-track firmware;
  clip demand measured on eight songs; Codex, DeepSeek and Grok polled on
  storage, playback, end behaviour and the save format; the owner chose
  passages as well as loops, the repeat count, the CLIP screen on selector
  button 16, and explicit clearing. A builder check against the code
  reproduced the hash values, the gate formula and the size budgets, and led
  to the SCENE rule for idle tracks, the extension mask in v8 and the
  freeing question. Verified against the code: the prepared rows built in the timer
  interrupt, the song edits' `memmove`, the free kit-word bits, `uart_getc()`
  dropping UART errors, selector buttons 14 and 15 in use, the importer's v7
  in the simulator's layout.
