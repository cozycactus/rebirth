# Key matrix scan on two MCP23017, steps 8 and 16 on a third: design

Date: 2026-11-16. Status: proposed.
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
- Press-to-event within 15 ms for pads, SCENE and MUTE, with the Core 0 loop as it is.
- Keep what the current driver guarantees: debounce, no event from a stall or a panic
  (resync), ring-full retention, chip health readback, retries paced at 100 ms, every I2C
  transfer bounded (2000 us) with its exact byte count checked, PLAY on GPIO 28 untouched.
- Core 1 is not touched: no new lock, no new interrupt, no DMA. Core 1 never touches I2C.
- `hardware/` is not changed.

## Hardware as the firmware sees it

All five expanders are MCP23017 on I2C0 (GPIO 8/9, 400 kHz, external 2.2 k pull-ups),
registers in BANK = 0 (power-on default; IOCON is not written).

| Chip | Addr | Pins | IODIR A/B | GPPU A/B | IPOL A/B | OLAT A/B at rest |
| --- | --- | --- | --- | --- | --- | --- |
| U701 step buttons | 0x20 | GPA0-6 = steps 1-7, GPB0-6 = steps 9-15; GPA7/GPB7 open | 0x7F / 0x7F | 0x7F / 0x7F | 0x7F / 0x7F | 0x00 / 0x00 |
| U703 steps 8, 16 | 0x24 | GPA0 = step 8, GPA1 = step 16; all other pins open | 0x03 / 0x00 | 0x03 / 0x00 | 0x03 / 0x00 | 0x00 / 0x00 |
| U702 step LEDs | 0x21 | all 16 outputs | unchanged | | | |
| U801 scanner | 0x22 | GPA0-6 = ROW0-6, GPB0 = ROW7; GPB1-7 = COL0-6, GPA7 = COL7 | 0x7F / 0x01 | 0x7F / 0x01 | 0x00 / 0x00 | 0xFF / 0xFF |
| U802 scanner | 0x23 | GPA0 = COL8, GPA1 = COL9; all other pins open | 0x00 / 0x00 | 0x00 / 0x00 | 0x00 / 0x00 | 0xFF / 0xFF |

- Every GPA7/GPB7 is an output (IODIR bit 7 = 0 on both ports of 0x20, 0x22, 0x23 and 0x24).
  Unused pins are outputs too, as the schematic notes ask; on U701/U703 they drive low, so
  an existing board with buttons to GND still on U701 GPA7/GPB7 sees no contention.
- Each key is row -> switch -> BAT54J anode -> cathode -> column. With the selected column
  low and the others high, a closed key pulls its row to about 0.25 V (VIL = 0.66 V); a key
  in a high column is reverse-biased. Several held keys never ghost.
- A chip that resets (brown-out, ESD) comes back with IODIR = 0xFF and every other register
  0: on U801 the rows lose their pull-ups and the columns float.

## Scan driver (`key_scan.c`, replaces `tca8418.c`)

```c
#define KEY_SCAN_ROW_ADDR 0x22   // U801: ROW0-7, COL0-7
#define KEY_SCAN_COL_ADDR 0x23   // U802: COL8, COL9
#define KEY_SCAN_ROWS 8
#define KEY_SCAN_COLS 10
bool key_scan_init(void);                      // configure both chips, no column selected
bool key_scan_check(bool *intact);             // read both IODIR back
bool key_scan_column(uint8_t col, uint8_t *down); // select col, read the rows
```

- `key_scan_init` writes, in this order, U801 OLAT = FF/FF, U802 OLAT = FF/FF, U801 GPPU,
  U801 IODIR, U802 IODIR (five 3-byte writes), so a column only becomes an output at its
  high level. It forgets which column is selected.
- `key_scan_check` reads IODIR (2 bytes) of U801 and of U802. False on an I2C error;
  otherwise `*intact` tells whether both hold the values above (a reset chip reads FF/FF).
- `key_scan_column(col)`: deselect first, then select, so at most one column is low when
  the rows are read:
  - col 0-7: if U802 may have a column low (col 8/9 selected, or the selection is unknown
    after an error), write U802 OLATA = 0xFF; then write U801 OLATA/OLATB in one 3-byte
    transfer (all bits 1, the column's bit 0).
  - col 8/9: if U801 may have a column low, write U801 OLAT = FF/FF; then U802 OLATA =
    0xFE (COL8) or 0xFD (COL9).
  - Then read U801 GPIOA/GPIOB in one transfer (pointer write, repeated start, 2-byte read).
    `*down` bit r = ROW r low (the firmware inverts; IPOL stays 0).
  - The same read returns the column pins of U801 (GPA7, GPB1-7). They must read back as
    driven (all high, or only the selected U801 column low); a mismatch fails the step like
    an I2C error. This catches a reset U801 on its first sample at no bus cost.
  - The write of the column and the read are separate transfers on purpose: the pointer
    write and the address bytes of the read give a released row about 67 us to rise through
    its pull-up before the port is sampled; the estimate needs about 10 us (bench item).
  - Moving between two U801 columns that sit in different bytes (6 -> 7) writes OLATA before
    OLATB, so for one byte time both are low. No row is read then; it is harmless.
  - Any failed transfer makes the selection unknown and returns false.

## Panel input (`panel_input.c`)

The API in `panel_input.h` stays (`init`, `poll(now_ms)`, `next`, `resync`, `panel_event_t`,
`PLAY_DEBOUNCE_MS`, `PLAY_BUTTON_PIN`, `PANEL_RETRY_MS`). `PANEL_DEBOUNCE_MS` becomes 5.
`PANEL_CHECK_MS` goes: the configuration is now checked on every scan.

### One bus step per pass

A scan is 11 steps: step 0 is `key_scan_check`, steps 1-10 are columns 0-9. Each
`panel_input_poll` does PLAY first (as now), then at most one step, then returns. The scan
is free-running: the loop is the clock. A pass never blocks for a whole scan (2-3 ms), so
`encoder_poll`, which decodes quadrature by polling in the same loop, keeps its rate.

### Debounce at sample time

Each key keeps `raw`, `accepted`, `suppressed`, `since_ms` as now. A key's state is touched
only in the pass that sampled its column: `see()` with the fresh sample, then `settle()`.
A change is reported when the key has read the same since it changed and the sample is at
least `PANEL_DEBOUNCE_MS` (5 ms) after the change, so every event rests on at least two
samples of that key, 5 ms apart. A key whose column is not sampled (bus down) never settles
on elapsed time alone. A full ring leaves the change pending; the next sample of the column
retries it. PLAY keeps its own debounce (20 ms) from `gpio_get` on every pass.

### Chip health

- U801 and U802 are one unit: a write that failed on U802 may have left COL8 or COL9 low,
  and a held SCENE or MUTE key would then pull its row low in every U801 column. So any
  failed transfer, a failed check, a check that reads a reset chip, or a column readback
  mismatch marks the scanner unconfigured: nothing is sampled until `key_scan_init` has
  written both chips again (an attempt after a failed one waits `PANEL_RETRY_MS`), and the
  scan restarts at step 0.
- On that loss every key's pending change is cancelled: `raw = accepted || suppressed`,
  `since = now`. A key goes back to its accepted state, a suppressed key back to "held", so
  ending a suppression again needs two up samples 5 ms apart. Accepted keys stay as they
  are: no release is reported for a key held through a bus error, and the key state needs
  no suppression after a re-init, because a scanner reads levels and a held key reads as
  held, not as a new press.
- Why no phantom press can come from a chip that reset: a reset chip gives at most one bogus
  sample per key before it is caught, because the check runs at the start of every scan and
  so between any two samples of the same column (U801 is usually caught at once by the
  column readback). One sample never settles (debounce needs two, 5 ms apart), and the loss
  cancels it. A failed transfer is the same: its step is not decoded, the loss cancels the
  rest.

### Resync and boot

`panel_input_resync()` (after a flash save/load and around a panic) does no I2C:
- drops every queued event, PLAY included, and handles PLAY as now (`forget_play`);
- takes every key as held: `raw = true`, `accepted = false`, `suppressed = true`. A key ends
  its suppression silently once it has read up for 5 ms; until then nothing is reported.
  - A key held through the stall reports nothing, its release reports nothing, and its next
    press is normal.
  - A key released during the stall presses normally again about 5 ms later.
  - A key pressed during the stall and still held reports nothing.
  - A press within about 5 ms after the resync is taken as part of the stall.
- restarts the scan at step 0.

`panel_input_init()` sets the keys up the same way, so a key held at power-up does
nothing, like PLAY today. It tries `key_scan_init` once; the first poll retries a failure.

### What goes away

`tca8418.c/.h`, the event FIFO, its overflow flag, the drain, the CFG readback and the
release/suppress split after a drain. There is no chip FIFO to overflow: a level scan cannot
lose a press, only delay it while the bus is down. The ring-full path stays.

## Step row (`mcp23017.c`)

The 16-bit step word and `step_buttons_t` stay; bit n = step n+1.

- Two chips, each with its own `configured`, retry clock and `rebaseline` flag: U701 for
  bits 0-6 and 8-14, U703 for bits 7 and 15. A missing or broken U703 costs only steps 8
  and 16 (plus one 2 ms timeout per 100 ms), and an old board without it keeps 14 steps.
- Configuration per chip: OLAT, IODIR, GPPU, IPOL, each one 3-byte write (OLAT first, so
  the output-only pins come up low).
- **Reads only when a sample is due.** Today `mcp23017_poll` reads the ports on every pass but
  takes a sample only every 20 ms (`MCP_DEBOUNCE_MS`). It now reads only then, or at once
  when a rebaseline is pending. Edges, `held` and the long-press timer still run on every
  pass, from the last sample, so the behaviour is the same with about 1/60 of the traffic.
- **Every sample proves the configuration.** Each chip is read from GPPUA (0x0C) to GPIOB
  (0x13) in one 8-byte transfer: GPPU must read back as configured, GPIOA/GPIOB carry the
  buttons. A mismatch means the chip reset: it is configured again and its next good read
  becomes its baseline, with no press, release or long press (as now). This replaces the
  1-second IPOL readback and the "three keys at once" guard, which could never fire for U703
  (two keys) and so would let a reset U703 press steps 8 and 16.
- A failed read keeps that chip's bits at the last accepted state and writes its
  configuration again before its next read (as now). Bits 7 and 15 of U701 are never used.
- `mcp23017_resync()` rebaselines both chips (as now, per chip). `mcp23017_read_raw()` reads
  both chips and falls back to the last accepted word.

## Timing and bus budget (estimates, 400 kHz, 22.5 us per byte with its ACK)

| Transfer | Wire bytes | Time |
| --- | --- | --- |
| Column write, U801 (addr, reg, A, B) | 4 | 90 us |
| Column write, U802 (addr, reg, A) | 3 | 68 us |
| Row read or IODIR check (addr, reg / addr, 2 data) | 5 | 113 us |
| Step-row sample per chip (addr, reg / addr, 8 data) | 11 | 248 us |

- **Scan:** check 2 x 113 + columns 0-7 8 x 203 + column 8 (90 + 68 + 113) + column 9
  (68 + 113) + U802 release before column 0 (68) = about 2.36 ms of wire time in 36 SDK
  calls; with call overhead and the rest of each pass (encoders, UI, cached LED mask) a scan
  takes S = 2.6-3 ms.
- **Per pass:** one scan step, 0.2-0.3 ms of I2C. Today every pass spends about 0.3 ms
  (TCA8418 count and INT_STAT reads, U701 ports). Every 20 ms the step-row sample adds
  0.5 ms; every 33 ms the frame pass (OLED 1 KB at 10 MHz, LED frame, render) adds an
  estimated 1.2-2.5 ms, as today.
- **Bus:** the scan fills most of the loop's bus time (it would otherwise sit idle); the step
  row needs 2.5 %, the step LEDs less. Core 0 is the only bus user.
- **Press-to-event:** at most one scan until the key's column is sampled, then the first
  sample at least 5 ms later: <= S + 5 ms + S, plus at most one frame pass and one step-row
  sample inside that window: about 2.9 + 5 + 2.9 + 2.5 + 0.5 = 13.8 ms worst, 7-9 ms
  typical. With the old 8 ms debounce the worst case would pass 15 ms; hence 5 ms, the low
  end of the spec's 5-10 ms. A host test runs the loop with a wire-time clock and checks
  <= 15 ms for every scan phase; the bench measures it.

## Tests

Fakes derive every read from what the driver wrote: a column is low only when its pin is an
output with OLAT 0; a row reads low when a closed key sits in a low column, high when it has
its pull-up, and a worst-case value when it floats (reset chip). The README pin map is
encoded in the test on its own, not taken from the driver. Every transfer is checked: IODIR
writes never set bit 7, at most one column is low at any row read, timeouts are 2000 us.

`test_panel_input.c` (TCA8418 tests mapped):

| Old test | New |
| --- | --- |
| init_configures_keypad | init_configures_scanner (all register values, write order, failure) |
| key_codes_map_to_controls | all 80 keys map to the README table |
| bounce_yields_one_press, release_is_reported | kept, on samples |
| ten_key_chord_all_reported | all 80 keys held at once: 80 presses, no ghost |
| overflow_resyncs_without_stuck_keys, failed_status_read_after_overflow_emits_nothing, failed_event_read_emits_nothing, overflow_survives_a_failed_transaction | failed transfer at every step of a scan: no phantom, held key not re-reported, a later press works |
| i2c_error_does_not_block, i2c_error_reconfigures_before_reading, init_failure_at_boot_recovers, dead_bus_is_retried_at_most_every_100_ms | kept on the scanner |
| failed_flush_stays_in_recovery | failed init stays in recovery, retried every 100 ms |
| play_* (4 tests), queued_play_survives_overflow | kept; PLAY survives scanner errors |
| resync_drops_pending_events, held_pad_survives_resync_silently, key_held_through_resync_is_silent_on_release, pad_released_during_stall_presses_again | kept |
| reinit_after_resync_keeps_held_key_suppressed, resync_flushes_without_reinit | re-init never re-reports a held key; resync does no I2C |
| key_held_through_overflow_is_silent_on_release, pad_released_in_an_overflow_presses_again | U801 or U802 reset before every step: no phantom, held SCENE not re-pressed |
| full_ring_keeps_changes_pending | kept |
| overflow_keeps_play_events_in_a_wrapped_ring | ring order across the wrap |
| reset_chip_is_reconfigured | found within one scan, rows floating low give no press |

New: one column low per row read, in scan order; one bus step per poll; a sample seen once
before the bus dies never becomes a press; a U802 write that fails after latching COL8 low
never ghosts; a key held at power-up is silent; press-to-event <= 15 ms over all phases;
transfers per second bounded.

`test_expanders.c`: both step chips configured with the values above; steps 8/16 come from
0x24 and U701 bits 7/15 are ignored; a reset U703 never presses 8 or 16; U703 missing keeps
the other 14 steps; reads only when a sample is due (bounded transfers per second); the
existing tests with the new readback values.

## Docs

`2026-11-16-session-panel-design.md` (grid electronics, hardware interface, input latency,
input servicing, flash save/load, testing), README (I2C table, file list, tests),
ARCHITECTURE (core table, hardware table, I2C and resync text), HARDWARE_STATUS (migration:
existing boards lose steps 8 and 16 on 0x20 and need U703 at 0x24, and the scanner on
0x22/0x23 instead of the TCA8418; bench list), JITTER_MEASUREMENT (load case), the `ui.c`
comment that names the TCA8418. The old implementation plan stays as history.

## Bench (HARDWARE_STATUS)

Press-to-event for pads, SCENE and MUTE; several keys held, no ghost; row rise time after
a column is released (scope on a row with a key held in the previous column); I2C load and
SDA integrity with the scan running; CV/Gate noise with the scan running; steps 8 and 16
on U703.

## Decisions for review

1. Free-running scan, one step per pass, versus a scan paced by the clock (README: every
   ~4 ms). Pacing leaves the bus idle and adds up to a scan period to the latency.
2. Debounce 5 ms evaluated only at a key's own samples, versus every pass.
3. Per-scan IODIR check of both scanner chips plus a free column readback, versus a 1 s
   readback like today.
4. Resync and boot take every key as held until seen up, versus a first-scan baseline.
5. Step row: two independent chips, reads only when a sample is due, every sample verified
   by GPPU, versus one combined unit with the IPOL readback.
