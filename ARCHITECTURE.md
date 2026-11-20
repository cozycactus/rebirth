# RP2040 Pattern Sequencer Architecture

## Execution and Locking

| Context | Work |
| --- | --- |
| Core 1 timer IRQ | 96 PPQN timing, playheads, note durations, MIDI production, CV/gate output; while following an external clock, the pulses still owed for a Clock and the loss deadline |
| Core 1 doorbell IRQ | `TIMER_IRQ_1`, forced by Core 0's UART interrupt: the external clock's events, in order (see External clock) |
| Core 0 main loop | Panel I2C polling (key matrix scan on two MCP23017, one bus step per pass; MCP23017 step buttons and LEDs), PLAY and encoders, edits, transport commands and launches; per ~33 ms frame: one transport snapshot, the RGB frame build and its LED DMA start, the OLED; stopped-only storage |
| Core 0 UART IRQ | First read every received byte, stamped with the time: Clock, Start, Continue, Stop and SPP into the external clock's ring for Core 1 (then force `TIMER_IRQ_1`), every byte but the first four into the input ring; then drain pending MIDI bytes into UART0, one byte per interrupt (hardware FIFO off) |

The playback lock serializes the complete timer handler with Start, Stop,
Continue, mode changes, selection, MUTE changes and Panic. Stop/Panic return after any
in-progress step has finished and its outputs have been cleared. Bytes already
in the UART hardware still need time to reach the receiving instrument.

Lock order is **playback -> shared state or MIDI**. UI edits take the shared lock,
release it, then refresh playback data. Transport commands and storage operations
are Core 0 APIs. The timer uses internal stop helpers under the playback lock.

The live mutes `track_mutes` are written under the shared lock: by Core 0, and by
the timer when it applies a due MUTE change; the timer reads the byte once per
pulse without it. The two masks of waiting MUTE changes, `mute_pending` and
`mute_target`, are cleared by `sequencer_init()` before the timer runs and from
then on written only under the playback lock: by Core 0, or by the timer, which
holds it throughout.

What can delay the timer: Core 0 sections that hold the playback lock, i.e.
transport commands, pad/scene launches, MUTE changes, Panic and the per-frame transport
snapshot (one short copy per frame, never one per UI pass); shared-lock
sections the timer waits on; and bus or flash contention from Core 0, such as
the LED DMA and Core 0 code running from flash through the XIP cache. The
snapshot, the only one of these that runs about 30 times a second, runs from
RAM together with the lock helpers it calls (`seq_lock`, `seq_unlock`, the
playback lock helpers), so its hold is a fixed short copy that never waits on
an XIP cache miss; the user-triggered sections run from flash. Panel I2C
polling, the LED frame build, display SPI, pattern conversion and flash
writes do not run under the playback lock. A dual-core split alone does not
establish physical jitter.

## Prepared Patterns

All 32 locations have a prepared row for each of T1–T8. Core 0 builds
these at initialization and after edits. A revision and row are published
together under the shared lock. T9–T64 store editable steps for all 32
locations and one decoded playback row per track. At a change of location or
revision, playback snapshots that track under the shared lock and decodes the
snapshot after releasing it. Song mode uses the same rows.

A row is tagged with its kind: the 16 events of a SYNTH or DRUM cell, or a
kit row (below), in one union. The event cache occupies 29,696 bytes (256 rows
of 116 bytes), plus revision counters and active rows.
Pattern and song data remain the editable source of truth. Code editing a
pattern must call `sequencer_refresh_track_playback()` after releasing the shared
lock, including for locations not currently playing.

## Kit Tracks

A kit track plays 12 lanes per step (AC, BD, SD, LT, MT, HT, RS, CP, CH, OH,
CC, RC) over MIDI only. Voice `v` is lane `v + 1`; every per-voice array is in
lane order.

- **Data.** `pattern_t.kit_steps[track][step]` is one `uint32_t` per step, lane
  `i` in bits `2i..2i+1` (0 off, 1 note, 2 accent, 3 flam; AC is 0 or 1; bits
  24-31 zero). It is used only where that location's `tracks[c].type` is
  `TRACK_TYPE_KIT`; the cell's `step_t` data is untouched, so a type change
  erases nothing. Kit settings (`kit_config_t`: 11 notes, `vel_normal`,
  `vel_accent`, `vel_max`, `flam_pulses`) belong to the track, not the location,
  in `g_seq.kit_config[]`; channel, gate length and stored mute come from
  `track_config_t`. `seq_set_kit_config()` stores the settings (clamped) and
  refreshes that track's 32 prepared rows, like `seq_set_track_config()`.
- **Prepared kit row.** `lanes[16]` (the cell's step words, zeroed at and beyond
  the length and when the stored mute is set), the 11 notes, the three
  velocities, channel, gate pulses and flam pulses. It is self-contained, so the
  revision and refresh path is the one the event rows use. A kit cell has a clip
  when any voice lane (not AC) is non-zero within the length; a stored mute does
  not hide it.
- **Two-kit limit.** A track counts as a kit when its type is KIT in any
  location; at most two tracks may be (`MAX_KIT_TRACKS`), so at most two kits
  play at once, also in Song mode and with mixed launches. `seq_set_track_config()`
  refuses a third kit before it changes anything and the save validator enforces
  the same rule.
- **Pulse order.** Each pulse: 1 MIDI Clock; 1a on a step boundary, the due MUTE
  changes (Playback Rules), so the live mutes of that pulse include them; 2 live
  mutes (a muted kit releases its sounding voices and cancels a pending flam);
  3 note-off countdowns; 4 flam countdowns (the second hits); 5 on a step
  boundary, the step. Under `TRACK LOOP` with a change waiting, the track's row
  is loaded in 1a, so a kind-change release (and the flam it cancels) can come
  before that pulse's note-off and flam countdowns. Kit
  messages of 2-4 go out through their own send call, never through `midi_buf`,
  which step 5 clears. With the gate at 100 % the countdown ends in step 3 of the
  pulse that begins the next step, before its Note Ons.
- **A kit step.** The voices go in the fixed order BD, SD, CP, CH, OH, RS, LT,
  MT, HT, CC, RC. A note is `AC ? vel_accent : vel_normal`, an accent
  `AC ? vel_max : vel_accent`, a flam plays its first hit at the note level and
  its second hit one level up `flam_pulses` later as Note On velocity 0 then Note
  On. The note-off countdown is `gate_pulses`, or
  `min(PULSES_PER_STEP, gate_pulses + flam_pulses)` on a step with a flam. Note
  Off of a kit voice is Note On with velocity 0, so a kit's messages share one
  status byte; SYNTH and DRUM tracks still send 0x80. One release channel per
  kit track: every voice is released at the latest at the next step boundary,
  before that step's hits, so all sounding voices come from one step and share
  the channel stored at that step; each keeps the note it was sent on. A
  settings edit therefore cannot strand a note (host builds check the invariant
  before a channel is stored).
- **Kind change.** When a row of another kind is installed (type change, load),
  the ISR first releases what the old kind left sounding. Stop, Panic, Start
  while playing and a play-mode change queue a release of every sounding kit
  voice before the MIDI Stop byte. The Stop, a real-time byte, may overtake
  them on the wire, and so may the Start and Clock of a restart, also real-time
  bytes; every voice is still released before the next run's notes.
- **Flam cap.** A flam's second hits are the only kit message that may be
  dropped. They are queued only if the TX queue then holds at most 319 bytes
  (`KIT_FLAM_MAX_QUEUED`, through `midi_try_send_bytes_capped()`). Otherwise
  the voices sound until their note-off and `flam_drops` goes up, one per kit
  per flam pulse. It shows on the pattern-select screen as `FLAM DROP <n>`
  when non-zero. The cap limits flams only: mandatory messages may fill the
  whole queue (1,023 bytes), so about 700 bytes stay for scheduled playback
  (two kits' note-offs and the worst step burst, 168 bytes as queued in one
  pulse) and for the releases and first hits of an immediate restart. Two kits
  flamming every voice keep every second hit up to 200 BPM.

## Playback Rules

- Each track has its own pattern, queue and 1-16 step playhead.
- Queued pattern changes apply on a launch quantization (default `1 BAR`):
  `BAR` and `BEAT` wait for a global step that is a multiple of 16 or 4, `STEP`
  applies on the next step, `TRACK LOOP` waits for that track's loop end (the
  old rule). With `BAR`, `BEAT` and `STEP` the new clip joins in phase: its
  playhead starts at `(global step % 16) % new length`, so it lines up with the
  bar. `TRACK LOOP` always starts the new clip at step 1. Queuing the location
  that is already playing just clears the queue and keeps the playhead.
  Shortening below the next step wraps to zero; extending preserves position.
  Under `BAR`, `BEAT` and `STEP`, neither shortening nor extending a clip
  applies a pending queue; under `TRACK LOOP`, shortening below the playhead
  counts as the loop end and does apply it. The quantization is not saved.
- A pad or scene press is decided and written in one critical section
  (`transport_launch()`, playback lock then shared state), so the timer never
  applies a queue between the decision and the write.
- A clip copy needs a pad that is really held, and the UI does not track that
  from press and release events alone: the panel layer can forget its keys
  without a release (panic, flash stall), stop seeing them while the scanner
  is lost (an I2C error, a chip that reset) and can report a pad down while
  its press still waits in the event ring. A pad is a
  copy source only when the UI handled its press and not its release, the
  panel layer reports it down now (`panel_input_pad_down()`), the panel
  layer's `panel_input_generation()` is what it was at that press, and 500 ms
  have passed since the UI handled it. The source is the location its pad
  stood for at the press. The copy is one `pattern_copy_track()` under the
  shared lock followed by a playback refresh of the target, and it leaves the
  transport's current and queued locations alone.
- Routing (`track_config_t`: type, channel, drum note, output mode, CV channel,
  gate length) belongs to the track: `seq_set_track_config()` writes it to all
  32 locations and refreshes that track's prepared rows. Older saves whose
  locations differ play as before until that track's config is edited.
- `muted` in `track_config_t` is a stored mute per location (from older saves);
  routing edits leave it as it is in each location. A track is audible when
  neither the live mute nor the stored mute of the location it plays is set;
  MUTE, the RGB lights, the selector's step LEDs 1-8 and the OLED mute lines
  all show a track as muted by this rule (`seq_get_track_mutes()`).
  MUTE on a muted track makes it audible when its change lands: the stored
  mute of every location is cleared as soon as the change is made (a press,
  or a group's release), the live bit when the change lands, and until then
  the live bit keeps the track silent (below).
- A MUTE change gives a track a target live mute: `transport_toggle_mutes()`
  (a press) the opposite of its state by the rule above, `transport_set_mutes()`
  the bit it is given; a track already in that state gets none and only loses a
  waiting change, so nothing waits and its stored mutes stay. A change applies
  at once while the transport is not playing or under `STEP`;
  `transport_toggle_mutes_now()` cancels and toggles at once in every case.
  Otherwise it waits (`mute_pending`, its target in `mute_target`;
  neither is saved) for the next launch point: a global step that is a multiple
  of 16 (`BAR`) or 4 (`BEAT`), or under `TRACK LOOP` that track's loop end by the
  queued pads' rule, decided on the same row the pad uses at that boundary
  (Song mode reads `TRACK LOOP` as the bar, where song entries change). The rule
  is read at each step boundary, so a quantization change moves a waiting change.
  A press of a track with a change waiting cancels it (`transport_cancel_mutes()`
  cancels without a press); a press after the change landed is a new change.
- A change is decided and published in one critical section (playback lock,
  then shared state), so the timer never applies a boundary between the decision
  and the write. The timer applies due changes at the step boundary before the
  live mutes and the song advance: a newly muted track's notes and kit voices are
  released in that pulse and its step does not play; a newly unmuted track plays
  that step.
- Stored mutes: a change to audible of a track muted by a stored mute sets its
  live bit in that section (the track is silent already), and the change clears
  that bit, at once or when it lands. After the section the stored mute of every
  location is cleared and the track's rows are refreshed
  (`seq_clear_stored_mutes()`), so from then on the live bit alone mutes it and a
  cancelled change leaves it live-muted. A launch point that falls inside the
  refresh can play one step of that track silent.
- Stop, Start while playing, a play-mode change, Panic (also the timer's on a
  MIDI overflow) and the end of a song that stops cancel waiting changes, as does
  a Start that finds an empty song; a change due on the song's last boundary
  lands before it stops. `transport_clear_queue()` and
  `transport_clear_track_queue()` are about pads and leave them.
- Selection while stopped is immediate and survives Start.
- Stop cancels pending queues and retains the current playing locations.
- Manual queues are rejected in Song mode. Song entries apply on 16-step bar
  boundaries with repeat counts. Only changed tracks restart their loops.
- Empty/all-skipped songs stay stopped. Trailing skipped rows respect Stop mode.
- The editor can address a different location from playback. Entering the
  pattern view or the step editor from the Session view targets the Session
  focus (track, location); those screens keep that target until they are left.
- Mutes release active notes and gates on the next internal pulse. Unmute takes
  effect at the next active step.
- Gate length also determines MIDI note duration. Slide/tie are **outgoing** step
  flags: slide connects to the next pitch, tie holds the same pitch. They sustain
  until the next step instead of ending at the configured gate length.
  Rests, mutes and destination changes release the previous output.
- Slide provides MIDI legato and a held gate. CV changes by one DAC write;
  there is no programmed analog glide curve.
- Swing is stored but not implemented in playback.

## Timing and MIDI

The internal timer runs at 96 PPQN (from the received Clock when following an
external clock; see External clock). MIDI Clock runs at **24 PPQN**, one message
every four internal pulses. At 120 BPM the integer timer period is 5,208 us; the
nominal MIDI Clock interval is approximately 20.833 ms.
[MIDI Timing Clock reference](https://docs.circuitpython.org/projects/midi/en/latest/api.html#adafruit_midi.timing_clock.TimingClock).

At a step boundary, the engine stages MIDI and CV/gate changes, commits gate
GPIO masks, then submits the MIDI batch. Gate edges are grouped; DAC writes and
DIN messages are sequential. This does not make all outputs simultaneous.

The 1,024-byte UART queue (1,023 bytes usable) accepts complete messages or
complete step batches (`MIDI_BUF_SIZE` is 160 bytes; the worst step burst is 102
bytes as stored, 82 on the wire). It keeps whole messages with their status
bytes, and the drain applies running status as it writes: a status equal to the
last one written is skipped, except on the first message after the queue ran
empty (its last byte written), which always carries its status. Real-time bytes
do not change the running status. The UART hardware FIFO is off and the drain
writes one byte per writable check, so the TX interrupt runs once per byte on
Core 0.

Clock and transport bytes go ahead of note bytes. `midi_send_clock()` sets one
pending-Clock flag (never a counter, so Clocks are not released in a bunch).
Start, Continue and Stop wait in their own small queue. At each message
boundary the drain sends a queued Start/Continue/Stop, then a pending Clock,
then the next note message. A Clock byte may wait for the byte being shifted out
and a complete channel message whose status is already in the holding register:
up to four byte times, 1.28 ms, before interrupt latency, against up to about
10 ms behind a kit burst without this rule. Stop,
Panic and `midi_clear_output_queue()` clear the Clock flag, so no Clock follows
a Stop; `midi_wait_output_idle()` waits for it too.

FIFO checks, dequeues, writes and TX interrupt changes share one lock. Overflow
stops playback and sends All Sound Off / All Notes Off instead of continuing
with a truncated message. Panic discards software-queued output; hardware FIFO
bytes precede the cleanup messages.

### MIDI input

UART0's receive interrupt is on wherever the TX interrupt enable is written. The
handler (Core 0, in RAM) first reads every readable byte into a 64-byte ring,
without the TX lock, and only then drains the TX queue. A byte is read even when
the ring is full: with the FIFO off the interrupt stays asserted until the data
register is read; the byte is counted in `midi_in_dropped()` and lost. The main
loop (`ui_update()`, Core 0) takes at most 48 bytes a pass, before the panic
check, and feeds them to the pure parser in `midi_in.c`. The ring has one
producer and one consumer, both on Core 0, so it has no lock. Clock, Start,
Continue and Stop do not enter this ring: they go to the external clock (below),
the one thing the input adds to Core 1.

A key is passed through to the track being edited with `midi_try_send_bytes_capped()`
only, as the ordinary sends set the overflow flag that stops the transport: a
Note On (with the Note Off of a retrigger) leaves only if the queue then holds
at most half its capacity, a Note Off may fill it and, if refused, stays pending
and is retried every pass. `thru_channel[note]` remembers the channel a note
started on, and its Note Off goes there. The table is released entirely at a
gap, on a keyboard's All Notes Off or All Sound Off, and before a flash save or
load. A gap (the dropped count changed, or a save or load, which switches
interrupts off) empties the ring and resets the parser, so a running status
cannot turn the bytes after a loss into a wrong note. A panic clears the table
(All Notes Off has gone out) and discards the input while it is latched.

### Pads as a keyboard

While a step is held (Pattern view, SYNTH cell, `ui.grid_hold & steps->held`, or
the edited step's button in Step Edit), `update_pad_keys()` turns the pads into a
keyboard. It takes one snapshot per pass, before the panel events (on, track, location,
reference step), that the pad routing and the lights both read, and fixes the window
`ui.pad_keys_base` only when the keyboard turns on. A step button that a key has
written under in the step editor does not toggle its step on a long press. A pad press then goes through
`handle_midi_event()` as a Note On of velocity 100 that leaves the step's velocity
alone, so the write, the pass-through table and the capped sends are the MIDI
keyboard's own; the pad is remembered in `ui.pad_key[track][row]` (note + 1) and is
never recorded as a copy source. The release uses that memory, not whether the
keyboard is still on, so a step let go first cannot hang the note. The panel layer
can lose a release without an event, so at the end of every pass a remembered key
whose pad `panel_input_pad_down()` no longer reports, or whose panel generation changed since its press, is released; a panic and a
flash save or load only forget the keys, their notes having been released. The
layout is `pad_key_note()` in `led_frame.c`, shared by the UI and the frame
builder; `led_state_t.keys` makes `led_frame_build()` light the pads from it.
Nothing runs in the timer interrupt and no lock is added.

### External clock

The clock source (`g_seq.clock_source`: INT, EXT, AUTO; Pattern Select step 15;
not saved, INT at power-up) says where the 96 PPQN pulses come from. The design
and the reasons are in
`docs/superpowers/specs/2026-11-17-external-clock-design.md`.

- **Receiving.** The UART interrupt reads `timer_hw->timerawl` for each byte and
  hands it to `clock_in_rx_byte()` (`clock_in.c`, in RAM). Clock, Start,
  Continue, Stop and SPP (`F2 lsb msb`, real-time bytes may come between its
  bytes) become `{time, kind, value}` events in a 32-entry single-producer ring
  (a barrier orders an entry before the head that publishes it). After a pass
  that pushed one it sets bit 1 of `timer_hw->intf`: alarm 1 is never armed,
  its interrupt is Core 1's doorbell. The UART interrupt takes no new lock.
- **Core 1.** The timer ISR and the doorbell ISR have the same priority and run
  under the playback lock; each drains the ring in order first (the alarm runs
  first when both are pending). The doorbell clears its request before it
  drains. Core 1 keeps, under the playback lock, whether it follows the Clock,
  whether a start is armed (waiting for the first Clock), the pulses owed for
  Clocks received and when the next is due, and the tempo estimate (the last
  128 Clock times). Every path that stops the transport goes through
  `stop_playback_outputs()`, which also lets go of the clock, so a song that
  ended is never restarted by the master's next Clock.
- **Pulses.** A Clock is a hard reference. Its first pulse (4n) runs when it is
  handled; the other three follow at a quarter of the mean of the last four
  intervals (the internal pulse period before two Clocks have been seen). A step
  is 24 pulses, six Clocks, so every step, MIDI Clock out and launch decision
  (a queued pad or a waiting MUTE change) falls on a received Clock and carries the master's timing, jitter included,
  plus the handling latency. No pulse runs ahead of the last Clock. An early
  Clock runs the at most three sub-pulses of the previous one at once (never a
  step boundary). A burst (a Clock less than half the estimate after the last,
  or one that finds the timer still behind) adds four owed pulses that the timer
  runs four times as fast (never closer than 250 us) until at most three are
  left; owed pulses beyond 48 (two steps) are dropped and counted, so a stall
  shifts the phase only when it is that long. While following, alarm 0 fires at
  the next owed pulse, the loss deadline, or one internal pulse period from
  now, whichever comes first, so a return to the internal timer never waits
  longer than today's first pulse. Its writes never fall at or behind the clock.
  Launch quantisation and Song bar changes read `pulse_count` at step
  boundaries as before.
- **Transport from the wire** (EXT, or AUTO; ignored with INT), applied by Core 1
  with helpers that run under the playback lock it holds. Start resets the
  position as `transport_start()` does and arms; the first Clock after it is
  pulse 0, and MIDI Start goes out just before it. Continue arms at the position
  rounded up to a multiple of four pulses; MIDI Continue goes out on the first
  Clock. Stop stops (MIDI Stop out). SPP while stopped sets the position: in
  Pattern mode every track's next step is SPP modulo its length; in Song mode the
  song is walked entry by entry as `advance_song_position()` would run it (one
  short `seq_lock` section per entry passed): the entry, its remaining repeats
  and each track's steps since its location last changed. SPP while playing is
  ignored, and an SPP past the end of a song that stops disarms a start.
- **Core 0.** `transport_start()` in EXT, or AUTO with Clocks arriving, arms
  instead of playing; PLAY while armed disarms (`transport_stop()`). Stopped no
  longer means Core 1 is idle, so one inhibit flag, set under the playback lock,
  covers each Core 0 section that relies on it: `transport_start()` between its
  two lock sections (it loads the rows with the lock released) and flash save
  and load (`sequencer_storage_begin()` / `_end()`, which refuse unless stopped
  and not armed). While it is set Core 1 ignores Start, Continue and SPP and
  starts nothing on a Clock.
- **Loss.** No Clock for `max(3 x interval, 50 ms)`, the interval being the
  longer of the short and the shown estimate (a burst shortens only the short
  one; 375 ms before either is known), checked at alarm 0's deadline and on the
  next Clock: EXT stops as on Stop (notes, gates and kit voices released, MIDI
  Stop out), also when it still plays on the internal timer after a change of
  source and has not yet followed the Clock; AUTO hands over to
  the internal timer at the measured tempo, which becomes `transport.bpm`, and
  follows again from its current position once three plausible intervals in a
  row have arrived (that Clock is the next multiple of four pulses). An interval
  longer than the timeout restarts the estimate (a flash save, a cable), and
  alarm 0 forgets an estimate whose last Clock is older than the timeout, so the
  32-bit difference to it never wraps round into "present" 72 minutes later.
- **Shown tempo.** The time over the last 96 intervals divided by their number,
  then averaged over Clocks with weight 1/16, in us x 16
  (`transport_snapshot_t.ext_shown_x16`); three Clocks in a row more than 4 % off
  restart that window from the last two intervals. Only an interval of a tempo
  (a quarter to four times the shown one) counts as off, so the bunched Clocks
  that end a stall do not restart it. Core 0 turns it into BPM and
  keeps the shown tenth until the value leaves it by more than 0.07 BPM.
- **MIDI Song mode** plays its file on its own absolute timeline (the timer ISR
  follows the song's event deadlines), so the external clock takes no part in
  it: PLAY starts the song as before, the wire's Start, Continue, Stop and SPP
  are ignored, and neither a lost clock nor a change of source stops it. Clocks
  still feed the tempo estimate.
- **Not covered.** A byte lost in the UART receiver (Core 0's interrupts off for
  more than one byte time) is not detected; a lost Clock leaves the device a
  Clock behind until the next Start. SPP is not forwarded.

After an overrun the timer re-arms a future deadline, preventing a past alarm
from stalling until the 32-bit counter wraps. It emits no catch-up burst.
Jitter snapshots are coherent; tempo changes do not count the new period as
timing noise. See [JITTER_MEASUREMENT.md](JITTER_MEASUREMENT.md).

### MUTE hold and groups

`ui.c` remembers one held MUTE key: the physical key (`ui.mute_held`, `MUTE_NONE`
for none; its release and the sweep go by it) and the logical track it stood for
on the page shown at its press (`ui.mute_held_track`), the time the UI handled its
press and the panel generation then, and the group (`ui.mute_group`, a
`track_mask_t` of logical tracks, so it can span pages) with each member's target
(`ui.mute_group_target`, bit set = muted). In the MIDI Song player the MUTE keys
go to the player before any of this, and opening it drops the hold. A MUTE press with no key held is an ordinary press
(`transport_toggle_mutes()`) and becomes the held key. Another key pressed less
than `PAD_COPY_HOLD_MS` (500 ms) after it, or when the panel generation is no
longer the one of its press, is an ordinary press as well. A later one joins: a
member pressed again leaves, a key with a waiting change only has it cancelled,
and any other key joins with the opposite of its state then as its target. While
the group is empty a join first withdraws the held key's waiting change with
`transport_cancel_mutes()`; the held key is in the group, with that change's
target, only when the call returns its bit. The join checks the generation only,
not whether the held key is still reported down: the panel samples every MUTE key
in one column, so a member's press and the held key's release can arrive in one
pass, and the panel already reports the key up while its release waits behind the
press; the pass's events are taken in order. The held key's release sets the
group to its targets in one `transport_set_mutes()` call, if the group is not
empty and the generation is the one of its press. Only that call is conditional:
every release of the held key ends the hold, and then no key is held. A press of the
held key itself means its release was lost: the hold and its group are dropped,
and the press is an ordinary one that starts a new hold. After the pass's events,
a held key that `panel_input_mute_down()` no longer reports, or whose generation
changed (in the pass of its press too), is dropped with its group. The panic
combo (both encoder buttons) and a flash save or load drop it where they drop the
pad holds. Stop does not, so a group let go after Stop changes at once, and
neither does the stop the timer makes itself on a MIDI output overflow: the UI
does not see that panic, so the group is kept and its release changes it at once,
as after Stop. A drop changes nothing in the transport, a withdrawn change
included. The selector's buttons 1-8 take their track out of the group before
`transport_toggle_mutes_now()`, and `led_state_t.mute_group` lights the group
steady white. The hold and the group are UI state on Core 0, kept between
transport calls that take the locks themselves: the hold adds no lock and nothing
to the timer interrupt. The MUTE calls do lock: each holds the playback lock, and
the shared lock inside it, for one short section, and the timer takes the shared
lock at a step boundary where a change waits (Playback Rules).

## Storage

The 2 MiB Pico flash reserves 192 KiB at offset `0xB0000` for project format
7, 1 MiB at `0xE0000` for the timed MIDI Song and 128 KiB at `0x1E0000` for
legacy eight-track projects. The linker keeps the firmware below these slots.
Saving requires stopped transport. After MIDI drains and the other core parks,
flash is programmed directly from SRAM. The three payloads (patterns, song,
kit settings) are page-aligned. CRC32 covers their unpadded bytes, and the
validity header is programmed last.

When the format-7 slot holds no valid project, the loader checks the old slot
for formats 4, 5 and 6. It restores the original eight tracks, initializes the
other 56 and expands each song entry. Version 6 also restores kit steps and
settings. Loading validates the whole project before changing RAM.

A format-7 slot whose save was cut short looks exactly like one that was never
written, so the loader does not tell the two apart: with no valid format-7
project it always falls back on the old slot, and the load reports which
project it gave (`LOADED OK` or `OLD SAVE LOADED`). A save verifies the
format-7 slot alone.

This is one save region. Power loss during erase/program may lose the previous
save; an incomplete new save is rejected. Preserving the old save across power
loss needs a future two-slot/journal design.

## Hardware Interfaces

| Function | GPIO / Interface |
| --- | --- |
| DIN MIDI TX and RX | 0, 1; UART0, 31,250 baud, 8N1; RX is J402, an H11L1 opto |
| SSD1309 OLED | 2 SCK, 3 MOSI, 4 CS, 5 DC, 6 reset; SPI0 |
| Jitter probe | 7 |
| I2C0 bus | 8 SDA, 9 SCL; 400 kHz; external pull-ups (2.2-4.7 kOhm), internal ones disabled |
| MCP23017 step buttons | I2C 0x20: steps 1-7 (GPA0-6), 9-15 (GPB0-6); I2C 0x24: step 8 (GPA0), step 16 (GPA1) |
| MCP23017 step LEDs | I2C 0x21 |
| MCP23017 panel key scanner | I2C 0x22: ROW0-6 (GPA0-6), ROW7 (GPB0), COL0-6 (GPB1-7), COL7 (GPA7); I2C 0x23: COL8 (GPA0), COL9 (GPA1); 8 x 10 matrix, 80 keys (64 pads, 8 SCENE, 8 MUTE); no INT line, polled |
| Two MCP4822 DACs | 10 SCK, 11 MOSI, 12/13 CS; SPI1 |
| Gates | 14-17; 3.3 V logic |
| Encoder 1 | 18, 19, switch 20 |
| Encoder 2 | 21, 22, switch 26 |
| Tempo LED | 25 |
| RGB LED chain | 27; 82 x SK6812MINI-E via PIO, level-shifted to 5 V |
| PLAY button | 28 to GND, internal pull-up, 20 ms debounce |

There are **two** encoders. Firmware uses MCP23017 expanders, not shift-register
button drivers. The Pico exposes 26 general-purpose GPIOs; this assignment uses
all of them (GPIO 27 LED data, GPIO 28 PLAY).

Every I2C transaction, initialisation included, uses the SDK timeout API (2000 us)
and checks the exact byte count, so a missing or stuck chip never hangs the UI,
but each failed transaction can delay one UI pass by up to its 2 ms timeout. Any
I2C error marks that chip (the key scanner: both chips as one unit) unconfigured:
its configuration is written again before the next read. An attempt after a failed
one waits 100 ms, not every pass; for the scanner and the step row, a failed first
check or sample after a configuration counts as a failed attempt. A dead bus
costs about one timeout per chip per 100 ms. PLAY is a plain GPIO and is sampled
first on every pass, whatever the bus does.

A chip that reset (brown-out, ESD) comes back with IODIR = 0xFF and the rest 0,
so every input chip has a canary register: a value that differs from the
power-on value, written first in the chip's configuration and read back after the
data it vouches for (IODIR on 0x22 and 0x23, IPOL on 0x20 and 0x24). A reset at
any moment after the canary write clears it. The scanner checks both of its chips
at the start of every scan, and reads the canaries again right after any sample
that would settle a key (U801's, and U802's for columns 8-9), which counts only if
they hold; the step row reads IPOL back after
every sample and uses the ports only if it holds. A chip that failed either check
is configured again, and the step row's next good read becomes its baseline
without events. The step-LED expander still has its IODIR read back about once a
second. A failed button read keeps the last accepted state and makes no long
press. A step long-presses only in a pass that just took a good sample of its
chip, so up to 20 ms late and never on a sample from before its release; the
step LED mask is cached only after a successful write. GPA7 and GPB7 of every expander are
outputs (output-only on current MCP23017 datasheets; a level change on them
during a read can corrupt SDA). Core 1 never touches I2C or the LEDs.

The key matrix is scanned by `key_scan.c` from Core 0: 11 bus steps per scan (a
configuration check, then columns 0-9), one step per UI pass, so a pass never
waits for a whole scan (2.7-3 ms) and the encoders keep their rate. At most one
column is low when the rows are read. A key's state changes only in the pass that
sampled its column, and only at a sample at least 5 ms (`PANEL_DEBOUNCE_MS`)
after the sample that first saw the change, with every sample between agreeing.
A sample that would settle a key (report it, or end its suppression) is followed
in the same pass by a read of the reset canaries (`key_scan_verify`) and counts
only if they hold, so a chip that resets after the scan's check never supplies it;
such a pass makes at most 6 transfers for columns 0-7 and 8 for columns 8-9 (the
column, then the canaries), any other scan step at most 4 (a configuration attempt makes 6 writes). Press to event is
about 7-10 ms typical and about 14.2 ms worst by estimate for a key changing alone and
about 15.3 ms for a ten-key chord, target 15 ms (bench item, chords as well as single
keys). The host loop model, which counts wire bytes only (no START/STOP or
SDK call overhead) and is not a device measurement, gives 11.69 ms worst for one
key, 12.86 ms for a ten-key chord and 11.77 ms for 50 presses a second held 60 ms.
Each settle costs bus time, so keys changing together slow each other down, and
bursts of presses far beyond playing can push press-to-event past 15 ms: in the
model, bursts of presses every 0.37-2 ms (thousands of key changes a second)
reach 15.59 ms, and the test asserts a 25 ms bound with no held press lost. The
device's call overhead, which the model leaves out, lowers that threshold, so
such traffic, far beyond playing, can pass 15 ms on the device. Contract: the scan has no event FIFO. A tap shorter than the
debounce, or one that starts and ends while the bus is down or the scanner is
being reconfigured, is lost. On any I2C error, failed check or mismatch the
scanner is configured again before any further sample; every pending key change
is cancelled, and a key held through it stays held, with no release or new press.
The step row reads its ports only when its 20 ms sample is due; U701 (0x20) and
U703 (0x24) are independent, so a missing U703 costs only steps 8 and 16.

A flash save stalls both cores (only a save uses the multicore lockout) and a
load stalls only Core 0; afterwards the inputs are resynced:
`panel_input_resync()` drops every queued panel event (PLAY included), does no
I2C and restarts the scan at the check; `mcp23017_resync()` and
`encoder_resync()` take the buttons as they are now, with no press, release or
long press from the stall; and the UI drops the step gestures in progress. When
a panic is released, the panel and the step buttons are resynced the same way.
After a resync, and at power-up, every key is taken as held until it has been
seen up for 5 ms (two samples): a key held through the stall reports nothing, nor
does its release; a key released during the stall presses normally again once
its suppression has ended; a key pressed meanwhile and still held reports
nothing until it is released. PLAY that was down stays suppressed until a release
has held for 20 ms. A bounce or a re-reported press never becomes a command.

### Power

The LEDs take 5 V from VBUS, so the device is powered through its USB socket
only and must stay within 500 mA at 5 V (RGB chain budget 200 mA). The firmware
has no USB stack and never enumerates, so a host may hold it to 100 mA; this is
a bench check on real laptops. The frame builder scales the RGB brightness to
keep the estimated current within the budget.

### MIDI Circuit

Use a buffered MIDI OUT circuit conforming to the 3.3 V variant of
[MIDI CA-033](https://www.midi.org/wp-content/uploads/wpforo/default_attachments/1709416667-ca33-MIDI-10-Electrical-Specification-Update.pdf).
The earlier pair of 220-ohm resistors was a 5 V assumption, not the 3.3 V
reference circuit. CA-033 specifies different resistors and driver current
requirements for 3.3 V. The actual board circuit has not been validated here.

### CV Circuit

The MCP4822's internal reference is **2.048 V**. Default gain remains 1x:
nominal range 0-2.048 V, with code 4095 just below full scale. MIDI note 24 is
0 V, note 36 is 1 V, note 48 is 2 V; higher notes saturate. Absolute pitch is
rounded to the nearest DAC code, avoiding cumulative semitone rounding error.

`-DREBIRTH_CV_DAC_GAIN=2` selects nominal 0-4.096 V. It needs sufficient DAC
supply/headroom and compatible SPI levels, typically 5 V power with proper
3.3-to-5 V logic buffering. A 3.3 V supply cannot produce 4.096 V, and 3.3 V
GPIO must not be assumed to meet the input-high specification of a 5 V DAC.
[MCP4822 datasheet](https://ww1.microchip.com/downloads/en/devicedoc/20002249b.pdf).

SPI1 runs at 1 MHz: each 16-bit DAC word takes 16 us on the bus, followed by
analog settling. Tie LDAC appropriately for the intended update behavior.
Use one active source track per CV/gate pair. Analog calibration, output buffers
and gate-level compatibility require bench verification.

## Verification Boundaries

Host tests execute the production timer, transport, MIDI queue, storage, UI and
OLED renderer against fake peripherals. Unix tests exercise concurrent Stop/Panic
during a pending gate commit. DAC command tests cover both gains. These tests do
not measure RP2040 timing or analog voltages.

Inspect the linked ELF/map for memory usage. Measure GPIO 7 and physical outputs
under load before claiming an ISR budget or timing margin; the kit track's ISR
budget, wire-level Clock interval and Core 0 interrupts-off window are bench items,
and so is following a real master (the latency from a received Clock to the step
it starts, and the jitter added). See
[HARDWARE_STATUS.md](HARDWARE_STATUS.md) for remaining checks.
