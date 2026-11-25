# Song Chain of 384 Rows — Design

Date: 2026-11-25. Status: draft for review, revised after an independent review
(Gemini, DeepSeek, Grok; see "Review changes"), and built on branch
`claude/song-512` (not yet reviewed as code). The project owner decided the
goal and its size: the Song-mode chain grows from 256 to 384 rows
(`MAX_SONG_ENTRIES`), so a MIDI song laid out as the device's clips fits whole
(a first draft proposed 512; the owner chose 384 for the RP2040's RAM). Clips
stay 16 steps. Decisions of the coordinating session are marked
**(coordinator)**; the owner may override them. The designer's own choices are
marked **(call)**. No assembled board exists: the figures come from firmware
builds, disassembly, host code and the simulator. "Today" means `origin/master`
86975bc.

## Intent

"It's a Sham, Man" (Twisted pack) is 361 bars. Laid out as 16-step clips, its
bass alternates two clips every bar, so the chain needs 355 rows; today it stops
after bar 262. Five of the eight Twisted songs are cut. At 384 rows every one
fits (`MidiClips.plan` in Node):

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

1. The chain holds 384 rows on every board, so a save made on one loads on the
   others **(call)**.
2. Saves in format 7 and eight-track saves (formats 4-6) still load, under both
   enum sizes (the device's `-fshort-enums` and the host's 4-byte enums).
3. A song edit holds the lock for less time at 384 rows than today at 256, and
   less than one MIDI byte time (320 us) in the RP2040 Release build (the Debug
   build, at -Og, does not unroll the copy: about 0.73 ms).
4. The simulator's clips mode writes as many rows as its core holds, in that
   core's save format; all eight Twisted songs play whole on the new core.

## Non-goals

- Clips longer than 16 steps (PR #18), more than 32 locations per track. 384
  stays whatever PR #18 decides **(coordinator)**.
- Faster scrolling: the song cursor stays a "choice", one row per click
  (README:210); row 384 is 383 clicks from the top **(coordinator)**.
- Freezing the pattern records (`pattern_v5_t`, `pattern_v6_t`, the format-7/8
  `pattern_t`): the pattern layout does not change here; the four-outputs work
  does that step.
- A save that survives power loss during the save (one slot, as now).

## Today

- **Record.** `song_entry_t` (`include/sequencer.h:149-152`) is 64 location
  bytes plus `repeats`: 65 bytes, alignment 1. `song_t` (`:159-164`) is 256 rows,
  `length` (`uint16_t`), `end_mode`, `name[9]`: 16,652 bytes on the device,
  16,660 on the host and in wasm. It lives once, in `g_seq.song`; nothing copies
  it (no undo, clipboard or staging buffer; no `malloc` user).
- **Widths.** Song positions are `uint16_t` everywhere (`sequencer.h:205`,
  `:329-333`, `src/sequencer.c:976`, `src/ui.c:88`, summed in `int16_t` at
  `:1833`, `src/display.c:910`). Rows print as `%3d` (`display.c:938`); the
  footer "384/384" ends at x 126 of 128 (`:952-953`). The 14-bit SPP
  (`src/clock_in.c:53-66`) reaches 1,024 bars. No change needed.
- **Save format 7** (`src/flash_storage.c:17-86`): one 192 KiB slot at
  `0xB0000`, erased whole on every save; header page, then patterns, song,
  kits, page-aligned. `SONG_BYTES` is `sizeof(song_t)` (`:43`), so the live
  song type fixes `KIT_OFFSET` (`:46`), the CRC span (`:137-142`), the
  validator (`:149-158`) and the load copy (`:341-343`).
- **The legacy song record is sized by the live constant.** `old_song_t`
  (`:64-69`) is `entries[MAX_SONG_ENTRIES]` and its length bound (`:209`) uses
  the same constant. Above 256 the device build stops at its size assert
  (`:73`, verified); the assert is `__arm__`-only, and the host writer
  `write_legacy_image()` (`test/test_storage.c:63-88`) uses the same struct, so
  host tests would pass while eight-track saves broke on the device.
- **Song edits under the lock.** `seq_lock()` is a spin lock with interrupts off
  on the calling core (`src/shared_state.c:14-17`); Core 1's timer takes it on
  every pulse (`src/sequencer.c:794`, 96 PPQN). `song_insert*()`,
  `song_remove()`, `song_move()` shift rows with `memmove` inside it
  (`src/song.c:85, 110, 142, 146`). The firmware links newlib through
  `nosys.specs` (Arm GNU Toolchain 15.2.Rel1); its ARMv6-M `memmove`, as linked
  in the RP2040 Release build, copies backward byte by byte always and forward
  by words only when both ends are word-aligned, which rows 65 bytes apart
  never are (disassembly). About 7 cycles a byte backward (insert, move to an
  earlier row), 9 forward (remove, move to a later row), from flash.
  `JITTER_MEASUREMENT.md:160, 171-176` and `HARDWARE_STATUS.md:276-278` still
  cost 9-byte eight-track rows.
- **Simulator.** `sim/web/midi_clips.js` hard-codes the format-7 wasm image:
  stride 65, `repeats` at row byte 64, `SONG_BYTES` 16,660 (`:25`), trailer at
  16,640/16,644/16,648, version 7 (`:314-323`); `PAGE = 256` (`:25`) is the
  flash page. `MidiClips.layout` (`:374`) is a constant object.

## Decisions

### 1. 384 rows (owner)

`MAX_SONG_ENTRIES` 384 (`include/sequencer.h:18`).

### 2. Frozen song records first (coordinator)

The first commit, before the constant changes:

- `OLD_SONG_ENTRIES` 256; `old_song_t` uses it, and so does its length bound
  (`flash_storage.c:65, 209`).
- `song_entry_v7_t` (64 locations, `repeats`: 65 bytes) and `song_v7_t` (256 of
  them, `length`, `end_mode`, `name[9]`). The live `song_end_mode_t` stays in
  both records, so each ABI keeps its own layout (a `uint8_t` field would move
  the host and wasm offsets the simulator writes).
- Size and offset asserts under both enum sizes, keyed on
  `sizeof(song_end_mode_t)`, not `__arm__`: `old_song_t` 2,316 / 2,324;
  `song_v7_t` 16,652 / 16,660, `length` at 16,640, `end_mode` at 16,642 /
  16,644, `name` at 16,643 / 16,648.
- Frozen format-7 constants and helpers: `V7_SONG_BYTES` = `sizeof(song_v7_t)`,
  `V7_KIT_OFFSET` = `SONG_OFFSET` + its page-rounded size (148,736 / 167,168),
  `payload_checksum_v7()`, `song_v7_valid()` (length <= 256), `v7_image_valid()`.
- Tests in the same commit: a golden format-7 image built from raw bytes at
  hand-computed offsets (not from `song_v7_t`), with 256 rows, loads exactly;
  `write_legacy_image()` checks `length <= OLD_SONG_ENTRIES`;
  `test_storage.c:346, 608` use `OLD_SONG_ENTRIES + 1`. They pass before and
  after the limit rises.

### 3. Rows of 68 bytes, moved by words from RAM (coordinator)

- `song_entry_t` becomes a union of the named fields
  (`pattern_index[64]`, `repeats`, `_pad[3]`) and `uint32_t words[17]`: 68
  bytes, alignment 4 by the language, aliasing-safe word access, every field
  read by name unchanged. Asserts: size 68, alignment 4,
  `offsetof(song_entry_t, repeats) == 64`.
- `_pad` is padding: append and insert write the whole row (pad 0), validation
  ignores it, the CRC covers it **(call)**.
- `song_move_rows(dst, src, rows)` replaces the 4 `memmove` calls: rows in the
  direction the overlap needs, each row copied as 17 word loads and stores
  (unrolled). A row's source and destination never overlap (a shift is at
  least one row), so the direction only matters between rows.
  `__no_inline_not_in_flash_func` (RAM, never inlined into a flash caller);
  under GCC, `optimize("no-tree-loop-distribute-patterns", "no-tree-vectorize")`
  so it is not turned into a `memmove` call or a vectorised copy with an
  alignment fallback; Clang builds (Emscripten, macOS host) get the plain loop.
  Exposed in `sequencer.h` with a "caller holds `seq_lock`" contract so host
  tests can call it.
- In the linked RP2040 Release build (disassembly): `song_move_rows()` sits in
  RAM (192 bytes), reached from `song_insert_patterns()` and `song_remove()`
  through a veneer (`song_move()` has no caller in the firmware and is dropped
  by the linker). A row is 17 `ldr`/`str` pairs plus 5-6 loop instructions:
  about 75 cycles a row, 4.4 a word, 1.1 a byte, by instruction count.

  | Edit at the front of a full song, RP2040 at 125 MHz | Bytes moved | Lock held |
  |---|---:|---:|
  | Today: 256 rows of 65, `memmove` | 16,575 | 0.93-1.19 ms |
  | 384 rows of 65, `memmove` (not chosen) | 24,895 | 1.39-1.79 ms |
  | **384 rows of 68, `song_move_rows()`** | 26,044 | **about 0.23 ms** |

  One pulse is 2.08 ms at 300 BPM. A hold delays at most one pulse by that
  much. Core 0's MIDI receive runs with the UART FIFO off (`midi_out.c:213`):
  a byte every 320 us, one kept in the holding register; at 0.23 ms none is
  lost, against 2-3 today. Cost: 1,152 bytes of rows plus the helper's code in
  RAM (about 200 bytes).

### 4. Format 8 (coordinator)

This change takes format 8; the four-outputs design is renumbered to format 9
in this change (its storage section, offsets recomputed). Format 8 is format 7
with `song_t` of 384 rows of 68 bytes: same slot, order, page alignment and CRC
rule (unpadded payload bytes, header last).

| Payload | Device offset, bytes | Host/wasm offset, bytes |
|---|---|---|
| header | 0, 256 | 0, 256 |
| patterns | 256, 131,456 | 256, 149,888 |
| song | 131,840, 26,124 | 150,272, 26,132 |
| kits | 158,208, 960 | 176,640, 960 |
| used of 196,608 | 159,232 (format 7: 149,760) | 177,664 (format 7: 168,192) |

- **Three image kinds.** `v8_image_valid()` (live macros, version 8),
  `v7_image_valid()` (frozen, decision 2), `old_image_valid()` (as today).
  `song_valid()` checks a `song_t` (length <= 384), `song_v7_valid()` a
  `song_v7_t` (<= 256); the pattern and kit checks are shared.
- **Load** picks the first valid of format 8, format 7, eight-track and
  branches on that kind: format 8 copies the song whole; format 7 zeroes
  `g_seq.song` and copies `length`, `end_mode`, `name` and each row's
  locations and repeats by field, kits from `V7_KIT_OFFSET`, and returns
  `LOADED OK` **(call)**; eight-track as today (`OLD SAVE LOADED`).
- **`flash_storage_is_valid()`** is format 8 or 7 or eight-track. **Save**
  writes format 8 and verifies format 8 alone.
- **Cost.** 37 more pages than format 7 (about +15 ms at 0.4 ms a page); the
  erase is unchanged; the bitwise CRC covers 9,472 more bytes.
- **Downgrade.** Firmware that reads only format 7 refuses a format-8 slot by
  its version byte and falls back to the eight-track save or fails, as at
  every earlier format step. The first format-8 save replaces the format-7 one.

### 5. The simulator reads the limit from the core (coordinator)

- `int sim_song_entries(void)` (`sim/core/sim_api.h`, `sim_main.c`) returns
  `MAX_SONG_ENTRIES`, like `sim_midi_song_capacity()`. `Device.songEntries()`
  returns it, or 256 when the core lacks the export.
- `midi_clips.js`: `layoutFor(rows)` returns the wasm layout for 256 (format 7:
  stride 65, song 16,660) or 384 (format 8: stride 68, song 26,132) and throws
  for any other value; `layout` stays as `layoutFor(384)`. `PAGE` stays 256.
  `repeats` is at row byte 64 in both; the pad and unused rows are 0.
- One row cap per plan: `plan(song, { rows })` (default 384 **(call)**) keeps
  `rows` on the plan; `store(p)` and `show(p, start)` read `p.rows`, so an
  image, `played`, `lost.entries` and the captions always agree.
  `app.js` passes `device.songEntries()`, so a 256-row core gets a format-7
  image. The "256 строк" texts (`midi_clips.js:8-9, 361`, `app.js:1869`) read
  the plan's rows.
- Not affected: the MIDI Song player and its UF2 export (the timed file in its
  own 1 MiB slot), and the "MIDI Song · выходы ReBirth" mode.

## RAM

`__HeapLimit` - `end` in bytes, including the 2,048-byte `.heap` reserve. The
first two 384 columns are trial builds of the record change alone; "as built"
is the implementation, `song_move_rows()` in RAM included (192 bytes on the
RP2040 Release build, 88 Debug, 278 on the RP2350). Today's Debug figure is
computed.

| Build | Today | 384 rows of 65 | 384 rows of 68 | As built |
|---|---:|---:|---:|---:|
| RP2040 Release (`firmware-release`) | 20,712 | 12,392 | 11,240 | **11,048** |
| RP2040 Debug (`firmware-debug`, CI) | 27,292 | 18,972 | 17,820 | **17,740** |
| RP2350 (`pico2-release`, `revb-rp2354b`, `revb-rp2350b`) | 282,312 | 273,992 | 272,840 | **272,560** |

`g_seq` grows from 149,640 to 159,112 bytes. No board has its own limit; the
flash map is the same on every board.

## Order with PR #31 (`claude/sham-live`)

PR #31 rewrites `store()`/`show()` around `chain()`, which holds the one
`entries.slice(0, 256)`, and adds `midi_live_check.js`, which tests a chain cut
short (Sham's set ends on bar 257, `:86`; "256 rows stored", `:137`). This
branch stays on `origin/master`; its simulator work is one commit, last. If #31
merges first, that commit is rebased: `chain()` slices `p.rows`, its tests build
plans with `rows: 256`, and `midi_live_check.js` reads the trailer from
`layoutFor(256)`. A trial merge (`git merge-tree`) conflicts in three files:
`midi_clips.js` (the plan's return fields, `store()` beside the new `chain()`,
the song record writes, `show()`'s caption, the exported `api`), `app.js` (the
plan and `writeStore` in the pads launch, the "вошли … строк" log line) and
`sim/README.md` (the chain paragraph).

## Tests

Host (`ctest --preset host-tests-debug`; the sanitizer build with and without
`-fshort-enums`, as CI runs it; a red `playback_tests` on a loaded machine may
be its 30 s timeout, so look for "Timeout" first and rerun it alone):

- `test_storage`: the golden format-7 image (decision 2), after the change
  loaded through the fallback with rows 256-383 zero even after RAM was
  scribbled; a format-7 image with length 257, a bad location in row 255, a
  flipped CRC byte, a bad end mode is rejected and falls back or fails with RAM
  unchanged; a format-8 round trip with 384 rows and the pad bytes 0; length
  385, a bad location in row 383, a flipped byte in row 383 rejected; an
  unknown version 9 rejected; a format-8 save cut short next to a valid format 7
  is not a successful save; eight-track images v4-v6 as today; a 256-row
  eight-track song.
- `test_song`: `song_move_rows()` against `memmove` (both directions, 0, 1 and
  383 rows); a full song of 384 rows: insert refused, remove row 0, move 0->383
  and 383->0, insert at 0 and at 383 after a remove, every row checked; pad
  bytes 0 after append and insert.
- `test_playback` (positions while Song mode plays; today's cases at
  `test_playback.c:706-745`): advance from row 255 to 256 and from 383 to the
  end (loop and stop); a live insert, remove and move at `song_position` >= 256.
- `test_clock`: an SPP that lands past row 255.
- `test_ui`: cursor at 383 turned past the end stays 383; insert and remove at
  a cursor past 255.
- Firmware: all five presets build; RAM re-measured; the RP2040 disassembly
  shows `song_move_rows()` in RAM, called (not inlined) from the three edit
  functions, and its loop cost.

Simulator (`sim/build.sh native` and `web` with the `sim/emsdk` link,
`sim/check.sh`):

- `midi_clips_check.js` section 1: the save the firmware writes is version 8,
  checked against `layoutFor(device.songEntries())`.
- Section 2: every song loads at 384, `lost.entries` 0 and `played` equal to
  `bars`; then SAVE after a 384-row load writes the same song record as
  `store()` did (length at `384 x 68`, repeats at byte 64).
- Section 3 plays Contains LSD and Sham at 384 (Sham whole); the cut path is
  kept with plans built for 256 rows, whose format-7 image the new core loads
  through the fallback.
- `Device.songEntries()` is 384, and 256 with the export removed.

## Docs

`ARCHITECTURE.md:433-450`; `README.md:19` (add "up to 384 rows"), `:343`,
`:498`; `sim/README.md:168, 199-201, 211, 223-225`; `include/flash_storage.h:16-24`;
`JITTER_MEASUREMENT.md:160, 168-176` (rows, every pulse, the new bound);
`HARDWARE_STATUS.md:276-278`; the `midi_clips.js` header comment.

## Review changes

Three reviewers read the first 384 draft (Codex was unavailable). Each finding
was checked against the code; G = Gemini, D = DeepSeek, K = Grok.

| Finding | Verdict | Change |
|---|---|---|
| Format-7 fallback used the live `SONG_BYTES`, `KIT_OFFSET`, checksum (D1, K2) | confirmed | frozen v7 constants, checksum and validator (decision 2) |
| `song_valid()` cannot take only a bound (G1, D2) | confirmed | `song_valid()` and `song_v7_valid()`; field copy |
| Two-state loader and `flash_storage_is_valid()` (D3, G1) | confirmed | three image kinds; save verifies format 8 alone |
| `_reserved[3]` gives no 4-byte alignment; aliasing; inlining (K1, D4, D8, G3) | confirmed | union with `words[17]`, asserts, `__no_inline_not_in_flash_func` |
| GCC-only `optimize` attribute (G3) | confirmed | guarded by `__GNUC__ && !__clang__` |
| `song.c` lacks the `__not_in_flash_func` header (G3) | confirmed | include `pico/stdlib.h` (the test fakes define it) |
| Asserts keyed on `__arm__` miss `-fshort-enums` host builds (G2, K4, D7) | confirmed | keyed on `sizeof(song_end_mode_t)` |
| "Every record frozen" overstated (G2, D7) | confirmed | scope narrowed to song records; patterns a non-goal |
| Frozen records with `uint8_t` enums (four-outputs spec) | confirmed wrong for the host (D7) | live enum kept; four-outputs text corrected |
| `PAGE = 256` is the flash page; `repeats` at byte 64 (K3, D5) | confirmed | decision 5 |
| `rows` must travel with the plan; old core gets format 7 (K3, D6, G4) | confirmed | `rows` kept on the plan |
| `layout` constant, other `rows` values (G4, D17) | confirmed | `layoutFor(rows)`, 256 or 384 only |
| Renumber four-outputs to 9 now (K5, D12, G) | confirmed | edited in this change |
| "Every step" (G5, D18, K6) | confirmed | every pulse (`sequencer.c:794`) |
| 3.5x claim (D13) | confirmed | table now 0.93-1.19 ms -> about 0.23 ms |
| Unroll to clear 320 us (G5) | confirmed | rows copied as 17 unrolled words |
| "newlib-nano" (D14) | refuted in part | the build links full newlib (`nosys.specs`); its ARMv6-M `memmove` as linked is byte-wise backward, verified by disassembly |
| RAM helper missing from the table (D21, G5) | confirmed | "as built" column |
| `write_legacy_image()` overflow (G6, D15) | confirmed | bound checked |
| Pad bytes not zeroed (K7, D16, G7) | confirmed | whole row written; round trip checks 0 |
| Test plan gaps (G8, D9, D10, D11, D20) | confirmed | Tests section |
| `test_song.c` cited for live position edits (G8) | confirmed | `test_playback.c:706-745` |
| Docs list (G9, D19) | confirmed | Docs section |
| Continue reads `song.length` under the playback lock only (K6) | noted | unchanged: a `uint16_t`, not moved bytes |

## Implementation notes

- The long-clips design (PR #18, branch `claude/long-clips-design`) also calls
  its save "format v8"; whichever of the two lands second renumbers.

- The artist check (`sim/test/artist_check.js`) counted a set's bars from the
  first note's end less its three bytes. When PLAY lands just before a pulse,
  Start and the first Clock are still on the wire and delay that note by up to
  640 us, past the check's 500 us margin, and a CV part's bars read one step
  rotated. The 384-row save stalls the device about 15 ms longer and moved one
  show's PLAY there (afterglow, "Два пика" and "Гипноз"). The check now also
  takes the tick grid from the first Clocks; it passes on the master core too.
- `sim_dbg_song()` (rows and the playing row) lets the checks see the chain:
  after LOAD Sham's chain is 355 rows, and the check follows it to row 354.

## Open questions

None left for the owner in this design; the coordinator's calls above stand
unless the owner overrides them.
