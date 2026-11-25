# Song Chain of 512 Rows — Design

Date: 2026-11-25. Status: draft for review. The project owner decided the goal:
the Song-mode chain grows from 256 to 512 rows (`MAX_SONG_ENTRIES`), so a MIDI
song laid out as the device's clips fits whole. Clips stay 16 steps
(`STEPS_PER_PATTERN`); longer clips are a separate open design (PR #18). Choices
this design makes are marked **(call)**; what the owner should settle is under
"Open questions". No assembled board exists: the figures below come from
firmware builds, host code and the simulator. Statements about today's code refer
to `origin/master` 86975bc; RAM figures are from builds of that commit with only
`MAX_SONG_ENTRIES` and the legacy song record changed (Pico SDK 2.2.0,
`arm-none-eabi-size -A`, `nm`).

## Intent

"It's a Sham, Man" (Twisted pack) is 361 bars. Laid out as 16-step clips, its bass
alternates two clips every bar, so the chain needs 355 rows; today it stops after
bar 262. Five of the eight Twisted songs are cut. With 512 rows every one fits
(`MidiClips.plan` in Node, origin/master):

| Song | Bars | Rows needed | Plays today (256 rows) |
|---|---:|---:|---:|
| 01 Contains LSD | 235 | 228 | 235 |
| 02 The Synths of Orpheus | 269 | 260 | 263 |
| 03 Centaurs Are Real | 356 | 321 | 278 |
| 04 A Magician Nevers Reveals His Tricks | 256 | 248 | 256 |
| 05 It's a Sham, Man | 361 | 355 | 262 |
| 06 Snarled Up | 282 | 272 | 260 |
| 07 Are You Sponged | 255 | 240 | 255 |
| 08 Sunstead | 281 | 269 | 268 |

## Goals

1. The chain holds 512 rows on every board, so a save made on one loads on the
   others **(call)**: the same rule as the long-clips design.
2. A project saved before this change (format 7) still loads, as do eight-track
   saves (formats 4-6).
3. The simulator's clips mode writes as many rows as the core it runs holds, and
   says so; all eight Twisted songs play whole.

## Non-goals

- Clips longer than 16 steps, more than 32 locations per track.
- Shorter `seq_lock` sections for song edits (see "Risks"; a follow-up).
- Faster scrolling in the song editor (an open question, not part of this change).
- A save that survives power loss during the save (one slot, as now).

## Today

- **Record.** `song_entry_t` (`include/sequencer.h:149-152`) is 64 location
  bytes plus `repeats`: 65 bytes, byte-aligned, no padding. `song_t`
  (`:159-164`) is 256 x 65 + `length` (`uint16_t`) + `end_mode` + `name[9]`:
  16,652 bytes on the device (`-fshort-enums`), 16,660 in the host and wasm
  builds (4-byte enums). It lives once, in `g_seq.song` (`:238`). Nothing else
  copies it: no undo, no clipboard, no staging buffer; save programs it straight
  from RAM (`src/flash_storage.c:294`), load copies it straight from XIP (`:342`).
  The firmware has no `malloc` user.
- **Widths.** Song positions are already `uint16_t`:
  `transport_t.song_position` (`sequencer.h:205`), `song_insert*()`,
  `song_remove()`, `song_move()` (`sequencer.h:329-333`, `src/song.c`),
  `locate_song()`'s `pos` (`src/sequencer.c:976`), `ui.song_cursor`
  (`src/ui.c:88`; the turn sums in `int16_t`, `:1833`), and
  `display_draw_song_view()`'s `pos` (`src/display.c:910`). `repeats` and
  `song_repeat_count` stay `uint8_t`. No position is `uint8_t`.
- **Display.** Rows print as `%3d` (`display.c:938`) and the footer as `%u/%u`
  at x 84 (`:952-953`): "512/512" is 7 x 6 px and ends at x 126 of 128. No
  display change.
- **SPP.** The Song Position Pointer is 14 bits of sixteenths
  (`src/clock_in.c:53-66`), 1,024 bars, whatever the chain length;
  `locate_song()` walks one entry per lock section. Unchanged.
- **Save format 7** (`flash_storage.c:17-86`): one 192 KiB slot at `0xB0000`,
  erased whole on every save (`STORE_ERASE` = the slot). Header page, then
  patterns, song, kits, each page-aligned; the song is `sizeof(song_t)` raw, so
  its size fixes `KIT_OFFSET` and the CRC span (`:137-142`). The validity header
  is programmed last. `VERSION` is 7 (`:86`).
- **The legacy song record uses the live constant.** `old_song_t`
  (`flash_storage.c:64-69`) is declared `entries[MAX_SONG_ENTRIES]`, and the
  legacy loader bounds its length by `MAX_SONG_ENTRIES` (`:209`). At 512 the
  device build stops at `_Static_assert(sizeof(old_song_t) == 2316)` (`:73`,
  verified). The host build has no such assert, and `write_legacy_image()`
  (`test/test_storage.c:63-88`) uses the same struct, so host tests would keep
  passing while v4-v6 saves no longer loaded on the device.

## RAM

| Build | Free above `.bss` today | At 512 rows |
|---|---:|---:|
| RP2040 Release (`firmware-release`) | 20,712 | **4,072** |
| RP2040 Debug (`firmware-debug`, built by CI) | about 27,300 | 10,652 |
| RP2350 (`pico2-release`, `revb-rp2354b`) | 282,312 | 265,672 |

"Free" is `__HeapLimit` - `end`; it includes the 2,048-byte `.heap` reserve, so
the RP2040 Release build keeps about 2 KB beyond it. `g_seq` grows from 149,640
to 166,280 bytes; `.bss` by exactly 16,640 on every build. Stacks sit in the
scratch banks and do not change. Rev B and the Pico 2 have no board-specific
limit; the flash map is the same on all boards (`midi_song_flash.ld` requires 2
MiB). The four presets above link at 512; `revb-rp2350b` (the same chip with
external flash) was not built.

On the RP2040 this uses most of the remaining headroom. Known demands on it: the
four-outputs design adds about 150 bytes with one port; the long-clips design
(PR #18) would *free* about 9 KB net (it removes 45,632 bytes of prepared rows),
but it lists "more than 256 song entries" as a non-goal and needs fewer rows for
the same songs. See "Open questions".

## Design

### Firmware

- `MAX_SONG_ENTRIES` 512 (`include/sequencer.h:18`). `song.c`, `sequencer.c`,
  `ui.c` and `display.c` need no change: their bounds use the constant and their
  types are wide enough.
- **Frozen records first** (the four-outputs design's step, taken as is): before
  the constant changes, the on-flash records of format 7 and older get their own
  typedefs with fixed sizes. `OLD_SONG_ENTRIES` 256 for `old_song_t` and its
  length bound (`flash_storage.c:65`, `:209`); a `song_v7_t` with 256 entries
  (16,652 bytes on the device, 16,660 on the host), with size and offset
  asserts under both ABIs.
- **Format 8** **(call)**: format 7 with `song_t` of 512 rows. Same slot, same
  order and page alignment, same CRC rule (unpadded payload bytes, header last).

  | Payload | Device offset, bytes | Host/wasm offset, bytes |
  |---|---|---|
  | header | 0, 256 | 0, 256 |
  | patterns | 256, 131,456 | 256, 149,888 |
  | song | 131,840, 33,292 | 150,272, 33,300 |
  | kits | 165,376, 960 | 183,808, 960 |
  | used of 196,608 | 166,400 (v7: 149,760) | 184,832 (v7: 168,192) |

  A plain size bump keeps every reader of `g_seq.song` unchanged; splitting
  rows 257-512 into a fourth payload was considered and rejected (two song
  records to keep in step for no gain: firmware that reads only format 7 refuses
  either one by its version byte).
- **Loader order.** A valid format 8 in the slot; else a valid format 7 in the
  same slot (its song record copied row by row into a zeroed `g_seq.song`, kits
  from the format-7 offset), reported as `LOADED OK` **(call)**, as the
  four-outputs design does for its own step; else the eight-track slot, formats
  4-6, as today. Format 7 is validated with `length <= 256`, format 8 with
  `length <= 512` (`song_valid()` takes the bound).
- **Save** writes format 8 only. Downgrade: firmware that knows only format 7
  does not read a format-8 slot (its loader requires version 7) and falls back
  to the eight-track save or a failed load, as every earlier format step did.
  The first format-8 save replaces the format-7 one (one slot).
- **Cost.** A save programs 65 more pages (+16,640 bytes; about 26 ms at the
  typical 0.4 ms a page, which the simulator also models, `sim/core/hal.c:433`);
  the erase is unchanged. The bitwise CRC32 runs over 16,640 more bytes on
  save-verify and load.

### Simulator

- **The core tells the page its chain length (recommendation).** A number from
  the compiler, not a feature bit: `int sim_song_entries(void)` in
  `sim/core/sim_api.h` and `sim_main.c` returning `MAX_SONG_ENTRIES`, like the
  existing `sim_midi_song_capacity()` (`sim_main.c:98`) and
  `sim_flash_store_size()` (`hal.c:462`). `Device.songEntries()` returns it, or
  256 when the core has no such export (the "absent in an older core" idiom,
  `device.js:180`). Feature bits (`sim/build.sh:21-27`) are booleans grepped
  from the source to choose a score; a capacity is a number, and the grep would
  be one more place to keep in step.
- **The capacity also selects the save layout** **(call)**. `midi_clips.js`
  hard-codes format 7: `SONG_BYTES` 16,660 (`:25`), the length, end mode and
  name at `SONG_OFFSET + 16640/16644/16648` (`:318-320`), the version byte 7
  (`:323`). These become functions of `rows`: song record `rows x 65 + 20`
  bytes, the trailer at `rows x 65`, `KIT_OFFSET` after it, version 8 when
  `rows` is 512 and 7 when 256. An older core thus still gets a format-7 image.
  The mapping holds only while this change is format 8; if it becomes 9 (Open
  question 2), the core reports its save version too (`sim_store_version()`).
- `plan()` runs in Node without a device, so the limit is a parameter:
  `plan(song, { rows })`, `store(p, { rows })`, `show(p, …, { rows })`, default
  512 **(call)**; `app.js` passes `device.songEntries()`. Every `256` in
  `midi_clips.js` (`:8-9`, `:251`, `:269`, `:314`, `:318`, `:361`) and the log
  line in `app.js:1869` ("256 строк") read `rows`; nothing else in `sim/web`
  (`index.html` included) names the chain length. With PR #31 merged, the cut lives in its
  `chain()`, so the slice is in one place.
- Unaffected: the MIDI Song player and its UF2 export (`openMidiSong`,
  `firmwareMidiImage`, its own 1 MiB slot at `0xE0000`,
  `sim/test/midi_song_check.js:48-60`), and the "MIDI Song · выходы ReBirth" mode,
  which plays the timed file, not the chain. The clips image is for the
  simulator only (`midi_clips.js:21-23`): no board UF2 carries a chain. The page
  keeps no save in browser storage (`localStorage` holds part sounds only,
  `app.js:902-965`), so there are no old images to migrate.

### Docs

`ARCHITECTURE.md:433-450` (format 7 → 8, the format-7 fallback),
`README.md:19` (the chain length), `sim/README.md:199-201` (no Twisted song is
cut at 512), `include/flash_storage.h:16-24`, and the stale song-edit figures in
`JITTER_MEASUREMENT.md:160, 171-176` and `HARDWARE_STATUS.md:276-278` (below).

## Risks

1. **Song edits hold `seq_lock` far longer than the docs say.** `seq_lock()` is
   a spin lock with interrupts off (`src/shared_state.c:14-17`). `song_insert*()`,
   `song_remove()` and `song_move()` shift the rows after the edit point with
   `memmove` under it (`song.c:84-88, 109-113, 141-149`). The firmware's newlib
   `memmove` copies byte by byte here: an insert, or a move to an earlier row,
   takes the backward byte loop (about 7 cycles a byte); a remove, or a move to
   a later row, the forward one (about 9), because rows 65 bytes apart are never
   both word-aligned (disassembly of the Release build). `JITTER_MEASUREMENT.md` still counts 9-byte eight-track rows
   (255 x 9 = 2,295 bytes, 130-165 us).

   | Edit at the front of a full song | Bytes | RP2040 at 125 MHz |
   |---|---:|---:|
   | 256 rows, today | 16,575 | 0.93-1.19 ms |
   | 512 rows | 33,215 | 1.86-2.39 ms |

   One pulse is 2.08 ms at 300 BPM and 4.28 ms at Sham's 146. Core 1's timer
   ISR takes `seq_lock` every step and waits; Core 0's MIDI receive interrupt
   runs with the UART FIFO off (`src/midi_out.c:213`): a byte arrives every
   320 us and the holding register keeps one, so about 2-3 input bytes (Clock
   included) can be lost today and about 5-6 at 512. This needs an edit
   near the front of a long song while it plays (README:336 allows it). Estimate
   from the instruction count, not a measurement. Proposed: accept for this
   change, correct the two docs, and keep shorter lock sections as the follow-up
   the long-clips design already names (Open question 3).
2. **RP2040 headroom** drops to about 2 KB beyond the heap reserve (see "RAM").
   A later RP2040 feature may not link. Proposed: a size line in the PR and a
   link-time check of the margin (Open question 1).
3. **Song advance timing** is unchanged per bar: `advance_song_position()` reads
   one row (`sequencer.c:698-765`). Its skip loop over zero-repeat rows can walk
   up to 512 rows under the lock in the ISR; only a song of mostly skipped rows
   reaches that, a few tens of microseconds.
4. **Simulator run time.** Sham plays 361 bars instead of 262, so whole-song
   checks of the clips mode (`midi_clips_check.js` section 3, PR #31's
   `midi_live_check.js`) run up to about 40 % longer.

## Tests

Host (`ctest --preset host-tests-debug` in `test/`; a red `playback_tests` on a
loaded machine may be its 30 s `TIMEOUT`, look for "Timeout" first):

- `test_song.c:84-91` and `test_ui.c:2415-2416` follow the constant (512 rows
  appended; the song screen rendered at row 512).
- `test_storage.c:346, 608`: `MAX_SONG_ENTRIES + 1` → `OLD_SONG_ENTRIES + 1`
  (257), so the legacy bound is what is tested.
- New: a format-7 image written with the frozen `song_v7_t` (256 rows, kits at
  the format-7 offset) loads as `LOADED OK` with rows, end mode, name and kits;
  a format-7 image with length 257 is rejected; a format-8 round trip with 512
  rows; format 8 rejects length 513; a bad format 8 next to a good format 7
  cannot happen (one slot), but a good format 7 next to a good eight-track save
  loads the format 7.
- Firmware: all CI presets build (`firmware-debug`, `pico2-release`,
  `revb-rp2354b`; `.github/workflows/ci.yml:83-112`) plus `firmware-release`,
  with the RAM table above re-measured.

Simulator (`sim/build.sh native`, `sim/build.sh web` with the `sim/emsdk` link in
the worktree, `sim/check.sh`):

- `midi_clips_check.js:45` asserts the version the core reports, `:70` the
  length at `rows x 65`; section 1 already checks the layout against a save the
  wasm firmware writes, so it proves the format-8 offsets.
- `:107-108` (Contains LSD fits, Sham is longer than the chain) and section 3
  (`:197`, Sham played up to the cut): the cut path keeps a test subject with
  `rows: 256`; a new assertion that all eight songs fit at 512 and
  `played` equals `bars`.
- PR #31's `midi_live_check.js:86` (Sham's set ends on bar 257) and `:137`
  ("256 rows stored") run with `rows: 256`; a new assertion that Sham's set at
  512 is not cut.

## Order with PR #31 (`claude/sham-live`)

PR #31 cuts a short chain further at its last phrase line and rewrites
`store()`/`show()` around a `chain()` helper. This change should land after it
**(call)** and touch only: the `rows` parameter threaded through `plan()`,
`chain()`, `store()` and `show()`; the layout constants; the two test
assertions above with `rows: 256`; and its README lines 262-264. The live-mute
logic itself is untouched.

## Open questions

1. **RP2040 headroom.** Is 4,072 bytes free (about 2 KB beyond the heap
   reserve) acceptable on the RP2040 builds? CI builds only the Debug preset for
   that chip, which keeps more (10,652). Alternatives: 384 rows (+8,320 bytes; the
   longest Twisted song needs 355); rows packed at 5 bits a location (41 bytes,
   512 rows = 20,992, +4,352 over today), at the cost of bit unpacking in the
   song advance and the editor; or 512 on the RP2350 only, giving up save
   portability.
2. **Format number.** The four-outputs design (on master, not built) also
   claims "format 8" as "the three v7 payloads unchanged" plus ports. Whichever
   lands first takes 8 and the other 9 (the loader then reads 9, 8, 7 and the
   eight-track slot), or both ship in one format step. The shared "frozen
   records first" commit should land once, before either.
3. **Song edit lock time.** Accept 1.9-2.4 ms with interrupts off on the
   RP2040 for now (documented), or make shorter lock sections part of this
   change?
4. **Scrolling.** The song cursor is a "choice" (README:210): one row per click,
   so the end of a 512-row chain is 511 clicks away. Keep, accelerate it like a
   number, or add a jump?
5. **Long clips.** If PR #18 lands, songs need far fewer rows. Should 512 stay
   then, or return to 256 and its RAM?
