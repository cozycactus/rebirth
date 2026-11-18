# Simulator

The whole device in a browser page: the firmware from `src/` runs unmodified
on a virtual board, the panel is drawn from what the firmware drives, and a
set of software synths plays what leaves the MIDI wire. A scripted Goa trance
artist can program the device from power-up and play one of eleven songs on it.

The public page, https://cozycactus.github.io/rebirth/, always runs `master`
(see "The public page" below). To run it locally:

```bash
sim/build.sh native        # native core + smoke test, needs only a C compiler
sim/build.sh web           # WebAssembly cores and version.js, needs sim/emsdk (see the script)
sim/check.sh               # every check for every score this firmware takes; `quick` for the device check only
node sim/test/artist_check.js       # every song in every arrangement; SONG=3 FORM=sunrise plays one
python3 -m http.server 8765 --bind 127.0.0.1 --directory sim/web   # then open http://localhost:8765
```

Run `sim/build.sh web` to generate `sim/web/core.js`, `sim/web/sh101_cpu.js`
and `sim/web/version.js`. The page shows which commit its firmware is from.
`sim/web/index.html` also opens straight from disk. It uses classic scripts
with the WebAssembly embedded in the respective core scripts. When an
AudioWorklet cannot load, including when opening the page from disk, the
303s and SH-101 use the same renderers on the main thread. Serving the page
over localhost or HTTPS allows the worklets to run on the audio thread.

The page links to the user manual, `manual-ru.pdf` beside it. On the public
page it is there; in a checkout it is not, unless built: `tools/build_manual_pdf.py`
builds it from `docs/manual-ru.md` on macOS (it reads the system fonts).
Hovering over a control of the panel or the page shows what it does.

## The public page

CI (`.github/workflows/ci.yml`, job Simulator) builds the simulator on every
push and pull request with Emscripten 7.0.0 and runs `sim/check.sh`: the device
check and the artist check (every song in every arrangement) for every score
the firmware takes, plus the offline SH-101 audio checks. A firmware change
that breaks the artist's gestures shows
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
| Five MCP23017: step buttons (0x20, steps 8/16 on 0x24), step LEDs (0x21), the key matrix (0x22 rows, 0x23 SCENE/MUTE columns) | Register models on a virtual I2C bus (`sim/core/hal.c`): IPOL, directions, pull-ups, output latches; a matrix row reads low through a closed key in a column driven low, and low too with its pull-up off (a chip that reset); chip reset, bus faults |
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
- **The sound of real gear.** The synths in `sim/web/rig.js` play the
  device's MIDI output. The 303s and the SH-101 voices use the circuit models
  described below.

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

## SH-101

The BASS (MIDI channel 3) and LEAD (channel 6) strips each have an independent
SH-101 voice. The **Голос** selector in the SH-101 panel switches the editable
patch. **Пробная нота** enables audio and plays C2 for BASS or C4 for LEAD.
The score's old cutoff ranges in Hz are mapped onto each patch's FREQ
slider, and each instrument uses its own fader, pan, mute and sends.

The default **Звук FX → SH-101** puts a third voice on the FX strip (MIDI channel 8).
A plain trigger sweeps pitch and cutoff down. An accent runs a noise and VCO
riser for the number of bars set by the score, using MIDI Clock. The FX strip
can also use the **ReBirth FX** patch.

The voice in `sim/web/sh101.js` uses the CEM3340 saw and PWM pulse, the
4013's three sub modes and a noise source. The C59 ADSR uses exponential
charge and discharge. GATE+TRIG uses last-note priority, while GATE and
LFO use low-note priority. AUTO portamento follows overlapping MIDI notes.
VCO MOD and MOD VCF take the selected LFO waveform. PWM taps the triangle
directly. BASS starts with static PWM and a stronger sub; LEAD starts with
LFO PWM. Both patches use A440 to match the rig.

- **D30/D31** selects the diode pair per voice. The MIDI voices and FX start
  with Roland's 1S2473. The ROM panel starts with Rev-A's 1N4148. The 1N4148
  fit uses the 25 °C curve from 10 µA to 1 mA.
  [Vishay 1N4148, Fig. 2](https://www.vishay.com/docs/81857/1n4148.pdf)
- Roland's circuit drawing gives the mixer resistors, four 240 pF IR3109
  integrators with 68 kΩ and 560 Ω local networks, and the TR26/TR27
  resonance path through C52, R129, D30/D31 and R113. The diode pair is
  solved inside the filter's trapezoidal step. The TR26 emitter path
  adds signal ahead of the BA662A VCA via R132 at higher resonance.
  [Roland SH-101 Service Notes, printed pp. 1, 2, 7](https://manuals.fdiskc.com/flat/Roland%20SH101%20Service%20Manual.pdf)
- Roland documents four OTAs, four buffers and the shared exponential
  control block. The model uses their nominal small-signal behavior.
  ALFA's diagram of one AS3109 stage illustrates the analogous input and
  buffer topology. Transistor parameters and buffer asymmetry of the
  original chip, device tolerances and the panel tapers remain
  approximations or outside this model. [Roland Juno-6 Service Notes, p. 6](https://www.synfo.nl/servicemanuals/Roland/JUNO-6_SERVICE_NOTES.pdf#page=6)
  · [ALFA AS3109, Fig. 2](https://www.alfatriode.lv/eng/sc/AS3109.pdf#page=3)
- The 1S2473 option uses a nominal fit to the ROHM forward curve at 25 °C.
  The owner's manual supplies switch behavior. [ROHM 1S2473, Fig. 2](https://datasheet.octopart.com/1S2471-Rohm-datasheet-101927.pdf#page=2)
  · [Roland SH-101 Owner's Manual, pp. 25–31](https://cdn.roland.com/assets/media/pdf/SH-101_OM.pdf)

`node sim/test/sh101_check.js` checks the four-pole response against the
analytical curve, bounded self-oscillation, divider pitch, note priority,
cancellation of queued MIDI and the two FX sweeps. Hardware calibration
would require test signals and captures from a real SH-101.

### Original 80C49 ROM

Open **SH-101 · ROM 80C49** in the synths card and select the 2048-byte
80C49-6-7301 image. The page verifies its hash and runs it locally on the
MCS-48 decoder from `cozycactus/sh101_midi` at 6 MHz. The 32-key matrix,
panel buttons, HOLD pedal and LFO or manual EXT CLK feed the virtual
U6. Its keyboard CV, oscillator RANGE, random DAC, Gate, portamento and
clock-reset outputs drive the same SH-101 audio model. [MCS-48 decoder source](https://github.com/cozycactus/sh101_midi/blob/8142852/src/mcs48_core.h)

For a tied sequencer note, hold HOLD while entering the new note and
select the GATE envelope mode for playback. KEY TRANSPOSE enters a rest
while LOAD is active. Shift-click holds a virtual key or switch and a
second click releases it. Sequence RAM lives in the open page.
[Roland SH-101 Owner's Manual, pp. 33–38](https://cdn.roland.com/assets/media/pdf/SH-101_OM.pdf)

The Rev-A netlist has active-low Gate through Q9, P24 wired to the Q7/Q6
reset circuit and separate sample-and-hold outputs from U10. U14 has
four 240 pF integrators with 68 kΩ and 560 Ω feedback. The resonance path
uses Q26/Q27 (2SC1815), C52 (10 µF), R129 (5.6 kΩ), R113 (15 kΩ), and
the antiparallel 1N4148 pair. The DSP models Q26/Q27 by their nominal
gain and follower behavior. [SH-101 schematic at tag Rev-A, 68f0691](https://github.com/cozycactus/sh-101/blob/Rev-A/sh-101.kicad_sch)

The web bundle contains the controller emulator. Supply the image through
the file picker, or use `?sh101=local` on a loopback server with a private
`sh101.rom` beside `index.html`. To extract that file from a sibling
`sh101_midi` checkout and exercise the original ROM regression:

```sh
python3 - ../sh101_midi/src/sh101_rom.h sim/web/sh101.rom <<'PY'
from pathlib import Path
import re, sys

array = Path(sys.argv[1]).read_text().split('{', 1)[1].split('}', 1)[0]
image = bytes(int(hex_byte, 16) for hex_byte in re.findall(r'0x[0-9a-fA-F]{2}\b', array))
assert len(image) == 2048
Path(sys.argv[2]).write_bytes(image)
PY
SH101_ROM=sim/web/sh101.rom node sim/test/sh101_rom_check.js
```

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
