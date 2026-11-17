# Quantised Mutes and Mute Groups — Design

Date: 2026-11-17. Status: requested by the session that planned the next
features (item 3 of `docs/research/similar-sequencers.md`, kept outside the
repository): MUTE follows the launch quantisation, its light blinks while it
waits, an immediate mode stays reachable, and holding one MUTE key and
pressing others makes a group that applies on release. The choices that
request left open are this design's calls and are marked **(call)**. Revised
the same day after an independent review (see "Review changes").

## Intent

Taking the kick and the bass out exactly on the downbeat is the main move of
a Goa set. Today a MUTE key acts at once, so the cut lands where the hand
lands, a few tens of milliseconds early or late. Pads and SCENE already wait
for the launch point (`launch_quant`, 1 BAR after power-up); MUTE should wait
for the same point, so the hand only has to be early.

## Goals

1. While the transport plays, a MUTE press waits for the next launch point of
   the current launch quantisation and the change lands there, on the step's
   boundary, exactly as an immediate mute pressed just before that step would.
2. The MUTE light shows that a change waits.
3. Pressing a waiting MUTE again cancels its change.
4. An immediate MUTE stays reachable.
5. Hold one MUTE key, then press others: they are collected and change
   together at the launch point after the held key is released.

## Non-goals

- Mutes in song rows, mute snapshots, solo.
- A new launch quantisation value, or a separate quantisation for mutes.
- Any change to pads, SCENE, PLAY or the save format.
- Showing a waiting change on the OLED or the selector's step LEDs (they keep
  showing the current state).

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
is the same moment a pad press counts. The panel reports a MUTE key 5 to
about 20 ms after it goes down (one key column per UI pass), so "the press"
means the moment the UI handles it.

**Immediate (call).** A MUTE acts at once, as today, when the transport is not
playing, or when the launch quantisation is NEXT STEP. NEXT STEP is the
setting for hands-on mutes: today a mute cuts on the next pulse and an unmute
sounds from the next active step, and NEXT STEP keeps exactly that. The
selector's buttons 1-8 stay immediate in every mode (they are an editing
screen, not a performance control).

**Waiting.** Otherwise the change waits for the next launch point:

| Quantisation | The change lands |
|---|---|
| 1 BAR | on the next global step that is a multiple of 16 |
| 1 BEAT | on the next global step that is a multiple of 4 |
| TRACK LOOP, Pattern mode | at that track's loop end, by the queued pads' rule |
| TRACK LOOP, Song mode | as 1 BAR (song entries change on bars, and a changed track restarts its loop there) |

The rule is read at each boundary, so changing the quantisation while a
change waits moves it as it moves a queued pad. Under TRACK LOOP a group's
tracks land at their own loop ends, together only when their loops end
together; this is stated, not fixed.

**A second press of a waiting MUTE cancels its change (call),** as a second
press of a queued pad cancels its launch. A press the UI handles after the
change has landed is a new change, not a cancel.

Stop, a restart, a play-mode change and a panic (also the one the timer raises
on a MIDI overflow) cancel every waiting change, as they cancel queued pads.
At the end of a song that stops, a change due on that last boundary has
already landed (changes are applied before the song advances); any other is
cancelled.

### What a change is

A change has a **target**, decided when it is made: the opposite of the
track's state then, by the existing rule (muted when the live mute or the
stored mute of the location it plays is set). To mute, the live bit is set.
To unmute, the live bit is cleared and, as today, the stored mute of every
location of that track is cleared.

**Stored mutes (call).** The timer can only change the live bit; clearing
stored mutes rewrites 32 locations and refreshes their prepared rows on
Core 0. So when an unmute is made, in one critical section, the live bit is
set (inaudible: the track is silent already) and the change "clear the live
bit" is queued, or applied when immediate. Then the stored mutes are cleared
and the rows refreshed. From then on the track is muted by its live bit only.
A cancelled unmute therefore leaves the track live-muted in every location,
which is also where two presses of the old immediate MUTE left it. A launch
point that falls inside the refresh (a millisecond or so) can play one step of
that track silent, the same exposure today's MUTE has.

### Groups

**A hold is a group only after half a second (call).** A MUTE key pressed
while no key is the held key is an ordinary press (above) and becomes the
**held key**. Another MUTE key pressed while the held key is down:

- less than 500 ms after the UI handled the held key's press: an ordinary
  press as well (two fingers together are two presses, which land together
  anyway when both come before the launch point);
- 500 ms or more after: it joins the **group** instead of acting, with its
  target decided now. If it has a change waiting, the press cancels that
  change and the key does not join. If it is already in the group, the press
  takes it out.

The first key to join also brings the held key in: if the held key's own
change is still waiting, it is withdrawn from the timer, with its target, and
becomes part of the group. If it already landed (the hold crossed a launch
point) or acted at once (immediate mode), the held key is not in the group:
in immediate mode the group is the other keys.

When the held key is released, the group's keys change in one call, to the
targets they joined with: at once when immediate, otherwise queued together
for the next launch point after the release. A release with an empty group
does nothing (the held key's own press has already acted). Releases of the
other keys do nothing. After the release there is no held key until a MUTE
key is pressed with none held.

500 ms is the clip-copy hold (`PAD_COPY_HOLD_MS`): the panel already tells a
deliberate hold from two quick presses that way.

**A hold the panel may have lost is not trusted.** A key joins only while the
held key is still reported down and the panel's generation is the one of its
press; a release applies the group only under that same generation; and an
end-of-pass sweep drops a held key the panel no longer reports down or whose
generation changed. Dropping the group changes nothing in it, a withdrawn
change of the held key included. A panic and a flash save or load drop it too.
Stop does not: a group released after Stop applies at once, as any press
while stopped.

The selector's buttons 1-8 take their track out of the group, then cancel its
waiting change and toggle it at once, in one engine call.

### Lights

`mute_light()` gains two states, checked before the existing bank blinks:

- **in the group**: steady white at full;
- **change waiting**: white at full on the odd sixteenths (the last sixteenth
  before every beat among them), the track's normal light on the even ones.

Twice the rate of the queued pad's blink and white, so it reads neither as the
eighth-note nor the half-note bank blinks (the track's hue or dim white
against off), and a waiting unmute of an unfocused muted track (normally dark)
still shows. A change made in the last sixteenth before its launch point is
white at once. While a change waits, the bank blinks of that MUTE are not
shown.

## How it is built

### Engine (`sequencer.c`, `sequencer.h`, `song.c`)

- `sequencer_state_t` gains `volatile uint8_t mute_pending` (bit per track: a
  change waits) and `volatile uint8_t mute_target` (the live bit it writes).
  Not saved; cleared by `sequencer_init()`. Written only under the playback
  lock (by Core 0, or by the timer, which holds it throughout).
  `track_mutes` stays under the shared lock, which the timer now also takes
  to write it; the timer's existing unlocked read of the whole byte stays.
- Core 0 API, each taking the locks itself:
  - `void transport_toggle_mutes(uint8_t mask)`: a MUTE press. For each track
    in `mask` a waiting change is cancelled; each track in `mask` with none
    gets a change to the opposite of its state. Tracks outside `mask` are not
    touched.
  - `void transport_set_mutes(uint8_t mask, uint8_t target)`: a group's
    release: each track in `mask` gets a change to its bit of `target`,
    replacing a waiting one.
  - `uint8_t transport_cancel_mutes(uint8_t mask, uint8_t *target)`: cancels
    the waiting changes in `mask` and returns them, with their targets.
  - `void transport_toggle_mutes_now(uint8_t mask)`: the selector. Cancels
    the waiting changes in `mask` and toggles those tracks at once, whatever
    the mode.
- All four share one shape: in a single section under the playback lock and
  then the shared lock, decide (cancel, targets, immediate or not), set the
  live bit of tracks to unmute, and apply or queue; after unlocking, clear the
  stored mutes of the tracks to unmute (one location per lock section, rows
  refreshed when any was set). The state needed for the decision is read in
  place under the lock: `seq_get_track_mutes()` takes the shared lock itself
  and must not be called there (a helper that reads it with the lock held is
  the way).
- `transport_snapshot_t` gains `mute_pending`.
- The timer applies due changes at a step boundary **before** step 2 of the
  pulse order (live mutes) and before the song advances, so the rest of the
  pulse treats them exactly as a mute set just before it: a newly muted
  track's sounding notes and kit voices are released in step 2 and its step
  does not trigger; a newly unmuted track plays this step. Due is the queue's
  rule from `pulse_count` (16 or 4 steps; NEXT STEP: every step). For TRACK
  LOOP in Pattern mode the waiting track's row is loaded first
  (`load_prepared_track()`, which phase 1 repeats as a no-op) and due is
  `track_step_count >= length` of that row, the very condition the queued pad
  uses; in Song mode TRACK LOOP is BAR. One shared-lock section, only when
  something waits.
- `mute_pending` is cleared by `stop_playback_outputs()` (Stop, panic, song
  end, a mode change, a restart while playing) and by `transport_start()`
  before its early return for an empty song. `transport_clear_queue()` and
  `transport_clear_track_queue()` are about pads and leave it.
- `seq_toggle_track_mute()` keeps its behaviour. Its stored-mute clearing
  becomes a helper in `song.c` shared with the new calls.

### Panel (`panel_input.c/.h`)

`bool panel_input_mute_down(uint8_t track)`, like `panel_input_pad_down()`.

### UI (`ui.c`, `session.c`)

- `session_mute()` calls `transport_toggle_mutes(1u << track)`.
- `handle_panel_events()` handles MUTE releases (today every non-pad release
  is dropped) and implements the held key and the group: `ui.mute_held`
  (track or none), its press time and panel generation, `ui.mute_group` and
  `ui.mute_group_target` (masks). An end-of-pass sweep, beside
  `release_lost_pad_keys()`, drops a lost held key. The panic combo and a
  flash save or load drop it.
- The selector's buttons 1-8 call `transport_toggle_mutes_now()` after taking
  the track out of the group.
- `led_state_t` gains `mute_pending` (from the frame's snapshot) and
  `mute_group` (from the UI).

## Constraints

- No save-format change; nothing on Core 0 waits for the timer.
- Lock order stays playback, then shared. The timer's extra work is one
  shared-lock section at step boundaries where a change waits.
- Every write of `track_mutes`, `mute_pending` and `mute_target` follows the
  rules above. The host tests can interleave the timer with a Core 0 call at
  a lock (`fake_before_lock`), which covers the decision sections; a race on
  real cores is still argued from the rules, not measured.

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

**Stored mutes.** Converted when the change is made (chosen). At the
boundary: the timer cannot refresh 32 rows, and doing it later on Core 0
would miss the step. A runtime override in the timer that ignores stored
mutes: more state in the prepared rows for a case only old saves have.

**The waiting light.** White on sixteenths (chosen). Blinking between the
track's light now and after the change: for several states it is identical
to a bank blink. White on the eighth-note phase: close to the dim-white
bank blink, and dark through the last eighth of a bar, so a late press would
show nothing before it lands.

## Simulator

`sim/build.sh` gets feature bit 8, "MUTE waits for the launch point", detected
from `transport_toggle_mutes` in `include/sequencer.h`. `Device.features()`
reports it as `quantMutes`; `sim/check.sh` passes `QMUTE=1` to every score it
runs on such a firmware; `artist_check.js` and `device_check.js` read it and
pass `quantMutes` to `Artist.buildShow()`, and the page passes it from
`Device.features()`.

`device_check.js` presses MUTE while the set plays and expects the change
within 180 ms; with `QMUTE` it checks that nothing changed yet, then runs past
the next bar and checks the change.

The unchanged score is expected to pass on the new firmware (taps on press;
`mutePair`'s two fingers are 12 ms apart). The `QMUTE` score keeps every
single mute and two-finger pair at its old moment (that case is the
compatibility case) and adds:

- captions saying that MUTE waits for the launch point and flashes white;
- a group where the bars before the downbeat are free (at least about two
  bars): the first MUTE held from the bar before the previous one, the second
  pressed at least 620 ms later but before that bar's end, the first held
  across the next downbeat and released at least 80 ms before the target
  downbeat. Where there is no room the pair stays two fingers;
- for every mute point, two checks of the mutes alone, about 40 ms before and
  40 ms after the downbeat, which an immediate mute fails; for a group also a
  check after the downbeat its hold crosses (nothing changed there: the held
  key's change was withdrawn) and a check, while it is held, that both MUTE
  lights are white.

## Left to the builder

- Names of static helpers and `ui` fields.
- Whether the group logic lives in `ui.c` or `session.c`.
- Exact helper names in `artist.js`, and where the forms have room for a group.

## Verification

- Host tests (`test_playback`, `test_ui`, `test_led_frame`,
  `test_panel_input`, `test_song`, `test_session` where it calls
  `session_mute`) for each rule above: the boundary per quantisation and mode;
  the downbeat note of a newly muted and a newly unmuted track, also when a
  queued pad lands on the same boundary; kit voices; TRACK LOOP after a length
  edit and with a queued pad of another length; cancel, and a press after the
  landing; the cancelling paths, the song-end boundary and the empty-song
  start; stored mutes, a cancelled stored unmute and a launch point inside the
  refresh (`fake_before_lock`); the group (hold threshold, targets fixed at
  join, withdrawal, membership toggles, empty group, lost hold at join, at
  release and in the sweep, panic, immediate mode); the selector against a
  waiting change and a group; the lights. The existing
  `test_grid_panel_keys_during_a_hold_leave_the_tap` expects an instant mute
  while playing and changes accordingly.
- `sim/check.sh` green for every score.
- Not verifiable here: how the white flash reads on the real panel.

## Review changes

The first draft was reviewed by Codex, Grok and DeepSeek (Gemini's run
failed: its API credits were exhausted). Changes:

- A change is published in the same critical section that decides it; the
  stored-mute clearing follows. The draft queued it only after the refresh,
  so a launch point or a panic in between could skip it or apply it on a
  stopped transport (all three).
- A group stores the targets its keys joined with and the release sets them;
  the draft toggled again at release, which a location change in between
  could invert (Codex, DeepSeek).
- The selector is one engine call that cancels and toggles (DeepSeek).
- TRACK LOOP due uses the row the queued pads use, loaded first; Song mode
  uses the bar (Codex, Grok, DeepSeek).
- The song-end boundary is defined (Codex, Grok).
- The waiting light uses sixteenths: the eighth-note phase was close to the
  dim-white bank blink and dark in the last eighth before a bar (all three).
- Holds are checked at join and at release, not only in the sweep (Codex).
- `device_check.js` was missing from the simulator plan (Codex, DeepSeek).
- Builder traps named: `seq_get_track_mutes()` under the lock, the empty
  song's early return, `sequencer_init()`, the empty group.

Not changed: an epoch counter or a runtime stored-mute override (Codex):
publishing at the decision removes the window they guarded. Running the
unchanged score as well as the `QMUTE` one on new firmware (Codex): the
`QMUTE` score keeps the old single and two-finger timings.
