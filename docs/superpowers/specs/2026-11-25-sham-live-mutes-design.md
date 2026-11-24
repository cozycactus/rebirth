# Live MUTE Over the Clips Song ("It's a Sham, Man") — Design

Date: 2026-11-25. Status: draft for review. The feature and its shape are
the owner's choice, made in conversation: the hands load a Twisted song laid
out as the device's clips ("Клипы на падах"), switch to Song mode, and while
the device plays the chain itself they perform on top of it with MUTE,
taking tracks out and bringing them back at breaks and drops. No pad or
SCENE launches (Song mode refuses them). The owner was told the song stops
at bar 262 of 361, because the chain holds 256 rows (`MAX_SONG_ENTRIES`).
The choices this design makes are marked **(call)**. The numbers below come
from a throwaway prototype run on the wasm core built from origin/master
86975bc (see "Prototype").

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
2. The hands play all of it (loading moves and MUTE performance) when they
   are built; the ring marks the same presses when they are not.
3. The mute script is derived from the laid-out song, works for all eight
   Twisted songs and is tuned and checked on "It's a Sham, Man".
4. Every MUTE change lands exactly on the downbeat it is meant for, and what
   sounds is exactly the chain minus the muted (track, bar) cells.
5. Checks in Node against the wasm core and the hands' whole-show check,
   both in `sim/check.sh`, a few seconds each.

## Non-goals

- Any firmware change. Everything used (Song mode, quantised MUTE, paging
  with TRACK) is on master.
- Pad or SCENE launches, synth knob moves, filter sweeps, tempo changes.
- Longer songs: the 256-row chain limit stays.
- A hand-written mute part per song (see "Approaches considered").
- The plain "Клипы на падах" mode and the full arrangement and MIDI Song
  modes: unchanged, still without the hands.

## Anti-goals (a failure even if it works)

- A move that empties the groove: kick, snare and hats out at once, or a
  mute held across a section change so the new part never arrives.
- A MUTE pressed on a track that is silent then (a key that does nothing).
- A press on a key whose change still waits: by the quantised-mutes rule a
  second press cancels it, so the move would silently not happen.
- The hands' 0.2–0.3 s build running while the show runs. (After a
  take-over the chain may still sound on an idle page, where the build is
  allowed, as the README already accepts for an artist's set.)

## Firmware facts this relies on (verified on 86975bc)

- Song mode: `transport_launch` returns false unless the mode is Pattern
  (`src/sequencer.c:1693`), so pads and SCENE do nothing. MUTE goes through
  `change_mutes` (`:1734`) with no mode check.
- Quantisation: `launch_quant` is 1 BAR at power-up (`src/main.c:20`), and
  `apply_due_mutes` reads TRACK LOOP as BAR in Song mode (`:618`). A MUTE
  pressed anywhere in a bar lands on the next global step that is a multiple
  of 16. Every clip is 16 steps, so global bars are chain bars.
- Persistence: the live mute is one bit per track (`g_seq.track_mutes`), not
  part of a chain row, so a mute holds while the chain changes that track's
  clip and the new clip is heard once it is unmuted. The save image's stored
  mutes are all clear.
- Stop (PLAY, a song that ends) drops waiting changes but keeps live mutes.
  The image ends with `SONG_END_STOP`: the chain stops after its last row.
- Paging: in the Session view a MUTE key stands for `page*8 + column`, the
  page being the focus track's (`panel_first_track`, `src/ui.c:518`). TRACK
  walks one track per detent; the laid-out drums are `TRACK_TYPE_DRUM`, not
  kit tracks, so no lane stops. After the loading moves the focus is track
  0: page 1 is tracks 0–7, and 8 detents reach page 2.
- Traps: a short VALUE press in the Session view cycles the launch
  quantisation, and a TRACK long press steps the bank. The performance uses
  neither.
- A MUTE key held 500 ms makes the next keys a group (`PAD_COPY_HOLD_MS`).
  Every press here is shorter (80 ms), and two keys pressed 12 ms apart are
  two plain presses that both land on the same bar line.

## Approaches considered (the mute script)

**A. Written by hand for Sham.** The best result for one song, if someone
listens bar by bar. But it is a score nobody can check against the song
except by ear, it does not carry over to the other seven songs, and it goes
stale if the layout in `midi_clips.js` ever changes.

**B. Derived from the plan (chosen).** The layout already knows, per track
and bar, what plays. Section changes show up as bars where many tracks start
or stop at once; the phrase grid is where they cluster. A few classic moves
placed by rules give a performance for any song in the library, and the
check can verify each move's premise (the track plays there, the move does
not cross a section change).

**C. Derived plus per-song overrides.** B with a table of corrections tuned
by ear, like `parts.json` for sounds. Worth it only after someone has
listened to B; the hook is easy to add later (an optional list of moves to
drop or add per song title). Left as an open question.

## The mute script (`MidiClips.mutes(plan)`)

Pure and deterministic: input the plan, output `{ phase, groups, moves }`.
Bars are 0-based chain bars below `plan.played`; the text quotes 1-based bar
numbers as the page and the log show them.

**What plays.** `on(t, b)`: track `t` has an active step in bar `b`.
`change(b)`: the number of tracks, Midi Gate aside, whose `on` differs
between bars `b-1` and `b`.

**Phrase grid (call).** The phase `φ` in 0–7 that maximises the sum of
`change(b)` over `b ≡ φ (mod 8)`. Phrase lines are the bars `≡ φ`. On Sham
φ is 1: the files start with a pickup bar, and its sections begin on bars
2, 10, … (1-based: the kick enters on bar 34, the hats on 42).

**Groups (call)**, by role and General MIDI note, only on pages 1–2 (tracks
0–15), each group the tracks of that kind there:

| Group | Tracks | Sham |
|---|---|---|
| kick | role `kick` on page 1 | 1 |
| hats | drum notes 42, 44, 46 on page 1 | 5 Hats CH, 6 Hats OH |
| snare | drum notes 38, 40 on page 1, names without "fill" | 2 Snares |
| bass | role `bass` on page 2 | 10 Bass |

The Midi Gate track (0 in seven songs) is never touched: muting it would cut
every gated voice at once. Every Twisted song puts its kick on track 1, its
hats and snare on page 1 and its bass on page 2 (tracks 9–14).

**Moves (call).** Each move mutes a group from bar `start` and brings it
back on the phrase line `L` (its return, `end = L`). The phrase lines are
taken in order, at most one move each, by these tests:

1. Skip `L` if it is less than 8 bars after the previous move's return, or
   later than 8 bars before the end of the set (below).
2. **Section line**, `change(L) ≥ 4`, and the kick plays in bars `L−2`,
   `L−1` and `L`:
   - **drop** if `change(L) ≥ 8`, the bass plays in `L−2` to `L`, and no
     drop returned in the 64 bars before `L`: kick and bass muted in
     `L−2` and `L−1`;
   - else **kick roll**: the kick muted in `L−1`.
3. **Groove line**, `change(L) ≤ 2`, at least 16 bars after the previous
   move's return, the kick playing in every bar from `L−8` to `L`. Groove
   lines that pass these tests are counted; the count advances whether or
   not a move is then placed. On an even count: **hats cut** (`L−4` to
   `L−1`) if the hats play in `L−4` to `L`, else **snare out** (`L−2`,
   `L−1`) if the snare plays in `L−2` to `L`. On an odd count: snare out if
   it can, else hats cut if it can. (On Sham this gives hats, hats, hats,
   snare, hats: the snare is often not steady.)
4. Any other line: no move.

- So a group plays in every bar it is muted in and on its return line, and
  the kick roll also needs the kick in the bar before: a move never sits on
  the song's own breaks, nor holds a track the song is about to drop.
- The tracks muted are those of the group that play in the muted bars (Sham
  bar 174: Hats CH only, the OH is silent there).
- One move at a time: a return comes at least 8 bars after the previous
  return (16 for a groove line), so a kick roll or drop can begin 6 bars
  after the previous move returned (Sham: hats back on 82, drop from 88).
- A move whose `start` or `end` downbeat falls on a tie continuation of a
  group track is dropped. None of the eight songs has one on either side;
  what the firmware sounds on un-muting into a tie was not tested, so it is
  avoided rather than specified.
- The presses of a change (below) must all be released 40 ms before its
  downbeat. If at the song's tempo they are not, they start a beat earlier;
  if still not, the move is dropped. At the Twisted tempos (136–147 BPM)
  nothing is moved or dropped; a fast imported song may lose moves.
- The spacing (8 and 16 bars), the change thresholds (2, 4, 8) and the drop
  interval (64 bars) are the tuning knobs, named constants at the top.
- The prototype asserted these on all eight songs: every move's groups play
  in the bars named above, every muted track in some muted bar, no track 0
  or past 15, the spacing, nothing in the last 8 bars, no tie at either
  edge.

**The script on Sham** (1-based bars; "back" is the downbeat it returns on):

| Bars muted | Back | Move | Tracks |
|---|---|---|---|
| 41 | 42 | kick roll | 1 |
| 57 | 58 | kick roll | 1 |
| 78–81 | 82 | hats cut | 5, 6 |
| 88–89 | 90 | drop | 1, 10 |
| 97 | 98 | kick roll | 1 |
| 126–129 | 130 | hats cut | 5, 6 |
| 142–145 | 146 | hats cut | 5, 6 |
| 160–161 | 162 | snare out | 2 |
| 174–177 | 178 | hats cut | 5 |
| 209 | 210 | kick roll | 1 |
| 233 | 234 | kick roll | 1 |
| 248–249 | 250 | drop | 1, 10 |

12 moves, 34 MUTE presses, 4 TRACK turns over 257 bars (7 min 2 s at 146
BPM). Nothing before bar 41 (the intro has no kick until bar 34); the
longest stretch without a move is bars 178–209 (31 bars, 51 s), across the
song's own breakdown. The other seven songs get 7–17 moves, 20–38 MUTE
presses, at most 3 in one bar (Orpheus), 0–4 TRACK turns.

## The show

`MidiClips.show(plan, start, { moves })` **(call: in `midi_clips.js`, no new
page file)** returns the loading moves it returns today, then the
performance and the end, as one list of actions in `Artist.schedule`'s form
(`mute` by physical column, `edge` on encoder 0, `play`, `say`), plus
`playAt` and `end`. `t0` for bar arithmetic is the MIDI Start, which the
core sends 20.3 ms after the PLAY press (the artist's 23 ms constant is near
enough for presses, not for a check: the check reads the Start).

**When the hand presses (call).**

- A change is pressed on beat 4 of the bar before the one it lands on,
  40–160 ms after the beat (seeded jitter, deterministic per song, as the
  artist's `s.rnd`). With BAR quantisation anywhere in that bar would do;
  beat 4 reads as "arming the change", and the MUTE light shows it waiting.
- The un-mute of a one-bar move is pressed on beat 2 of the muted bar, so
  the hand stays over the key (0.82 s after the mute press at 146 BPM). On
  beat 4 the gap is 1.64 s, past the hands' 1.5 s idle limit, and the hand
  left and came back inside one bar in the prototype.
- No key is pressed while its own change waits: the next press of a track
  comes after its landing. The two keys of a group are pressed 12 ms apart
  (a one-hand pair when at most two columns apart, as the hats are).
- A move spanning both pages (the drop) starts on beat 2: kick on page 1,
  TRACK +8 detents (32 edges, about 0.2 s), bass on page 2. The artist stays
  on page 2 for the two muted bars; the return is bass, TRACK −8, kick. Every
  other move is on page 1, where the set starts and ends.

**Captions** (`say`, Russian, one per change): what goes and why, e.g.
"MUTE Kick на четвёртую долю: бочка уходит на такт перед новой частью",
"Страница 2 (TRACK): бас", "MUTE Kick и Bass вернутся с сильной долей".

**The end (call, open question 1).** When the chain holds the whole song
(Contains LSD, A Magician…, Are You Sponged) the chain stops by itself after
its last row and the artist only watches. When it is cut short (Sham and
four others) the artist presses PLAY 60 ms before the downbeat of the last
phrase line inside the chain, so the set ends on a phrase line instead of
wherever row 256 runs out: on Sham after bar 257, five bars before the
chain's own stop after bar 262. With the 20 ms debounce the stop lands about
40 ms before the downbeat; in the prototype nothing sounded from that
downbeat on. `end` is that stop (or the chain's) plus 1.5 s, as today.

## The page (`index.html`, `app.js`)

**Where (call).** A fourth choice in "Воспроизведение": **"Клипы на падах ·
артист играет MUTE"**, next to the plain one. It is offered for every song
of the library, imported ones too: when `mutes()` finds no move (no kick on
page 1, say) the show is the plain loading show and the log says that no
moves were found. "Сыграть" starts it as it starts the other MIDI modes.

**`startMidiSong` (call).** The `pads` branch takes the new mode as well:
the same plan, store, voices and loading, then `MidiClips.mutes(plan)` and
`show(plan, …, { moves })`; `padsRun.performed = true`; `generation++`
before scheduling, so nothing of the last show's hands carries over; and
`planHands()` once `show` is set. The log line adds the number of moves and,
when the set ends early, the bar it ends on.

**The hands' gates.** `padsRun` also routes the channel voices and outlives
the song, so it stays; a flag on it says whether this run is performed:

| Site | Today | Then |
|---|---|---|
| `handsShown` | `… && !padsRun` | `… && !(padsRun && !padsRun.performed)` |
| `planHands` | returns on `padsRun` | returns on `padsRun && !padsRun.performed` |
| `buildable` | idle, or a preparation with no `padsRun` | unchanged |
| `startMidiSong` | no `generation++`, no `planHands()` | both, for the performed mode |

`buildable` unchanged is what keeps the rule: the build never runs during a
clips run, performed or not. So the hands play this show only if they were
built before "Сыграть" (on the idle page, which they are within a second of
ticking the box); otherwise this show gets the ring for every press, and
the hands are built when it ends, as for a skipped preparation (open
question 2). Without the box ticked the ring marks the presses, as it does
for the loading moves today.

**Stop, take-over, reboot.** As today for a clips run. "Стоп" ends the
show (`endShow`: scheduled presses cleared, the plan dropped) and presses
PLAY, so the chain stops; live mutes stay set. A click on the panel ends the
show the same way, but the chain plays on with the mutes the artist left (a
take-over inside the drop leaves kick and bass muted; the MUTE keys are the
user's now). A reboot clears all of it.

**Older cores.** The option is offered only when `device.features()`
reports `quantMutes`, as the page picks scores by feature: with immediate
MUTE every change would land where the press is, a beat early. The comments at the three gates say why
the performed mode is the one clips run the hands play.

## Checks

**`sim/test/midi_live_check.js` (new, in `sim/check.sh` after
`midi_clips_check.js`).** All eight Twisted songs, each from boot: write the
store, schedule the whole show, run to `end`.

1. The script: Sham's moves are the table above; for every song the
   invariants the prototype asserted (see "Moves") and at most 3 MUTE
   presses in a bar.
2. The actions: after PLAY only `mute`, `edge` on encoder 0 (whole detents,
   ±8 in all) and the final `play`; no press of a track whose change waits.
3. The landings: at 25 ms before every landing downbeat the live mutes are
   the ones before it, at 40 ms after they are the new ones (the artist
   check's rule); at the middle of every bar the mask is the script's.
   `inspect().mutes` is an `int`, tracks 0–31: enough, as moves stay on
   pages 1–2.
4. What sounds: the note starts from the MIDI Start to the end are the
   plan's minus the cells of muted (track, bar), by step, channel and note
   (the multiset comparison of `midi_clips_check.js`), and nothing from the
   stop's downbeat on.
5. The end: the transport is stopped, at the planned bar (PLAY) or after
   the chain's last row; live mutes 0; no overruns, MIDI overflow or fatal.

Cost: 0.4 s per song in the prototype, about 4 s for eight.

**`sim/test/hands_check.js`.** A second loop after the artist's shows
builds the eight performed shows from the MIDI files (`MidiSongs`,
`MidiClips`) and runs (1)–(2) on all eight, (3)–(6) on Sham with the live
set sampled every 20 ms, every one of them with `HANDS_FULL=1`. The whole
show counts as live for (5)–(6): it plays at speed 1, loading included, so
the check is given the first action's time as `prepEnd` (the page keeps
`playAt − 200 ms` for its phases). The prototype posed all eight this way:
every check passed, peak 2.07 m/s, no crossings or palm overlaps, 3.5–6.3 s
a show. Default cost about 5 s; `HANDS_FULL=1` about 35 s more.

## Docs

- `sim/README.md`, "Clips on the pads": the performed mode, the moves and
  their rules in a short list, the early end of a cut-short chain, the new
  check. "The hands": "They play the artist's sets only: not … nor its
  loading as the device's clips" becomes: they play the artist's sets and
  the performed clips mode; the plain clips mode, a MIDI Song and the PAD
  phrases still have no hands. The build rule paragraph gains one sentence:
  a performed clips show started before the hands are built gets the ring.
  The hands_check paragraph names the eight performed shows.
- `docs/superpowers/specs/2026-11-17-sim-hands-design.md`: this reverses a
  recorded decision, so it gets an amendment, not an edit of history. Under
  "As built", after the "Merge of master's SH-101 voices…" item, a new item:
  "2026-11-25, `2026-11-25-sham-live-mutes-design.md`: the hands also play
  the performed clips mode ('Клипы на падах · артист играет MUTE'): its
  loading and MUTE performance are one show at speed 1 with a plan. The
  plain clips mode and a MIDI Song still get none, and the build still never
  runs while either loads or plays." The sentence "A MIDI Song has no artist
  and gets no plan" stays true and stays.
- `sim/publish.sh`: no change (no new page file; `index.html` keeps 12
  script tags). `sim/check.sh`: one line, and its header comment.
- This spec: "Status", then "Review changes" and "As built" as the others.

## Prototype

Scratch scripts outside the repository derived the moves for all eight
songs, built the shows and ran them on the wasm core (`sim/build.sh web`
in a worktree, emsdk linked from the main checkout): the mask was the
script's at the middle of every bar, all 24 Sham landings were exact at
−25 / +40 ms, the notes were the plan's minus the muted cells on all eight
songs with nothing after the end, and `hands_check.js`'s checks passed on
all eight shows posed and sampled every 20 ms. It confirmed the focus on
track 0 after LOAD (page 1) and the page turn by 8 detents. Its presses
were at a fixed 60 ms after the beat (the jitter was not tried), and it
did not listen: nobody has heard the moves yet.

## Risks

- The moves are placed by counting tracks, not by ear. A kick roll can land
  where the song already thins out in a way the count misses. Mitigation:
  listen to Sham before merging; the knobs and option C exist.
- The drop's page turn is the busiest moment: four presses and a 32-edge
  turn inside about three beats. It passed the hands' limits with room
  (peak 1.95 m/s against 4), but it is the first thing to look at on the
  page.
- A stop or take-over inside a move leaves tracks muted on a device that
  plays on. Consistent with "the device stays as the artist left it", but
  it may surprise.

## Open questions for the owner

1. **The end of Sham.** Stop with PLAY on the last phrase line (after bar
   257, recommended: it ends on a phrase), let the chain run out (after bar
   262, mid-phrase, as you were told), or shorten the stored chain to the
   phrase line so the device stops there by itself with no press.
2. **Hands not built at "Сыграть".** As designed, that show gets the ring.
   Alternative (recommended): allow the build in the `midi-loading` phase of
   the performed mode, before anything is scheduled; the device is not
   playing then, so the 0.3 s stall is silent.
3. **Derived only, or overrides by ear later (option C)?** Recommended:
   derived only now; add overrides if listening finds bad spots.
4. **Density.** 12 moves in 7 minutes; returns at least 8 bars apart (16
   before a hats or snare move), the longest gap 31 bars. More moves
   (spacing 4 and 8) or fewer?

## Review changes

None yet: this draft has not been reviewed.
