# Live MUTE Over the Clips Song ("It's a Sham, Man") — Design

Date: 2026-11-25. Status: draft, revised the same day after two independent
reviews (DeepSeek, Grok; Codex was unavailable), see "Review changes". The
feature and its shape are the owner's choice, made in conversation: the
hands load a Twisted song laid out as the device's clips ("Клипы на
падах"), switch to Song mode, and while the device plays the chain itself
they perform on top of it with MUTE, taking tracks out and bringing them
back at breaks and drops. No pad or SCENE launches (Song mode refuses them).
The owner was told the song stops at bar 262 of 361, because the chain holds
256 rows (`MAX_SONG_ENTRIES`). The choices this design makes are marked
**(call)**; the numbers come from a throwaway prototype run on the wasm core
built from origin/master 86975bc (see "Prototype").

## Intent

Today "Клипы на падах" is a loading routine: the ring marks a few presses,
PLAY, and then seven minutes of a device playing by itself with nobody at
it. The hands sit that mode out on purpose (the hands spec, "As built", and
`sim/README.md`, "The hands"). With this feature someone is at the device:
the kick drops out for the bar before a new section and slams back on its
downbeat, the hats go for four bars and return on the phrase, kick and bass
leave together before the big drops. It should sound like a live act working
a finished track, never like the track being taken apart: the arrangement
the chain plays goes on underneath, and every move resolves on a downbeat.

## Goals

1. A new way to play a song from the MIDI library: laid out as clips, loaded
   and started by the artist as today, then performed with MUTE until the
   end, all at speed 1.
2. The hands play all of it (loading moves and MUTE performance); the ring
   marks the same presses when the hands cannot be built in time.
3. The mute script is derived from the laid-out song, works for all eight
   Twisted songs and is tuned and checked on "It's a Sham, Man".
4. Every MUTE change lands exactly on the downbeat it is meant for, and what
   sounds is exactly the chain minus the muted (track, bar) cells.
5. The plain "Клипы на падах" mode stays byte for byte what it is today.
6. Checks in Node against the wasm core and the hands' whole-show check,
   both in `sim/check.sh`.

## Non-goals

- Any firmware change. Everything used (Song mode, quantised MUTE, paging
  with TRACK) is on master.
- Pad or SCENE launches, synth knob moves, filter sweeps, tempo changes.
- Longer songs: the 256-row chain limit stays.
- A hand-written mute part per song (see "Approaches considered").
- Changes to the plain clips mode, the full arrangement or MIDI Song.

## Anti-goals (a failure even if it works)

- A move that empties the groove (kick, snare and hats out at once), or one
  that holds a track silent across a section change, so a new part never
  arrives.
- A MUTE pressed on a track that is silent in the bar it is pressed in, or
  a move that leaves half of a group playing (one bass voice of two).
- A press on a key whose change still waits: by the quantised-mutes rule a
  second press cancels it, so the move would silently not happen.
- The hands' 0.2–0.3 s build running while the device plays this show.
  (After a take-over the chain may still sound on an idle page, where the
  build is allowed, as the README already accepts for an artist's set.)

## Firmware facts this relies on (verified on 86975bc)

- Song mode: `transport_launch` returns false unless the mode is Pattern
  (`src/sequencer.c:1693`), so pads and SCENE change nothing in the chain.
  A pad in the Session view still moves the focus before that call
  (`src/session.c:113`), which would retarget later MUTEs: the show presses
  no pad. MUTE goes through `change_mutes` (`:1734`) with no mode check, and
  acts on the press; the release only ends a held key.
- Quantisation: `launch_quant` is 1 BAR at power-up (`src/main.c:20`), and
  `apply_due_mutes` reads TRACK LOOP as BAR in Song mode (`:618`). A MUTE
  pressed anywhere in a bar lands on the next global step that is a multiple
  of 16. Every clip is 16 steps, so global bars are chain bars.
- Persistence: the live mute is one bit per track (`g_seq.track_mutes`), not
  part of a chain row, so a mute holds while the chain changes that track's
  clip. The save image's stored mutes are all clear.
- Stop (PLAY, or a song that ends) drops waiting changes but keeps live
  mutes. The image ends with `SONG_END_STOP`: the chain stops on the
  boundary after its last row.
- Paging: in the Session view a MUTE key stands for `page*8 + column`, the
  page being the focus track's (`panel_first_track`, `src/ui.c:518`). TRACK
  walks one track per detent; the laid-out drums are `TRACK_TYPE_DRUM`, not
  kit tracks, so there are no lane stops. After boot and LOAD the focus is
  track 0: page 1 is tracks 0–7, and 8 detents reach page 2 (tracks 8–15).
- Traps: a short VALUE press in the Session view cycles the launch
  quantisation, and a TRACK long press steps the bank. The performance uses
  neither.
- A MUTE key held 500 ms makes the next keys a group (`PAD_COPY_HOLD_MS`).
  Every press here is 80 ms, so two keys pressed 12 ms apart are two plain
  presses that land on the same bar line.

## Approaches considered (the mute script)

**A. Written by hand for Sham.** The best result for one song, if someone
listens bar by bar. But nothing can check it except an ear, it does not
carry over to the other seven songs, and it goes stale if the layout in
`midi_clips.js` changes.

**B. Derived from the plan (chosen).** The layout knows, per track and bar,
what plays. Section changes show up as bars where many tracks start or stop
at once; the phrase grid is where they cluster. A few classic moves placed
by rules give a performance for any song in the library, and the check can
verify each move's premise.

**C. Derived plus per-song overrides by ear,** like `parts.json` for sounds.
Worth it only after someone has listened to B; easy to add later.

## The mute script (`MidiClips.mutes(plan)`)

Pure and deterministic, and it does not change the plan. Input: the plan,
which now also carries the file's time signatures (`plan.meters`, from a new
`song.meters` that `MidiSongs.read` takes from the FF 58 meta events).
Output: `{ setEnd, stretches, groups, moves }`, each move `{ kind, start,
end, press, tracks }`. Bars are 0-based chain bars below `plan.played`; the
text quotes 1-based bars as the page and the log show them.

**What plays.** `on(t, b)`: track `t` has an active step in bar `b`
(false outside 0 … `played`−1). `change(b)` for `b ≥ 1`: the number of
tracks, the gate role aside, whose `on` differs between bars `b−1` and `b`;
`change(0)` is 0 (bar 0 is never a phrase line that counts).

**Meter (call).** The layout cuts every song into four-beat chunks and does
not read the meter. A chain bar is *usable* when it starts on a musical bar
line of a 4/4 bar of the file. Maximal runs of usable bars are *stretches*;
a move lies inside one stretch, press bar to return. Seven songs are 4/4
throughout. Are You Sponged has a 6/4 bar at chain bar 119 and another at
126.5: its stretches are bars 0–118 and 128–254, and nothing is placed in
between (its musical downbeats fall on beat 3 of chain bars there). A song
with no usable stretch gets no moves.

**Phrase grid (call).** Per stretch, the phase `φ` in 0–7 that maximises the
sum of `change(b)` over the stretch's bars `b ≥ 1` with `b ≡ φ (mod 8)`;
those bars are its phrase lines. On Sham `φ` is 1: the file opens with a
pickup bar, and the kick enters on bar 34 (1-based), the hats on 42.

**The set end.** `setEnd` is where the performance ends: when the chain
holds the whole song, `plan.played`; when it is cut short, the last phrase
line at or before `plan.played` (Sham: chain bar 257, so the last bar heard
is bar 257; the chain alone would play to 262). Open question 1 decides how
the device stops there; the moves are the same either way.

**Groups (call)**, by role and General MIDI note, never the gate role, only
on pages 1–2:

| Group | Tracks | Sham |
|---|---|---|
| kick | role `kick` on page 1 | 1 |
| hats | drum notes 42, 44, 46 on page 1 | 5 Hats CH, 6 Hats OH |
| snare | drum notes 38, 40 on page 1, names without "fill" | 2 Snares |
| bass | role `bass` on page 2 | 10 Bass |

Every Twisted song puts its kick on track 1, its hats and snare on page 1
and its bass on page 2 (tracks 9–14).

**What a move mutes (call).** A move has a press bar (the bar before its
first muted bar), muted bars, and a return line. For each of its groups, the
tracks of the group that play anywhere from the press bar to the return
must each play in every one of those bars; then all of them are muted.
Otherwise the move is not made. So a pressed key always cuts a sounding
part, a part that was about to enter is never held out, and a drop takes
the whole bass with it.

**Moves (call).** Phrase lines `L` are taken in order, at most one move
each:

1. Skip `L` if `L` < previous return + 8, or `L` > `setEnd` − 8.
2. **Section line**, `change(L) ≥ 4`:
   - **drop** if `change(L) ≥ 8` and no drop returned in the 64 bars before:
     kick and bass muted in `L−2`, `L−1` (press bar `L−3`);
   - else, or if the drop cannot be made, **kick roll**: the kick muted in
     `L−1` (press bar `L−2`).
3. **Groove line**, `change(L) ≤ 2`, `L` ≥ previous return + 16, the kick
   playing in every bar from `L−8` to `L`. Such lines are counted from 0;
   the count advances on each one whether or not a move is placed. Even
   count: **hats cut** (`L−4` … `L−1`), else **snare out** (`L−2`, `L−1`).
   Odd count: snare out, else hats cut.
4. Any move also needs: `change(b) ≤ 2` for each of its muted bars (no
   section change inside), press bar to return inside one stretch, and no
   tie continuation (the previous step tied to the same note) on a muted
   track at the first muted bar or the return. Un-muting into a tie would
   strike the note again (`note_is_on` is cleared by the mute), so those
   edges are avoided; none of the eight songs has one where a move would go.
5. A move whose presses do not fit at the song's tempo (below) is not made.

The knobs are named constants: spacing 8 and 16, change thresholds 2, 4, 8,
the drop interval 64, the hats and snare lengths 4 and 2.

**The script on Sham** (1-based bars; "back" is the downbeat it returns on):

| Bars muted | Back | Move | Tracks | Pressed in bar |
|---|---|---|---|---|
| 41 | 42 | kick roll | 1 | 40 |
| 57 | 58 | kick roll | 1 | 56 |
| 78–81 | 82 | hats cut | 5, 6 | 77 |
| 88–89 | 90 | drop | 1, 10 | 87 |
| 97 | 98 | kick roll | 1 | 96 |
| 136–137 | 138 | snare out | 2 | 135 |
| 152–153 | 154 | snare out | 2 | 151 |
| 209 | 210 | kick roll | 1 | 208 |
| 238–241 | 242 | hats cut | 5 | 237 |
| 248–249 | 250 | drop | 1, 10 | 247 |

10 moves, 26 MUTE presses, 4 TRACK turns over 257 bars (7 min 2 s at 146
BPM). The longest stretch without a move is bars 155–208 (54 bars, 89 s:
the song's own breakdown and its long sections with the snare not steady),
then the intro to bar 40. All eight songs:

| Song | Moves | MUTE presses | TRACK turns | Max presses in a bar |
|---|---|---|---|---|
| Contains LSD | 7 | 20 | 0 | 2 |
| The Synths of Orpheus | 15 | 36 | 4 | 3 |
| Centaurs Are Real | 15 | 36 | 0 | 2 |
| A Magician Nevers Reveals His Tricks | 9 | 26 | 2 | 2 |
| It's a Sham, Man | 10 | 26 | 4 | 2 |
| Snarled Up | 13 | 34 | 0 | 2 |
| Are You Sponged | 11 | 28 | 4 | 2 |
| Sunstead | 13 | 30 | 2 | 2 |

## The show (`MidiClips.show`)

**Opt-in (call).** `MidiClips.show(plan, start)` stays exactly today's
function and result (`{ actions, playAt }`): the plain clips mode and
`midi_clips_check.js` keep calling it so. Only `show(plan, start, { moves,
end })` appends the performance and the end, and returns `{ actions, playAt,
t0, end }`, the performed part after the loading actions. `t0` = `playAt` +
23 ms, the artist's start latency (the core sends MIDI Start 20.3 ms after
the PLAY press; a few ms do not move a press across a bar line, and the
check reads the real Start). `end` = the device's stop plus 1.5 s. No new
page file: the code goes in `midi_clips.js`.

**When the hand presses (call).** Each move is two changes (mute, return),
each landing on a downbeat `B`. A change is a list of gestures:

- Its first press is on a beat of bar `B−1` plus a jitter of 40–160 ms,
  seeded by the song title (deterministic, as the artist's `s.rnd`): beat 4
  for a change on one page; beat 2 when the change turns the page (both the
  drop's mute and its return); beat 2 of the muted bar for the return of a
  one-bar move, so the hand hovers over the key (0.82 s after the mute press
  at 146 BPM, under the hands' 1.5 s idle limit).
- Keys on one page: at most two in one gesture, pressed 12 ms apart, and
  only when at most two columns apart (one hand); any other key is a further
  gesture 150 ms after the last press. No song has a third key now.
- A page turn: the current page's keys, then TRACK ±8 detents (32 edges, 3
  ms apart, 13 ms between detents, about 0.2 s), starting 100 ms after the
  last press on the old page (the other hand turns while the pressing hand
  lifts), then the new page's keys 80 ms after the last edge. A drop's mute:
  kick, turn to page 2, bass; the artist stays on page 2 for the two muted
  bars; its return: bass, turn to page 1, kick. The set starts and ends on
  page 1.
- Fit: the last press of a change must be at least 60 ms before `B` (the
  press is what counts; the panel reports a MUTE 5–20 ms after it). If not,
  the change starts one beat earlier, never before the mute it returns from
  has landed (+100 ms), so no key is pressed while its change waits; if it
  still does not fit, the whole move is not made, its mute included. At the
  Twisted tempos everything fits: with the jitter the last press is 243–293
  ms before its downbeat.
- **Captions** (`say`, Russian, one per change): what goes and why, e.g.
  "MUTE Kick на четвёртую долю: бочка уходит на такт перед новой частью",
  "Страница 2 (TRACK): бас", "MUTE Kick и Bass вернутся с сильной долей".

**The end** — the owner decides (open question 1). Both options give the
same moves, and the last return is at least 8 bars before the stop, so a
stop never cancels a waiting return and the live mutes are 0 at the end.
When the chain holds the whole song (Contains LSD, A Magician…, Are You
Sponged) both are the same: the chain stops by itself.

- **Option B, shorter chain (recommended).** For the performed mode the
  store keeps only the chain rows up to `setEnd` (Sham: 251 rows of 256, the
  last row's repeats cut), and `SONG_END_STOP` stops the device exactly on
  that downbeat. No press, nothing timed; the check asserts the exact bar.
  The plain mode keeps its 256 rows.
- **Option A, timed PLAY.** The chain stays at `plan.played` (256 rows);
  the artist presses PLAY 60 ms before the downbeat of `setEnd`. On the core
  the Stop goes out 34–36 ms before that downbeat on every song, and the
  last 1/16 step's tail is cut by as much. It is a visible last gesture, but
  it depends on the debounce and the UI pass, and the check must assert the
  Stop's time.

## The page (`index.html`, `app.js`)

**Where (call).** A fourth choice in "Воспроизведение": **"Клипы на падах ·
артист играет MUTE"**, offered only when `device.features()` reports
`quantMutes` (with immediate MUTE every change would land early). It serves
every library song, imported ones too. A song with no moves still gets the
performed show (`padsRun.performed` set: the hands do the loading) and the
log says there is no MUTE part, and why (no kick on page 1, no 4/4 stretch).

**`startMidiSong`, in this order (call)** — the `pads` branch, for the new
mode:

1. Before `await rig.midiParts(…)`: `buildDuringLoad = request` (the launch
   serial). If the box is ticked and the hands are not built,
   `await Promise.race([loadHands(), 4 s])`. The phase is `midi-loading`:
   the device is booted and stopped, nothing is scheduled, so the 0.3 s
   build is silent. Then `buildDuringLoad = 0`, and `request !==
   launchSerial` returns as today.
2. `mutes(plan)`, `run = MidiClips.show(plan, device.now() + 300000,
   { moves, end })`; `show = { actions, prepEnd: run.playAt − 200 ms,
   t0: run.t0, barUs, end: run.end }`; `Artist.schedule`.
3. `padsRun = { …, performed: true }`, then `phase = 'prep'`,
   `refreshMidiParts()`, `generation++`, and only then `planHands()`, whose
   test `phase !== 'prep' && phase !== 'live'` stays.

**The gates:**

| Site | Today | Then |
|---|---|---|
| `handsShown` | `… && !padsRun` | `… && !(padsRun && !padsRun.performed)` |
| `planHands` | returns on `padsRun` | returns on `padsRun && !padsRun.performed` |
| `buildable` | idle, or a preparation with no `padsRun` | also `midi-loading` while `buildDuringLoad === launchSerial` |
| `startMidiSong` | no `generation++`, no `planHands()` | both, as step 3 |

A build that has not finished by step 2 does not start later in this show
(`buildable` is false again), so the show gets the ring for every press and
the hands are there from the next show. Without the box ticked the ring
marks the presses, as it does for the loading moves today.

**Stop, take-over, reboot.** As today for a clips run. "Стоп" ends the
show (`endShow`: scheduled presses cleared, the plan dropped) and presses
PLAY, so the chain stops; live mutes stay set. A click on the panel ends the
show the same way, but the chain plays on with the mutes the artist left (a
take-over inside the drop leaves kick and bass muted; the MUTE keys are the
user's now). A reboot clears all of it.

## Checks

**`sim/test/midi_live_check.js` (new; in `sim/check.sh` after
`midi_clips_check.js`; skipped with a message when the core lacks
`quantMutes`).** All eight Twisted songs, each from boot, the end option
chosen:

1. Plain mode unchanged: `mutes(plan)` leaves the plan as it was (a deep
   compare); `show(plan, s)` has no `t0`/`end` and its actions are the first
   ones of `show(plan, s, opts)`; `store(plan)` is the same bytes as before
   `mutes()` ran. `midi_clips_check.js` is not edited and stays the
   regression for the plain mode.
2. The script: Sham's moves are the table above (a fixture, so the count's
   parity and the rules cannot drift); for every song, every muted track
   plays in its press bar, muted bars and return; every group track that
   plays in that span is muted; `change ≤ 2` in muted bars; one stretch; no
   tie at the edges; the spacing; the last return 8 bars before the end.
3. The actions: after PLAY only `say`, `mute` (column 0–7), `edge` on
   encoder 0 in whole detents (±8, summing to 0 by the end) and, under
   option A, the final `play`; at most two MUTE downs within 100 ms, at most
   two columns apart; no press of a track between its previous press and
   that press's landing (from the actions: the core exposes no pending bits,
   and a cancelled change would fail 4 too).
4. The landings: 25 ms before every landing downbeat the mutes are the
   ones before it, 40 ms after it the new ones; at the middle of every bar
   the mask is the script's. `inspect().mutes` is an `int` (tracks 0–31),
   enough for pages 1–2.
5. What sounds: the note starts from the MIDI Start to the end are the
   plan's minus the cells of muted (track, bar), by step, channel and note
   (the multiset of `midi_clips_check.js`, where a re-struck tie counts as
   an extra), and nothing from the stop's downbeat on.
6. The end: stopped on the planned downbeat (B: exactly; A: the Stop 0–60
   ms before it), live mutes 0, no overruns, MIDI overflow or fatal.
7. Page wiring, read from `app.js`'s source as `hands_unit.js` reads its
   panel (brittle by nature: it names the edit to make when it fails): in
   the performed branch `performed: true`, then `phase = 'prep'`, then
   `generation++`, then `planHands()`, in that order and after
   `Artist.schedule`; `show.end` and `show.t0` from `run`; the three gate
   expressions of the table; `buildDuringLoad` set before and cleared after
   the `midiParts` await.

Cost: 2.5 s for all eight in the prototype; with the plain-mode compares
and the source reads, about 4 s.

**`sim/test/hands_check.js`.** After the artist's shows, a second loop
builds the eight performed shows from the MIDI files (`MidiSongs`,
`MidiClips`, the same jitter) and runs all of (1)–(6) on each, with the live
set sampled every 20 ms. The whole show counts as live: it plays at speed 1,
loading included, so the check takes the first action's time as `prepEnd`
(the page keeps `playAt` − 200 ms for its phases). The prototype posed all
eight so: every check passed, peak 1.95 m/s, no crossings or palm overlaps,
3.6–4.5 s a show. That adds about 35 s to the default run's 70 s (M1);
`HANDS_FULL=1` runs them too.

## Docs

- `sim/README.md`, "Clips on the pads": the performed mode, the moves and
  their rules in a short list, the meter rule, the end, the new check; and
  "five of the eight Twisted songs stop 4–79 bars before their end" becomes
  6–99 (Sham is 99). "The hands": "They play the artist's sets only: not …
  nor its loading as the device's clips" becomes: they play the artist's
  sets and the performed clips mode; the plain clips mode, a MIDI Song and
  the PAD phrases still have none. The build paragraph gains: in the
  performed mode the hands may be built while the song loads (the device
  stopped), else that show gets the ring. The `hands_check` paragraph names
  the eight performed shows and the new cost.
- `docs/superpowers/specs/2026-11-17-sim-hands-design.md`: this reverses a
  recorded decision, so it gets an amendment, not an edit of history. Under
  "As built", after the "Merge of master's SH-101 voices…" item, a new item:
  "2026-11-25, `2026-11-25-sham-live-mutes-design.md`: the hands also play
  the performed clips mode ('Клипы на падах · артист играет MUTE'): its
  loading and MUTE performance are one show at speed 1 with a plan, and the
  hands may be built while it loads, before anything is scheduled. The
  plain clips mode and a MIDI Song still get none, and the build still never
  runs while either plays." That item also corrects the stale "12 script
  tags" there: `index.html` has 13, which is `PAGE_FILES` less
  `LOADED_BY_APP`.
- `sim/publish.sh`: no change (no new page file). `sim/check.sh`: one line
  and its header comment.
- This spec: "Status", then "As built" as the others.

## Prototype

Scratch scripts outside the repository implemented the rules above (meters
read from the files), built the shows with the seeded jitter, the gestures
and the page turns, and ran them on the wasm core (`sim/build.sh web` in a
worktree, emsdk linked from the main checkout) under both end options and
under "let the chain run out". On all eight songs: every landing exact at
−25 / +40 ms, the mask the script's at the middle of every bar, the notes
the plan's minus the muted cells with nothing after the end, the stop where
planned, live mutes 0, no overrun or fatal. `hands_check.js`'s checks
passed on all eight shows posed and sampled every 20 ms. Two timings were
tried and rejected: starting the page turn 250 or 450 ms after the last press
gave it to the pressing hand, which then peaked at 3.3–3.9 m/s; 100 ms gives
it to the other hand. Nobody has listened to the moves yet.

## Risks

- The moves are placed by counting tracks, not by ear; listen to Sham
  before merging. The knobs and option C exist.
- The drop is the busiest moment: two presses and a 32-edge turn inside
  about three beats, twice. It passed with room (1.95 m/s against 4), but
  the hand choice depends on the turn's timing (above): a builder who moves
  it must re-run the hands check.
- A take-over inside a move leaves tracks muted on a device that plays on.
- The meter rule needs `MidiSongs.read` to report FF 58 events; a file with
  a wrong or missing meter is treated as 4/4.

## Open questions for the owner

1. **How the set ends** (on Sham: after bar 257, the last phrase line inside
   the chain, instead of 262). Option B, the device's stored chain is cut at
   that line and stops by itself (recommended: exact, nothing timed). Option
   A, the artist presses PLAY just before that line, and the chain itself
   still holds 262 bars. Or a third: let the chain run out at 262,
   mid-phrase, as first described.
2. **Hands not built at "Сыграть".** Default now: the page may build them
   while the song loads (silent, the device stopped), up to 4 s; if that is
   not enough, that one show gets the ring.
3. **Derived moves only, or corrections by ear later?** Default: derived
   only until someone has listened.
4. **Density.** Default: keep the spacing (8 bars, 16 before a hats or snare
   move). Sham gets 10 moves in 7 minutes, the longest gap 54 bars.

## Review changes

DeepSeek (D1–D22) and Grok (G1–G10), each checked against the code, and the
musical ones re-run on the core with the real jitter and press scheduling.

Confirmed and fixed:

- D1, G1: `planHands()` while the phase is `midi-loading` returns, and with
  the new `handsShown` there would be neither hands nor ring. Fixed by the
  order in `startMidiSong` and the source check 7.
- D2, G4 (its page half), D20: the page took `end` and `t0` from the plan,
  not the show. `show()` returns both; the page uses them.
- D4, G2: muted tracks could be silent in the press bar (Sham bars 125 and
  173, LSD, Sunstead). The press bar now counts; Sham's hats at 130, 146 and
  178 are gone.
- D5, G2: partial drops (one bass voice of two). The whole-group rule.
- G2: a hats window could cross a section change. `change ≤ 2` in the muted
  bars (it removed 0–9 moves a song). Grok's other test, the same clip on
  every bar of the window, is not adopted: 52 of 93 moves fail it only
  because a part alternates variations bar by bar.
- D6, G6: Are You Sponged has 6/4 bars at chain bars 119 and 126.5 (FF 58
  events). The meter rule and stretches; it keeps 11 moves outside them.
- D7: the plain mode must not change. `show()` is opt-in; check 1.
- D8, G4: the timed PLAY's margin and the ambiguous end. `setEnd` is
  defined once; the end is open question 1 with both options measured.
- D9: "`buildable` unchanged" contradicted the recommended build. The build
  during `midi-loading` is now the default, with its gate.
- D10: no check covered the page. Check 7.
- D11: check 3 allows `say` and bounds the column and the encoder.
- D12: no binding for pending mutes. Check 3 reads the actions instead.
- D13: the groove count starts at 0, and the Sham table is a fixture.
- D14, G10: `change(0)` is 0 and bar 0 does not count for the phase.
- D15, G3: groups of keys. At most two keys a gesture, two columns apart.
- D16: the margin is on the press, not the release.
- D17, G5: check costs restated; all eight shows posed by default.
- D18: README's "4–79 bars" is stale (Sham is 99): fixed in Docs.
- D19: the new check is skipped without `quantMutes`.
- D21: each move carries `kind`, `press`, `start`, `end`, `tracks`.
- D22: the gate is excluded by role, not as track 0.
- G3 (in part): the drop's return was not stated to start on beat 2. Both
  of a drop's changes now do; with the 160 ms jitter the last press is still
  at least 243 ms early. The page turn's start was tuned (Prototype).
- G7: the pad-moves-focus sentence corrected.
- G8: zero moves still sets `performed`; `t0` uses the 23 ms constant.
- G9: live mutes are 0 at the end because every return precedes the stop,
  not because Stop clears them; stated.
- G10: `index.html` has 13 script tags, not 12.

Refuted:

- D3: ties at the Orpheus (bar 42) and Sunstead (bar 53) drops. The cells
  there carry TIE because a long note starts on the downbeat and ties
  forward; the step before is not tied, so nothing is re-struck. The test is
  for a continuation (the previous step tied to the same note), and no move
  of the eight songs has one.
- G3 (in part): "one beat is not enough for the drop's return". The
  prototype already started both of a drop's changes on beat 2; one beat
  was only the fallback text. With the real jitter all drops fit.
- G4 (in part): "the return on bar 250 survives only with `plan.played` as
  the end". With `setEnd` = 257 (0-based) the skip is `L > 249`, and that
  return is `L = 249`.
- G5 (in part): Grok asked to keep `midi_clips_check`'s tie rule rather
  than a cell subtraction. Check 5 uses that multiset, in which a re-struck
  tie is an extra, so both hold.
