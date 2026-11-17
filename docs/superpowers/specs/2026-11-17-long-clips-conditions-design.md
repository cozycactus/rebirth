# Clips up to 64 Steps and Per-Step Trig Conditions — Design

Date: 2026-11-17. Status: the approach and four choices below were made by the
project owner (relayed by the session that started this work): a 64-step
ceiling with dense storage, kit steps kept for all eight tracks, a manual FILL
from a sustain pedal on the MIDI input, and the pads of the Pattern view
showing the whole clip. Everything marked "my call" is open to review in the
pull request. Base: `master` at bcf6871.

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
  change is about 190 KB of 256 KiB (see Budget).

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
    uint8_t ratchet;    // reserved, 0 in v7: bits 0-2 count code, 3-4 ramp
    uint8_t micro;      // reserved, 0 in v7: delay in pulses, 0-23
} step_t;               // 6 bytes
```

`cond`, `ratchet` and `micro` belong to the step **position** of the clip and
apply to whichever kind the cell plays: a kit cell's step `s` takes its
condition from `steps[track][s].cond`, as its lanes come from
`kit_steps[track][s]`. A type change therefore keeps them with the position
(my call: one place to edit and store, no kit-only copy).

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
| `pattern_buf` removed | −31,232 |
| prepared rows, rows in play, revisions removed | −31,648 |
| new state: pass counters, bar counter, FILL, page pins, overview, 256-byte save tail | about +600 |
| code moved to RAM (step read, condition evaluation) | about +1,500 |
| **Total** | **about 191,900** |

That leaves about 68 KB of the 256 KiB main SRAM (less the 2 KiB heap). A
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

It then releases `seq_lock` and decides, from the copy, what to play, exactly
as the prepared row did: the drum note for a DRUM cell, velocity 127 for an
accent, nothing for a stored mute or a step at or beyond the length, the kit's
note and velocity tables for a kit. Gate pulses are computed from
`gate_length_pct` with the same rounding as today.

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
- **Placement.** The step read and the condition evaluation run from RAM
  (`__not_in_flash_func` or inlined into the ISR), like the kit path today.
- **Lock hold.** The longest `seq_lock` writer becomes a clip copy (64 steps of
  both kinds, 640 bytes) and a location load (5,196 bytes, transport stopped).
  The bench check of the ISR time and Clock interval (HARDWARE_STATUS.md) is
  repeated after this change.

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
- **`bar_count`**: whole 16-step bars since Start. Song mode resets
  `pulse_count` every bar; this counter is not reset.

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
One evaluation covers the whole step: for a kit, all 12 lanes, AC included.

- **False is a rest.** The step plays exactly as an inactive step would: the
  previous output is released at this boundary as for a rest (a slide or tie
  from the step before ends here), the step's own slide and tie do nothing, no
  kit voice is hit and no flam is scheduled. The next step that plays triggers
  afresh.
- **Probability.** The draw is a hash, not a stateful generator, so it does not
  depend on how many steps were evaluated before (my call, after the poll):
  `h = mix32(seed ^ track << 24 ^ location << 16 ^ pass << 6 ^ step)`; the step
  plays when `((h & 0xFFFF) * 100) >> 16 < X`. No division in the ISR. The
  seed is taken from the timer at Start on the device (so runs differ); host
  tests set it through a test hook; in the simulator the virtual clock makes it
  reproducible for a given score.
- **FILL** is one value for all tracks, taken once per step boundary:
  `fill_active = pedal_down || fill_mode == ON || (fill_mode == EVERY_N &&
  bar_count % N == N − 1)`.
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
  - Pattern mode, a queue pending: BAR, the next global step that is a multiple
    of 16 (16 when the current one is); BEAT, of 4; STEP, the next step; TRACK
    LOOP, the wrap (`D = L − s` at position `s`).
  - Song mode: the end of the current entry (`(repeats left − 1) * 16 +
    16 − global % 16`), if the next entry played (skipping 0-repeat entries,
    wrapping on LOOP) has another location for the track, or the song ends in
    STOP. If it keeps the location, the walk goes on to the entry after it,
    adding its bars, until `D` exceeds `L` or 8 entries were looked at; then no
    change is known.
  - No change known: LST false, !LST true. A queue set mid-pass affects only the
    steps after it; a cancelled queue makes LST false again. Under STEP a queue
    applies at the very next boundary, so an LST step plays only if the queue
    was set before its boundary. Stop, Panic and a manual Stop cannot be
    foreseen.

### FILL sources

- **Pedal.** The MIDI input parser reports Control Change 64 on any channel as a
  new event; a value of 64 or more is down, below 64 up. Not passed through. An
  input gap (lost bytes, a flash save or load), All Notes Off, All Sound Off and
  a panic put the pedal up.
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

Every step press binds to the absolute step `page * 16 + button` at the press,
as a press already binds to its cell and lane. Taps, holds, encoder edits, keys
from the MIDI keyboard and from the pad keyboard all address absolute steps. A
button beyond the clip's length does nothing, as now.

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

In the Pattern view and both step editors (my call: the step editors are part of
editing one clip), while the pad keyboard is not on, the pads show the clip:

- pad (column `c`, row `r`, row 0 at the top) is step `r * 8 + c`; page `p` is
  rows `2p` and `2p + 1`;
- beyond the length: off; an empty step: dim in the track's hue, dim white on the
  shown page; an active step (a kit lane value): half in the hue, an accent (or a
  kit accent or flam) full; the playhead: white, full;
- a pad press pins the track's page to the pad's page; in a step editor it also
  selects that step for editing (within the length). It never launches, queues
  or copies a clip, and is never a copy source. Its release does nothing.

Routing of a pad press, in order: the pad keyboard (a SYNTH step held, as today),
the overview (Pattern view and step editors), else launch or copy as today (the
Session view, the selector, track config, song edit). SCENE, MUTE and PLAY are
unchanged everywhere.

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
  action today) switches what VALUE turns between velocity and condition. In
  condition mode a VALUE detent moves every held step one item along the list
  (an inactive held step is switched on first, as with any encoder edit). TRACK
  still turns the note, TRACK press and long and VALUE long still toggle accent,
  slide and tie. The mode returns to velocity when every held step is released
  (my call: no hidden mode). The info line shows the lowest held step's
  condition in condition mode (`S17 COND 1:4`).
- **Note step editor:** a short tap (released before the long press, nothing
  typed on a key meanwhile) on the button of the step already being edited
  switches VALUE between velocity and condition; today that press only selects
  the same step. Holding that button still makes the pads a keyboard, and its
  long press still toggles the step. The screen shows the condition, and which
  of velocity and condition VALUE turns.
- **Kit step editor:** TRACK walks the lane cursor AC ... RC and then one more
  stop, COND, where VALUE turns the step's condition; at that stop the step
  LEDs show the steps of the page that have a condition.
- The Session view does not edit conditions; a long press there opens the step
  editor as now.

### Gesture map

What changes, against `src/ui.c` at bcf6871:

| Where | Control | Today | Now | Kind of change |
|---|---|---|---|---|
| Pattern view, no hold | VALUE short | nothing | page cycle | free gesture |
| Pattern view, steps held | VALUE short | nothing (cancels the taps) | velocity ↔ condition | free gesture |
| Pattern view, step editors | pad (no keyboard) | launch or copy | overview: pin page / pick step | **owner-approved change** |
| Note step editor | tap on the edited step | selects it again | velocity ↔ condition | no-op becomes an action |
| Kit step editor | TRACK past RC | clamped at RC | COND stop | new stop |
| Pattern selector | 14, 15, 16 | nothing | halve, double, FILL mode | free buttons |
| Pattern selector | 11, 12 | ±1 within 1-16 | ±1 within 1-64 | range only |
| MIDI input | CC64 | ignored | FILL pedal | new input |

Unchanged on purpose: PLAY, SCENE and MUTE act on press; TRACK long (bank in
Session, back to Session in the Pattern view); VALUE long (selector; tie on held
steps); VALUE short in Session (launch quantization); both encoders long
(panic); a pad held 500 ms then another pad of the track (copy, Session view);
held SYNTH step plus pad (keyboard); step taps and long presses in Session.

## Save format v7

### Region

`STORE_RESERVED` becomes 256 KiB, the last 256 KiB of flash (offset 0x1C0000).
Versions 4-6 were written at the old offset 0x1E0000 (the last 128 KiB), which
lies inside the new region. A boot check refuses saving (`SAVE FAILED`, and the
reason on the OLED) if the firmware image reaches the region start.

### Layout

| Offset | Content |
|---|---|
| 0 | header page: magic `RBTH`, version 7, CRC32, reserved |
| 256 | patterns, `MAX_PATTERNS` x `pattern_t`, page-rounded (166,400) |
| then | song, page-rounded (2,560) |
| then | kit settings `kit_config[NUM_TRACKS]`, page-rounded (256) |

The CRC covers the three payloads at their unpadded lengths. Records are the
RP2040's `pattern_t`, `song_t` and `kit_config_t`, as in v6, with
`_Static_assert`s on `__arm__` for every size and the offset of `kit_steps`; the
host tests build with `-fshort-enums` as now.

### Validation

Before RAM is replaced: everything v6 checks, and lengths 1-64 (`track_length`
0-64), every one of the 64 steps of every track, hidden ones included (note and
velocity ≤ 127, flag bits 4-7 zero, a known condition code, `ratchet` and
`micro` zero), kit words with bits 24-31 zero and AC 0 or 1, at most two kit
tracks across all locations, a valid song and kit settings, the CRC.

### Save

Transport stopped, MIDI output idle, as now. Then:

1. the CRC is computed over the RAM data in place (nothing writes it while the
   transport is stopped and core 0 is in the save);
2. core 1 is parked, interrupts go off;
3. the erase covers the region from its start to the end of the payload or the
   old header page at 0x1E0000, whichever is further, rounded up to 4 KiB
   (42 sectors, 168 KiB, today), so a stale v6 header can never come back after
   a torn v7 save;
4. the patterns, song and kit settings are programmed straight from `g_seq`, in
   whole pages, each last partial page through one static 256-byte buffer padded
   with 0xFF (the SDK programs from RAM; `g_seq` is in RAM);
5. the header is programmed last.

`pattern_buf` goes. The simulator's cost: 42 sectors and 662 pages, about
1.95 s against the artist's 2.6 s wait.

### Load and migration

Load probes the v7 header at the region start. If that is not a valid v7 save,
it probes the old offset for a v4, v5 or v6 header and migrates:

- each legacy record is read through a frozen type into a static buffer (972
  bytes; not on the 2 KiB stack);
- a location gets its routing, lengths (1-16), swing and name; steps 0-15 of
  each track get note, velocity and flags, with `cond`, `ratchet` and `micro`
  zero; kit words 0-15 are copied (v6) or zero (v4, v5); steps 16-63 are zero;
- the song and (v6) the kit settings are copied; v4 and v5 get default kit
  settings.

Legacy records are read field by field at the byte offsets the RP2040 wrote
them at (a v5 record is 459 bytes, a v6 record 972, kit words at offset 460,
`track_config_t` 7 bytes with one-byte enums), from constants that use the
literal 16 and never the live `step_t`, `track_config_t` or step constants. On
`__arm__` the constants are asserted against frozen copies of the old structs.
Today `pattern_v5_t` embeds the live `step_t`, so growing the step without this
would break v4 and v5 loads. Reading by offsets, rather than casting to a host
struct, lets the committed fixtures (below) load in every host build, with and
without `-fshort-enums`, and in the simulator.

After an update the user must LOAD before the first SAVE: the save erases the
old header. Older firmware cannot read a v7 save (it finds no header at its
offset). Both go into the manual.

## Simulator and CI

- `sim/core/hal.c`: `STORE_BYTES` 256 KiB, so `sim_flash_store()` points at the
  v7 header.
- `sim_dbg_step()` and `sim_dbg_kit_step()` take steps 0-63 (today `& 15`);
  `sim_dbg_step()` returns the condition in bits 24-31. New: `sim_dbg_pass()`,
  `sim_dbg_fill()`.
- `sim/build.sh`: feature bit 8 when the firmware has long clips and conditions;
  `sim/check.sh` and the page offer a `long` score on such firmware.
- The four existing scores do not change and stay green after every phase: with
  lengths up to 16, page 1 and no conditions the device behaves as today. Their
  `verifyPatterns` keeps checking 16 steps.
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
each quantization and in Song mode, false-condition rests with slide and tie,
kit steps; the probability hash with a fixed seed; the pedal events and gaps;
page pinning, hold pinning, absolute binding, the overview routing and lights,
halve and double, the condition editing gestures.

## Risks and bench items

- **RAM**: about 192 KB of 256 KiB. Confirm with `arm-none-eabi-size` after each
  phase and record it in HARDWARE_STATUS.md.
- **ISR time and jitter** with direct reads: measure GPIO 7 and the Clock
  interval, as for the kit.
- **Erase time on the real chip** for 172 KiB (block erases where aligned):
  measure; the UI is stalled for it, with the transport stopped.
- **Update path**: a SAVE before a LOAD after updating loses a v6 save.
- **Probability draws** depend on the seed taken at Start: two runs of one set
  differ on the device, not in the simulator.

## Left to the builder

The exact LED levels of the overview, the pixels of the OLED page strip, the
condition list's wording on screen, the probability values offered, whether the
eight `seq_lock` sections at a step boundary become one, and the names of new
functions. Anything that changes a gesture, the format or a condition's meaning
goes back to the owner.
