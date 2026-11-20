# Four MIDI Outputs — Design

Date: 2026-11-20. Status: the routing model (a port per track, default by page,
saved in the project), Clock and transport on every output, the fold to output A
on one-output boards, the overload rule (skip notes, keep playing, lower tracks
first) and the density promise (the whole wire, by the table) were chosen by the
project owner in conversation. The owner then approved three sections as
presented: "Behaviour"; "How it is built"; and "Simulator", "Tests" and "Not
verifiable without a board" together. Details added or corrected inside them
after reading the code, and everything else that is the designer's call, are
marked **(call)** inline or listed in "Left to the builder". No assembled board exists:
nothing here was run on hardware. Statements about today's code refer to
`origin/master` f8a8737.

## Intent

Rev B has four DIN outputs, A-D (J401, J403, J404, J405), but the firmware
drives one: UART0 TX on GPIO 0. One wire carries 46,875 / BPM bytes per step.
64 tracks need 390 bytes a step: on the host fake they held at 120 BPM (390.6)
and stopped at 140 BPM, because a TX queue overflow stops the transport. Four
outputs give four times the wire, and separate instruments per socket.

## Goals

1. Every track has a port A-D, saved in the project; default by page: T1-16 A,
   T17-32 B, T33-48 C, T49-64 D.
2. Clock, Start, Stop and Continue on every output.
3. One-output boards (Pico, Pico 2) play every track through A and keep the
   stored port.
4. No stop on MIDI load in Pattern and Song mode: an overloaded output skips
   Note Ons, lower tracks first, and no other output is affected.
5. A port skips nothing while its load fits its wire (the density table).

## Non-goals

- Ports for MIDI Song tracks. They need a field in the song image and in the
  exporter (`sim/web/midi_songs.js`): a follow-up. Idea to record, not designed:
  an event's track byte is below 64 for every event but a Clock (which carries
  0xFF), so its two high bits are free for a port.
- A per-output switch for Clock and transport.
- Raw byte-for-byte MIDI Thru from IN to an OUT (already a non-goal of the
  external clock design).
- A per-instrument port selector in the simulator's rig.

## Anti-goals

- A stuck note: a Note Off on another port or channel than its Note On, or a
  Note Off owed for a Note On that was skipped.
- An overloaded output delaying the Clock or the notes of another output.
- An overload that stops the transport in Pattern or Song mode.
- A one-output board that rewrites a project's ports on load or save.
- A glitch low on a TX line at start-up or hand-over (a receiver reads a byte).

## Behaviour

### Routing

- A `Port:` row follows the MIDI channel row on the SYNTH, DRUM and kit lists.
  The port belongs to the track, not the location: all 32 locations share it,
  as they share `kit_config`. It is saved in the project.
- A Note Off always goes to the port and channel its Note On went to, also when
  the setting changes while the note sounds: today's rule for the channel
  (`active_note_state`, `kit_runtime`, `thru_channel`), extended to the port.
  The same-route test in `prepare_trigger` includes the port, so a tie or slide
  never joins notes on two ports and a change never strands a note.
- Stop releases notes per port and sends Stop on every output. Panic clears all
  queues and sends All Sound Off / All Notes Off on every output.

### One-output boards

On Pico and Pico 2 GPIO 23, 24 and 29 are used inside the module (SMPS mode,
VBUS sense, VSYS ADC), so these boards have output A only and every track plays
through it. The stored port stays untouched through load and save, and the row
shows it with the output used: `Port:   B (A)`.

Old saves get the port by page: a v7 project loaded on Rev B spreads over four
outputs (on a Pico nothing changes); eight-track saves (v4-v6) hold T1-8 only,
so they stay on A.

### Overload: admission per output

Evaluated per physical port when a step is built in the timer ISR (phase 1 of
`run_pulse`).

- **D**, the bytes the wire sends in one step at the tempo played: the step
  duration in us / 320, rounded down, where the step duration is
  `PULSES_PER_STEP` times the pulse spacing the timer itself uses **(call)**.
  Internal clock: `pulse_interval_us`, the truncated interval (at 187.5 BPM D is
  249 bytes, where 46,875 / BPM gives 250). Following an external clock:
  `ext_sub_us()`, a quarter of the Clock interval estimate, and the internal
  period until two Clocks have been seen. So D always exists.
- **Step budget** D - 6: six Clock bytes go out every step. Unused budget is not
  carried over.
- **Charges.** A SYNTH or DRUM Note On charges 6 (itself and its Note Off,
  whenever that is sent). A tie or slide continuation on the same pitch charges
  0. A kit voice hit charges 4: kit messages are Note On with one status byte per
  kit, so 2 data bytes for the hit and 2 for its release (Note On velocity 0);
  the first admitted voice of a kit step charges 2 more for the two status
  bytes. A flammed voice reserves 4 more for its second hit, and the first
  flammed voice of a kit step 1 more: the second hits are a send of their own
  with their own status byte **(call; the measurement below needs it)**. A flam
  without room plays as a plain hit and counts in `flam_drops`.
- **Order.** Tracks in track order (T1 first); kit voices in `kit_voice_order`
  (BD, SD, CP, CH, OH, RS, LT, MT, HT, CC, RC).
- **Admission.** A Note On is admitted when its charge fits the remaining budget
  and the real queue passes the guard: bytes waiting in that port's TX queue
  when the step began, plus the Note On bytes already admitted for the port in
  this step, plus the message, must not exceed the guard limit
  `G = min(D + 32, MIDI_TX_QUEUED_MAX - MIDI_MANDATORY_MAX)`. The room under the
  guard is reserved when the fill is read (see "No overflow by construction").
- **A skipped Note On.** The track's previous note is released as on an
  inactive step. CV and gate of the track fire as usual (a MIDI+CV track keeps
  its CV note). No MIDI Note Off is sent later for it. The port's drop counter
  goes up by one per skipped note or kit voice.
- **Lag.** An admitted Note On waits behind at most D + 32 queued bytes plus the
  real-time bytes put ahead of it: about one step plus 10 ms. Releases are never
  refused: from playback alone a Note Off waits behind at most D + 32 + 258
  bytes (143 ms at 300 BPM), and behind 1,023 bytes (327 ms) at the very most.
- **Last line.** The overflow flag and `panic_playback()` stay, as the MIDI Song
  player's rule and as a safety net; the host tests show that Pattern and Song
  playback cannot reach them (see "No overflow by construction").

### The density promise

A port skips nothing while its load fits the wire at the tempo played. For n
notes per step, each a Note On and a Note Off on its own channel, the load is
6n + 6 bytes with the Clock, so n = floor((D - 6) / 6), nominally
floor((floor(46,875 / BPM) - 6) / 6):

| BPM | D (bytes per step) | Budget | Notes per step on one output |
|---|---|---|---|
| 120 | 390 | 384 | 64 |
| 140 | 334 | 328 | 54 |
| 180 | 260 | 254 | 42 |
| 240 | 195 | 189 | 31 |
| 300 | 156 | 150 | 25 |

Running status and shared channels only lower the load. Corollaries:

- **Default layout, SYNTH and DRUM tracks:** 16 per output, 102 bytes a step
  (measured) against 156.25 at 300 BPM. Nothing skips at any tempo.
- **Kits** exist only on T1-8 (`KIT_TRACK_SLOTS`), so by default both are on A.
  Two kits with all 11 voices on every step plus 14 SYNTH tracks on one output:
  182 bytes a step (measured; the charges give 2 x 46 + 14 x 6 + 6). It fits to
  240 BPM (195); at 300 BPM A's higher tracks skip. Two kits with every voice
  flammed: 188 bytes (charges 2 x 91 + 6), which also fits to 240 BPM.
- **Pico:** all 64 tracks on the one wire fit to 120 BPM (390 against 390).

**Measured on the host** (build of f8a8737, the fake UART in wire-time mode,
320 us a byte, no interrupt latency, no flash stalls; 8 bars per cell at 100,
120, 140, 160, 180, 200, 240 and 300 BPM, 64 bars for the cells near the edge).
Wire bytes include the six Clock bytes. "Holds to": the highest of these tempos
that played without overflow; every cell above it stopped, in 8 or 64 bars.

| Load on one wire | Wire bytes per step | Holds to |
|---|---|---|
| A: N SYNTH tracks, own channels, N = 8, 16, 24, 25 | 54, 102, 150, 156 | 300 |
| A: N = 26, 32, 48 (overflow at bar 11, 22, 53 of 64) | 162, 198, 294 | 240, 200, 140 |
| A: N = 64 (held 64 bars at 120) | 390 | 120 |
| C: two kits, 11 voices every step, + K SYNTH, K = 0, 6 | 98, 134 | 300 |
| C: K = 14 (held 64 bars at 240) | 182 | 240 |
| C: K = 22, 30 | 230, 278 | 200, 160 |
| D: as C with every voice flammed, K = 0, 6, 14, 22, 30 | 188 to 368 | see below |

Family D survived above the wire's capacity only because today's flam cap
(`KIT_FLAM_MAX_QUEUED`, 319) dropped second hits (K = 0: 86 in 8 bars at
300 BPM); K = 14 still stopped at 300. Loads just above capacity survived 8 bars
and overflowed later; loads just below held 64 bars. Every surviving run put all
768 Clocks on the wire, the latest 1,276 us after its pulse (bound 1,280 us).

### Drop counters

`sequencer_jitter_stats_t` gains `note_drops[4]`, one per output. On Pattern
Select the only free text line is y = 8, which today holds `FLAM DROP n`
between the title (y = 0) and the big pattern label (y = 16); the lines at 30,
37, 44 and 56 are taken. Proposal **(call)**: `DROP A12 B3`, only outputs with
drops, then ` F7` for flam drops, within the 21 columns of the 6-pixel font;
with no note drops the line stays `FLAM DROP n`. Step 13 on that screen (the
jitter reset) clears them with the other statistics.

### Keyboard pass-through

A key from MIDI IN, or a pad shown as a key, goes to the port and channel of the
track being edited (`midi_in_cell()`), the port resolved as for playback. The
table remembers port and channel per note (`thru_channel` becomes a route), so
the Note Off follows its Note On. The queue caps apply to that port's queue.

### MIDI Song player

Unchanged in this step: its events carry no port, every note goes to A, and its
overflow rule (stop) stays. It shares calls that act on every output: its Clock
events call `midi_send_clock()`, PLAY sends Start, `midi_song_end()` sends All
Notes Off. So in MIDI Song mode B-D get Clock, Start, Stop and All Notes Off and
no notes **(call: harmless, and Clock on every output is the owner's rule)**.

## How it is built

### Hardware resource per output

| Output | Socket | GPIO | Driver | Pin notes |
|---|---|---|---|---|
| A | J401 | 0 | UART0 TX: PL011, FIFO off, TX interrupt on core 0 | unchanged |
| B | J403 | 23 | `pio1` state machine 0 | UART1 RX and RTS only |
| C | J404 | 24 | `pio1` state machine 1 | has UART1 TX, not used |
| D | J405 | 29 | `pio1` state machine 2 | UART0 RX only |

Every TX net has a 10 k pull-up to +3V3 (R401, R411, R421, R431 in
`draw_midi_out`) ahead of a 74LVC1G14 and an MMBT3904 current loop; idle high
means no loop current. B-D share one program in `pio1`; their interrupt is
PIO1_IRQ_0, handled on core 0 **(call)**. `pio0` stays with the LED chain. The
third block stays free for the microSD interface, which needs GPIO base 16 set
before any program is added to its block. GPIO 23, 24 and 29 are reachable with
base 0. RP2040 boards never use PIO for MIDI.

`rgb_leds_init()` claims the first free state machine of `pio0` and falls back
to `pio1`; MIDI claims `pio1` state machines 0-2 by number (`pio_sm_claim`
asserts when one is taken). `midi_init()` runs before `ui_init()` calls
`rgb_leds_init()`, so the order holds today; the LED claim is fixed to `pio0`
so a change of order cannot hand it a MIDI state machine.

Not chosen: C on UART1 (only GPIO 24 has a UART TX function, so B and D need
PIO anyway, and C on UART1 would make three code paths); all four on PIO
(changes the verified output A path and the Pico builds, where MIDI IN shares
UART0).

### The PIO program

PIO has no "TX FIFO empty" interrupt, only "TX FIFO not full". The program
raises an IRQ flag itself when it has pulled a byte, and the handler then feeds
the next one. At most one byte waits behind the one on the line, as in the
PL011's holding register, and a Clock still waits at most four byte times
(1.28 ms) before interrupt latency. A sketch, to be checked with pioasm:

```
.program midi_tx
.side_set 1 opt
    pull       side 1 [6]   ; idle and stop bit high; stalls here when empty
    irq nowait 0 rel side 1 ; flag n for state machine n: feed the next byte
    set x, 7   side 0 [7]   ; start bit
bitloop:
    out pins, 1             ; LSB first, autopull off
    jmp x-- bitloop    [6]
```

Ten bits of eight cycles; the stop bit is high through the pull, its delay and
the irq. Divider clk_sys / (8 x 31,250): 600 at the RP2350's default 150 MHz,
exact (the firmware never changes the system clock). "Writable" for a PIO port
means its TX FIFO is empty. The handler clears each set flag
(`pio_interrupt_clear`) and drains that port. Pin hand-over, so the line never
glitches low: `pio_sm_set_pins_with_mask` (high), `pio_sm_set_pindirs_with_mask`
(output), then `pio_gpio_init`, then configure and enable the state machine.
`ws2812_program_init` uses the opposite order and is not copied.

### Backend seam

Under the queue a port is four operations: writable, put one byte, TX interrupt
enable (the UART's TX enable, the PIO flag's source enable), transmitter idle.
The firmware implements them for the PL011 and for PIO. The host fakes and the
simulator implement A as today's UART and B-D as byte pipes (320 us a byte, one
byte waiting), not as PIO registers.

### Queue layer (`midi_out.c`)

Per port: a 1,024-byte TX queue, the running-status state
(`midi_tx_message_left`, `_last_status`, `_ran_empty`), the 4-entry
Start/Continue/Stop queue, the pending-Clock flag, the overflow flag and the
dropped-byte count. Today's byte order rules hold per port: whole messages,
running status in the drain, real-time bytes at message boundaries ahead of
notes, Stop cancels a pending Clock. One TX lock for all ports, as today. The
step buffer is split by port and flushed as one batch per port.

| Today (`include/sequencer.h`) | After |
|---|---|
| `midi_send_bytes(data, length)` | `midi_send_bytes(port, data, length)` |
| `midi_try_send_bytes_capped(data, length, max)` | gains `port` first |
| `midi_send_note_off(ch, note)` | gains `port` (sequencer, MIDI Song toggles) |
| `midi_send_note_on`, `midi_send_cc` | no caller in `src/` (tests use the first): gain `port` |
| `midi_buf_note_on`, `midi_buf_note_off` | gain `port` |
| `midi_buf_clear()`, `midi_buf_flush()` | all ports; the flush is one send per port |
| `midi_send_clock/start/stop/continue()` | unchanged: every present port |
| `midi_send_all_notes_off()` | unchanged: every present port |
| `midi_clear_output_queue()`, `midi_wait_output_idle()` | every present port |
| `midi_output_overflowed()` | true when any port overflowed |
| new | one call that reads every port's fill and reserves the guard's room; the flush releases it |
| new | `midi_ports_present()`; a setter in test and simulator builds |
| new | `MIDI_MANDATORY_MAX`; `THRU_*_QUEUE_MAX` and the flam cap asserted against it |
| `sequencer_state_t` | gains `uint8_t track_port[NUM_TRACKS]` |
| `sequencer_jitter_stats_t` | gains `uint32_t note_drops[4]` |
| `track_param_t`, `kit_param_t` | gain `TRACK_PARAM_PORT`, `KIT_PARAM_PORT` after the channel |
| `display_draw_track_config()`, `_kit_config()` | gain the stored port and the output used |
| new | `void seq_set_track_port(uint8_t track, uint8_t port)` |

Under `MOCK_HARDWARE` (`test_sync`, `test_sync_gain2`) `midi_out.c` has no
queue and writes every byte straight to the fake UART; the port is ignored
there. The per-port tests run in the builds with the real queue.

### Port count

The logical count is always 4: the stored value 0-3, validation, the UI and
`note_drops[4]`. The physical count `MIDI_PORTS` is 4 when the board header
defines the three pins and 1 otherwise, modelled on `REBIRTH_CV_ENABLE_PIN`:
`hardware/rev-b/boards/rebirth_revb_rp2354b.h` and `_rp2350b.h` gain
`REBIRTH_MIDI_OUT_B_PIN 23`, `_C_PIN 24`, `_D_PIN 29`; the Pico builds use the
SDK's `pico.h` and `pico2.h`, which define none. Host tests and the simulator
build with 4 and can set the present ports to 1-4 at run time (only there;
accepted while stopped **(call)**, then every prepared row is refreshed).

A logical port is resolved to a physical one in one place, `port < present ?
port : 0`, where the project value is read: when a row is prepared and when a
key is passed through. Everything below works with physical ports.

### Sequencer (`sequencer.c`, `song.c`)

- `sequencer_state_t` gains `uint8_t track_port[NUM_TRACKS]`, under the shared
  lock like `kit_config`. `sequencer_init()` sets the defaults by page beside
  `kit_config_defaults()`; `load_stopped()` sets them in the v7 and eight-track
  branches (not `pattern_init()`: the port is not a location's).
- `snapshot_row_locked()` reads the port and `prepare_row()` stores the
  physical port once per row, in the padding after `kind` and `length` (the
  union starts at offset 4), so the 116-byte row does not grow; a port per event
  would add 16 bytes to each of the 320 rows (`prepared_patterns`, 256;
  `prepared_playback`, 64): 5,120 bytes. Rows of T9-64 are prepared inside the
  timer ISR by `load_prepared_track()` through the same snapshot.
- `active_note_state_t` gains the port (64 bytes); `kit_runtime_t` gains it in
  its padding (19 bytes padded to 20). Kit releases and `kit_release_all()` are
  built per port.
- `seq_set_track_port(track, port)` in `song.c` stores the port under the shared
  lock and refreshes that track's 32 prepared rows, like `seq_set_kit_config()`.
  `jitter_reset_now()` clears `note_drops`.
- ISR cost per step: one queue-fill read per present port under the TX lock
  (one lock section for all four), then per track a compare and an add, per kit
  voice an add. To be measured on GPIO 7 with 64 tracks.

RAM: the RP2040 release build of f8a8737 (Release, SDK 2.2.0,
`arm-none-eabi-size -A`) has `.data` 15,084 + `.bss` 226,152 = 241,236 bytes;
the heap ends at 243,480 of the 262,144-byte RAM region. With `MIDI_PORTS` 1 the
RP2040 gains about 150 bytes of state plus code. Rev B (RP2350, 520 KiB) also
carries three more 1,024-byte queues and the split step buffer.

### No overflow by construction

`MIDI_MANDATORY_MAX` = 3 x (NUM_TRACKS + MAX_KIT_TRACKS x KIT_VOICES) = 258
bytes: one release for everything that can sound on one port (one note per
track, one per kit voice). Every optional send must leave that much room,
statically asserted against 1,023 - 258 = 765:

| Optional send | Cap today | Cap after |
|---|---|---|
| Admitted step Note Ons | none (the overflow stops) | `G = min(D + 32, 765)` |
| Flam second hits (`KIT_FLAM_MAX_QUEUED`) | 319 | 319 |
| Pass-through Note On (`THRU_ON_QUEUE_MAX`) | 511 | 511 |
| Pass-through Note Off (`THRU_OFF_QUEUE_MAX`) | 767 | 765 |

Optional bytes are queued only while the port's fill stays at or under 765.
Every other send of Pattern and Song playback is a release of something
sounding: `kit_release()`, `kit_release_all()`, `send_note_off_if_active()`, the
Note Offs of the step batch (slides included) and of `stop_playback_outputs()`.
At most 258 bytes of releases can be owed at any moment, so the fill stays at or
under 1,023. A pass-through Note Off is not owed: refused, it is retried every
pass. Panic clears the queue before its releases and the 96 bytes of All Notes
Off. MIDI Song events are the one other mandatory send, and that mode keeps the
stop rule. The three overflow checks (timer ISR while following, timer ISR on
the internal clock, doorbell ISR) stay.

The argument covers the TX queue. The flag has two other causes in `midi_out.c`:
the 4-entry real-time queue, and the step buffer, whose share for each port
must hold the full `MIDI_STEP_BUFFER_BYTES` (450), as every track may use one
port.

**The reservation (call).** The guard reads a port's fill when phase 1 starts,
and the batch is queued later, at the flush. Phase 1's own direct sends in
between (kit leftovers in `play_kit_step()`, a kind change in
`load_prepared_track()`) are releases and are counted, but core 0's
pass-through could queue on the same port in between. So the read reserves the
room: in the one TX lock section that reads the fills, each port's `reserved`
is set to `max(0, G - fill)`; phase 1 admits Note On bytes only up to that
reservation; every capped send (pass-through, flam second hits) is accepted
only if `fill + reserved + length` stays within its cap; the flush, in its own
lock section, clears the reservation and queues the batch. A pass-through key
that arrives in that window may be refused as on a full queue today: a Note On
is dropped, a Note Off is retried on the next pass. Between steps the
reservation is zero, so flams and keys see the real fill.

### Storage (`flash_storage.c`)

- **Format v8** = the three v7 payloads unchanged, then a fourth,
  `uint8_t track_port[NUM_TRACKS]` (64 bytes, one page), covered by the CRC. Same
  slot at `0xB0000`: header 256 + patterns 131,584 + song 16,896 + kits 1,024 +
  ports 256 = 150,016 of 196,608 bytes (v7 uses 149,760). `pattern_t`,
  `track_config_t` and every legacy record stay byte-identical.
- **Loader order.** A valid v8 in the slot; else a valid v7 in the same slot
  (ports by page, reported as a normal load, `LOADED OK`); else the eight-track
  slot, v4-v6, as today (ports by page, so A for T1-8; `OLD SAVE LOADED`).
  Validation rejects a port above 3; ports B-D are valid on a one-output board.
- **Downgrade.** Firmware that knows only v7 does not read a v8 slot (its
  loader requires version 7): it falls back to the eight-track save or reports
  a failed load, as every earlier format step did.
- **Frozen records first.** `pattern_v5_t` and `pattern_v6_t` embed the live
  `track_config_t` and `step_t`, and their size asserts (459, 972, `old_song_t`
  2,316) run only under `__arm__`. Before the work starts the on-flash records
  of v7 and older get their own typedefs (enums as `uint8_t`) with size and
  offset asserts under the device ABI: v7 `pattern_t` is 4,108 bytes and
  `song_t` 16,652 with `-fshort-enums` (4,684 and 16,660 with 4-byte enums).
  Loading is tested with images written by the old writers (v5, v6, v7) built
  with `-fshort-enums`; today's `write_legacy_image()` serializes live types.
- **Idle wait.** `midi_wait_output_idle()` waits for every present port: queue,
  real-time queue and Clock flag empty, then the transmitter idle (PL011:
  `uart_tx_wait_blocking`; PIO: TX FIFO empty, TXSTALL cleared and read set
  again, then one more byte time for the stop bit; one bit time would do). Its
  three call sites: in `save_stopped()` before `multicore_lockout_start_blocking()`
  and again after it, before `save_and_disable_interrupts()`; in `load_stopped()`
  before RAM is changed.

### UI and display (`ui.c`, `display.c`)

- `track_param_t` gains `TRACK_PARAM_PORT` after `TRACK_PARAM_CHANNEL`: SYNTH
  rows grow from five to six, DRUM from six to seven. `display_draw_track_config()`
  scrolls by the cursor instead of today's `first = param_cursor ==
  TRACK_PARAM_DRUM_CV ? 1 : 0`, with `^` and `v` as now. Row text
  `" Port:   %c"`, or `" Port:   %c (A)"` when folded.
- `kit_param_t` gains `KIT_PARAM_PORT` after `KIT_PARAM_CHANNEL` (19 rows). The
  kit list prints `" %-9s%s"` with upper-case labels, so its row reads
  `" PORT     B"` or `" PORT     B (A)"`, not `Port:`.
- `handle_track_config()` edits the row with VALUE, one port per detent, through
  `seq_set_track_port()`; the port is not in `track_config_t`, so
  `edit_track_param()` and `edit_kit_param()` do not carry it. The row is
  editable on a one-output board too **(call)**: a project can be prepared there
  for Rev B, and the `(A)` shows where it plays meanwhile.

### Documents to update with the implementation

`ARCHITECTURE.md` ("Timing and MIDI", including its stale `MIDI_BUF_SIZE` 160
and 102-byte figures: the step buffer is `MIDI_STEP_BUFFER_BYTES`, 450;
"Storage"; "Hardware Interfaces"), `HARDWARE_STATUS.md`, `README.md`,
`sim/README.md`, and the firmware status sentence of `hardware/rev-b/README.md`.

## Constraints

- Output A's path and the Pico builds stay as they are. Lock order stays
  playback, then shared state or MIDI.
- Nothing optional may push a port's fill above 765 (asserted). `pattern_t`
  and every legacy record keep their bytes.

## Approaches considered

**Routing.** Fixed by page: no setting, but a track can never reach an
instrument on another socket. A per-page table: four settings, but a track
could not leave its page. Per track (chosen): one value per track, defaults by
page.

**Hardware.** UART0 for A plus PIO for B-D (chosen); the alternatives are under
"Hardware resource per output".

**Overload.** Stop as today: the set ends on stage. Mute the output for a bar:
an audible hole in every track of it. Skip Note Ons (chosen): the higher tracks
lose notes, everything else plays.

**Admission mechanism.** (a) A fixed queue-fill limit: simple, but an overloaded
port then runs up to about 190 ms behind the others. (b) Queue fill against one
step of wire, no cost model: under overload with short gates whole steps
alternate between full and nearly empty for the higher tracks. (c) A byte budget
per step with the queue guard (chosen): under overload the first N tracks play
every step, and the budget is exactly the wire's capacity.

**Where the port is stored.** In `track_config_t`: grows `pattern_t` by 64 x 32
bytes and changes every legacy record. A per-track block like the kit settings
(chosen): one page, nothing else moves.

## Simulator

- The core is built with four outputs. `hal.c` models B-D each as its own wire
  beside the UART: 320 us a byte, writable one byte time before the line is
  free, one byte waiting; their interrupt is due like `hal_uart_irq_due()`.
- `SIM_EV_MIDI` carries `port << 8 | byte` rather than a new field: the 16-byte
  `sim_event_t` that `device.js` reads at a fixed stride keeps its shape, and an
  older core reads as port A. `Device.drain()` keeps one running-status parser
  per port (today one `status` and `data`) and adds `port` to each MIDI message;
  `sim/test/smoke.c` masks the byte.
- The page gets a board switch "Pico (1 output) / Rev B (4 outputs)", which sets
  the present ports, and a byte indicator per output. The rig's instruments
  take channel messages from all four outputs merged and real-time bytes from A
  only **(call)**: the page's tempo estimate (`app.js`, 0xF8) and its Stop
  handling (0xFC), `device_check.js`, `clock_check.js` and `artist_check.js`
  count Clock, Start and Stop, which every output now carries.
- `sim_dbg_track_config` gains the stored port at bit 28 (bits 28-31 are free);
  `sim_dbg_note_drops(port)` reads the counters. `sim/build.sh` gets feature bit
  32, "four MIDI outputs", detected from `seq_set_track_port` in
  `include/sequencer.h`, as bit 8 is from `transport_toggle_mutes`.
- `sim/test/ports_check.js`, run by `sim/check.sh` when the core reports bit 32:
  four tracks on four ports set from the panel, each port's bytes carrying only
  its track; the fold with one port (every byte on A, the row showing `(A)`);
  an overloaded port (40 tracks on B at 300 BPM) skipping its higher tracks with
  its counter rising while A, C and D play every note and Clock, the transport
  playing on, and no note left on after Stop; Clock, Start and Stop counted per
  port. The existing checks run on the four-output board, reading real-time
  bytes from A.

## Tests

Host tests, the production code with the fakes; the fake UART is output A, three
fake byte pipes in the same wire-time model are B-D, and the fake IRQ table
gains the PIO interrupt.

- **Queue** (`test_playback`): per-port byte order, running status and Clock
  priority; a blocked port does not delay the others; Clock on all four within
  four byte times.
- **Routing:** Note On / Note Off pairs on one port when the setting changes
  mid-note, ties, slides and kit voices included; Stop and Panic on every port;
  the fold to A with one present port, stored ports unchanged.
- **Density** (`test_playback`, `test_clock`): per tempo of the table, no skip
  at the table value, skips above it, lower tracks first, kit voice order; D
  from the played interval and from the Clock estimate.
- **Overload:** no stuck note and no overflow at any load, including 64 tracks
  plus two flamming kits on one port at 300 BPM over 64 bars; drop counters;
  pass-through keys hammered on the overloaded port between the fill read and
  the flush never overflow it (the reservation).
- **Storage** (`test_storage`): v8 round trip; v4-v7 images written by the old
  writers; a v8 save cut short; a port above 3; the idle wait on four ports.
- **UI** (`test_ui`): the Port rows, scrolling, the folded text; pass-through
  port and channel per note.
- **Build:** `firmware-release`, `pico2-release`, `revb-rp2354b`,
  `revb-rp2350b`, with RAM read from each map file. The sanitizer job runs the
  host tests with and without `-fshort-enums`, as today.

## Not verifiable without a board

Nothing here may be reported as checked:

- The PIO program on silicon: waveform, baud, stop bit, side-set while stalled,
  the `rel` IRQ flag numbering, TXSTALL as the idle test.
- The line from reset until the transmitter starts. RP2350 pads reset with the
  isolation latch set and the internal pull-down on (`0x116`), against the 10 k
  pull-up: a receiver may see a spurious byte.
- Core 0 interrupt load with four outputs (up to 12,500 interrupts a second
  against 3,125 today) and its effect on the UI loop, panel scan and encoders.
- Clock latency and skew between outputs.
- Timer ISR time with 64 tracks and the budget bookkeeping (GPIO 7).
- The four current loops electrically (CA-033 at 3.3 V, supply current).
- How real instruments react to skipped notes and running status.

Unverified even on paper (not in the local SDK 2.2.0): how a `rel` IRQ index is
computed (expected: flag plus state machine number, modulo 4); whether the
side-set of `pull side 1` applies while the state machine stalls on it; whether
TXSTALL is set again on every stalled cycle after it is cleared; the value of
the internal pull-down.

## Left to the builder

- Exact names; one tagged step buffer or four.
- Whether port A's Note On charge may use measured running-status savings, and
  whether the guard margin of 32 bytes needs tuning against the tests.
- The golden-image format for the old-writer tests.
- Where exactly the `DROP` line fits on y = 8, and its text for large counts.
- Whether the Port row wraps from D to A or stops at the ends.
- A pass-through retrigger whose old note is on another port: today the Note Off
  and the Note On are one capped batch; across ports they are two sends.

## Deferred (decisions for later, with the owner)

- Ports for MIDI Song tracks (song image and exporter).
- A per-output Clock and transport switch.
- Per-instrument port selection in the simulator's rig.
