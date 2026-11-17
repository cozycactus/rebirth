# Clips up to 64 Steps and Per-Step Trig Conditions — Design

Date: 2026-11-17. Status: the approach and four choices below were made by the
project owner (relayed by the session that started this work): a 64-step
ceiling with dense storage, kit steps kept for all eight tracks, a manual FILL
from a sustain pedal on the MIDI input, and the pads of the Pattern view
showing the whole clip. Everything marked "my call" is open to review in the
pull request. Base: `master` at bcf6871. Revised the same day after two
independent reviews of the written spec and plan (see "Review changes").

**This design departs from the direction it was started with.** The brief
proposed a shared pool of steps because "64 steps everywhere is about 114 of
128 KB". That estimate counted 7 bytes per step (a `step_t` and a kit word) in
the save region as it is. Measured on today's build, dense storage fits once two
redundant RAM copies of the step data go (the save's copy buffer and the
prepared playback rows) and the save region grows from 128 to 256 KiB (the
firmware uses about 102 KB of the 2 MB flash). The pool is kept below as a
rejected approach with its numbers; it is the path if clips ever need 128 steps.

Builds on: `2026-11-16-session-panel-design.md` (pads, launch quantization,
clip copy), `2026-11-16-drum-kit-track-design.md` (kit storage, "a type change
erases nothing"), `2026-11-16-hold-step-edit-design.md`,
`2026-11-17-pad-keyboard-design.md`, `2026-11-17-midi-keyboard-entry-design.md`.
Research: `docs/research/similar-sequencers.md` in the main checkout (not in git).

## Intent

A clip is 16 steps. A Goa trance lead or acid line is often a 2- or 4-bar
phrase, so today it is two or four clips launched in turn by hand. The owner
wants Ableton-like clips: one clip as long as the phrase, its length adjustable.
The second half of the work is variation without longer clips: a step that
plays every second pass, only in a fill, only on the first pass, or by chance.
These are the two features the research ranked first, and both change the save
format, so they share one format version.

## Goals

1. A clip (one track at one location) is 1 to 64 steps long, set in the
   pattern selector. The 16 step buttons show one page of 16 steps.
2. The view follows the playhead by default; choosing a page, or holding a step,
   pins it. The OLED shows the page being edited and the page being played.
3. Lengthening can duplicate the clip into the new steps (Ableton's Double Loop).
4. In the Pattern view the 8x8 pads show the whole clip, eight steps a row, and a
   pad press opens its page.
5. Every step has a trig condition: X% probability, A:B, FILL, !FILL, 1ST,
   !1ST, LST, !LST. FILL is engaged by a sustain pedal on the MIDI input, a
   latched setting, or automatically every N bars.
6. Every step reserves room for a ratchet and a micro-timing offset, so adding
   them later needs no new format.
7. Saves of versions 4, 5 and 6 still load.

## Non-goals

- Clips longer than 64 steps (needs the pool; see Approaches).
- Ratchets and micro-timing themselves (only their bytes are reserved).
- A probability combined with another condition on one step; PRE, NEI and other
  Elektron conditions (codes are reserved for them).
- Per-lane conditions on kit steps: one condition covers the whole kit step.
- Quantised mutes and MIDI clock input (separate work, no format change).
- A save that survives power loss during the save (two slots; listed as an
  option, not built here).
- A change to Song mode's model: entries still last whole 16-step bars.

## Anti-goals

- A clip that plays differently after the update: a migrated save, at
  lengths up to 16 and with no conditions, plays note for note as before.
- An edit that fails because "memory is full". Every clip can be 64 steps at
  once.
- A type change that erases steps. Both step kinds stay for every track, as now.
- A held step that changes page under the finger.
- Timing jitter added to the timer interrupt: what it reads under `seq_lock`
  per step stays a short copy.

## Constraints and measurements

- Build of `master` bcf6871, Release, Pico SDK 2.2.0: code ends at
  0x10018F3C (about 102 KB of flash); `.data` 10,988 + `.bss` 106,516 =
  117,504 bytes of the 256 KiB main SRAM (the two 2 KiB stacks are in the
  scratch banks; the heap section is 2 KiB).
- In that RAM: `g_seq` 33,636 (of which the patterns are 31,104),
  `pattern_buf` 31,232 (the save's page-padded copy), `prepared_patterns` 29,696
  (256 rows of 116 bytes) plus 928 bytes of rows in play and 1,024 of revisions.
- Struct sizes with `arm-none-eabi-gcc -fshort-enums`: a 6-byte step; a location
  with 64 steps of both kinds is 5,196 bytes (kit words at offset 3,148); 32
  locations are 166,272 bytes. `song_t` is 2,316, `kit_config_t` 15,
  `track_config_t` 7.
- The simulator erases a 4 KiB sector in 40 ms and programs a page in 0.4 ms
  (`sim/core/hal.c`, typical values, not measured). The artist waits 2.6 s after
  pressing SAVE; `device_check.js` waits 2.5 s. `sim_flash_store()` points at
  the last 128 KiB.
- `STEPS_PER_PATTERN` (16) means four things today: the clip's step count, the
  page shown on the step buttons, the bar used by launch quantization, phase
  alignment and Song mode, and the step count of the frozen legacy record
  `pattern_v5_t`, which embeds the live `step_t`. 77 uses in `src/`, `include/`
  and `sim/core/`.
- `TRANSPORT_PAUSED` is never set: PLAY after Stop is always Start, so Continue
  cannot be reached from the panel.
- The MIDI input parser already decodes Control Change (for All Notes Off and
  All Sound Off).
- Host builds differ from the RP2040 in enum size: CI runs the host suite with
  and without `-fshort-enums`; the default preset has neither flag, and the
  simulator builds without it. `song_t` holds an enum (`end_mode`), so its
  layout differs too (2,316 bytes on the RP2040, 2,324 on the host).
- The longest live `seq_lock` writer today is a song edit: `song_insert_patterns()`,
  `song_remove()` and `song_move()` move up to 2,295 bytes while playing.
- In the SYNTH/DRUM Pattern view and the note step editor a step button beyond
  the clip's length still edits that (hidden) step; the Session view and kit
  cells ignore it.
- A pad held 500 ms and another pad of its track copy the clip on every screen
  (`handle_pad_press()` does not look at the mode).
- `queue_patterns()` queues even the location already playing; the queue then
  applies without a change.
- The Russian manual, `docs/manual-ru.md`, is in the main checkout but not in
  git, like the research report.

## Approaches considered

Codex, DeepSeek and Grok were polled independently on the storage model, the
playback path, the format, the condition semantics and the gestures (Gemini
was out of credit). All three rejected the pool for a 64-step ceiling; all
three found that one spare byte cannot hold a ratchet (3 + 2 bits) and a
micro-timing offset (5 bits), so the step is 6 bytes.

**Storage.** Decided by the owner: **dense, both kinds for all tracks (D).**

| | RAM for steps | Save payload | Fails? | Type change |
|---|---:|---:|---|---|
| A. Dense 64, kit words in two slots | 117,088 | ~120 KB, fits 128 KiB | never | a third kit track evicts an old kit's steps |
| B. Pool: page 0 fixed, pages 2-8 from a shared pool of 16-step pages (256 pages) | ~88,000 | ~92 KB | lengthen and copy can be refused | erases nothing |
| C. One 6-byte record holding either kind | 100,704 | ~104 KB | never | must erase or convert the track |
| **D. Dense 64, both kinds for every track** | **166,272** | **~170 KB, region grows to 256 KiB** | **never** | **erases nothing** |

- A narrows "a type change erases nothing" and adds slot assignment, eviction
  and slot validation to save about 49 KB of RAM.
- B is the only way to 128 steps. It costs an allocator, page tables to
  validate, refusals when the pool is full, a policy for pages hidden beyond a
  shortened length, and an indirection in every step access, the ISR included.
  A full pool of 64-step clips costs as much memory as D.
- C reverses the owner's kit-storage decision.
- D keeps today's layout, two arrays per location, only longer. RAM after the
  change is about 194 KB of 256 KiB (see Budget).

**Playback path.** My call: **the timer reads the step it plays directly**, under
`seq_lock`, at the step boundary. The prepared rows cannot grow: 256 rows of 64
events would be about 115 KB. A cache of only the eight playing rows would need
rebuilding ahead of every launch and every Song entry. Reading 6 bytes (or a kit
word) and the track's routing per track per step is shorter than today's
worst case, a 116-byte row copy.

**Save region.** My call: **one region of 256 KiB at the end of flash**, single
slot as today. Two slots of 256 KiB (512 KiB) would make a save survive power
loss; that is an option for later, not part of this work.

**Manual FILL.** Decided by the owner: **a sustain pedal (CC64) on the MIDI
input.** Rejected: holding TRACK and pressing MUTE 8 (changes two controls), a
long VALUE press while playing (loses the selector during playback and meets
the TIE gesture on held steps).

**Pads in the Pattern view.** Decided by the owner: **an overview of the clip**;
clips launch from the Session view and with the SCENE buttons. Rejected:
keeping the launch there with a page map on the OLED only.

## Data model

### Constants

`STEPS_PER_PATTERN` goes. Every use is replaced by one of:

| Constant | Value | Meaning |
|---|---:|---|
| `MAX_CLIP_STEPS` | 64 | steps stored per clip; the length range is 1..64 |
| `STEPS_PER_PAGE` | 16 | steps on the step buttons at once |
| `CLIP_PAGES` | 4 | `MAX_CLIP_STEPS / STEPS_PER_PAGE` |
| `STEPS_PER_BAR` | 16 | the bar for launch quantization, phase alignment, Song mode, auto FILL |
| `LEGACY_STEPS` | 16 | step count of v4-v6 records; a literal in the frozen legacy types |
| `DEFAULT_CLIP_STEPS` | 16 | length of a new location |

### The step

```c
typedef struct {
    uint8_t note;       // 0-127
    uint8_t velocity;   // 0-127
    uint8_t flags;      // STEP_FLAG_ACTIVE, ACCENT, SLIDE, TIE; bits 4-7 zero
    uint8_t cond;       // trig condition code, 0 = always (see Conditions)
    uint8_t ratchet;    // reserved, 0 in v7 (ratchet count and ramp, later)
    uint8_t micro;      // reserved, 0 in v7 (micro-timing, later)
} step_t;               // 6 bytes
```

`step_t.cond` is the condition of a SYNTH or DRUM step. **A kit step keeps its
own condition in bits 24-31 of its kit word**, which are free today (the
validator requires them zero, and 12 lanes use bits 0-23). Each kind keeps its
condition, so a type change still erases nothing and a SYNTH clip's conditions
never silently gate the kit hits at the same positions, or the other way round
(changed after the critique; the first draft shared `step_t.cond`). Everything
that reads lanes masks the word with `0x00FFFFFF` (`kit_cell_has_clip()`
already masks with `0x00FFFFFC`); `kit_set()`, `kit_edit()` and `kit_press()`
change only their lane's bits and keep the condition; copy, double and shift
move whole words.

The reserved bytes avoid a layout change, not a version: the firmware that first
writes a non-zero `ratchet` or `micro`, or a condition code v7 does not know,
writes version 8 with this same layout and loads v7 as it is. Their encoding is
fixed by that feature (a signed `micro` for early hits is possible: the byte is
not yet defined). A v7 firmware rejects a v8 save whole, as every older firmware
rejects a newer format today; downgrades stay unsupported. Kit steps have no
spare bits left for a ratchet or micro-timing; that is decided with the feature.

### The location

```c
typedef struct {
    step_t         steps[NUM_TRACKS][MAX_CLIP_STEPS];
    track_config_t tracks[NUM_TRACKS];
    uint8_t        length;                    // fallback, 1-64
    uint8_t        track_length[NUM_TRACKS];  // 0 = use length, else 1-64
    uint8_t        swing;
    char           name[9];
    uint32_t       kit_steps[NUM_TRACKS][MAX_CLIP_STEPS];
} pattern_t;            // 5,196 bytes on the RP2040
```

The field order is today's; only the two step arrays grow. A new location is 16
steps long. Steps beyond a clip's length are kept, as now: shortening hides
them, lengthening one step at a time brings them back.

### Budget

RAM, from today's 117,504 bytes:

| Change | Bytes |
|---|---:|
| patterns 31,104 → 166,272 | +135,168 |
| the save's buffers removed: `pattern_buf`, `song_buf` (2,560), `kit_buf` (256) | −34,048 |
| prepared rows, rows in play, revisions removed | −31,648 |
| new state: pass counters and residues, bar counter, Song lookahead, FILL, page pins, overview, 256-byte save tail | about +700 |
| static scratch `pattern_t` for validating a legacy save | +5,196 |
| code moved to RAM (step read, condition evaluation) | about +1,500 |
| **Total** | **about 194,400** |

That leaves about 66 KB of the 256 KiB main SRAM (less the 2 KiB heap). A
later per-step byte (a CC lock, a gate length) costs 16 KB.

Flash: header 256 + patterns 166,400 (page-rounded) + song 2,560 + kit settings
256 = 169,472 bytes (165.5 KiB) in a 256 KiB region.

## Playback

### Reading a step

The prepared rows (`prepared_patterns`, `prepared_playback`, the revisions,
`fill_prepared_track()`, `load_prepared_track()`) are removed. At each step
boundary, for each track, under the playback lock as today and inside one
`seq_lock` section per track, the timer copies:

- the playing location's `tracks[track]` (7 bytes) and the track's length;
- the step record at the track's position (6 bytes), and for a kit cell the kit
  word (4 bytes) and `kit_config[track]` (15 bytes).

The length and the step are read in the same section, and a position at or
beyond that length reads as an inactive step there, so an edit that shortens the
clip between two sections can never make the timer play a hidden step.

It then releases `seq_lock` and decides, from the copy, what to play, exactly
as the prepared row did: the drum note for a DRUM cell, velocity 127 for an
accent, nothing for a stored mute or a step at or beyond the length, the kit's
note and velocity tables for a kit. Gate pulses are `gate_length_pct * 24 / 100`
as today, computed without a division as `(pct * 24 * 5243) >> 19` (exact for
10-100, checked over the whole range in a test).

- **Kind change.** The timer remembers the kind it last played per track. When
  the copy has the other kind, it first releases what the old kind left
  sounding (`kit_release()` or `send_note_off_if_active()`), as
  `load_prepared_track()` does now.
- **Edits.** A write under `seq_lock` is complete before the timer can copy it.
  An edit is heard the next time its step comes up, as now. Nothing needs a
  revision.
- **`sequencer_refresh_track_playback()`** stays, with the same callers, and now
  only recomputes the clip mask bit of that cell (any active step, or for a kit
  any voice lane, within the length). It scans up to 64 steps on core 0.
- **Placement.** Everything the timer calls for this runs from RAM: the step
  read, the length and kind tests (not `pattern_get_track_length()` or
  `kit_is_cell()`, which live in flash), the condition evaluation and the LST
  distance. Fields are copied one by one, not by struct assignment, which may
  call `memcpy` in flash. `static inline` does not guarantee inlining, so the
  check is the ELF. It is relative: the timer on `master` already reaches flash
  through veneers (`send_note_off_if_active()`, `prepare_note_off()`,
  `cv_set_note()`, `gate_commit()`, `stop_playback_outputs()`, ...) and calls the
  SDK's `__wrap___aeabi_uidiv` and `__wrap___aeabi_uidivmod` (in flash) for
  `pulse_count % PULSES_PER_STEP` and the like. The rule: the new code itself is
  in RAM and calls neither flash nor a division helper, and the ISR's list of
  callees in flash and of division helpers grows by none against the baseline
  taken before the change (`load_prepared_track()` leaves it). Moving the old
  callees into RAM stays the existing bench item.
- **Lock hold.** A song edit (up to 2,295 bytes moved) stays the longest live
  `seq_lock` writer; a clip copy becomes 640 bytes, a location load 5,196 bytes
  (transport stopped). The bench check of the ISR time and Clock interval
  (HARDWARE_STATUS.md) is repeated after this change, with a song edit near the
  start of a full song, a clip copy and a doubling while playing.

### Counters

Two counters are added to the transport, reset by Start:

- **`pass[track]`** (`uint32_t`): passes the track's clip has completed since it
  started playing. It is 0 during the first pass. It goes up by one whenever the
  track's position wraps to step 0, including the wrap forced when the clip is
  shortened below the playhead. It is reset to 0 when the track's playing
  location changes: a queue applied in Pattern mode, a Song entry with another
  location for the track, Start. A launch of the location already playing only
  clears the queue and leaves the counter, as it leaves the playhead. A clip
  that joins mid-clip after a quantised launch is in pass 0 until it first
  wraps. Edits and live mutes do not touch it.
  With it, for each track, the residues `pass_mod[B]` = `pass % B` for B = 2..8,
  each reset with `pass` and stepped (wrapping at B) when `pass` grows, so A:B
  needs no division.
- **`bar_count`**: completed 16-step bars since Start: 0 throughout the first
  bar, increased at every later bar boundary before FILL is computed for that
  boundary. Song mode resets `pulse_count` every bar; this counter is not
  reset.

Phase alignment is unchanged: under BAR, BEAT and STEP a new clip starts at
`(global step % 16) % length`, so a long clip launched on a bar starts at its
step 1, and one launched on a beat starts within its first bar. Song mode is
unchanged: entries last `repeats` bars of 16 steps, a track whose location does
not change keeps playing (a 64-step clip runs on through entries), a track whose
location changes restarts at step 1.

## Conditions

### Codes

The code is stored in `step_t.cond`. Codes never move; new values take
reserved codes. A save with an unknown code is rejected.

| Code | Condition | Plays when |
|---|---|---|
| `0x00` | ALWAYS | always (every migrated and new step) |
| `0x01`-`0x63` | X% (code = X, 1-99) | a random draw is below X |
| `0x64`-`0x6F` | reserved | |
| `0x70` / `0x71` | FILL / !FILL | FILL is / is not active |
| `0x72` / `0x73` | 1ST / !1ST | `pass == 0` / `pass != 0` |
| `0x74` / `0x75` | LST / !LST | this is / is not the step's last pass (below) |
| `0x76`-`0x7F` | reserved (PRE, !PRE, ...) | |
| `0x80 \| (B−1)<<3 \| (A−1)` | A:B, 2 ≤ B ≤ 8, 1 ≤ A ≤ B | `pass % B == A − 1` |
| other `0x80`-`0xFF` | reserved (A > B, B = 1; !A:B later) | |

The panel offers this list, one item per VALUE detent (my call; the list is a
table in `ui.c` and can change without touching the format): ALWAYS, 90%, 75%,
66%, 50%, 33%, 25%, 10%, FILL, !FILL, 1ST, !1ST, LST, !LST, 1:2, 2:2, 1:3,
2:3, 3:3, 1:4 ... 4:4, ... 1:8 ... 8:8 (49 items, clamped at both ends).

### Evaluation

A condition is evaluated in the timer once per step occurrence, at the step
boundary, for an active step of an audible track (a kit step with any lane set).
One evaluation covers the whole step: for a kit, all 12 lanes, AC included,
with the condition in bits 24-31 of the kit word.

- **False is a rest.** The step plays exactly as an inactive step would: the
  previous output is released at this boundary as for a rest (a slide or tie
  from the step before ends here), the step's own slide and tie do nothing, no
  kit voice is hit and no flam is scheduled. The next step that plays triggers
  afresh.
- **Probability.** The draw is a hash, not a stateful generator, so it does not
  depend on how many steps were evaluated before (my call, after the poll):

  ```c
  static inline uint32_t mix32(uint32_t x) {   // "lowbias32"
      x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
      return x;
  }
  h = mix32(mix32(seed ^ pass) ^ (track << 24) ^ (location << 16) ^ step);
  roll = ((h & 0xFFFF) * 100) >> 16;            // 0-99; the step plays when roll < X
  ```

  Golden values, computed outside the firmware (seed, track, location, pass,
  step → h, roll): (1234, 0, 0, 0, 0) → 0x707A0DC2, 5; (1234, 0, 0, 0, 1) →
  0x67FC9E59, 61; (1234, 0, 0, 1, 0) → 0x659059AB, 35; (1234, 3, 5, 2, 17) →
  0x850D1A07, 10; (1234, 7, 31, 1000, 63) → 0xBDBCE268, 88; (1, 0, 0, 0, 0) →
  0x58F54975, 28. Over the 10,000 inputs `i` = 0..9,999 with seed 1, track
  `i % 8`, location `(i / 8) % 32`, pass `i / 256` and step `i % 64`, X = 50
  plays 4,962 times. The
  seed is taken from the timer at Start on the device (so runs differ); host
  tests set it through a test hook, `sequencer_set_seed()`, and a seed set that
  way is kept by every later Start until the hook clears it; in the simulator
  the virtual clock makes it reproducible for a given score.
- **A:B** reads `pass_mod[B] == A − 1`; 1ST and !1ST read `pass == 0`.
- **FILL** is one value for all tracks, taken once per step boundary:
  `fill_active = pedal_down || fill_mode == ON || (fill_mode == EVERY_N &&
  (bar_count & (N − 1)) == N − 1)`; N is 2, 4, 8 or 16, so EVERY 4 fills bars
  4, 8, 12 ... counted from 1. The phase is the song's, from Start, not each
  clip's: a 64-step clip launched in bar 3 with EVERY 4 has its fill in its own
  second bar (global bar 4), as a phrase-long fill should line up with the set's
  phrases, not with when a clip happened to be launched (my call; the manual
  says so, and a test launches a clip out of phase).
- **LST**: the step plays only the last time it comes round before the clip
  changes. At a boundary where the track plays a clip of length `L`, let `D` be
  the number of boundaries until the track's location changes, if a change is
  already known (`D = 1`: the next boundary plays the new clip). The step now
  playing would come round again `L` boundaries later, so LST is true when
  `D ≤ L`. With TRACK LOOP, and with clips that line up with the bar, this is
  exactly the pass in which the change happens; with a clip that does not line
  up (12 steps under BAR), each LST step still plays once more before the
  change, some in the full pass and the rest in the cut one. (The three
  reviewers defined it per pass, `D ≤ L − s`; that silences LST steps that come
  after the cut point of a misaligned clip. My call.)
  - Pattern mode, a queue pending **to another location** (`queued !=
    current`; a queue to the location already playing applies without a
    change and is no change): BAR, the next global step that is a multiple of
    16 (16 when the current one is); BEAT, of 4; STEP, the next step; TRACK
    LOOP, the wrap (`D = L − s` at position `s`).
  - Song mode: the timer does not walk the song per step. A per-track
    lookahead, `song_change_bars[track]` (1-4 = the location changes at the end
    of the current bar or of the 2nd, 3rd or 4th bar from now; 0 = no change
    within 4 bars), is computed once per bar inside `advance_song_position()`,
    under the same lock and with the same rules (0-repeat entries skipped, LOOP
    wrapping to entry 0, a position past the length meaning entry 0, the song's
    end in STOP counting as a change), and again by every song edit under its
    lock. The walk visits each entry at most once and stops as soon as 4 bars
    are covered, so its cost is bounded once per bar, not per step and track.
    Then `D = (song_change_bars − 1) * 16 + 16 − global % 16`. Four bars cover
    the longest clip, so `D ≤ L` is decided exactly.
  - No change known: LST false, !LST true. A queue set mid-pass affects only the
    steps after it; a cancelled queue makes LST false again. A queue is applied
    at a boundary before that boundary's step is evaluated, so under STEP, and
    with a 1-step clip under TRACK LOOP, no step of the old clip is evaluated
    after the queue is set: LST then never fires. Stop, Panic and a manual Stop
    cannot be foreseen.

### FILL sources

- **Pedal.** The MIDI input parser reports Control Change 64 on any channel as a
  new event. The pedal goes down at a value of 64 or more and up at 31 or less;
  values between keep its state, so a continuous pedal near the middle does not
  flicker. It is not passed through to the MIDI output and never writes a step.
  An input gap, All Notes Off, All Sound Off and a panic put the pedal up; when
  that lifts a pedal that was down, the OLED shows `FILL OFF`. A pedal still
  held through a gap stays up until it is released and pressed again, as a
  switch pedal sends only changes: a lost message may end a fill early, never
  leave one stuck on. Stop and Start leave the pedal as it is (it is the
  pedal's state, not the transport's).
- **Gaps include UART errors** (changed after the critique). Today the receive
  interrupt reads with `uart_getc()`, which drops the error bits, so a byte lost
  to a UART overrun (the one-byte holding register, with the FIFO off, unread
  for more than 320 µs) or a framing or break error goes unnoticed, and a lost
  `B0 40 00` would leave FILL on. The handler reads `uart_get_hw()->dr` and,
  when OE, BE, PE or FE is set, counts a lost byte (`midi_in_dropped()`) and
  drops the byte, so the existing gap rule fires. This also protects today's
  passed-through notes from a lost Note Off.
- **FILL mode**, step button 16 in the pattern selector, cycles OFF → ON →
  EVERY 2 → EVERY 4 → EVERY 8 → EVERY 16 → OFF. EVERY N makes the last bar of
  every N bars since Start a fill. Not saved, like the launch quantization (my
  call). The selector shows the mode; every screen with the transport symbol
  draws it inverted while FILL is active.

## The panel

### Pages

Each track has a page pin in UI RAM: FOLLOW (the default) or a page 0-3. The
**shown page** of a cell is computed once per UI pass:

1. while any step button pressed on this screen is still down, the page it was
   pressed on (a hold pins the view until every held step is released);
2. else a pinned page, clamped to the clip's last page `(length − 1) / 16`;
3. else, if the transport plays this location on this track, the page of its
   playhead;
4. else page 0.

The frozen page is one per screen, not per track, and it also holds while an
encoder button that joined a hold is down.

Every step press binds, at the press, to its cell (track, location, lane) and
to the absolute step `page * 16 + button`, on every kind of cell (today only
kit presses are bound to a cell; SYNTH and DRUM presses read the selected
track when they act). Taps, holds, long presses (the editor opens on the
absolute step), encoder edits, keys from the MIDI keyboard and from the pad
keyboard all address absolute steps. The encoder targets that outlive a step's
release today (`grid_enc_target`, `grid_enc_long_done`) become 64-bit masks of
absolute steps bound to their cell, so pressing the same button again on
another page never retargets them. Buttons beyond the length behave as today
on every page: the Session view and kit cells ignore them; the SYNTH/DRUM
Pattern view and the note step editor edit the hidden step; their LEDs stay
dark. LED masks stay 16 bits, relative to the shown page: the playhead is
turned into a button number on the shown page before a mask is built (`1u <<
40` would be undefined). Every "none" for a step or a playhead becomes 0xFF:
today `cell_playhead()` and the display API use 16 (`STEPS_PER_PATTERN`) for
none, which would read as step 17.

- **Pattern view, VALUE short press with no step held** (no action today):
  FOLLOW → page 1 → ... → last page → FOLLOW, with a notice (`PAGE 2/4`,
  `FOLLOW`). On a clip of 16 steps or less it only shows `PAGE 1/1`.
- **Session view**: the step buttons show the focused cell's shown page; the
  track's pin from the Pattern view applies; VALUE short press keeps cycling the
  launch quantization.
- **Step editors**: the shown page is the page of the edited step; the step
  buttons choose a step on it. TRACK press in the Pattern view opens the editor
  on the first step of the shown page.
- **Step LEDs** show the shown page: its active steps (on a kit, the lane's
  steps), dark beyond the length; the playhead blinks on its button only when it
  is on the shown page.
- **OLED.** Every clip view (piano roll, drum grid, kit lane strip, Session
  focus line, step editors) shows a page strip: one cell per page of the length,
  the shown page solid, the playing page blinking on 8th notes, and a mark that
  tells FOLLOW from a pinned page. Step numbers on screen are absolute (`S17`).
  The piano roll and the strips draw the shown page's 16 steps.

### The pad overview

In the Pattern view (the owner's choice) and both step editors (my proposal,
**pending the owner**: the step editors are part of editing one clip, and there
a pad press would pick the step; without it the pads keep launching and copying
in the step editors as today), while the pad keyboard is not on, the pads show
the clip:

- pad (column `c`, row `r`, row 0 at the top) is step `r * 8 + c`; page `p` is
  rows `2p` and `2p + 1`;
- beyond the length: off; an empty step: dim in the track's hue, dim white on the
  shown page; an active step: half in the hue, an accent full; the playhead:
  white, full. On a kit cell the overview shows the lane the step LEDs show (the
  remembered lane; the cursor lane in the kit editor): a note half, an accent or
  flam full;
- a pad press within the length pins the track's page to the pad's page; in a
  step editor it also selects that step for editing. A press beyond the length
  does nothing. It never launches, queues or copies a clip, and is never a copy
  source. Its release does nothing.

Routing of a pad press, in order: the pad keyboard (a SYNTH step held, as today),
the overview (Pattern view and step editors), else launch or copy as today (the
Session view, the selector, track config, song edit). So the copy by pad hold,
which today works on every screen, no longer works in the Pattern view and the
step editors (part of the owner's choice for the pads there). SCENE, MUTE and
PLAY are unchanged everywhere.

### Length

In the pattern selector, for the selected track and location:

| Button | Today | Now |
|---|---|---|
| 11 | length − 1 (on press) | unchanged, 1-64 |
| 12 | length + 1 (on press) | unchanged, 1-64 |
| 13 | reset jitter counters | unchanged |
| 14 | — | halve: `max(1, length / 2)`; steps beyond stay |
| 15 | — | double with copy: `min(64, 2 * length)`; each new step `s` becomes a copy of step `s − length` (both kinds, the condition and the reserved bytes) |
| 16 | — | FILL mode |

Doubling overwrites steps hidden beyond the length; adding one step does not
(it shows the hidden step). The step LEDs light 11 to 16.

### Editing a condition

- **Pattern view, SYNTH or DRUM cell, steps held:** VALUE short press (no
  action today) switches what VALUE turns between velocity and condition. It
  counts as "with steps held" when the VALUE button joined a hold (its targets
  are not empty when the press arrives, which is at the button's release), in
  whichever order the step and VALUE are let go; the page cycle runs only with
  no hold and no encoder targets, as navigation does today. In
  condition mode a VALUE detent moves every held step one item along the list
  (an inactive held step is switched on first, as with any encoder edit). TRACK
  still turns the note, TRACK press and long and VALUE long still toggle accent,
  slide and tie. The mode returns to velocity when every held step is released
  (my call: no hidden mode). The info line shows the lowest held step's
  condition in condition mode (`S17 COND 1:4`).
- **Note step editor:** a tap on the button of the step already being edited
  switches VALUE between velocity and condition; today that press only selects
  the same step. It is a tap when, at its press, the button was the edited
  step, and it is released before the long press with no encoder turned or
  pressed and no key (MIDI or pad) played meanwhile. Holding that button still
  makes the pads a keyboard, and its long press still toggles the step. Opening
  the editor, leaving it and selecting another step put VALUE back on velocity.
  The screen shows the condition, and which of velocity and condition VALUE
  turns.
- **Kit step editor:** TRACK walks the lane cursor AC ... RC and then one more
  stop, COND, where VALUE turns the kit step's condition (bits 24-31 of its
  word); at that stop the step LEDs show the steps of the page that have a
  condition.
- The Session view does not edit conditions; a long press there opens the step
  editor as now.

### Gesture map

What changes, against `src/ui.c` at bcf6871:

| Where | Control | Today | Now | Kind of change |
|---|---|---|---|---|
| Pattern view, no hold | VALUE short | nothing | page cycle | free gesture |
| Pattern view, steps held | VALUE short | nothing (cancels the taps) | velocity ↔ condition | free gesture |
| Pattern view | pad (no keyboard) | launch, or copy with a pad held | overview: pin page; no launch, no copy | **owner-approved change** |
| Note and kit step editors | pad (no keyboard) | launch, or copy with a pad held | overview: pick the step; no launch, no copy | **change pending the owner** |
| Note step editor | tap on the edited step | selects it again | velocity ↔ condition | no-op becomes an action |
| Kit step editor | TRACK past RC | clamped at RC | COND stop | new stop |
| Pattern selector | 14, 15, 16 | nothing | halve, double, FILL mode | free buttons |
| Pattern selector | 11, 12 | ±1 within 1-16 | ±1 within 1-64 | range only |
| MIDI input | CC64 | ignored | FILL pedal | new input |

Unchanged on purpose: PLAY, SCENE and MUTE act on press; TRACK long (bank in
Session, back to Session in the Pattern view); VALUE long (selector; tie on held
steps); VALUE short in Session (launch quantization); both encoders long
(panic); a pad held 500 ms then another pad of the track (copy, on every screen
but the Pattern view and the step editors); held SYNTH step plus pad
(keyboard); step taps and long presses in Session; buttons beyond the length
(as today, per screen). `pattern_shift()` and `pattern_shift_track()`, which no
control calls, rotate within the clip's length (both kinds), so a 16-step
clip shifts as today.

## Save format v7

### Region

`STORE_RESERVED` becomes 256 KiB, the last 256 KiB of flash (offset 0x1C0000).
Versions 4-6 were written at the old offset 0x1E0000 (the last 128 KiB), which
lies inside the new region. At boot and before every save the firmware checks
that its image (`__flash_binary_end`) ends below the region; if not, saving is
refused and the notice reads `FLASH LAYOUT` (at boot and on SAVE) instead of
`SAVE FAILED`. `flash_storage.h` gains
`typedef enum { FLASH_OK, FLASH_ERR_LAYOUT, FLASH_ERR_PLAYING, FLASH_ERR_INVALID, FLASH_ERR_VERIFY } flash_storage_error_t;`
and `flash_storage_error_t flash_storage_last_error(void)`.

### Layout

| Offset | Content |
|---|---|
| 0 | header page: magic `RBTH`, version 7, CRC32, reserved |
| 256 | patterns, `MAX_PATTERNS` x `pattern_t`, page-rounded (166,400) |
| then | song, page-rounded (2,560) |
| then | kit settings `kit_config[NUM_TRACKS]`, page-rounded (256) |

The CRC covers the three payloads at their unpadded lengths. Records are the
build's own `pattern_t`, `song_t` and `kit_config_t`, as in v6, with
`_Static_assert`s on `__arm__` for every size and the offset of `kit_steps`
(5,196, 2,316, 15, 3,148). A v7 image is therefore only portable between builds
with the same enum size; host builds without short enums and the simulator
make their own (5,268 bytes per location, a 171,776-byte payload). Only the
legacy readers are layout-independent.

### Validation

Before RAM is replaced: everything v6 checks, and lengths 1-64 (`track_length`
0-64), every one of the 64 steps of every track, hidden ones included (note and
velocity ≤ 127, flag bits 4-7 zero, a known condition code, `ratchet` and
`micro` zero), kit words with a known condition code in bits 24-31 and AC 0 or
1, at most two kit tracks across all locations, a valid song and kit settings,
the CRC.

### Save

Transport stopped, MIDI output idle, as now. Then:

1. the CRC is computed over the RAM data in place (nothing writes it while the
   transport is stopped and core 0 is in the save);
2. core 1 is parked, interrupts go off;
3. the sector holding the old header (0x1E0000) is erased first, then the region
   from its start to the end of the payload, rounded up to 4 KiB:
   `STORE_ERASE = round_up_4k(max(payload end, 0x1E0000 + 4 KiB) − 0x1C0000)`,
   42 sectors (168 KiB) today, in both layouts, as three calls:
   `[0x1E0000, 0x1E1000)`, then `[0x1C0000, 0x1E0000)` (two 64 KiB blocks, which
   the SDK erases as blocks), then `[0x1E1000, 0x1EA000)`. A cut at any point of
   the erase therefore leaves neither the new v7 header nor the old v6 one, so
   an old save can never come back after a torn v7 save;
4. the patterns, song and kit settings are programmed straight from `g_seq`, in
   whole pages, each last partial page through one static 256-byte buffer padded
   with 0xFF (the SDK programs from RAM; `g_seq` is in RAM);
5. the header is programmed last.

`pattern_buf`, `song_buf` and `kit_buf` go. The simulator (host layout) erases
42 sectors at its 40 ms each and programs 671 pages (662 on the RP2040): about
1.95 s against the artist's 2.6 s wait and `device_check.js`'s 2.5 s.

On the board the stall is not the simulator's figure. With the W25Q16JV's
typical times (64 KiB block 150 ms, 4 KiB sector 45 ms, page 0.4-0.8 ms) today's
save (two blocks, 134 pages) takes about 0.35-0.4 s; v7 (two blocks, ten
sectors, 662 pages) about 1.0-1.3 s; at the datasheet's maxima several seconds.
The power-loss window grows with it. Measuring the save on the board is a
condition for merging the format change (plan Task 4).

### Load and migration

Load probes the v7 header at the region start. If that is not a valid v7 save,
it probes the old offset for a v4, v5 or v6 header and migrates:

- validation runs first over all 32 legacy records, each converted into one
  static scratch `pattern_t` (5,196 bytes in `.bss`, counted in the budget; a
  `pattern_t` must never be a local, as the core 0 stack is 2 KiB); only then
  are the records converted again, straight into `g_seq`, one location per lock
  section;
- a location gets its routing, lengths (1-16), swing and name; steps 0-15 of
  each track get note, velocity and flags, with `cond`, `ratchet` and `micro`
  zero; kit words 0-15 are copied (v6) or zero (v4, v5); steps 16-63 are zero;
- the song and (v6) the kit settings are copied; v4 and v5 get default kit
  settings.

Legacy images are read field by field at the byte offsets the RP2040 wrote them
at, from constants that use the literal 16 and never the live `step_t`,
`track_config_t`, `song_t` or step constants:

| | v4, v5 | v6 |
|---|---:|---:|
| pattern record | 459 bytes (steps 0, tracks 384, length 440, track_length 441, swing 449, name 450) | 972 (the same, kit words at 460) |
| patterns at | 256 | 256 |
| song at | 15,104 | 31,488 |
| kit settings at | — | 34,048 (8 x 15 bytes) |
| CRC over | patterns 14,688 + song 2,316 (v5; v4 has none) | patterns 31,104 + song 2,316 + kit settings 120 |

A legacy song is 2,316 bytes: entries (8 locations + repeats each) at 0, length
(16-bit little-endian) at 2,304, end mode (one byte) at 2,306, name at 2,307.
Raw bytes are range-checked before they become an enum or a `bool`. On
`__arm__` the constants are asserted against frozen copies of the old structs
(`legacy_step_t` of 3 bytes, `legacy_track_config_t` of 7, `pattern_v5_t`,
`pattern_v6_t`, `song_v6_t`), which nothing else uses.
Today `pattern_v5_t` embeds the live `step_t`, so growing the step without this
would break v4 and v5 loads. Reading by offsets, rather than casting to a host
struct, lets the committed fixtures (below) load in every host build, with and
without `-fshort-enums`, and in the simulator.

After an update the user must LOAD before the first SAVE: the save erases the
old header. Older firmware cannot read a v7 save (it finds no header at its
offset). Both go into `README.md` and the manual (`docs/manual-ru.md`, which is
not in git: the pull request lists the manual's changes for the owner).

## Simulator and CI

- `sim/core/hal.c`: `STORE_BYTES` 256 KiB, so `sim_flash_store()` points at the
  v7 header.
- `sim_dbg_step()` and `sim_dbg_kit_step()` take steps 0-63 (today `& 15`);
  `sim_dbg_step()` returns the condition in bits 24-31. New: `sim_dbg_pass()`,
  `sim_dbg_fill()`.
- `sim/build.sh`: feature bit 8 when the firmware has long clips and conditions;
  `sim/check.sh` and the page offer a `long` score on such firmware.
- The scores this firmware takes today (`fw2`, `keys`, `pads`; `base` runs only
  on firmware without the last-note rule) do not change and stay green after
  every phase: with lengths up to 16, page 1 and no conditions the device
  behaves as today. Their `verifyPatterns` keeps checking 16 steps. The
  artist's model (`Model.copy()` and the step tables) grows to 64 steps for the
  new score.
- **The `long` score** (built on `pads`) adds to one song's preparation and set:
  - a lead clip doubled 16 → 32 with button 15, then page 2 opened with VALUE
    short and steps 17-32 edited;
  - a pad press in the overview that opens page 2 of another clip;
  - conditions: closing hats with FILL on the last two steps of the clip, a lead
    step on 1:2, a crash on 1ST;
  - the pedal pressed for one bar during the set (CC64 on the MIDI input, as the
    `keys` score sends notes), and FILL mode EVERY 8 once.
- **Its checks**: memory up to each clip's length (conditions included); on the
  wire, the 32-step clip's second half in the second bar, 1:2 steps in every
  other pass only, FILL steps only in the pedal bar and the auto-fill bars, 1ST
  only in the first pass after the launch. Probability is not checked on the
  wire; host tests check it with a fixed seed.
- `device_check.js`: a v7 save round trip, the save time below the artist's
  wait, and a v6 image placed at the old offset loading through migration.

## Tests on the host

Fixtures first: before anything changes, v4, v5 and v6 images written by
today's code are committed as binary files under `test/fixtures/`, with what
they hold, so the migration is tested against bytes the old firmware wrote and
not against images regenerated from the new types. They are made by a small
tool built from today's sources with `-fshort-enums`, which first asserts that
its records have the RP2040's sizes (459 and 972 bytes); the simulator cannot
make them, as it builds without short enums.

Then, among others: the step and location sizes; v7 round trip and every
validation rule; the three legacy fixtures migrating; the save erasing through
the old header page; playback of 17-64 step clips, lengths changed while
playing, phase join of long clips, Song entries running a long clip on; kind
changes with direct reads; every condition code, with `pass`, FILL, LST under
each quantization and in Song mode (a queue to the playing location, a long run
of 0-repeat entries), false-condition rests after a slide, after a tie and after
a kit flam; the probability hash against the golden values; no division and no
flash call on the ISR path (ELF check); the pedal events and gaps; page
pinning, hold pinning, absolute binding (long presses and encoder targets
included), the overview routing and lights, halve and double, the condition
editing gestures in both release orders; a torn save at several erase points.

## Risks and bench items

- **RAM**: about 194 KB of 256 KiB. Confirm with `arm-none-eabi-size` after each
  phase and record it in HARDWARE_STATUS.md.
- **ISR time and jitter** with direct reads: measure GPIO 7 and the Clock
  interval, as for the kit.
- **Save time on the board**: about 1.0-1.3 s typical for 168 KiB and 662
  pages, several seconds at the datasheet's maxima (see Save); the UI is
  stalled for it, with the transport stopped. Measured before the format change
  is merged.
- **The pedal after a gap** stays up until it is pressed again: a fill can end
  early, never stick.
- **Update path**: a SAVE before a LOAD after updating loses a v6 save.
- **Probability draws** depend on the seed taken at Start: two runs of one set
  differ on the device, not in the simulator.

## Review changes

Codex and DeepSeek reviewed the first draft against the code (Grok failed to
start, Gemini was out of credit). Changed after checking each point in the code:

- The legacy song and kit settings are read by byte offsets too (`song_t` holds
  an enum), with a table of every legacy offset; the frozen structs no longer
  embed live types.
- The erase starts with the old header's sector, so a torn v7 save cannot bring
  a v6 save back; the erase span is an explicit expression.
- A:B, FILL EVERY N and the gate length need no division (residue counters, a
  mask, a multiply-shift); the ISR path is checked in the ELF for flash calls and
  divisions, as `static inline` proves nothing.
- `mix32` is defined, with golden values computed outside the firmware; the
  first draft's input packing let `(location 1, pass 0)` collide with
  `(location 0, pass 1024)`.
- LST: a queue to the playing location is no change; under STEP and with a
  1-step clip LST cannot fire; the Song walk follows `advance_song_position()`
  and skipped entries no longer use up the horizon.
- `bar_count` counts completed bars; EVERY 4 fills bars 4, 8, 12.
- Encoder targets that outlive a step's release become absolute 64-bit masks;
  every press binds its cell and absolute step; the VALUE short press is judged
  by the encoder's targets, so the release order does not matter; the hold page
  is one per screen.
- Buttons beyond the length keep today's per-screen behaviour (the first draft
  wrongly said they do nothing everywhere); the copy by pad hold, which works on
  every screen today, is gone only where the pads are the overview.
- Sizes and timings for the simulator's layout (5,268 bytes per location,
  671 pages); the longest live `seq_lock` writer is a song edit (2,295 bytes).
- The step editor's tap is defined; the pedal's state across Stop, Start,
  panic, save and load; the boot layout check and its notice; `pattern_shift()`
  within the length; the manual is not in git.

A further independent critique (a subagent of the starting session that
compiled the structs for host and ARM) found, besides points already fixed
above:

- A kit step's condition moves from the shared `step_t.cond` into its own kit
  word (bits 24-31), so a type change no longer lets one kind's conditions gate
  the other's hits.
- UART overrun, break and framing errors count as an input gap, so a lost pedal
  release cannot leave FILL on; the pedal gets hysteresis and a `FILL OFF`
  notice.
- Song-mode LST no longer walks the song per step in the ISR: a per-track
  lookahead of up to 4 bars is computed once per bar in
  `advance_song_position()` and by song edits.
- The reserved bytes avoid a layout change, not a version: their first use
  writes v8 with the same layout.
- "None" for a step or playhead is 0xFF, not 16; the step and the length are
  read in one lock section.
- `song_buf` and `kit_buf` go with `pattern_buf` (RAM about 194 KB); the save's
  time on the board is estimated (1.0-1.3 s typical) and measured before
  merging the format change.
- EVERY N stays in the song's phase from Start, now stated and tested with a
  clip launched out of phase.

## Left to the builder

The exact LED levels of the overview, the pixels of the OLED page strip, the
condition list's wording on screen, the probability values offered, whether the
eight `seq_lock` sections at a step boundary become one, and the names of new
functions. Anything that changes a gesture, the format or a condition's meaning
goes back to the owner.
