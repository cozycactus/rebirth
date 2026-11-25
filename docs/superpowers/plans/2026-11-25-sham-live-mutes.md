# Live MUTE Over the Clips Song — Implementation Plan

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a fourth playback choice, "Клипы на падах · артист играет MUTE": the
artist loads a library song as the device's clips, starts the chain in Song
mode and performs it with quantised MUTE moves derived from the layout; the
hands play all of it.

**Spec:** `docs/superpowers/specs/2026-11-25-sham-live-mutes-design.md`
(approved 2026-11-25; read it first, this plan does not repeat its reasons).

**Branch:** `claude/sham-live`. `sim/emsdk` is linked from the main checkout
only to build; never `git add -A`, add files by name.

## Global constraints

- Classic scripts, no `fetch` for code, no new page file: `publish.sh` and
  `index.html`'s 13 script tags stay.
- The plain clips mode is unchanged: `MidiClips.show(plan, start)` and
  `MidiClips.store(plan)` return exactly what they return today, and
  `sim/test/midi_clips_check.js` is not edited.
- No firmware change (`src/`, `include/` untouched).
- Each task: its check first, seen failing, then the code; `sim/check.sh`
  passes before the last commit.

## Tasks

### 1. Meters and the mute script

- [ ] `sim/test/midi_live_check.js`, part 1 (no core): every Twisted song's
  `song.meters` (Are You Sponged: 6/4 at chain bars 119 and 126.5); Sham's
  `MidiClips.mutes(plan)` equals the spec's table (fixture); the invariants
  of the spec's "Moves" for all eight; `mutes()` leaves the plan unchanged.
- [ ] `midi_songs.js`: `read()` returns `meters` from FF 58 events.
- [ ] `midi_clips.js`: `plan()` carries `meters`; `mutes(plan)` as the spec.
- [ ] Run the check; commit.

### 2. The performed show and the shortened chain

- [ ] Check part 2 (core): the plain `show`/`store` unchanged; the
  performed show's actions (kinds, columns, detents, gesture rule, no press
  while waiting); on the core: landings at −25/+40 ms, the mask mid-bar,
  the note starts, the exact stop, live mutes 0, no overrun or fatal;
  skipped without `quantMutes`. Add it to `sim/check.sh`.
- [ ] `midi_clips.js`: `show(plan, start, { moves, bars })` with the seeded
  jitter, gestures, page turns and captions; `store(plan, { bars })`.
- [ ] Run `midi_clips_check.js` and the new check; commit.

### 3. The page

- [ ] Check part 3: `app.js`'s source wiring (the order in the performed
  branch, the three gates, `buildDuringLoad`, `show.end`/`t0` from `run`).
- [ ] `index.html`: the fourth option. `app.js`: the option offered by
  `quantMutes`; `startMidiSong` in the spec's order; `handsShown`,
  `planHands`, `buildable`.
- [ ] Commit.

### 4. The hands' check

- [ ] `hands_check.js`: the eight performed shows, (1)–(6) with the whole
  show live; `sim/check.sh` comment.
- [ ] Run it; commit.

### 5. Docs and verification

- [ ] `sim/README.md` ("Clips on the pads", "The hands", 6–99 bars), the
  hands spec's amendment (and its 13 script tags), this spec's "As built".
- [ ] `sim/build.sh web`, the full `sim/check.sh`, a headless Chrome run
  over HTTP (the performed Sham with the hands: the build while loading, a
  kick roll, the drop with its page turn, no console error; the plain clips
  mode with the ring). Commit.
