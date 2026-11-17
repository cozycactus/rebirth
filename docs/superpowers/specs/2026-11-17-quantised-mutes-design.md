# Quantised Mutes and Mute Groups — Design

Date: 2026-11-17. Status: requested by the session that planned the next
features (item 3 of `docs/research/similar-sequencers.md`, kept outside the
repository): MUTE follows the launch quantisation, its light blinks while it
waits, an immediate mode stays reachable, and holding one MUTE key and
pressing others makes a group that applies on release. The choices that
request left open are this design's calls and are marked **(call)**.

## Intent

Taking the kick and the bass out exactly on the downbeat is the main move of
a Goa set. Today a MUTE key acts at once, so the cut lands where the hand
lands, a few tens of milliseconds early or late. Pads and SCENE already wait
for the launch point (`launch_quant`, 1 BAR after power-up); MUTE should wait
for the same point, so the hand only has to be early.

## Goals

1. While the transport plays, a MUTE press waits for the next launch point of
   the current launch quantisation and the change lands there, on the step's
   boundary, as an immediate mute pressed just before that step would.
2. The MUTE light shows that a change waits.
3. Pressing a waiting MUTE again cancels its change.
4. An immediate MUTE stays reachable.
5. Hold one MUTE key, then press others: they are collected and change
   together at the launch point after the held key is released.

## Non-goals

- Mutes in song rows, mute snapshots, solo.
- A new launch quantisation value, or a separate quantisation for mutes.
- Any change to pads, SCENE, PLAY or the save format.

## Anti-goals

- A two-finger hit just ahead of the downbeat landing a bar late.
- A change landing on the downbeat while that downbeat's note of a newly muted
  track still plays (or a newly unmuted track's downbeat note is missing).
- A collected group that fires on its own because the panel lost a release.
- A change left waiting after Stop that fires at the next Start.

## Behaviour

### When a MUTE press counts

**On press (call).** The press is what is timed: a press 70 ms before the
downbeat lands on that downbeat. This keeps the old timing habit working and
is the same moment a pad press counts.

**Immediate (call).** A MUTE acts at once, as today, when the transport is not
playing, or when the launch quantisation is NEXT STEP. NEXT STEP is the
setting for hands-on mutes: an unmute already took effect only at the next
active step, so the only difference from a queued change would be a mute's cut
up to one step later. The selector's buttons 1-8 stay immediate in every mode
(they are an editing screen, not a performance control) and cancel a waiting
change of that track first.

**Waiting.** Otherwise the change waits for the next launch point:

| Quantisation | The change lands |
|---|---|
| 1 BAR | on the next global step that is a multiple of 16 |
| 1 BEAT | on the next global step that is a multiple of 4 |
| TRACK LOOP | at that track's loop end (the step on which it starts its clip again) |

The rule is the one queued pads use, read at each boundary, so changing the
quantisation while a change waits moves it as it moves a queued pad. In Song
mode it applies too (song entries change on bars, so 1 BAR is an entry
boundary). Under TRACK LOOP a group's tracks land at their own loop ends,
together only when their loops end together; this is stated, not fixed.

**A second press of a waiting MUTE cancels its change (call),** as a second
press of a queued pad cancels its launch. The track stays as it is.

Stop, a restart, a play-mode change, a panic (also the one the timer raises
on a MIDI overflow) and a song that stops at its end cancel every waiting
change, as they cancel queued pads.

### What a change is

A change is decided at the press: the track becomes the opposite of what it
is now by the existing rule (`seq_get_track_mutes()`: muted when the live mute
or the stored mute of the location it plays is set). To mute, the live bit is
set. To unmute, the live bit is cleared and, as today, the stored mute of
every location of that track is cleared.

**Stored mutes (call).** The timer can only change the live bit; clearing
stored mutes rewrites 32 locations and refreshes their prepared rows on
Core 0. So an unmute is prepared at the press: the live bit is set at once
(inaudible, the track is already silent), then the stored mutes are cleared
and the rows refreshed, and only then is the change "clear the live bit"
queued (or applied, when immediate). From the press on, a stored-muted track
is muted by its live bit only. A cancelled unmute therefore leaves the track
live-muted in every location, which is also where two presses of the old
immediate MUTE left it.

### Groups

**A hold is a group only after half a second (call).** A MUTE key pressed
while no group key is held is an ordinary press (above) and becomes the
**held key**. Another MUTE key pressed while the held key is down:

- less than 500 ms after the UI handled the held key's press: an ordinary
  press as well (two fingers together are two presses, which land together
  anyway when both come before the launch point);
- 500 ms or more after: it joins the **group** instead of acting. If it has a
  change waiting, the press cancels that change and the key does not join. If
  it is already in the group, the press takes it out.

The first key to join also brings the held key in: if the held key's own
change is still waiting, it is withdrawn from the timer and becomes part of
the group. If it already landed (the hold crossed a launch point) or acted at
once, the held key is not in the group.

When the held key is released, the group's keys all change in one call: at
once when immediate, otherwise queued together for the next launch point
after the release. Releases of the other keys do nothing. After the release
there is no held key until a MUTE key is pressed with none held.

500 ms is the clip-copy hold (`PAD_COPY_HOLD_MS`): the panel already tells a
deliberate hold from two quick presses that way.

**A hold the panel may have lost is not trusted.** If the held key is no
longer reported down without its release having been handled, or the panel's
generation changed since its press, the group is dropped: nothing in it
changes, including a withdrawn change of the held key. A panic and a flash
save or load drop it too.

### Lights

`mute_light()` gains two states, checked before the existing bank blinks:

- **in the group**: steady white at full;
- **change waiting**: white at full on the eighth-note blink phase
  (`blink_fast`, the queued pad's rhythm), the track's normal light on the
  other phase.

White is used so that the waiting state cannot be read as the existing
blinks (the track's hue or dim white against off, for a queued or current
location outside the shown bank), and so that a waiting unmute of an
unfocused muted track (normally dark) still shows. The OLED is unchanged.

## How it is built

### Engine (`sequencer.c`, `sequencer.h`)

- `sequencer_state_t` gains `volatile uint8_t mute_pending` (bit per track: a
  change waits) and `volatile uint8_t mute_target` (the live bit the change
  writes). Not saved. Written only under the playback lock (by Core 0 or by
  the timer, which holds it throughout). `track_mutes` stays under the shared
  lock, which the timer now also takes to write it.
- `void transport_toggle_mutes(uint8_t mask)`: per track in `mask`, a waiting
  change is cancelled; every other track gets a change to the opposite state,
  all of them together. Three steps: (1) under the playback and shared locks,
  cancel, decide the targets, set the live bit of the tracks to unmute;
  (2) clear those tracks' stored mutes, one location per lock section, and
  refresh their rows when any was set; (3) under both locks again, apply the
  targets when not playing or under NEXT STEP, otherwise add them to
  `mute_pending`/`mute_target`. Between (1) and (3) no other change can reach
  these tracks: only this call queues, and they have nothing waiting.
- `uint8_t transport_cancel_mutes(uint8_t mask)`: cancels and returns the
  waiting changes in `mask` (used for the held key's withdrawal and by the
  selector).
- `transport_snapshot_t` gains `mute_pending`.
- The timer, at a step boundary, applies due changes **before** step 2 of the
  pulse order (live mutes), so the rest of the pulse treats them exactly as a
  mute set just before it: a newly muted track's sounding notes and kit
  voices are released in step 2, its step does not trigger; a newly unmuted
  track plays this step. Due is the queue's rule computed from `pulse_count`
  before a song advance resets it; for TRACK LOOP it is
  `track_step_count >= length` with the length of the prepared row published
  for the location the track plays (the length phase 1 then loads). One
  shared-lock section, only when something waits.
- Every path that cancels queued pads (or stops the transport) clears
  `mute_pending`: `stop_playback_outputs()` covers Stop, panic, song end, a
  mode change and a restart while playing; `transport_start()` clears it too.
- `seq_toggle_track_mute()` stays for the selector. The stored-mute clearing
  it does is shared with `transport_toggle_mutes()` (a helper in `song.c`).

### Panel (`panel_input.c/.h`)

`bool panel_input_mute_down(uint8_t track)`, like `panel_input_pad_down()`.

### UI (`ui.c`, `session.c`)

- `session_mute()` calls `transport_toggle_mutes(1u << track)`.
- `handle_panel_events()` handles MUTE releases (today every non-pad release
  is dropped) and implements the held key and the group:
  `ui.mute_held` (track or none), its press time and panel generation, and
  `ui.mute_group` (mask). An end-of-pass sweep drops a held key the panel no
  longer reports down or whose generation changed, as for pad keys.
- The selector's buttons 1-8 call `transport_cancel_mutes()` before
  `seq_toggle_track_mute()`.
- `led_state_t` gains `mute_pending` (from the frame's snapshot) and
  `mute_group` (from the UI).

## Constraints

- No save-format change; nothing waits on Core 0 for the timer.
- Lock order stays playback, then shared. The timer's extra work is one
  shared-lock section at step boundaries where a change waits.
- The two-core race on `track_mutes` cannot be shown by the host tests or the
  simulator (one thread): it is right by the lock rule above.

## Approaches considered

**When a press counts.** On press (chosen): the old timing still works and a
late arm still lands. On release, as Hapax does for its groups: one rule for
taps and groups, but a tap 70 ms ahead releases after the downbeat and lands a
bar late, and an immediate mute would lag by the length of the tap.

**What makes a group.** A hold of 500 ms (chosen). Any overlap of two MUTE
keys: simpler, but every two-finger hit becomes a group that waits for the
release, which near the downbeat lands a bar late, the first anti-goal.

**The immediate mode.** NEXT STEP plus stopped, plus the selector (chosen). A
fifth quantisation value OFF: it would need a meaning for pads too and changes
the VALUE cycle (the simulator's artist counts presses to get back to 1 BAR).
A gesture: the panel has no free modifier.

**Where a change is applied.** In the timer at the step boundary (chosen).
Core 0 watching the transport: it would land up to a UI pass late, after the
step's notes.

**Stored mutes.** Converted at the press (chosen). At the boundary: the timer
cannot refresh 32 rows, and doing it later on Core 0 would miss the step.

**The waiting light.** White (chosen). Blinking between the track's light now
and after the change: for several states it is identical to a bank blink.

## Simulator

`sim/build.sh` gets feature bit 8, "MUTE waits for the launch point", detected
from `transport_toggle_mutes` in `include/sequencer.h`; `Device.features()`
reports it as `quantMutes`, `sim/check.sh` passes `QMUTE=1` to every score it
runs on such a firmware, and the page passes it to `Artist.buildShow()`.

The unchanged score is expected to pass on the new firmware (taps on press;
`mutePair`'s two fingers are 12 ms apart). The `QMUTE` score shows the feature:

- the captions say that MUTE waits for the downbeat and blinks white;
- a pair is played as a group where the bar before the downbeat leaves room
  (the cursor at least about 1 s ahead): hold the first key, press the second
  after more than half a second, release the first before the downbeat; where
  there is no room it stays two fingers;
- every mute point gets two extra checks of the mutes alone, about 40 ms
  before and 40 ms after the downbeat, which an immediate mute would fail.

## Left to the builder

- Names of static helpers and `ui` fields.
- Whether the group logic lives in `ui.c` or `session.c`.
- Exact helper names in `artist.js`.

## Verification

- Host tests (`test_playback`, `test_ui`, `test_led_frame`,
  `test_panel_input`, `test_song`) for each rule above: the boundary per
  quantisation, the downbeat note of a newly muted and a newly unmuted track,
  kit voices, cancel, the group (hold threshold, withdrawal, membership
  toggles, lost hold, panic), the cancelling paths, stored mutes, the
  selector, and the lights.
- `sim/check.sh` green for every score.
- Not verifiable here: how the white flash reads on the real panel.
