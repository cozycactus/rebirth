# Drum-kit track: design

Date: 2026-11-16. Status: approved by the user on 2026-11-16, after a review by Codex, DeepSeek
and Grok (Gemini could not run). No firmware code yet.
Builds on: `docs/superpowers/specs/2026-11-16-session-panel-design.md` (branch `session-panel`).
Reference for ReBirth behavior: OpenRB-338 (https://github.com/pingw33n/openrb-338, GPL-3.0 like
this project): `crates/orb-engine/src/tr909.rs`, `docs/ui_spec.md` sections 6-7.

Decisions are marked: **(U)** decided by the user, **(A)** the author's call, open to correction.

## Intent

The sequencer has 8 tracks, and today every drum sound takes one of them (`TRACK_TYPE_DRUM` plays
one fixed `drum_note`). Kick, clap, two hi-hats and a percussion sound use 5 of 8 tracks, which
leaves too few for the bass, acid lines, leads, pads and effects of a layered goa / acid trance set.

A new track type, the **kit**, plays a whole drum machine from one track, the way the TR-909
section of ReBirth does: one track, several drum voices, each with its own lane of 16 steps. The
target machine is a 909 (an original TR-909 or a 909-family machine such as the RD-9, TR-09 or
TR-8S) played over DIN MIDI **(U)**.

The panel and the 8-track count do not change.

## Goals

- One track plays up to 11 drum voices of a 909 plus its accent lane.
- A kit cell is launched, queued, muted and shown on the grid exactly like any other track's cell.
- The 16 step buttons edit one lane at a time; the lane is picked with the TRACK encoder.
- Accent and flam are programmable per step, as on the 909.
- Sequencer timing stays as it is: no measurable jitter added, MIDI Clock no worse than today, no
  transport stop from kit traffic.

## Non-goals (later, if ever)

- Muting single voices inside a kit live (the kick stays on its own track, see "The kick").
- Kit voices on the CV/Gate outputs (a 909 has no trigger inputs; the 4 gates stay for the synths).
- Velocity per step beyond the three levels below; per-voice MIDI channels.
- Other lane layouts (an 808 layout with LC/MC/HC/CL/MA); the notes are editable, so another
  machine can be driven through the 909 layout with its own note map.
- Merging existing one-sound DRUM tracks into a kit; clip copy tools.
- Swing, MIDI input, external sync (as in the session-panel design).

## Anti-goals (a failure even if it works)

- Any Core 1 timing regression: GPIO 7 jitter stats or wire-level MIDI Clock worse than today
  beyond the budget below.
- A kit producing wrong notes on the drum machine (garbage from misread step data, stuck notes).
- The transport stopping because of kit MIDI traffic.
- Any live kit action that needs a menu.

## Constraints

- RP2040, Core 1 timer ISR at 96 PPQN; the ISR plays prepared rows and does not read pattern data
  (`src/sequencer.c`).
- DIN MIDI at 31,250 baud: one byte is 0.32 ms, a 3-byte message about 1 ms. Every hit on the
  same step queues behind the others.
- Measured firmware RAM today (`session-panel`, `arm-none-eabi-size`): `.data` 4.4 KB + `.bss`
  69.8 KB of 264 KB. Of that: `g_seq` 17.1 KB, `prepared_patterns` 28.9 KB, the save's static
  `pattern_buf` 14.8 KB.
- The target ABI uses one-byte enums: `pattern_t` is 459 bytes on the RP2040 and 532 bytes on the
  macOS host where the tests run. Nothing may hard-code either size; layouts come from the types.
- Flash storage: 128 KB reserved at the end of flash; today's save uses about 18 KB.
- Routing belongs to the track: `seq_set_track_config()` writes a track's config to all 32
  locations (session-panel design, ARCHITECTURE.md "Playback Rules"). Older saves whose locations
  differ still play as before.

## Approaches considered

**How a kit is stored.** Decided by the user: **a separate array (U)**.
- **Separate array.** Kit steps get their own field, never aliased with `step_t`. A wrong type
  plays silence, not wrong notes; changing a track's type erases nothing. Costs 16 KB of RAM and
  flash, mostly unused.
- Packed into the same 48 bytes as the cell's `step_t[16]` (12 lanes x 16 steps x 2 bits is exactly
  48 bytes). No memory growth, but the same bytes mean different things by type: every step
  function and the save validator must branch on type, a type mismatch plays garbage notes, and a
  type change must erase all 32 cells of the track.
- A kit as 12 hidden ordinary tracks. Reuses `step_t` but multiplies memory by 12 (~147 KB).
  Rejected.

**Picking the lane the step buttons edit.** Decided by the user: **the TRACK encoder walks the
kit's lanes (U)**.
- **TRACK rotation steps through the lanes of a kit** like ReBirth's 12-position instrument
  selector. No chords, no conflict with any press. Passing a kit costs 12 detents; a pad press on
  another track's playing clip jumps there without changing playback.
- Push-and-turn TRACK selects the lane, plain turn selects the track. Faster, but the TRACK long
  press (bank change, fires at 500 ms while held) would have to move to release, and a slow
  push-turn would still risk a bank change.
- Hold the kit's MUTE and press a step. Rejected: MUTE acts on press, so as a modifier it would
  either mute the whole kit during the selection or start acting late, on release.

**The kick (A).** Recommended: the kick stays an ordinary `DRUM` track on the 909's MIDI channel
(note 36), and the kit plays everything else. In 90s live rigs each drum sound had its own mixer
channel and the kick was dropped there; here the MUTE row is that mixer, so the kick keeps its own
one-press mute and its own clips. The kit still has a BD lane for anyone who wants the kick inside.
Drums then take 2 of 8 tracks.

- The kit's AC lane does not reach a kick on its own track: tracks stay independent in the ISR,
  since the two can play different locations, lengths and phases. The kick is accented with its own
  step accent flag (velocity 127). Its plain steps use the step velocity (100 when switched on), not
  the kit's `vel_normal`; on a machine whose accent threshold lies below 100 that velocity is set
  lower in the step editor.
- The kick track and the kit's BD lane send the same note on the same channel, and a kit Note Off
  would cut the kick track's note. The BD lane stays empty while a separate kick track is used.
- The README says all three.

## The kit

### Lanes and step values

A kit has 12 lanes in ReBirth's TR-909 column order (A):

| Lane | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Name | AC | BD | SD | LT | MT | HT | RS | CP | CH | OH | CC | RC |

Lane 0 (AC) is the total accent: it plays nothing itself and makes every hit on its step one level
louder. Lanes 1-11 are voices; voice `v` is lane `v + 1`, and every per-voice array below is indexed
by voice in this lane order.

A voice step holds one of four values, as in ReBirth's 909 (A):

| Value | Meaning |
|---|---|
| 0 | off |
| 1 | note |
| 2 | accented note |
| 3 | flam |

An AC step is 0 (off) or 1 (on). ReBirth's value 4 (accent + flam) has no entry here, as in
ReBirth's own panel.

### Storage

`pattern_t` gains a last field (A):

```c
uint32_t kit_steps[NUM_TRACKS][STEPS_PER_PATTERN]; // lane i in bits 2i..2i+1, bits 24-31 zero
```

One `uint32_t` per step keeps a step's 12 lanes in one aligned word for the ISR. The field is used
only when that location's `tracks[c].type` is `TRACK_TYPE_KIT`; the `step_t` data of the same cell
is kept untouched and is used again if the type is changed back. 32 locations x 8 tracks x 16 steps
x 4 bytes = 16 KB, plus one alignment byte per pattern on the target.

The fields before `kit_steps` keep their offsets, so an old pattern record is a byte prefix of the
new struct; only the record stride in an old flash image differs (see "Storage").

A kit cell's length is the cell's existing `track_length[c]`; all lanes share it.

The `pattern.c` helpers that init, clear or shift a cell or a track (`pattern_init`,
`pattern_clear`, `pattern_clear_track`, `pattern_clear_step`, `pattern_shift`,
`pattern_shift_track`) treat `kit_steps` alongside `steps`, whatever the type, so the two never
drift apart (A). `pattern_copy` already copies the whole struct.

### Kit settings

A new `TRACK_TYPE_KIT` joins `TRACK_TYPE_SYNTH` and `TRACK_TYPE_DRUM`. The MIDI channel, gate
length and stored mute of a kit come from `track_config_t` as for any track. `output_mode`,
`cv_channel` and `drum_note` are ignored for a kit: kits are MIDI only (A).

The kit's own settings belong to the track, not to the location (A), in a new array
`kit_config_t kit_config[NUM_TRACKS]` in `g_seq`:

| Field | Range | Default | Meaning |
|---|---|---|---|
| `note[11]` | 0-127 | BD 36, SD 38, LT 45, MT 47, HT 50, RS 37, CP 39, CH 42, OH 46, CC 49, RC 51 | MIDI note per voice, in lane order (the RD-9's default map, close to General MIDI; other models are set on the bench) |
| `vel_normal` | 1-127 | 80 | a note without AC |
| `vel_accent` | 1-127 | 110 | an accented note without AC, or a note with AC |
| `vel_max` | 1-127 | 127 | an accented note with AC |
| `flam_pulses` | 1-12 | 3 | time from a flam's first hit to its second, in 96-PPQN pulses (12.9 ms at 145 BPM) |

Every `kit_config` gets these defaults at power-up (`sequencer_init()`, which runs before any load
and also sets up every host test) and when an old save without kits is loaded. A track switched to KIT uses its already initialized settings.

The three velocities give the three dynamic levels the 909 has (normal, accent, accent plus total
accent). Machines with a single accent threshold work with the defaults as long as the threshold
lies between `vel_normal` and `vel_accent`; the exact thresholds of the user's model are a bench
item.

**At most two kit tracks (A).** A track counts as a kit when its type is KIT in any location. At
most two tracks may be kits, so at most two kits ever play at once, also in Song mode and with
mixed launches (see "MIDI load"). The type-change path refuses a third kit track with a message on
the OLED before changing any data, and the save validator enforces the same rule.

## Playback (Core 1)

### Prepared kit rows

The ISR keeps playing prepared rows. `prepared_track_pattern_t` gets a kind tag and a union (A):
the existing 16 events for SYNTH and DRUM, or for a KIT:

- `lanes[16]`: the cell's `kit_steps`, with steps at and beyond the length zeroed and everything
  zeroed when the stored mute is set (as `STEP_FLAG_ACTIVE` is cleared today);
- the kit's 11 notes, the three velocities, `flam_pulses`, the MIDI channel and `gate_pulses`.

A prepared kit row is self-contained and smaller than an event row (about 82 bytes against 112), so
the existing revision/refresh path (`sequencer_refresh_track_playback`, `load_prepared_track`) works
unchanged, and `prepared_patterns` grows only by the tag (under 1 KB). Editing the kit settings goes
through a new `seq_set_kit_config(track, cfg)` that stores them and refreshes that track's 32
prepared rows, like `seq_set_track_config`.

A kit cell **has a clip** when any voice lane (1-11) has a non-zero value within its length; AC
alone is not a clip. As for other tracks, this is computed before the stored-mute blanking, so a
stored mute does not hide the clip on the grid (`test_clip_mask_ignores_stored_mute`).

### Runtime state per kit track

The ISR keeps, per track that plays a kit: the sounding mask (11 bits) with the note each sounding
voice was sent on and one release channel. Every voice is released at the latest at the next step
boundary, before that step's hits, so all sounding voices come from one step and share the channel
stored at that step; a later settings edit therefore cannot strand a note (host builds check this
invariant before a channel is stored). Then the note-off countdown, the flam mask, the flam countdown and the flam velocity (one per step, as AC is per
step). That is under 30 bytes per track.

### Order of work in one pulse

Every pulse, in this order (A; the first three are what the ISR does today for other tracks):

1. MIDI Clock (see "MIDI Clock ahead of note bytes").
2. Live mutes: a muted track releases what it has sounding; for a kit, every sounding voice, and a
   pending flam is cancelled.
3. Note-off countdowns. A kit whose countdown reaches 0 releases all its sounding voices.
4. Flam countdowns. A kit whose flam countdown reaches 0 plays the second hits.
5. On a step boundary only: the step (below), built in `midi_buf` and flushed by `commit_step()`.

Steps 2-4 for kits are sent with their own `midi_send_message()` call, each from a small local
buffer, and never through `midi_buf`, which is cleared at the start of step 5 and by Stop. Their
messages are therefore already in the TX queue when a step's Note Ons are built. With the gate at
100 % the countdown ends exactly on the next step boundary, in step 3 of that pulse, before the new
Note Ons.

**A kind change releases first.** When `load_prepared_track` installs a row whose kind differs from
the row the track played (a type change in the track config, or a load), the ISR first releases
everything the old kind left sounding: the event note on its stored route (MIDI and the CV gate),
or all kit voices, and a pending flam. Only then does the new row play. This covers tie and slide
notes, whose countdown is 0 today, and type changes during playback.

### A kit step

At a step boundary, for a kit track that is not muted:

1. Read the step word. AC is on when lane 0 is non-zero.
2. Any voice still sounding at this point is released first, in a message of its own before the
   step burst (it can only happen after an edit or a kind change, since the countdown is at most
   one step).
3. For each voice in the fixed order BD, SD, CP, CH, OH, RS, LT, MT, HT, CC, RC (A; a fixed table
   of voice indices, so the kick and the timing-critical sounds go first in the MIDI burst), by its
   value:
   - 1 (note): Note On at `AC ? vel_accent : vel_normal`;
   - 2 (accent): Note On at `AC ? vel_max : vel_accent`;
   - 3 (flam): Note On at the note level now (the first hit), and the voice joins the kit's flam
     mask; the second hit, one level louder, comes `flam_pulses` later (pulse order step 4) as Note
     Off then Note On. ReBirth plays a flam the same way: the step at x0.8, then a retrigger at x1.5
     after the flam interval (`tr909.rs`, `step`).
4. The voices that sounded join the sounding mask, and the note-off countdown is set to
   `gate_pulses`, or `min(PULSES_PER_STEP, gate_pulses + flam_pulses)` when the step has a flam, so
   the second hit keeps a full gate or the rest of the step.

Note Offs of kit voices are sent as Note On with velocity 0 (A), so they share one status byte with
the kit's Note Ons; SYNTH and DRUM tracks keep sending 0x80 as today.

`flam_pulses` is when the second hit is put in the TX queue. On the wire the interval also depends
on what is queued: after a dense step burst the second hit leaves only when the burst has gone.
With one kit of six voices the burst is 13 bytes (4 ms) and a 12 ms flam stays a flam.

**Overload rule (A).** A flam's second hit is the only kit message that may be dropped. Flam
second hits must leave 288 TX queue bytes free; the headroom covers the releases and new first
hits of an immediate restart as well as scheduled playback. Otherwise that step's second hits are
skipped, the voices stay sounding until their note-off, and a counter shown with the jitter stats
goes up. Scheduled playback adds at most 168 bytes in mandatory messages in one pulse (two kits'
note-offs and the worst step burst, as stored in the queue with every status byte); the first
design reserve of 192 bytes left no room for a restart (Start while playing, or Stop then an
immediate Start) under two dense kits, whose releases join the backlog before the new run's first
step, and the queue overflowed (multi-model review). Everything else fits the DIN rate at every
tempo up to 300 BPM (see "MIDI load"), so kit traffic never trips the TX-overflow panic.

### Mutes, stop, panic, launches

- A **live** mute (the MUTE row) releases all sounding kit voices on the next pulse and cancels a
  pending flam.
- A **stored** mute (track config) takes effect at the next step, as it does for other tracks
  today: the refreshed row is installed at the step boundary, and voices already sounding end at
  their countdown.
- Stop, Panic, Start while playing and a play-mode change release all sounding kit voices and
  cancel flams through the direct send path, never through `midi_buf`. The releases are queued
  before the MIDI Stop byte; the Stop, a real-time byte, may overtake them on the wire, as may the
  Start and Clock of a restart (real-time bytes too), and every voice is still released before the
  next run's notes.
  Panic still clears the TX queue and sends All Notes Off afterwards.
- Launches, launch quantization and Song mode treat a kit like any track. A pending flam always
  resolves inside its step (`flam_pulses` <= 12 < 24 pulses), so it never crosses a launch point.
  At the end of a song the ISR leaves before the step block; the releases above do not depend on
  that block.
- An edit refresh during a flam does not change the second hit: its notes and velocity were taken
  at the step.

### MIDI queue and running status

The TX queue keeps whole messages, each with its status byte. Running status is applied by the
drain as it writes to the UART (A): a status byte equal to the last one written is skipped, except
for the first message after the queue has run empty (the moment its last byte was written), which
always carries its status byte so that a receiver that missed a byte resynchronizes. Real-time
bytes (Clock, Start, Continue, Stop) do not change the running status. Message boundaries are
therefore known in the queue without re-parsing a compressed stream.

`MIDI_BUF_SIZE` grows from 64 to 160 bytes and the TX queue from 256 to 512 bytes (A): the worst
step burst is 102 bytes as stored (82 on the wire, below), a pulse with two kits' note-offs as well
holds 168, and the overload rule keeps a 288-byte reserve. Host tests check the worst designed
burst fits, and a restart under two dense kits.

The step burst is built in track order, so the kick and kit on the leftmost columns get the
earliest bytes; the README says so. Within one step the last Note On leaves after the earlier ones:
about 4 ms for one kit of six voices, about 7 ms for a kit with all 11, and 26 ms from first to
last in the worst designed step. That spread is inherent to DIN MIDI.

### MIDI Clock ahead of note bytes

Today Clock bytes join the same first-in-first-out TX queue as the notes (`src/midi_out.c`), behind
up to 32 bytes already in the UART's hardware FIFO. The next Clock byte comes 4 pulses after a
step: 65 bytes of wire time at 120 BPM, 52 at 150 BPM. Today's worst step burst is 48 bytes, so
Clock is never held back at those tempos. A kit step burst (up to 82 bytes) or a flam burst would
hold the next Clock byte back by up to about 10 ms at 150 BPM. That breaks the anti-goal, so the TX
path changes (A):

- `midi_send_clock()` sets one pending-Clock flag (not a counter, so Clocks are never released in
  a bunch) under the TX lock and runs the drain at once. The UART TX interrupt stays enabled while
  the queue is non-empty **or** a Clock is pending, so a Clock goes out even when no note is
  queued.
- Start, Continue and Stop are real-time bytes too: they wait in a small queue of their own and go
  out at the next message boundary, ahead of queued note messages, in the order they were sent.
  After a quick Stop and Start they therefore never sit behind the previous run's note bytes.
- At a message boundary the drain sends, in this order: a queued Start / Continue / Stop, then a
  pending Clock, then the next note message. A receiver never sees a Clock before the Start it
  belongs to (the existing test expects Start, Clock, Note On), and a Clock never waits longer
  than the byte on the line and one message.
- Stop and Panic clear the pending Clock in the same critical section in which they queue their
  bytes, so no Clock leaves after a Stop. `midi_clear_output_queue()` clears it too, and
  `midi_wait_output_idle()` (used before a flash save) also waits for it and for the real-time
  bytes.
- The UART's hardware FIFO is turned off, so nothing is committed in front of a pending Clock
  beyond the byte being shifted out and the one in the holding register. The UART interrupt then fires once per byte on Core 0: at most
  3,125 per second, each a few microseconds.
- Core 0 must not keep interrupts off for longer than 100 us (a third of a byte time), or bytes
  start to have gaps and a Clock waits longer. The bench measures the longest such window (the
  shared-state lock sections, the I2C and SPI drivers); a longer one is shortened, the FIFO stays
  off.

Result: A Clock byte may wait for the byte being shifted out and a complete channel message whose
status is already in the holding register: up to four byte times, 1.28 ms, before interrupt
latency. Comparison with the baseline remains a bench requirement. Note messages keep their order
among themselves, and Start, Continue and Stop among themselves.

### MIDI load

Worst designed step: two kits with all 11 voices on, plus six other tracks each releasing and
starting a note. Kits: 2 x (3 + 10 x 2) = 46 bytes; others: 6 x 6 = 36 bytes; 82 bytes, about
26 ms of wire time. Each kit's note-offs come in a separate message of 23 bytes. A flam message for
a kit with all 11 voices flammed is 1 + 11 x 4 = 45 bytes (Note On velocity 0 and Note On per
voice, one status byte).

Sustained load with every lane on every step, both kits, all other tracks busy and MIDI Clock, no
flams: about 134 bytes per step (note-ons 46, kit note-offs 46, other tracks 36, six Clock bytes),
which is 43 % of the DIN rate at 150 BPM and 86 % at 300 BPM. Flamming every voice of both kits on
every step adds 90 bytes per step and would exceed the rate above about 210 BPM; the overload rule
drops second hits instead. The realistic case (goa, 130-150 BPM, a kit of 4-8 voices) stays well
under half the rate.

### ISR time

A kit step costs 11 two-bit tests and up to 11 two-byte appends; a flam or note-off pulse
up to 11 four- or two-byte appends. Estimated at a few microseconds per kit on the RP2040.
Requirement: with two dense kits (every voice, every step, flams on) the GPIO 7 maximum ISR
duration rises by no more than 20 us over today's figure, and IRQ latency, period error and
wire-level MIDI Clock stay as they are (JITTER_MEASUREMENT.md, a new load case).

## Panel (Core 0)

### Focus and the TRACK encoder (U)

For a kit track, the focus is (track, location, lane). TRACK rotation walks a sequence of stops: a
SYNTH or DRUM track is one stop; a kit track is 12 stops, AC to RC. Rotating right into a kit
lands on AC, rotating left into it lands on RC (A), so a position is always the same number of
detents from its neighbors, as on a physical selector. Rotating changes the lane without changing
the focused location; leaving the kit moves to the next track as today.

**One detent is one stop (A).** `encoder_get_delta()` today returns quadrature edges, multiplied by
4 or 8 when turned fast. Walking the stops uses whole detents instead: edges are accumulated and
divided by the edges per detent of the chosen encoder part (the remainder is kept), without
acceleration. VALUE keeps its acceleration for tempo and values. (Final review: every screen reads
whole detents, `encoder_get_turn()`. Numbers accelerate by detent rate, x4 then x8 on a fast turn,
in one place, `encoder_accel_detent()`; choices never accelerate.)

Each kit track remembers its last focused lane in UI RAM (A; not saved; BD, lane 1, after
power-up, not the zero value AC). A pad press on a kit column moves the focus there onto that
remembered lane.

**A step press belongs to the lane it was made on (A).** Since `session-panel` commit a86ef4a a
Session step press acts, on release or long press, on the cell that was focused when it was made,
whatever the focus does in between. A kit press also remembers the lane: a press made on SD acts on
SD even if TRACK has moved to LT or CH before the release. The Pattern view binds its presses the
same way.

### Step buttons on a kit lane (A)

In the Session view and in the Pattern view, with the focus on a kit lane (on release, unless it
became a long press, as the Session view does today):

- a voice lane cycles off -> note -> accent -> off, as one click does on ReBirth's 909; a flam step
  goes to off;
- the AC lane toggles off/on;
- setting CH on a step clears OH on that step and the other way round, as ReBirth's 909 editor does
  (the machine chokes one with the other anyway);
- a long press opens the kit step editor on that step;
- steps beyond the cell's length are dark and do nothing.

Edits go through `sequencer_refresh_track_playback`, so they are heard at once. No kit editor
changes the cell's `step_t` data.

### Pattern view on a kit (A)

The Pattern view on a kit track uses the same stops for TRACK rotation, the same step-button rules
as above, the focused lane on the step LEDs and the OLED, and a TRACK press opens the kit step
editor on the selected step instead of the note editor. The note/velocity/slide/tie step editor
never opens on a kit track.

### Kit step editor (A)

Shows the step number and the values of all 12 lanes at that step. TRACK rotation (by detents)
moves the lane cursor, starting on the focused lane; VALUE rotation sets that lane's value (off /
note / accent / flam; AC off / on), with the CH/OH rule applied. VALUE moves by the detent count:
one step per click, clamped, with no acceleration (multi-model review). Step buttons select another step
within the length; the old step editor's gestures (active toggle by long press, slide and tie) do
not exist here. VALUE press returns to the screen it was opened from.

### Lights and OLED

- **Grid, SCENE, MUTE:** no change; a kit column follows the same light rules with the kit's "has a
  clip" rule. MUTE mutes the whole kit.
- **Step LEDs** (single color, on/off), in the Session view and the Pattern view: the steps of the
  focused lane whose value is non-zero, within the length; the playhead blinks as today. Accent and
  flam are not visible on these LEDs.
- **OLED, Session screen:** when the focus is on a kit, the focus line names the track, location,
  lane (`SD`) and length, and a 16-cell strip shows the lane's values with four distinct glyphs
  (off, note, accent, flam) and the playhead.

### Track config (A)

TYPE cycles SYNTH -> DRUM -> KIT; KIT is skipped with a message when two other tracks are kits
already. For a kit the screen lists: TYPE, CHANNEL, GATE, FLAM (pulses, with the time at the
current tempo), VEL NORM, VEL ACC, VEL MAX, then one NOTE entry per voice (BD to RC). OUTPUT and CV
are hidden for a kit. A type change keeps both the `step_t` data and the kit steps; the engine
releases what was sounding (see "A kind change releases first"). CHANNEL, GATE and TYPE are written
with `seq_set_track_config` (all 32 locations); the rest with `seq_set_kit_config`.

## Storage: format version 6

**v6 layout (A):** header page; patterns (`MAX_PATTERNS` x the new `pattern_t`, padded to a page);
song (padded to a page); kit settings (`kit_config[NUM_TRACKS]`, padded to a page). The checksum
covers the three payloads at their unpadded lengths. Saves while stopped only, as today.

**Layouts are per version.** `flash_storage_is_valid()` and `flash_storage_load()` accept versions
4, 5 and 6 and take every offset and length from the version, never from the current types alone:

| | v4 | v5 | v6 |
|---|---|---|---|
| Pattern record | legacy `pattern_t` | legacy `pattern_t` | new `pattern_t` |
| Patterns at | 256 | 256 | 256 |
| Song at | 256 + page-rounded legacy patterns | same as v4 | 256 + page-rounded new patterns |
| Kit settings | none | none | after the page-rounded song |
| Checksum | none | legacy patterns + song | new patterns + song + kit settings |

On the target the legacy pattern record is 459 bytes (patterns 14,688 bytes, song at 15,104); the
new one is 972 (patterns 31,104 bytes, song at 31,488). The legacy struct (today's `pattern_t`,
renamed) stays in the code so that its size and stride come from the compiler on each ABI.

**Loading v4 and v5:** each legacy record is copied into the first bytes of a zeroed new
`pattern_t` (its fields keep their offsets), `kit_steps` stay zero and every `kit_config` gets the
defaults. v4 keeps loading without a checksum. After any load all prepared rows are refreshed, as
today.

**Validation adds:** `type` may be KIT; at most two tracks are a kit in any location taken together
(the rule above); bits 24-31 of every kit step word are zero and the AC lane is 0 or 1; the kit
settings are in range. Types may still differ between the locations of a track, as old saves
allow; the engine handles a kind change at any launch.

**Memory:** `g_seq` grows by 16,416 bytes for `kit_steps` (with the alignment bytes) and about 120
bytes for the kit settings; the prepared rows and the kit runtime state add about 1 KB. The
pattern payload grows to 31,104 bytes, which is not a whole number of flash pages. If the save
programs whole pages straight from RAM and pads only the last page through a one-page buffer, the
14.8 KB static `pattern_buf` goes away and RAM ends at about 77 KB; if the copy buffer is kept and
grown, about 108 KB (builder's call; both fit). Flash use rises to about 35 KB of the 128 KB
reserved.

## Testing

Host tests (the existing fakes and `test_playback.c` harness):

- lane get/set helpers, the value cycle, the AC toggle, the CH/OH rule; `pattern.c` helpers move
  and clear `kit_steps` together with `steps`;
- prepared kit rows: each voice's note in lane order, velocity levels for every value x AC, stored
  mute, length cut-off, "has a clip" (AC alone is not; a stored mute does not hide it);
- playback in the real ISR code: a dense kit step emits Note Ons in the voice order; AC levels; the
  flam's second hit at exactly `flam_pulses` with the right velocity; note-offs at the countdown
  as Note On velocity 0, with gate 10 %, 50 % and 100 % and with a flam (gate 10 % and flam 12
  included); the pulse order (note-offs and flams never lost to `midi_buf_clear`); a live mute and a
  stored mute; Stop, Panic, Start while playing and a play-mode change release kit voices; a type
  change KIT <-> SYNTH (with a tie/slide note) and KIT <-> DRUM during playback leaves no Note On
  unpaired and no gate high; launches under every quantization; Song mode with kits at different
  locations; an edit refresh during a flam; the end of a song;
- MIDI load: two dense kits plus six busy tracks for 64 bars at 150, 200 and 300 BPM, and with
  every voice flammed at 300 BPM: no TX overflow, no burst truncated, second hits dropped and
  counted only in the flam case;
- the TX path over a fake UART that drains one byte per byte time: running status in the drain
  (status after the queue ran empty); a pending Clock with an empty queue goes out at once; with a
  busy queue at the next message boundary, never inside a message, waiting at most one message
  during an 82-byte step burst and a flam burst; after a queued Start or Continue, never before it;
  never after a Stop; cleared by Panic and `midi_clear_output_queue`; waited for by
  `midi_wait_output_idle`; 96 pulses of an empty pattern give exactly 24 Clock bytes 4 pulses
  apart;
- storage: v6 round trip; real v5 and v4 images built with the legacy layout (song at the legacy
  offset, checksum over the legacy lengths) load with empty kits and default settings; invalid kit
  data and a third kit track (also spread over different locations) are rejected;
- UI: detents walk the stops one by one at any speed, across a kit in both directions; the pad jump
  onto the remembered lane (BD after power-up); short and long presses on kit lanes in the Session
  and Pattern views; a step held while the lane or track changes does nothing; the kit step editor;
  a type change keeps both kinds of step data; the third-kit refusal; kit settings at their
  defaults after power-up.

Bench (HARDWARE_STATUS.md): the user's 909 receives the note map, the three velocity levels give
audible normal / accent / accent + AC, running status and Note On velocity 0 are understood, flams
sound as flams at 140-150 BPM; GPIO 7 statistics and the wire-level MIDI Clock interval with two
dense kits against today's baseline (measured from a build of the current firmware first); the
longest interrupts-off window on Core 0 and the UI loop time with the per-byte UART interrupt.

## Documentation

README (the kit track, how to set it up for a 909, kick on its own track with the BD lane empty,
the kick's accents and velocity, drums on the leftmost columns), ARCHITECTURE.md (kit data,
prepared kit rows, the pulse order, the TX queue with running status and Clock ahead of note bytes,
flash layout v6), JITTER_MEASUREMENT.md (the two-kit load case), HARDWARE_STATUS.md (bench items
above).

## Left to the builder

Names of types and functions; the exact layout of the prepared kit row and the union; how the
legacy struct is named; whether the save programs from RAM or keeps a copy buffer; the OLED glyphs
and the layout of the kit step editor and the config list; the edges per detent of the encoder
part; the precise RAM figures after the change (from the target ELF).
