# Key matrix scan on two MCP23017, steps 8 and 16 on a third: design

Date: 2026-11-16. Status: implemented (branch session-panel-mcp-scan); reviewed by Codex, DeepSeek, Grok;
Gemini could not run.
Changes the firmware of the Session panel (`2026-11-16-session-panel-design.md`) to drive
the board of PR #10 (branch `session-panel-pcb`, `hardware/schematic/README.md`, decisions
10 and 11, "Interfaces the firmware relies on").

## Why

- **Latency.** The TCA8418 scans its keypad every 25 ms and reports a press 25-40 ms late
  (TI SCPS215G, 8.6.2.15), debounce on or off. The design asks for press-to-event within
  15 ms. The board drops the TCA8418 (0x34) for the design's own fallback: two MCP23017,
  U801 at 0x22 and U802 at 0x23, scanned from Core 0.
- **GPA7/GPB7 are output-only** on the current MCP23017 datasheet (DS20001952 rev. D and
  later); a level change on them during a read can corrupt SDA. Steps 8 and 16 were on GPA7
  and GPB7 of U701 (0x20). The board moves them to U703 at 0x24, GPA0 and GPA1.

## Goals and limits

- Same `panel_event_t` interface and the same meaning of the matrix: ROW r = slot r+1,
  COL 0-7 = track 1-8, COL 8 = SCENE of that row, COL 9 = MUTE (track = row + 1).
- Press-to-event within 15 ms for pads, SCENE and MUTE, with the Core 0 loop as it is and a
  working bus (a failing transfer costs up to its 2 ms timeout; a flash save/load or a
  panic ends in a resync). *Changed after the final review:* a playing requirement (one
  key, chords, fast playing); bursts of presses far beyond playing can pass it (see
  "Timing and bus budget").
- Keep what the current driver guarantees: debounce, no event from a stall or a panic
  (resync), chip health readback, retries paced at 100 ms, every I2C transfer bounded
  (2000 us) with its exact byte count checked, PLAY on GPIO 28 untouched.
- Core 1 is not touched: no new lock, no new interrupt, no DMA. Core 1 never touches I2C.
- `hardware/` is not changed.

## Hardware as the firmware sees it

All five expanders are MCP23017 on I2C0 (GPIO 8/9, 400 kHz, external 2.2 k pull-ups),
registers in BANK = 0 (power-on default; IOCON is not written).

| Chip | Addr | Pins | IODIR A/B | IPOL A/B | GPPU A/B | OLAT A/B |
| --- | --- | --- | --- | --- | --- | --- |
| U701 step buttons | 0x20 | GPA0-6 = steps 1-7, GPB0-6 = steps 9-15; GPA7/GPB7 open | 7F / 7F | 7F / 7F | 7F / 7F | 00 / 00 |
| U703 steps 8, 16 | 0x24 | GPA0 = step 8, GPA1 = step 16; all other pins open | 03 / 00 | 03 / 00 | 03 / 00 | 00 / 00 |
| U702 step LEDs | 0x21 | all 16 outputs | unchanged | | | |
| U801 scanner | 0x22 | GPA0-6 = ROW0-6, GPB0 = ROW7; GPB1-7 = COL0-6, GPA7 = COL7 | 7F / 01 | 00 / 00 | 7F / 01 | FF / FF at rest |
| U802 scanner | 0x23 | GPA0 = COL8, GPA1 = COL9; all other pins open | 00 / 00 | not written | not written | FF / FF at rest |

- Every GPA7/GPB7 is an output (IODIR bit 7 = 0 on both ports of 0x20, 0x22, 0x23 and 0x24).
  Unused pins are outputs too, as the schematic notes ask; on U701/U703 they drive low, so
  an existing board with buttons to GND still on U701 GPA7/GPB7 sees no contention.
- Each key is row -> switch -> BAT54J anode -> cathode -> column. With the selected column
  low and the others high, a closed key pulls its row to about 0.25 V (VIL = 0.66 V); a key
  in a high column is reverse-biased. Several held keys never ghost.
- A chip that resets (brown-out, ESD) comes back with IODIR = 0xFF and every other register
  0: on U801 the rows lose their pull-ups and the columns float. Its I2C interface restarts:
  the rest of a read in progress comes back as 0xFF with a full byte count.

### Reset detection rule (all four input chips)

Each chip has one **canary** register: a value that differs from the power-on value,
written **first** in the chip's configuration and never written again by normal operation.
A reset at any moment after that first write clears it, whatever was written after; a reset
before it is repaired by the rest of the sequence. The canary is read back **after** the data
it vouches for, so a reset during the data read is caught too. Canaries: IODIR on U801 and
U802, IPOL on U701 and U703.

## Scan driver (`key_scan.c`, replaces `tca8418.c`)

```c
#define KEY_SCAN_ROW_ADDR 0x22   // U801: ROW0-7, COL0-7
#define KEY_SCAN_COL_ADDR 0x23   // U802: COL8, COL9
#define KEY_SCAN_ROWS 8
#define KEY_SCAN_COLS 10
bool key_scan_init(void);                          // configure both chips, selection unknown
bool key_scan_check(bool *intact);                 // read the configuration back
bool key_scan_column(uint8_t col, uint8_t *down);  // select col, read the rows
bool key_scan_verify(uint8_t col, bool *intact);   // the canaries after a sample of col
```

- `key_scan_init` writes U801 IODIR (canary), IPOL 00/00, GPPU, OLAT FF/FF, then U802
  IODIR (canary), OLAT FF/FF: six 3-byte writes. After a reset the columns become outputs
  at OLAT 0 for the few microseconds until OLAT is written; no row is read before the next
  check, so that is harmless. It sets the selection to unknown.
- `key_scan_check` reads U801 0x00-0x03 (IODIR and IPOL) and U802 0x00-0x01 (IODIR). False
  on an I2C error; otherwise `*intact` tells whether all six bytes hold the values above.
  IPOL is in the check because a non-zero IPOL inverts every row (all keys pressed).
- `key_scan_verify(col)` (*added after the final review*, for the settle proof) reads only
  the canaries a reset clears, after the sample of `col`: U801 IODIR (pointer write and 2
  bytes) for COL0-7, and U802 IODIR as well for COL8/9. A U802 reset only floats COL8/COL9,
  which cannot pull a row low while a U801 column is sampled. IPOL corruption, which no
  reset causes, stays with `key_scan_check`. False on an I2C error (or, with no transfer, a
  col past COL9); it never touches the column selection.
- `key_scan_column(col)`: deselect first, then select, so at most one column is low when
  the rows are read:
  - col 0-7: if U802 may have a column low (col 8/9 selected, or the selection unknown),
    write U802 OLATA = 0xFF; then write U801 OLATA/OLATB in one 3-byte transfer (all bits
    1, the column's bit 0).
  - col 8/9: if U801 may have a column low (col 0-7 selected, or unknown), write U801 OLAT
    = FF/FF; then U802 OLATA = 0xFE (COL8) or 0xFD (COL9).
  - Then read U801 GPIOA/GPIOB in one transfer (pointer write with `nostop`, repeated
    start, 2-byte read). `*down` bit r = ROW r low (the firmware inverts; IPOL stays 0).
  - Any failed transfer sets the selection to unknown and returns false. The selection is
    the driver's own state: nothing else (resync included) resets it, so the next column
    step always releases a column that may still be low.
  - The column write and the read are separate transfers on purpose: the pointer write and
    the address bytes of the read give a released row about 67 us to rise through its
    pull-up before the port is sampled; the estimate needs about 10 us (bench item).
  - Moving from a column in OLATB (0-6) to COL7 (OLATA) writes OLATA first, so for one byte
    time both columns are low. No row is read then; it is harmless.
- Not done: reading the U801 column pins back from the same GPIO read. The datasheet does
  not say what GPIO bit 7 returns for the output-only GPA7/GPB7, and the per-scan check
  already bounds a reset to one sample per key, which the settle proof keeps from settling.
  A column pin shorted to ground is a board fault the firmware does not detect (bring-up
  check: hold each SCENE and MUTE key, no other key may report).

## Panel input (`panel_input.c`)

The API in `panel_input.h` stays (`init`, `poll(now_ms)`, `next`, `resync`, `panel_event_t`,
`PLAY_DEBOUNCE_MS`, `PLAY_BUTTON_PIN`, `PANEL_RETRY_MS`). `PANEL_DEBOUNCE_MS` becomes 5.
`PANEL_CHECK_MS` goes: the configuration is now checked on every scan.

### One bus step per pass

A scan is 11 steps: step 0 is `key_scan_check`, steps 1-10 are columns 0-9. Each
`panel_input_poll` does PLAY first (as now), then at most one step, then returns. The scan
is free-running: the loop is the clock. A pass never blocks for a whole scan (2-3 ms), so
`encoder_poll`, which decodes quadrature by polling in the same loop, keeps its rate.

*Changed after the final review:* a column step whose sample would settle a key reads the
reset canaries right after the row read, in the same pass (`key_scan_verify`; the settle
proof, under "Chip health"). Such a pass makes at most 6 transfers for COL0-7 (4 for the
column, 2 for U801 IODIR) and 8 for COL8/9 (4, and 4 for both chips' IODIR). Every other
scan step makes at most 4; a configuration attempt makes 6 writes. The next scan still starts with the full
`key_scan_check`.

### Debounce at sample time

Each key keeps `raw`, `accepted`, `suppressed`, `since_ms` as now. A key's state is touched
only in the pass that sampled its column: `see()` with the fresh sample, then `settle()`.
A change is reported at the first sample of its column that is at least `PANEL_DEBOUNCE_MS`
(5 ms) after the sample that first saw it, with every sample in between agreeing; so every
event rests on at least two samples of that key. A key whose column is not sampled (bus
down) never settles on elapsed time alone. PLAY keeps its own debounce (20 ms) from
`gpio_get` on every pass.

### What is reported, and what can be lost

A key change is reported only if it lasts until the settling sample: a tap shorter than the
debounce, or one that starts and ends while its column is not sampled (bus down, scanner
being reconfigured), is lost, as is a press during or just after a resync (below). This
holds across a chip reset too: the settle proof (under "Chip health") keeps a reset chip
from supplying the settling sample. A level
scan has no event FIFO; an event latch would need the chips' INT pins, which the board does
not wire. One step reads one column, so a pass makes at most 8 key events and one PLAY
event, and `ui_update` drains the ring (32) on every pass; the ring cannot fill in normal
operation. If it ever does, the change stays pending until the column's next sample, and a
tap completed meanwhile is lost (as today).

### Chip health

- U801 and U802 are one unit: a write that failed on U802 may have left COL8 or COL9 low,
  and a held SCENE or MUTE key would then pull its row low in every U801 column. So any
  failed transfer, a failed check, or a check that reads other values marks the scanner
  unconfigured: nothing is sampled until `key_scan_init` has written both chips again and
  the next check passes (an attempt after a failed one waits `PANEL_RETRY_MS`; a failed
  first check after a configuration counts as a failed attempt), and the scan restarts at
  step 0.
- On that loss every key's pending change is cancelled: `raw = accepted || suppressed`,
  `since = now`. A key goes back to its accepted state, a suppressed key back to "held", so
  ending a suppression again needs two up samples 5 ms apart. Accepted keys stay as they
  are: no release is reported for a key held through a bus error, and the key state needs
  no suppression after a re-init, because a scanner reads levels and a held key reads as
  held, not as a new press.
- Why no phantom press can come from a chip that reset: after a reset at any time after its
  canary was written, the canary reads wrong at the next check. The check runs at the start
  of every scan, so between any two samples of the same column: a bogus first sight is
  cancelled by the loss before the key's next sample, and one sample never settles. A
  failed transfer is the same: its step is not decoded, and the loss cancels the rest. A
  partial write is a failed transfer. A read cut by a reset returns 0xFF (rows high, no key
  down), which can only look like a release.
- **The settle proof** (*changed after the final review*). A chip that resets after the
  scan's check could still supply the sample that settles a change: a tap seen closed once
  would become a press when the next sample read the floating rows low; a held key that
  chattered open once would be released by rows floating high, or by a read cut to 0xFF
  (ROW7 is GPIOB), then pressed again when it read held after the re-init; and a suppressed
  key could be let go the same way and then press while still held. So the column step
  finds out first whether its sample would settle any of the column's 8 keys (report a
  change, or end a suppression), by running `see` and `settle` on copies. If it would,
  `key_scan_verify` reads the canaries right after the row read, in the same pass: U801's
  IODIR, and U802's too for COL8/9 (a U802 reset only floats COL8/COL9, which cannot pull a
  row low while a U801 column is sampled). Only if they hold is the sample applied; if the
  read fails or a canary has changed, the scanner is lost and nothing of the sample is
  applied. IPOL is not read there: no reset changes it to a wrong value (a reset clears the
  canary too), and the full check that starts every scan covers a stray write. A sample
  that settles nothing is applied as before, and the next scan's check still bounds a bogus
  first sight. So every event, and every end of a suppression, rests on at least two
  samples, both proved good by canaries read after them. The cost is 2 transfers (0.11 ms)
  for COL0-7, 4 (0.23 ms) for COL8/9, in the passes that settle a key. (A first version ran
  the whole `key_scan_check`, 0.27 ms, there; it stretched the scans more under load.)
- Residual risk, not handled: after a reset GPA7/GPB7 are inputs until the next
  configuration, and a level change on them during a read may corrupt SDA (datasheet rev.
  D). The window is at most one scan (step row: one 20 ms sample). A corrupted read either
  fails (handled) or would have to return the exact canary values to pass. A slave that
  holds SDA low makes every transfer time out; recovering the bus (clocking SCL) is not
  implemented, as today.

### Resync and boot

`panel_input_resync()` (after a flash save/load and around a panic) does no I2C and does not
touch the scanner's column selection:
- drops every queued event, PLAY included, and handles PLAY as now (`forget_play`);
- takes every key as held: `raw = true`, `accepted = false`, `suppressed = true`. A key ends
  its suppression silently at the first sample that has read it up for 5 ms (two up
  samples; the second one proved by the settle proof); until then nothing is reported. That
  takes up to about S + 5 ms + S after the resync (S = scan period, below).
  - A key held through the stall reports nothing, its release reports nothing, and its next
    press is normal.
  - A key released during the stall presses normally again once its suppression has ended.
  - A key pressed during the stall, or before its suppression ended, and still held reports
    nothing until released.
- restarts the scan at step 0.

`panel_input_init()` sets the keys up the same way, so a key held at power-up does
nothing, like PLAY today. It tries `key_scan_init` once; the first poll retries a failure.

### What goes away

`tca8418.c/.h`, the event FIFO, its overflow flag, the drain, the CFG readback and the
release/suppress split after a drain.

## Step row (`mcp23017.c`)

The 16-bit step word and `step_buttons_t` stay; bit n = step n+1.

- Two chips, each with its own `configured`, retry clock, `rebaseline` and `trusted` flags:
  U701 for bits 0-6 and 8-14, U703 for bits 7 and 15. A missing U703 costs only steps 8 and
  16 (its address NACK is quick; a retry every 100 ms), and an old board without it keeps 14
  steps.
- Configuration per chip, each a 3-byte write: IPOL (canary), OLAT 00/00, IODIR, GPPU.
- **Reads only when a sample is due.** Today `mcp23017_poll` reads the ports on every pass but
  takes a sample only every 20 ms (`MCP_DEBOUNCE_MS`). It now reads only then, or at once
  when a rebaseline is pending. Between samples the edges and `held` stay as the last
  sample left them.
- **Long press only on a fresh, trusted sample** (*changed after the final review*). A long
  press fires only in a pass that just took a good, IPOL-verified sample of the key's chip:
  the first such sample at or after 500 ms, so up to one sample period (20 ms) late, with no
  extra I2C. The timer used to run on every pass from the last sample; the 20 ms sample
  grid drifts against the press time with the pass length, so a step released 480-500 ms
  after its press could long-press before the next sample saw the release (on a healthy
  bus; master did the same).
- **Every sample is proved after it is read.** Per chip: read GPIOA/GPIOB (2 bytes), then
  read IPOL back (2 bytes); the GPIO bytes are used only if IPOL holds the configured value.
  A mismatch means the chip reset (or IPOL was overwritten): the chip is configured again
  and its next good read becomes its baseline, with no press, release or long press (as
  now). A failure before a chip's first good sample after its configuration arms the 100 ms
  retry for that chip. This replaces the 1-second IPOL readback and the "three keys at once" guard, which
  could never fire for U703 (two keys) and so would let a reset U703 press steps 8 and 16.
- A failed read, a failed configuration or a mismatch makes the chip untrusted: its bits
  keep their last accepted state, they make no long press until a good sample of that chip
  (today the whole poll returns early), and its configuration is written again before its
  next read. The other chip carries on. A step released while its chip fails comes back as
  a release with the chip's next good sample.
- Bits 7 and 15 of U701 are never used. `mcp23017_resync()` rebaselines both chips (as now).
  `mcp23017_read_raw()` reads both chips and falls back to the last accepted word.

## Timing and bus budget (estimates, 400 kHz, 22.5 us per byte with its ACK)

| Transfer | Wire bytes | Time |
| --- | --- | --- |
| Column write, U801 (addr, reg, A, B) | 4 | 90 us |
| Column write, U802 (addr, reg, A) | 3 | 68 us |
| Row read or 2-byte register read (addr, reg / addr, 2 data) | 5 | 113 us |
| U801 check read, 4 bytes | 7 | 158 us |

- **Scan:** check (158 + 113) + columns 0-7 (8 x 203) + column 8 (90 + 68 + 113) + column 9
  (68 + 113) + U802 release before column 0 (68) = about 2.41 ms of wire time in 36 SDK
  calls, 107 wire bytes. With call overhead and the rest of each pass (encoders, UI, cached
  LED mask, about 30 us) a scan takes S = 2.7-3 ms. Each column whose sample settles a key
  adds the canaries after it (5 wire bytes, 0.11 ms, for COL0-7; 10, 0.23 ms, for COL8/9),
  so a scan in which every column settles a key (a chord across all ten columns, or its
  release) is about 1.35 ms longer.
- **Per pass:** one scan step, 0.2-0.3 ms of I2C; a pass that settles a key adds its
  canaries: at most 6 transfers for COL0-7 and 8 for COL8/9, about 0.5 ms at most. Today every pass spends about 0.3 ms
  (TCA8418 count and INT_STAT reads, U701 ports). Every 20 ms the step-row sample adds about
  0.5 ms (two chips x two 2-byte reads); every 33 ms the frame pass (OLED 1 KB at 10 MHz
  = 0.82 ms, render, LED frame and its DMA start, the transport snapshot) adds an
  estimated 1.2-2.5 ms, as today.
- **Bus:** the scan fills most of the loop's bus time (it would otherwise sit idle); the step
  row needs about 2.5 %, the step LEDs less. Core 0 is the only bus user.
- **Press-to-event:** at most one scan until the key's column is sampled, then the first
  sample at least 5 ms later: <= S + 5 ms + S, plus at most one frame pass and one step-row
  sample inside that window, plus the canaries in the settling pass: about 3 + 5 + 3 + 2.5
  + 0.5 + 0.2 = 14.2 ms worst for a key changing alone, 7-10 ms typical. A scan in which
  all ten columns settle is about 1.35 ms longer (see above), so a ten-key chord is about
  15.3 ms worst by estimate, against 12.86 ms in the model below. The bench must measure
  chords as well as single keys.
  `now_ms` is whole milliseconds; flooring both stamps can only make a settle earlier. With
  the old 8 ms debounce the worst case would pass 15 ms; hence 5 ms, the low end of the
  spec's 5-10 ms (the knob if the bench finds the frame pass longer).
- **Playing, measured in the host loop model** (*changed after the final review*). A host
  test runs the loop with a wire-time clock (+30 us per pass, a 2.5 ms frame every 33 ms, a
  0.5 ms step-row sample every 20 ms) and checks <= 15 ms on playing:
  - one key alone, in every column, at every phase of the 660 ms period of both tasks in
    0.37 ms steps (18000 presses): worst 11.69 ms;
  - a ten-key chord, one key per column, at the same phases: worst 12.86 ms;
  - a playing stream of 50 presses a second (beyond a fast two-handed roll), each held
    60 ms, for 10 s: worst 11.77 ms.
  The clock counts wire bytes only, with no START/STOP or SDK call overhead: these are host
  model figures, not device measurements, and the bench item stays.
- **Heavy traffic can pass 15 ms.** A settle costs bus time (its canaries), so keys changing
  together slow each other down. Under a scan just under 5 ms long a key needs a third scan
  after it is first seen. Bursts of presses far beyond playing do this: the stress test
  (three lanes of 80 presses, one every 0.37-2 ms, each held 30 ms, then a quiet span)
  reaches 15.59 ms in the model. The device's call overhead, which the model leaves out,
  lowers the point at which this starts, so such traffic, far beyond playing, can pass
  15 ms on the device. No held press is lost: the test checks that each press held 30 ms
  gets exactly one down, within 25 ms.

## Tests

Fakes derive every read from what the driver wrote: a column is low only when its pin is an
output with OLAT 0; a row reads low when a closed key sits in a low column, high when it has
its pull-up, and a worst-case value when it floats (reset chip). The README pin map is
encoded in the test on its own, not taken from the driver. Every transfer is checked: IODIR
writes never set bit 7, at most one of the ten columns is low at any row read, timeouts are
2000 us, a pointer write before a read has `nostop` set and the read has not. Faults can be
injected at any transfer (fail once, fail from here on), as a write that lands only up to
its n-th byte before failing, as a short byte count, as a reset of one chip before any
transfer, and as a reset inside a read (the rest reads 0xFF with a full count).

`test_panel_input.c` (TCA8418 tests mapped):

| Old test | New |
| --- | --- |
| init_configures_keypad | init_configures_scanner (all register values, write order, failure) |
| key_codes_map_to_controls | all 80 keys map to the README table |
| bounce_yields_one_press, release_is_reported | kept, on samples |
| ten_key_chord_all_reported | all 80 keys held at once: 80 presses, no ghost |
| overflow_resyncs_without_stuck_keys, failed_status_read_after_overflow_emits_nothing, failed_event_read_emits_nothing, overflow_survives_a_failed_transaction | each fault at every transfer of init and of a scan, with keys pending down and up: no phantom, held key not re-reported, a later press works |
| i2c_error_does_not_block, i2c_error_reconfigures_before_reading, init_failure_at_boot_recovers, dead_bus_is_retried_at_most_every_100_ms | kept on the scanner |
| failed_flush_stays_in_recovery | failed init stays in recovery, retried every 100 ms |
| play_* (4 tests), queued_play_survives_overflow | kept; PLAY survives scanner errors |
| resync_drops_pending_events, held_pad_survives_resync_silently, key_held_through_resync_is_silent_on_release, pad_released_during_stall_presses_again | kept |
| reinit_after_resync_keeps_held_key_suppressed, resync_flushes_without_reinit | re-init never re-reports a held key; resync does no I2C and keeps the selection |
| key_held_through_overflow_is_silent_on_release, pad_released_in_an_overflow_presses_again | reset of either chip before every transfer of init and scan: no phantom, held SCENE not re-pressed |
| full_ring_keeps_changes_pending | kept |
| overflow_keeps_play_events_in_a_wrapped_ring | ring order across the wrap |
| reset_chip_is_reconfigured | found by the next check, rows floating low give no press |

New: one column low per row read, in scan order; one bus step per poll; a sample seen once
before the bus dies never becomes a press; a tap that ends during an outage is lost (the
contract); a U802 write that fails after latching COL8 low never ghosts; a resync while
COL8 is selected with SCENE held never ghosts; a key held at power-up is silent;
press-to-event <= 15 ms over all phases; transfers per second bounded. After the final
review: a U801 reset after the scan's check never settles a tap seen once (rows floating
low), a held key's one-sample chatter (rows floating high, or a reset inside the ROW7 read)
or the end of a held key's suppression after a resync, and the next pass is the
configuration; the fault sweep adds a reset with rows floating high and keys that change
between two samples inside its window; a settling pass makes at most 6 transfers for
COL0-7 and 8 for COL8/9. Press-to-event: one key, a ten-key chord and a 50 presses/s
stream within 15 ms; a stress test (a press every 0.37-2 ms) within 25 ms with no press
lost. `test_key_scan.c`: `key_scan_verify` reads 2 transfers for COL0-7 and 4 for COL8/9,
finds a U801 reset for every column and a U802 reset for COL8/9, a reset inside its read
and one during init, fails on any failed or short transfer, ignores IPOL, keeps the
selection, and refuses a column past COL9.

`test_expanders.c`: both step chips configured with the values above, IPOL first; steps
8/16 come from 0x24 and U701 bits 7/15 are ignored; a reset U703, before or inside its read,
never presses 8 or 16; U703 missing keeps the other 14 steps; no long press from a chip
whose reads fail while the key is released; reads only when a sample is due (bounded
transfers per second, init excluded); the existing tests with the new values. After the
final review: a step held 499 ms on a sample grid shifted by one 2 ms pass makes no long
press (steps 3, 8 and 16; a healthy bus, the other chip missing, the key's own chip failing
from the release), and a real long hold long-presses once, in a pass that sampled its chip.

## Docs

`2026-11-16-session-panel-design.md` (grid electronics, hardware interface, input latency,
input servicing, flash save/load, testing), README (I2C table, file list, tests),
ARCHITECTURE (core table, hardware table, I2C and resync text), HARDWARE_STATUS (migration:
existing boards lose steps 8 and 16 on 0x20 and need U703 at 0x24, and the scanner on
0x22/0x23 instead of the TCA8418; bench list), JITTER_MEASUREMENT (load case), the `ui.c`
comment that names the TCA8418. The old implementation plan stays as history.

## Bench (HARDWARE_STATUS)

Press-to-event for pads, SCENE and MUTE; the frame pass and scan period on the device;
several keys held, no ghost; each SCENE and MUTE key held, no other key reports; row rise
time after a column is released (scope on a row with a key held in the previous column);
I2C load and SDA integrity with the scan running; CV/Gate noise with the scan running;
steps 8 and 16 on U703.

## Decisions (after review)

1. Free-running scan, one step per pass (kept by all reviewers): pacing to ~4 ms leaves the
   bus idle and adds up to a scan period to the latency.
2. Debounce 5 ms evaluated only at a key's own samples (kept by all).
3. Per-scan check of both scanner chips; canary written first and read after the data
   (changed after review: IODIR alone, written last, missed a reset during init; IPOL
   added). No column-pin readback.
4. Resync and boot take every key as held until seen up (kept by all); the window is up to
   S + 5 ms + S, and the scanner keeps its column selection across a resync.
5. Step row: two independent chips, reads only when a sample is due, IPOL read back after
   every sample, no long press from an untrusted chip (changed after review from GPPU read
   before the data).
6. The settle proof (after the final review, found by three reviewers): a column sample
   that would settle a key is followed in the same pass as the sample, before it is applied, by a read of the reset canaries
   (`key_scan_verify`: U801 IODIR, and U802 IODIR for COL8/9) and applied only if they
   hold; up to 6 transfers in such a pass for COL0-7 and 8 for COL8/9, the full scan-start
   check kept. The 15 ms press-to-event requirement is tested on playing (one key, a chord,
   a 50/s stream); heavy traffic is only bounded (25 ms in the model).
7. Step row long press only in a pass that took a good sample of the key's chip (after the
   final review): up to one sample late, never on a sample taken before the release.
