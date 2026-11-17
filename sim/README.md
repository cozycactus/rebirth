# Simulator

The whole device in a browser page: the firmware from `src/` runs unmodified
on a virtual board, the panel is drawn from what the firmware drives, and a
set of software synths plays what leaves the MIDI wire. A scripted Goa trance
artist can program the device from power-up and play one of eleven songs on it.

```bash
sim/build.sh native        # native core + smoke test, needs only a C compiler
sim/build.sh web           # WebAssembly core, needs sim/emsdk (see the script)
node sim/test/artist_check.js       # every song in every arrangement; SONG=3 FORM=sunrise plays one
python3 -m http.server 8765 --bind 127.0.0.1 --directory sim/web   # then open http://localhost:8765
```

`sim/web/index.html` also opens straight from disk: the page uses classic
scripts and the WebAssembly is embedded in `core.js`. From disk a browser will
not load an AudioWorklet module, so the two 303s then run on the main thread
(the same code, but it costs more and can crackle); served over HTTP they run
in a worklet.

The page links to the user manual: `sim/web/manual-ru.pdf` is a symbolic link
to `output/pdf/rebirth-manual-ru.pdf`, which `tools/build_manual_pdf.py` builds
from `docs/manual-ru.md`. Hovering over a control of the panel or the page
shows what it does.

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

Each 303 also has the effects of the Bench page (`BenchFx` in `rig.js`, ported
from that page's `buildFx`: distortion, a tempo-synced ping-pong delay, a
reverb with a generated tail), in place of sends to the rig's own delay and
reverb. Its tempo is the device's MIDI Clock. These eight knobs act when they
are turned: a move scheduled ahead or stretched over time is taken at once.

On the page each 303 is drawn as the Bench page draws its one: the panel with
its knobs, switch and ladder-current meter, and the Effects board, in that
page's colours and type (three fonts from Google Fonts; without a network the
fallbacks are used). Above them is a small mixer: a fader and a mute for every
instrument, a lamp that shows the device sending it a note, and the master
with a peak meter. The mixer's mute is not the device's MUTE. Every strip has
a pan knob and, except the 303s (which have their own delay and reverb), sends
to the rig's delay and reverb, taken after the fader; they start where the mix
had them fixed. The 909's reverb send reaches the snare, clap and crash only.

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
  the tracks leaving one by one;
- `sunrise`: no kick at first, the melody alone in reverb, then the kick and
  the bass together; the melody leads and leaves last;
- `twopeaks`: the kick and the bass from the first bar, a short break, a
  first peak, a long break and a second peak;
- `hypnotic`: no break at all; the acid leads, the kick drops out for a bar
  now and then.

A clip on the device has at most 16 steps. The arrangements make longer
phrases the way the device does: two clips of a track taken in turn, a clip of
12 or 14 steps against the bar, mutes and scenes. An arrangement is a list of
the artist's moves written with the helpers of `stage()`; it also records what
the check should find: the rows and mutes at chosen bars, the clips that play
whole in a bar, the notes changed live, and the tracks muted for good.

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
