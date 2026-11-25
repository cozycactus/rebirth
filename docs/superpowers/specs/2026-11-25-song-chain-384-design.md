# Song Chain of 384 Rows — Design

Date: 2026-11-25. Status: draft for review. The project owner decided the goal
and its size: the Song-mode chain grows from 256 to 384 rows
(`MAX_SONG_ENTRIES`), so a MIDI song laid out as the device's clips fits whole
(a first draft proposed 512; the owner chose 384 for the RP2040's RAM). Clips
stay 16 steps (`STEPS_PER_PATTERN`). Decisions taken by the coordinating
session are marked **(coordinator)**: the owner may override them. The
designer's own choices are marked **(call)**. No assembled board exists: the
figures come from firmware builds, host code and the simulator. Statements
about today's code refer to `origin/master` 86975bc. RAM figures are from
builds of that commit with only the song record and the legacy song record
changed (Pico SDK 2.2.0, `nm`: `__HeapLimit` - `end`).

## Intent

"It's a Sham, Man" (Twisted pack) is 361 bars. Laid out as 16-step clips, its
bass alternates two clips every bar, so the chain needs 355 rows; today it stops
after bar 262. Five of the eight Twisted songs are cut. At 384 rows every one
fits (`MidiClips.plan` in Node, origin/master):

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
   others **(call)**: the long-clips design follows the same rule.
2. Saves in format 7 and eight-track saves (formats 4-6) still load.
3. A song edit holds the lock for less time at 384 rows than it does today at 256.
4. The simulator's clips mode writes as many rows as its core holds, and all
   eight Twisted songs play whole.

## Non-goals

- Clips longer than 16 steps (PR #18), more than 32 locations per track. 384
  stays whatever PR #18 decides **(coordinator)**.
- Faster scrolling in the song editor. The cursor stays a "choice" moving one
  row per click (README:210), so row 384 is 383 clicks from the top
  **(coordinator)**.
- Splitting song edits into several lock sections.
- A save that survives power loss during the save (one slot, as now).

## Today

- **Record.** `song_entry_t` (`include/sequencer.h:149-152`) holds 64 location
  bytes plus `repeats`: 65 bytes, byte-aligned. `song_t` (`:159-164`) holds 256
  rows, then `length` (`uint16_t`), `end_mode` and `name[9]`. It is 16,652 bytes
  on the device (`-fshort-enums`) and 16,660 in the host and wasm builds. It
  lives once, in `g_seq.song` (`:238`). Nothing copies it: no undo, clipboard or
  staging buffer, and no `malloc` user. Save programs it from RAM
  (`src/flash_storage.c:294`) and load copies it from XIP (`:342`).
- **Widths.** Song positions are `uint16_t` everywhere: `song_position`
  (`sequencer.h:205`), the edit API (`:329-333`, `src/song.c`), `locate_song()`
  (`src/sequencer.c:976`), `ui.song_cursor` (`src/ui.c:88`, summed in
  `int16_t` at `:1833`) and `display_draw_song_view()` (`src/display.c:910`).
  Rows print as `%3d` (`display.c:938`). The footer "384/384" ends at x 126 of
  128 (`:952-953`). The 14-bit SPP (`src/clock_in.c:53-66`, 1,024 bars) is
  unchanged. No code change is needed for widths or the display.
- **Save format 7** (`flash_storage.c:17-86`) uses one 192 KiB slot at
  `0xB0000`, erased whole on every save. It holds a header page, then patterns,
  song and kits, each page-aligned. The song is `sizeof(song_t)` raw, so its
  size fixes `KIT_OFFSET` and the CRC span. A bigger song therefore needs a new
  format.
- **The legacy song record is sized by the live constant.** `old_song_t`
  (`flash_storage.c:64-69`) is `entries[MAX_SONG_ENTRIES]`, and the legacy
  length bound (`:209`) uses the same constant. Above 256 the device build stops
  at `_Static_assert(sizeof(old_song_t) == 2316)` (`:73`; verified at 384 and
  512). The host build has no such assert, and `write_legacy_image()`
  (`test/test_storage.c:63-88`) uses the same struct. Host tests would
  therefore keep passing while eight-track saves no longer loaded on the device.
- **Song edits are slow under the lock.** `seq_lock()` is a spin lock with
  interrupts off (`src/shared_state.c:14-17`). `song_insert*()`, `song_remove()`
  and `song_move()` shift the rows after the edit point with `memmove` inside it
  (`song.c:85, 110, 142, 146`). Rows 65 bytes apart are never both
  word-aligned, so the firmware's newlib `memmove` copies them byte by byte (Release build,
  disassembled):
  - backward, for an insert or a move to an earlier row, at about 7 cycles a byte;
  - forward, for a remove or a move to a later row, at about 9 cycles a byte.

  `memmove` also runs from flash. `JITTER_MEASUREMENT.md:160, 171-176` and
  `HARDWARE_STATUS.md:276-278` still count 9-byte eight-track rows (130-165 us).

## Decisions

1. **384 rows** (owner). `MAX_SONG_ENTRIES` 384 (`include/sequencer.h:18`).
2. **Frozen records first** **(coordinator)**. The first commit, before the
   constant changes, gives every on-flash record of format 7 and older its own
   typedef:
   - `OLD_SONG_ENTRIES` 256 for `old_song_t` and its length bound
     (`flash_storage.c:65, 209`);
   - `song_v7_t`, made of 256 rows of 65 bytes (16,652 bytes on the device,
     16,660 on the host).

   Each gets size and offset asserts under both ABIs. The test change ships in
   the same commit: `test_storage.c:346, 608` bound by `OLD_SONG_ENTRIES + 1`,
   and a format-7 image written with `song_v7_t`. Both tests prove an
   eight-track save and a format-7 save still load once the limit is raised.
   The four-outputs design's "frozen records first" step is this same step,
   done once here.
3. **Rows of 68 bytes, moved by words in RAM** **(coordinator: the fastest
   safe copy, contained)**. `song_entry_t` gains `uint8_t _reserved[3]` and
   4-byte alignment, so every row starts on a word boundary. Each of the 4
   `memmove` calls in `song.c` becomes `song_move_rows(dst, src, rows)`.
   - **The helper.** It is a word loop in both directions, about 10 lines, and
     runs from RAM (`__not_in_flash_func`), like `seq_lock()`. It carries
     `optimize("no-tree-loop-distribute-patterns")`, so GCC cannot turn it back
     into a `memmove` call.
   - **Speed.** Compiled for the Cortex-M0+ at -O2, the loop is `ldr`, `str`,
     `subs`, `cmn`, `bne`: about 8 cycles a word, or 2 a byte.
   - **Padding is needed.** At 65 bytes the rows are never both word-aligned.
     Newlib's backward `memmove` copies byte by byte even when the rows are
     aligned. So inserts need both the padding and the helper.
   - **Reserved bytes.** Insert and append write them as 0. Validation ignores
     them **(call)**.
   - **Unchanged.** Every reader of a row uses its fields by name.

   | Edit at the front of a full song, RP2040 at 125 MHz | Bytes moved | Lock held |
   |---|---:|---:|
   | Today: 256 rows of 65, byte `memmove` | 16,575 | 0.93-1.19 ms |
   | 384 rows of 65, byte `memmove` (not chosen) | 24,895 | 1.39-1.79 ms |
   | **384 rows of 68, word loop** | 26,044 | **about 0.42 ms** |

   These are instruction-count estimates, not measurements. One pulse lasts
   2.08 ms at 300 BPM.
   - **Core 1 timer.** The ISR takes `seq_lock` every step, so it can wait up
     to the time in the table.
   - **MIDI input.** Core 0's receive interrupt runs with the UART FIFO off
     (`src/midi_out.c:213`). A byte arrives every 320 us, and the holding
     register keeps one. About 2-3 bytes can be lost today, about 1 after this
     change.
   - **RP2350.** It runs the same loop faster (Cortex-M33 at 150 MHz).
   - **Docs.** `JITTER_MEASUREMENT.md` and `HARDWARE_STATUS.md` get the new
     bound.
   - **Cost.** The padding adds 1,152 bytes of RAM (384 x 3).
4. **Format 8** **(coordinator)**. Whichever format change lands first takes
   8. The four-outputs design is a design only, so this change takes format 8,
   and that spec should later say 9 (not edited here). Format 8 is format 7 with
   `song_t` of 384 rows of 68 bytes. It uses the same slot, order and page
   alignment, and the same CRC rule (unpadded payload bytes, header programmed
   last).

   | Payload | Device offset, bytes | Host/wasm offset, bytes |
   |---|---|---|
   | header | 0, 256 | 0, 256 |
   | patterns | 256, 131,456 | 256, 149,888 |
   | song | 131,840, 26,124 | 150,272, 26,132 |
   | kits | 158,208, 960 | 176,640, 960 |
   | used of 196,608 | 159,232 (format 7: 149,760) | 177,664 (format 7: 168,192) |

   - **Loader order.** First, a valid format 8 in the slot.
     - Else a valid format 7 in the same slot. Its `song_v7_t` rows are copied
       one by one into a zeroed `g_seq.song`, the kits come from the format-7
       offset, and the load reports `LOADED OK` **(call)**.
     - Else the eight-track slot, formats 4-6, as today.
     - `song_valid()` takes the bound: 256 for format 7, 384 for format 8.
   - **Save** writes format 8 only.
     - It programs 37 more pages than today, about +15 ms at the simulator's
       0.4 ms a page. The erase is unchanged.
     - The bitwise CRC32 covers 9,472 more bytes.
   - **Downgrade.** Firmware that reads only format 7 refuses a format-8 slot
     by its version byte. It falls back to the eight-track save, or the load
     fails, as at every earlier format step. The first format-8 save replaces
     the format-7 one.
5. **The simulator reads the limit from the core** **(coordinator)**.
   - **The export.** `int sim_song_entries(void)` goes in `sim/core/sim_api.h`
     and `sim_main.c` and returns `MAX_SONG_ENTRIES`. It is a number from the
     compiler, like `sim_midi_song_capacity()` (`sim_main.c:98`), not a feature
     bit grepped from the source (`sim/build.sh:21-27`).
   - **The page.** `Device.songEntries()` returns it, or 256 when the core has
     no such export (`device.js:180` shows the same "absent in an older core"
     fallback).
   - **The layout follows the capacity.** In `midi_clips.js`, 256 selects
     format 7 (rows of 65), and 384 selects format 8 (rows of 68, record
     `rows x 68 + 20` bytes, trailer at `rows x 68`, `KIT_OFFSET` after it). An
     older core still gets a format-7 image. This mapping holds while this
     change is format 8. If it ends up 9, the core reports its save version too.
   - **The `rows` option.** `plan(song, { rows })`, `store(p, { rows })` and
     `show(p, …, { rows })` take it, with default 384 **(call)**. `plan()` runs
     in Node without a device. `app.js` passes `device.songEntries()`.
   - **The 256s that change.** Each 256 in `midi_clips.js` (`:8-9, 25, 251, 269,
     314-323, 361`) and the "256 строк" log line (`app.js:1869`) read `rows`.
     Nothing else in `sim/web` names the chain length, `index.html` included.
   - **Not affected.** The MIDI Song player and its UF2 export (`openMidiSong`,
     `firmwareMidiImage`, the 1 MiB slot at `0xE0000`) play the timed file, not
     the chain. No board UF2 carries a chain: the clips image is for the
     simulator only (`midi_clips.js:21-23`). The page keeps no save in browser
     storage (`app.js:902-965`).

## RAM

Each figure is `__HeapLimit` - `end`, in bytes. It includes the 2,048-byte
`.heap` reserve. Every figure is measured except the Debug build's "Today",
which is computed (18,972 + 8,320). All five presets link at 384.

| Build | Today | 384 rows of 65 | 384 rows of 68 (chosen) |
|---|---:|---:|---:|
| RP2040 Release (`firmware-release`) | 20,712 | 12,392 | **11,240** |
| RP2040 Debug (`firmware-debug`, CI) | 27,292 | 18,972 | 17,820 |
| RP2350 (`pico2-release`, `revb-rp2354b`, `revb-rp2350b`) | 282,312 | 273,992 | 272,840 |

- `g_seq` grows from 149,640 to 159,112 bytes (+9,472).
- The stacks sit in the scratch banks and do not change.
- No board has its own limit, and the flash map is the same on every board.

## Order with PR #31 (`claude/sham-live`)

PR #31 rewrites `store()`/`show()` around a `chain()` helper that holds the one
`entries.slice(0, 256)`. It also adds `midi_live_check.js`, which tests a chain
cut short (Sham's set ends on bar 257, `:86`; "256 rows stored", `:137`). This
branch keeps the overlap to its last commit, "Simulator: the chain length from
the core". The firmware commits touch no file of #31.

- **If #31 merges first**, rebase onto master. Conflicts are possible only in
  `midi_clips.js` and `app.js:1869`. Resolve them by threading `rows` through
  `chain()`, `store()` and `show()` and keeping #31's logic. Then run
  `midi_live_check.js:86, 137` with `rows: 256`, so the cut path keeps its test
  subject, and add one assertion that Sham's set at 384 is not cut.
- **If this merges first**, #31 takes the same steps from its side.

## Tests

Host (`ctest --preset host-tests-debug` in `test/`). A red `playback_tests` on
a loaded machine may be its 30 s `TIMEOUT`, so look for "Timeout" first.

- **Frozen-records commit:**
  - an eight-track save (formats 4, 5 and 6) and a format-7 image load at the
    raised limit;
  - a format-7 length of 257 is rejected;
  - `test_storage.c:346, 608` use `OLD_SONG_ENTRIES + 1`.
- **Format 8:**
  - a round trip with 384 rows;
  - length 385 is rejected;
  - a format-7 slot loads as `LOADED OK` with its rows, end mode, name and kits.
- **`song_move_rows()`:** equal to `memmove` for every overlap and direction
  over a full song. Insert, remove and move keep the playing position, as the
  existing `test_song.c` cases do.
- **Already following the constant:** `test_song.c:84-91` and
  `test_ui.c:2415-2416` (the song screen at row 384).
- **Firmware:** `firmware-release`, `firmware-debug`, `pico2-release`,
  `revb-rp2354b` and `revb-rp2350b` build. Re-measure the RAM table and check
  the disassembly to confirm `song_move_rows()` is a word loop placed in RAM.

Simulator (`sim/build.sh native`, `sim/build.sh web` with the `sim/emsdk` link,
`sim/check.sh`):

- `midi_clips_check.js:45` checks the version the core reports, and `:70`
  checks the length at the trailer offset. Section 1 already compares the
  layout with a save the wasm firmware writes, so it proves the format-8
  offsets.
- `:107-108` and section 3 (`:197`) cover Contains LSD fitting and Sham being
  played up to the cut. They run with `rows: 256`. Add an assertion that all
  eight songs fit at 384, with `played` equal to `bars`.
- Whole-song checks of the clips mode run longer: Sham plays 361 bars, not 262.

## Docs

- `ARCHITECTURE.md:433-450`: format 8 and the format-7 fallback.
- `README.md:19`: the chain length.
- `sim/README.md:199-201`: no Twisted song is cut.
- `include/flash_storage.h:16-24`.
- `JITTER_MEASUREMENT.md:160, 171-176` and `HARDWARE_STATUS.md:276-278`: the
  new song-edit bound.

## Open questions

1. The 68-byte rows cost 1,152 bytes and buy about 3.5x shorter edit locks. The
   alternative is 65-byte rows, with the edit lock at 1.39-1.79 ms documented.
   Keep the padding?
2. The four-outputs spec should be renumbered to format 9 when that work
   starts. Who notes it there?
