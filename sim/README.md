# Simulator

The whole device in a browser page: the firmware from `src/` runs unmodified
on a virtual board, the panel is drawn from what the firmware drives, and a
set of software synths plays what leaves the MIDI wire. A scripted Goa trance
artist can program the device from power-up and play one of eleven songs on it.

The public page, https://cozycactus.github.io/rebirth/, always runs `master`
(see "The public page" below). To run it locally:

```bash
sim/build.sh native        # native core + smoke test, needs only a C compiler
sim/build.sh web           # WebAssembly core and version.js, needs sim/emsdk (see the script)
sim/check.sh               # every check for every score this firmware takes; `quick` for the device check only
node sim/test/artist_check.js       # every song in every arrangement; SONG=3 FORM=sunrise plays one
python3 -m http.server 8765 --bind 127.0.0.1 --directory sim/web   # then open http://localhost:8765
```

`sim/web/core.js` and `sim/web/version.js` are built, not kept in git: run
`sim/build.sh web` first. The page then shows which commit its firmware is
from. `sim/web/index.html` also opens straight from disk: the page uses
classic scripts and the WebAssembly is embedded in `core.js`. From disk a
browser will not load an AudioWorklet module, so the two 303s then run on the
main thread (the same code, but it costs more and can crackle); served over
HTTP they run in a worklet.

The page links to the user manual, `manual-ru.pdf` beside it. On the public
page it is there; in a checkout it is not, unless built: `tools/build_manual_pdf.py`
builds it from `docs/manual-ru.md` on macOS (it reads the system fonts).
Hovering over a control of the panel or the page shows what it does.

## The public page

CI (`.github/workflows/ci.yml`, job Simulator) builds the simulator on every
push and pull request with Emscripten 7.0.0 and runs `sim/check.sh`: the device
check and the artist check (every song in every arrangement) for every score
the firmware takes. A firmware change that breaks the artist's gestures shows
there, on its own pull request. On a push to
`master` it also packages the page with `sim/publish.sh package`: the page,
its scripts asked for with the commit (so a browser never mixes two builds),
and in `source/` the GPL-3.0 corresponding source of `core.js`.

When CI has succeeded on master's tip, `.github/workflows/publish-simulator.yml`
lays that package over the site repository, cozycactus/cozycactus.github.io,
with `sim/publish.sh apply`, and pushes it to `main`; GitHub Pages serves it a
minute later. It writes only the page's own files and `rebirth/source/`, never
the manual or the rest of the site. A failed check publishes nothing, and the
last good page stays. It needs the secret `SITE_DEPLOY_KEY` (a deploy key with
write access on the site repository) and the repository variable
`SIM_PUBLISH=on`; run it by hand to publish master's tip again.

## What is real and what is modelled

| Part | In the simulator |
|---|---|
| Sequencer, UI, MIDI queue, panel input, OLED drawing, LED frames, flash storage, CV/gate (`src/*.c` except the two below) | The firmware itself, compiled to WebAssembly |
| `src/main.c` | Compiled inside `sim/core/sim_main.c`: its `init_defaults()` and `g_seq` are the real ones; its endless loop is repeated by `core0_pass()` |
| `src/rgb_leds.c` (PIO + DMA) | `sim/core/sim_rgb_leds.c`: same interface, same refusal of a frame within 3 ms of the last |
| TCA8418, two MCP23017 | Register models on a virtual I2C bus (`sim/core/hal.c`): key FIFO and overflow, IPOL, directions, output latches, chip reset, bus faults |
| UART | A PL011 with its FIFO off: 320 us a byte, TX interrupt when the holding register empties |
| Timer | One virtual microsecond clock; alarm 0 fires the real `sequencer_timer_isr` |
| OLED | The 1024 bytes of each `ssd1309_flush()` |
| Encoders, PLAY, gates, tempo LED | GPIO levels; an encoder detent is four quadrature edges |
| MIDI IN | Bytes arrive on the UART's one-byte receiver, 320 us apart; a byte that finds the last one unread is lost, as when a flash erase keeps interrupts off |
| MCP4822 | Decoded SPI words |
| Flash | A 2 MiB array; erase and program take typical (not measured) times and stall both cores |

## What it does not show

- **Timing noise.** Interrupts run at the instant they are due, between core 0
  passes. Lock waits, flash cache misses and I2C or SPI transfer times are not
  modelled, so the jitter figures of the real board cannot be read from here
  (see `JITTER_MEASUREMENT.md`).
- **Two cores racing.** Everything runs on one thread. A lock taken twice is
  reported as a simulator error, not as a wait.
- **The core 0 loop rate.** A pass every 1 ms is an assumption (`SIM_UI_PASS_US`).
- **Electrical behaviour.** No bounce, no brown-out, no analogue CV settling.
- **The sound of real gear.** The synths in `sim/web/rig.js` stand in for a
  909, a bass, a lead and so on; they are not part of the device and are not
  tuned by ear. The two 303s (ACID 1 with the saw, ACID 2 with the square) are
  the model described below.

## The 303s

`sim/web/dsp.js` is the model of the x0xb0x filter, envelope, accent, VCA and
oscillator from [x0xb0x VCF Bench](https://github.com/cozycactus/x0xb0x-vcf-bench),
copied unchanged from commit `8384696` (CC0; `cmp` it against that repository
to check, and replace the file to update). It is a model of an ngspice
simulation of one x0xb0x build, not of a measured instrument; its README says
what it was fitted to.

`sim/web/acid.js` is the only glue. The model has its own sequencer, which is
not used: the device is the sequencer. A Note On becomes the lines a x0xb0x's
firmware drives: the CV is latched and the gate rises; a note that starts
while another is held raises SLIDE and moves the CV with the gate still up; a
velocity of 120 or more raises ACCENT. The seven knobs (Tune, Cutoff,
Resonance, Env Mod, Decay, Accent, Volume) and the waveform switch are the
model's own, 0 to 1 of a turn, and start where the Bench page starts them; the
channel strip in `rig.js` brings the level up to the rest of the rig.

The effects of the Bench page are ported from that page's `buildFx` into
`rig.js`. Each 303 keeps the distortion (`Distortion`: Drive and Color) in its
channel, before the fader. The tempo-synced ping-pong delay and the reverb
with a generated tail (`SendFx`) are the rig's send effects, shared by every
instrument: each mixer strip has a delay send and a reverb send, taken after
its fader, and the delay's echoes also reach the reverb. The delay follows the
device's MIDI Clock, down to its slowest tempo (a 1/2 note at 20 BPM is 6 s).
The two returns and the feedback glide when the artist moves them over bars;
Time, Size, Tone, Drive and Color act when they are turned. The returns are
set so that the mix is as loud as with the rig's earlier delay and reverb
(checked by rendering the same bars of a set through both).

On the page each 303 is drawn as the Bench page draws its one: the panel with
its knobs, switch and ladder-current meter, and its Distortion board; the
shared Effects board is drawn the same way above them, in that page's colours
and type (three fonts from Google Fonts; without a network the fallbacks are
used). Above it is a small mixer: a fader, a mute, delay and reverb sends and
a pan for every instrument, a lamp that shows the device sending it a note,
and the master with a peak meter. The mixer's mute is not the device's MUTE;
as on a console, it silences the strip and its sends, and what is already in
the delay and the reverb rings out. The sends and pans start where the mix
had them fixed; the 909's reverb send reaches the snare, clap and crash only.
The pad synth has no strip.

## The artist

`sim/web/artist.js` is a score of finger movements: key and button levels,
quadrature edges, and knob moves on the synths. It never reads or writes the
device's memory. `sim/test/artist_check.js` runs it against the firmware in
Node and checks afterwards that the device holds the clips the score meant to
program, plays the clips each section expects, and sends well-formed MIDI.

Everything the artist does on the synths (filter sweeps, delay, reverb) is
labelled as such on the page: the sequencer sends no CC.

There are eleven songs (`Artist.SONGS`, `options.song`): the first set, and
ten written for the page in the manner of Hallucinogen, Astral Projection,
Total Eclipse and The Infinity Project (three, three, two and two). None
copies a record. A song is a key, a tempo, the notes of its clips, three live
edits and the synth settings. The ten new ones are written in scale degrees,
one row of steps per clip; a song the set cannot play (an unknown key, a clip
missing, a copy with other steps than its source, a live edit too far to turn
between two beats) is refused when the file loads.

Every song fills the same clip slots, so any song can be played in any of the
four arrangements (`Artist.FORMS`, `options.form`; each song names the one it
was written for):

- `classic`: from the kick alone to a peak, a break with a riser, the return,
  the tracks leaving one or two at a time;
- `sunrise`: no kick at first, the melody alone in reverb, then the kick and
  the bass together; the melody leads and leaves last;
- `twopeaks`: the kick and the bass from the first bar, a short break, a
  first peak, a long break and a second peak;
- `hypnotic`: no long break; the acid leads, the kick drops out for a bar
  now and then, and once, with the bass, for two.

A clip on the device has at most 16 steps. The arrangements make longer
phrases the way the device does: two clips of a track taken in turn, a clip of
10 to 14 steps against the bar (an arp of 3 to 7), mutes and scenes. An
arrangement is a list of the artist's moves written with the helpers of
`stage()`; it also records what the check should find: the rows and mutes at
chosen bars and, for a change on the beat, at chosen beats; the clips that
play whole in a bar; the notes changed live; a kick fill on the wire; and the
tracks muted for good, silent to the end of the set.

The score has four variants, chosen by what the firmware under test can do
(`Artist.buildShow(seed, options)`; the checks read `FW2=1`, `KEYS=1` and
`PADS=1`). `sim/build.sh` reads what the firmware can do from its source and
the page offers only the variants that fit (`Device.features()`):

- default: every note dialled from C4, every clip typed in every row;
- `fw2`: a new step starts at the track's last note, and clips are copied by
  holding a pad and pressing another pad of the track;
- `keys` (with `fw2`): a MIDI keyboard on the input gives a held step its
  pitch.
- `pads` (with `fw2`): no keyboard; while a step is held the pads are a
  four-octave piano and the artist takes each pitch from one pad (the encoder
  only for a pitch outside the window).

The page has an on-screen keyboard, computer keys and Web MIDI for the same
input. With a firmware that does not listen on its DIN input it says so.
