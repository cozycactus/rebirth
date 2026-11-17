# Following an External MIDI Clock — Design

Date: 2026-11-17. Status: the panel place (step 14 in Pattern Select), the
power-up source (INT) and the AUTO behaviour on a lost clock (play on, lock
again when the clock returns) were chosen by the project owner in
conversation. The timing architecture was put to an independent poll of
Codex, Grok and DeepSeek (Gemini could not run); where this design departs
from their advice it says so.

## Intent

ReBirth is always the clock master today. Playing with a DJ, a drum machine
or a DAW needs it to follow a 24 PPQN MIDI Clock arriving on its DIN input,
with Start, Stop, Continue and Song Position Pointer, and to stay safe when
that clock jitters or disappears.

## Goals

1. A clock source setting INT / EXT / AUTO, not saved, INT at power-up.
2. In EXT the 96 PPQN engine runs from the received Clock: four internal
   pulses per Clock, the first of them when the Clock arrives.
3. Start, Stop, Continue from the wire; Song Position Pointer while stopped,
   in Pattern and in Song mode.
4. The measured tempo on the OLED (`EXT 145.0`), steady to 0.1 BPM from a
   steady master.
5. A lost clock never leaves a note or gate on. EXT stops; AUTO plays on at
   the measured tempo and locks to the clock again when it returns.
6. No change to the save format and none to INT playback.

## Non-goals

- Smoothing the master's jitter out of the step timing (a PLL). See "Timing".
- Forwarding SPP, or a raw MIDI thru of the clock bytes.
- Honouring SPP while playing (a DAW loop that relocates while running):
  ignored; the master's next Stop / SPP / Continue relocates.
- Detecting a byte lost in the UART's receive register (PL011 overrun). With
  the FIFO off that needs core 0's interrupts off for more than one byte time;
  a lost Clock shifts the phase by a quarter of a beat's sixth until the next
  Start. A bench item.
- Saving the setting.

## Panel

- **Pattern Select, step 14** cycles CLOCK INT -> EXT -> AUTO. The OLED shows
  `CLOCK EXT` for a second and `CLK EXT` beside the queue line; step 14's LED
  is lit when the source is not INT. Pattern Select already holds the global
  functions (SAVE, LOAD, the jitter reset).
- **Tempo line.** INT shows the tempo as today. With EXT or AUTO the tempo
  carries a prefix: `EXT 145.0` while the external clock is followed or is
  present, `EXT  --.-` in EXT with no clock, `INT 120.0` when AUTO runs on its
  own timer. The prefix blinks while a start waits for the first Clock. In the
  Session view the line is `EXT 145.0 1 BAR` (the word BPM gives way to the
  prefix, so `TRACK LOOP` still fits in 21 columns).
- **VALUE turns** set the tempo only while the internal timer is the source:
  INT, or AUTO with no clock present. Otherwise a turn shows `TEMPO EXT`.
- **PLAY** in EXT, and in AUTO with a clock present: stopped, it arms a start
  on the next Clock; armed, it disarms; playing, it stops. In AUTO with no
  clock it starts the internal timer as today.
- **Pattern Select footer.** With EXT or AUTO the jitter line shows the
  external clock instead: `X:<max deviation us> L:<losses> C:<catch-ups>`.

## Receiving (core 0, UART interrupt)

- The UART interrupt handler reads every byte as today. It reads
  `timer_hw->timerawl` for each byte and hands it to `clock_in_rx_byte()`
  (`src/clock_in.c`, in RAM), which recognises Clock, Start, Continue, Stop
  and SPP (`F2 lsb msb`, real-time bytes may come between its bytes; any other
  status byte abandons it) and pushes `{time, kind, value}` into a 32-entry
  single-producer ring for core 1. Clock, Start, Continue and Stop no longer
  go into the 64-byte note ring (the note parser ignored them anyway); SPP's
  bytes still do, so the note parser's running status ends as before.
- After a pass that pushed an event, the handler forces `TIMER_IRQ_1` on core 1
  by setting bit 1 of `timer_hw->intf`. Alarm 1 is never armed; core 1 is the
  only core with that IRQ enabled. A full ring drops the event and counts it.
- The handler takes no new lock. The ring publishes with a memory barrier
  between the entry and the head.

## Core 1

Both core 1 vectors, `TIMER_IRQ_0` (alarm 0, the pulse timer) and
`TIMER_IRQ_1` (the doorbell), have the same priority and run one service
routine under the playback lock: clear their request, drain the event ring in
order, then do what is due. When both are pending the alarm runs first, so the
alarm path drains the ring too. The doorbell clears its request before it
drains, so an event pushed during the drain rings again rather than being
missed.

### Following state

Kept by core 1 under the playback lock: `following` (the pulses come from the
Clock), `armed` (none, START, CONTINUE: waiting for the first Clock), `owed`
(pulses due for Clocks received), the time the next owed pulse is due, and the
estimator. Every path that sets `TRANSPORT_STOPPED` (Stop, Panic, the overflow
panic, a song that ends, a play-mode change, a lost clock) goes through one
helper that also clears `following`, `armed` and `owed`, so the master's next
Clock never restarts a song that ended.

### Timing

Each Clock is a hard reference: **its first pulse (4n) runs when the Clock is
handled**, the other three follow at a quarter of the estimated Clock interval.
A step is 24 pulses, six Clocks, so every step boundary, MIDI Clock out and
launch decision falls on a received Clock and carries the master's timing plus
the handling latency (tens of microseconds). The sub-pulses (note-offs, flams,
kit releases, gates) are spaced by the estimate. No pulse runs ahead of the
last Clock received. This is option (a) of the poll, chosen by Grok and
DeepSeek; Codex preferred a bounded PLL, which smooths the master's jitter but
can play a step before its Clock and lags a tempo change.

- **A Clock that comes early** (the common jitter case) finds at most three
  sub-pulses of the previous Clock not yet run. They run at once, then the new
  4n. None of them can be a step boundary.
- **A burst** (a Clock less than half an interval after the last one, or a
  Clock while still behind) adds four owed pulses, which the timer then runs
  four times as fast as normal (never closer than 250 us) until at most three
  are left. A stall of the master's USB link that ends in a bunch of Clocks
  becomes a short faster passage, not several steps at once. All three
  reviewers advised dropping the extra Clocks instead; that keeps the notes
  tidy but leaves the device permanently behind the master by the dropped
  Clocks, so this design catches up and caps the backlog instead: owed pulses
  beyond 48 (two steps) are dropped and counted. The invariant tested is
  "pulses = 4 x Clocks received since the start, one Clock out per Clock in",
  below the cap.
- **Estimate.** The interval used for the sub-pulses is the mean of the last
  four intervals. An interval longer than the loss timeout restarts the
  estimator (the gap after a flash save, a cable plugged in). Until two Clocks
  have been seen the internal tempo is the guess.

### Lost clock

The clock is lost when no Clock arrives for `max(3 x interval, 50 ms)`
(375 ms before the interval is known), checked by alarm 0 at that deadline:

- **EXT:** stop as on a Stop from the wire: notes and gates released, kit
  voices released, MIDI Stop out; the OLED shows `EXT  --.-`.
- **AUTO:** the transport plays on from where it is on the internal timer, at
  the measured tempo (it becomes the INT tempo, which VALUE can then change).
  When three intervals in a row are plausible again (within half to twice the
  estimate before them), the device follows the Clocks from its current position: the next Clock's 4n is the next
  multiple of four pulses (the sub-pulses up to it run at once). The tempo
  locks again; whatever phase the gap cost stays until the next Start.

### Start, Stop, Continue, SPP

Applied by core 1 from the ring, with internal helpers that run under the
playback lock it already holds (never `transport_start()`, which takes it).

- **Clock** before any Start only feeds the estimator.
- **Start** (EXT, or AUTO): resets the position as `transport_start()` does
  (releasing what plays, if playing), arms START and follows. The first Clock
  after it is pulse 0: MIDI Start goes out, then the first pulse runs (step 1
  and MIDI Clock). In Song mode with no playable entry it does nothing.
- **Continue** (stopped): arms CONTINUE at the current position rounded up to
  a multiple of four pulses, so the first Clock is a Clock boundary. MIDI
  Continue goes out on that Clock.
- **Stop:** stops (as above) and disarms; the position is kept for Continue.
- **SPP** (stopped, EXT or AUTO): sets the position to `24 x value` pulses. In
  Pattern mode every track's next step is `value % its length`. In Song mode
  the song is walked bar by bar from its first entry as playback would run it
  (repeats, skipped entries, the end mode): the entry, its remaining repeats,
  the step in the bar, and for each track the steps since its location last
  changed, modulo its length. A position past the end of a song that stops
  leaves it stopped. An SPP while armed for START arms CONTINUE instead.
- In INT the wire's transport bytes are ignored.

### Launch quantisation, Song mode, MIDI out

Nothing changes in the launch rules or Song bar changes: they read
`pulse_count` at step boundaries, which the Clock now drives. MIDI Clock out
stays where it is (every fourth pulse), so downstream gear sees one Clock per
Clock received, re-timed by ReBirth, and only while it plays; Start, Continue
and Stop go out when ReBirth's own transport changes.

### Changing the source while playing

- To INT while following: the internal timer takes over at the measured tempo.
- To EXT or AUTO while on the internal timer: follows once three plausible
  intervals have been seen, as AUTO does after a loss. To EXT with no Clock
  arriving: stops at once (EXT never runs on the internal timer).

### Core 0 and storage

- `transport_start()` in EXT (or AUTO with a clock present) resets and loads
  the rows as today, then arms START instead of setting PLAYING.
- Stopped no longer means core 1 is idle: a Start and a Clock from the wire
  could start it. One inhibit flag, set under the playback lock, covers every
  core 0 section that relies on that: `transport_start()` between its two lock
  sections (it loads the prepared rows with the lock released), and flash
  save and load (`sequencer_storage_begin()` / `_end()`, which refuse unless
  stopped and not armed). While it is set core 1 ignores Start, Continue and
  SPP and starts nothing on a Clock. Armed counts as busy for `STOP TO SAVE`.

### Measuring the shown tempo

Core 1 keeps the last 128 Clock times. The shown interval is the time over the
last 96 intervals (one bar) divided by their number, then averaged over Clocks
with weight 1/16, in microseconds times 16. When the short estimate differs
from it by more than 4 % for three Clocks in a row, the window restarts from
the last two intervals, so a tempo change shows within about a beat. Core 0
turns it into BPM and keeps the shown tenth unless the value moves by more
than 0.07 BPM from it.

## Statistics

`sequencer_jitter_stats_t` gains `ext_clocks`, `ext_max_dev_us` (largest
|interval - estimate|), `ext_losses`, `ext_catchups` and `ext_dropped`. While
following, alarm 0's irregular deadlines are not recorded as ISR latency or
period error.

## Simulator

- `hal.c` gets `TIMER_IRQ_1` (the fakes' IRQ tables become three entries,
  indexed by number) and `intf`; a forced IRQ is dispatched in
  `sim_run_until`, `core0_wait_until` (which also applies inputs as they fall
  due) and during a flash stall.
- `sim_dbg_clock_source`, `sim_dbg_ext_tenths` (the shown tempo) and
  `sim_dbg_clock_state` let the check read the state.
- `sim/test/clock_check.js`, run by `sim/check.sh` when the core reports
  feature 8 (external clock): sets EXT from the panel, sends 145 BPM, checks
  the shown tempo, Start, one Clock out per Clock in, steps on the Clocks,
  Stop, a lost clock (stopped, gates low, Stop out), AUTO playing on through a
  loss and locking again, and Continue after an SPP.

## Tests

Host tests (`test/test_clock.c`, the production sequencer with the fakes):
the estimator, the SPP parser with interleaved real-time bytes, Start then a
Clock 320 us later (one step, not two), pulses per Clock at 20, 145 and
300 BPM, early Clocks and bursts (no step lost or doubled, the backlog cap),
a two-kit flam burst without a TX overflow, Stop and Continue between
sub-pulses, SPP in Pattern and Song mode (repeats, skipped entries, unchanged
tracks, the end), loss in EXT and AUTO with ties and gates, the relock, the
inhibit around save and load, a song end not restarted by Clocks, and INT
output unchanged.
