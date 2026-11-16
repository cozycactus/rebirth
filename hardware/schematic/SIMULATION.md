# SPICE simulation of the analog parts

Benches for the CV outputs, gate outputs, encoder RC filters, MIDI OUT/IN and the
+5V_LED inrush of the schematic in this folder (generator `gen/build.py`). The values
in the benches are the schematic's. ngspice 46, 2026-11-16.

```bash
python3 hardware/schematic/sim/run.py            # every bench, under a minute
python3 hardware/schematic/sim/run.py cv midi    # some: cv cvv gate enc midi inrush
MCP6001_LIB=/path/to/MCP6001.txt python3 hardware/schematic/sim/run.py cvv
```

`sim/run.py` writes each deck to `sim/out/*.cir`, runs `ngspice -b` (set `NGSPICE` for
another binary) and prints the measured numbers; `sim/out/results.json` keeps them.
`sim/models.lib` holds the generic models. `cvv` repeats the CV benches with
Microchip's own MCP6001/2/4 macro-model, which is not in the repository (see Models);
it runs only when `MCP6001_LIB` names the unzipped `MCP6001.txt`. The benches are built
by hand from the sheets: the kicad-happy analyzer does not see the MCP6004 units' pins
and reads C51x/R51x as RC filters, so its automatic pass covers only the encoder filters
and decoupling (last section).

## Result

| Circuit | Verdict | Key numbers |
| --- | --- | --- |
| CV buffer, C51x **1 nF** (changed) | OK | DC error = MCP6004 offset only; PM >= 94 deg for any cable C (Microchip model; >= 73 deg with the pessimistic generic one); overshoot 6 % into 1 nF, 28 % into 10 nF; short 4.8 mA |
| CV buffer, C51x 100 pF (as drawn before) | stable, badly damped | 31-38 % overshoot and +3.4-4.4 dB peaking into 1 nF, 64-68 % / +10-12 dB into 10 nF |
| Gate outputs | OK | high 4.95 V (100 k) / 4.53 V (10 k), 4.07 V worst; rise 2.1-2.3 us into 1 nF, 21-23 us into 10 nF |
| Encoder RC filters | OK | GPIO follows the contact in <= 0.2 ms; spikes shorter than 30 us (worst-case thresholds) to 100 us (mid) rejected; fast turn with bounce decodes 24 of 24 |
| MIDI OUT | OK | loop current 6.5 mA (4.5 mA worst) into a MIDI 1.0 receiver; edges 50 ns over 15 m |
| MIDI IN (H11L1) | OK | 1.7-4.7x the H11L1 turn-on current for 5 V, CA-033 and old 3.3 V senders; bit width error <= 1.2 % |
| +5V_LED inrush (U201) | OK | charged in 1.7-3.0 ms at the current limit, below the 5 ms minimum FAULT deglitch |

**One design change:** C511-C514 100 pF -> **1 nF C0G** (`gen/build.py`, `cv_stage`).
With R511 1 k inside the loop and the feedback taken from the jack through R521 10 k,
the jack sees a second-order response with natural frequency 1/sqrt(R52x C51x R51x
C_cable) and damping 0.5 sqrt(R52x C51x / (R51x C_cable)). With 100 pF the damping is
0.5 at 1 nF of cable and 0.16 at 10 nF, so the stage, stable as an amplifier, rings.
1 nF gives 1.6 and 0.5. The cost is a slow tail: a 2 V step into 10 k settles to
1 LSB (0.5 mV) in 73 us instead of 11 us, far below any musical time. A sweep of
100 p / 330 p / 470 p / 1 n / 2.2 n is in the CV section. Microchip's model, run after
the change, gives the same overshoot within 1 % and confirms the choice. Check after
the change: `build.py --check` 0 netlist problems, 0 ERC violations; the schematic diff
is the four values and the sheet note.

## Models

The generic models are in `sim/models.lib` with the datasheet numbers next to them.

- **MCP6004, Microchip model (`cvv`)**: `MCP6001_MM_F.zip` (Rev F, 2012, `MCP6001.txt`,
  for MCP6001/1R/1U/2/4) from `https://ww1.microchip.com/downloads/en/DeviceDoc/`,
  SHA-256 `bb024a6252252cc3876fd92f30598fafbd1e8c787eb7f9844747656508a68b9f`. Its
  licence allows use with Microchip parts only, so it stays out of this GPL repository:
  download it and point `MCP6001_LIB` at it. It is written for PSpice; ngspice runs it
  with `set ngbehavior=ps` (run.py writes that `.spiceinit`). Two output-clamp tables
  (G35, G36) have unbalanced parentheses; run.py repairs them in its copy, and the
  as-published file gives the same short-circuit recovery to the microsecond. Checked
  against the datasheet at 5 V:

  | | Model | Datasheet |
  | --- | --- | --- |
  | Open-loop gain | 112.1 dB | 112 dB typ |
  | Unity-gain frequency / PM (G = +1, 10 k, 60 pF) | 1.07 MHz / 91 deg | 1 MHz / 90 deg typ |
  | Slew rate up / down | 0.61 / 0.72 V/us | 0.6 V/us typ |
  | Swing, 10 k to VDD/2 | 9.6 mV from VSS, 9.8 mV from VDD | <= 25 mV |
  | Output at 0 V input, 10 k or 100 k to GND | 0.21 mV | not given |
  | Short-circuit current | 20.9 mA | 23 mA typ at 5.5 V |
  | Offset | -0.6 mV (its typical) | +-4.5 mV max |

  The model does not clamp the inverting input to VSS (its header: no behaviour outside
  the normal operating region), so the back-drive input currents below come from the
  generic model.
- **MCP6004, generic (`MCP6004G`, `cv`)**: rail-to-rail macro-model built from the
  datasheet: GBW 1 MHz, slew 0.6 V/us, AOL 112 dB, VOS a parameter (+-4.5 mV max),
  input C 6 pF / 3 pF, ESD diodes on the inputs and output, output limited to 5 mV from
  either rail, +-23 mA short-circuit current. The second pole FP2 (4 MHz; 2 MHz as a
  stress case) and the open-loop output resistance RO (100 R and 1 k) are swept. It is
  more pessimistic than Microchip's (PM 74 deg vs 91 deg at G = +1). kicad-happy's
  generated `OPAMP_MCP6004` (single pole, ideal output) gives the same CV peaking within
  0.3 dB as well.
- **74AHCT125, 74LVC1G17 (`CMOSBUF`)**: threshold, 2-3 ns edge, output resistance from
  the VOH/VOL limits (AHCT at 4.5 V: <= 70 R / 45 R; LVC1G17 at 3 V: <= 23 R sinking).
- **Opto LEDs**: diodes fitted to VF = 1.4 V typ / 1.7 V max at 1.6 mA (6N138) and
  1.2 V typ / 1.5 V max at 10 mA (H11L1). **H11L1 output**: LED current -> 0.2 us
  detector low-pass -> delay (1.2 us typ, 4 us max) -> Schmitt at IF(on) 1.2 mA typ /
  1.6 mA max with IF(off)/IF(on) 0.75 typ / 0.5 worst -> open collector.
- **MIDI cable**: 15 m as three 5 m LC sections (0.5 uH/m loop, 80 pF/m, 0.17 R/m), no
  loss beyond DC resistance, so it rings more than a real cable.
- **TPS2553**: behavioural: soft-start ramp (tr 1.1 ms typ), then constant current at
  IOS = 234 / 267 / 303 mA (R201 = 100 k, datasheet equations), rDS(on) 85 mR.
- **RP2040 inputs**: VIH 2.0 V, VIL 0.8 V, Schmitt hysteresis >= 0.2 V, pull-up 50-80 k
  (RP2040 datasheet, "Pin specifications"). The real thresholds lie anywhere inside these
  limits, so the encoder bench checks the extremes.

## 1. CV output stage (sheet `cv_gate`, `cv_stage`)

```
Vbus  vbus 0 5                        ; also 4.5 V
Vdac  dac 0 ...                       ; MCP4822 output, ideal, slew 0.55 V/us for large steps
Xu503 dac fb vbus 0 amp MCP6001       ; U503A-D, Microchip model (cvv)
*     dac fb amp vbus 0 MCP6004G      ; generic model (cv)
R511  amp jack 1k
R521  fb jack 10k
C511  fb amp 1n                       ; 100p before the change
Rload jack 0 100k | 10k
Ccable jack 0 0 | 1n | 10n | 100n
```

**DC** (DAC swept 0-2.048 V in 0.5 mV steps):

| Model | Load | Offset | Jack at code 0 | Jack follows the DAC from | Error at 1.000 V | Error at 2.048 V | Op-amp output at 2.048 V |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Microchip, 5 V | 100 k / 10 k | -0.50 mV | 0.21 / 0.20 mV | 1.0 mV | -0.50 mV | -0.59 mV | 2.068 / 2.252 V |
| Microchip, 4.5 V | 100 k / 10 k | -0.53 mV | 0.21 / 0.19 mV | 1.0 mV | -0.53 mV | -0.61 mV | 2.068 / 2.252 V |
| generic, VOS 0 | 100 k / 10 k | 0 | 4.9 / 4.5 mV | 5 / 4.5 mV | +0.004 mV | +0.001 mV | 2.069 / 2.253 V |
| generic, VOS -4.5 mV | either | -4.5 mV | 4.5-4.9 mV | 9-9.5 mV | -4.50 mV | -4.50 mV | |
| generic, VOS +4.5 mV | either | +4.5 mV | 4.5-4.9 mV | 0-0.5 mV | +4.50 mV | +4.50 mV | |

The load does not move the pitch: the error is the op-amp's offset (a constant, removed
by calibration; 4.5 mV max = 5.4 cents) plus 0.09 mV of gain error across the range.
Near 0 V the op-amp is not the limit: with the load to ground it sinks nothing, and
Microchip's model reaches 0.21 mV (the generic model's 5 mV limit was a pessimistic
guess; the datasheet's "25 mV at 10 k to VDD/2" is a sinking load). The MCP4822 swings
down to 10 mV (typ, and is accurate to 1 LSB only above it), so the floor is the DAC's:
with the offset, notes near note 24 read up to ~17 cents (14.5 mV) sharp. The sheet
note now says so (it said ~25 mV from the MCP6004).

**Loop gain** (loop broken at the inverting input with 1 MH / 1 F, op-amp input C kept
on the feedback node; RL 100 k, 10 k gives the same within 3 deg):

| Cable C | PM, Microchip, C51x 1 nF | PM, Microchip, 100 pF | PM, generic 1 nF (RO 100 R / 1 k) | PM, generic 100 pF (RO 1 k) | GM, Microchip 1 nF |
| --- | --- | --- | --- | --- | --- |
| 0 | 106 deg | 105 deg | 76 / 74 deg | 74 deg | 32 dB |
| 1 nF | 95 deg | 105 deg | 77 / 74 deg | 87 deg | 39 dB |
| 10 nF | 101 deg | 114 deg | 78 / 83 deg | 101 deg | 39 dB |
| 100 nF | 102 deg | 26 deg (three crossovers) | 79 / 84 deg | 21 deg (three crossovers) | 39 dB |

Generic with FP2 = 2 MHz, no cable: 63 deg (1 nF) / 64 deg (100 pF). The amplifier
loop is stable with either capacitor up to 10 nF; the problem with 100 pF is the
closed-loop response at the jack:

**Steps and AC response at the jack** (RL 100 k; Microchip model, generic RO 1 k in
brackets; semitone = 83.3 mV step with a 50 ns edge; full scale = 0 -> 2.048 V at the
DAC's 0.55 V/us; settle = to +-0.5 mV, 1 LSB):

| Cable C | C51x | AC peaking | Semitone overshoot | Semitone settle | Full-scale overshoot | Full-scale settle |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | **1 nF** | 0 dB (0) | 0 % (0) | 1.2 us (0.6) | 0 mV (0) | 34 us (32) |
| 0 | 100 pF | 0.1 dB (0.2) | 2 % (3) | 2.6 us (2.7) | 17 mV (29) | 11 us (9) |
| 1 nF | **1 nF** | 0.5 dB (0.5) | 6 % (6) | 29 us (29) | 118 mV (121) | 75 us (65) |
| 1 nF | 100 pF | 3.9 dB (4.4) | 34 % (38) | 12 us (12) | 363 mV (394) | 27 us (24) |
| 10 nF | **1 nF** | 3.0 dB (3.0) | 28 % (28) | 82 us (82) | 558 mV (558) | 179 us (157) |
| 10 nF | 100 pF | 11.2 dB (11.8) | 66 % (68) | 124 us (129) | 916 mV (510) | 216 us (207) |

Into 10 k with 1 nF (Microchip): 0 % overshoot up to 1 nF of cable, 23 % at 10 nF; up
to 1 nF of cable the slowest settle is 73 us (full scale, no cable), at 10 nF 153 us.
VBUS 4.5 V changes nothing (the output stays >= 2.2 V below the rail). A 3.5 mm patch
cable is ~0.1 nF/m, so 1 nF is ~10 m and 10 nF is a stress case.

The C51x sweep (generic model, RL 100 k, RO 100 R / 1 k):

| C51x | Overshoot, 1 nF cable | Overshoot, 10 nF | Full-scale settle, no cable | Full-scale settle, 1 nF |
| --- | --- | --- | --- | --- |
| 100 pF | 31-38 % | 64-68 % | 9 us | 20-24 us |
| 330 pF | 14-17 % | 44-47 % | 16 us | 22-23 us |
| 470 pF | 11-13 % | 39-40 % | 16 us | 33 us |
| **1 nF** | **6 %** | **27-28 %** | **32 us** | **64-65 us** |
| 2.2 nF | 3 % | 17-18 % | 83 us | 119-120 us |

**Shorted jack** (DAC at 2.048 V, 200 us short, Microchip model): 4.81 mA through R511
(23 mW in the 0603), op-amp output at 4.81 V; the MCP6004's own limit is ~21 mA, so the
1 k sets the current. After the short opens the jack jumps to 4.93 V for a few
microseconds and is back within 1 LSB in 101 us (37 us with 100 pF). Four shorted
outputs draw 19 mA from VBUS. The generic model: 4.54 mA, 100 us.

**Another module's output patched into a CV jack** (DC, DAC at 1 V; Microchip / generic):

| Source | R511 current / power | Inverting input current (generic) |
| --- | --- | --- |
| +-10 V or +-12 V through 1 k | <= 8.3 / 7.9 mA, <= 69 / 63 mW | <= 0.31 mA |
| -12 V stiff (0 R) | 16.3 / 15.2 mA, 267 / 230 mW | 1.13 mA |
| +12 V stiff (0 R) | 11.6 / 10.8 mA, 134 / 117 mW | 0.63 mA |

Within the MCP6004 maximum ratings (+-2 mA into the inputs, +-30 mA at the output).
A stiff +-12 V rail patched in by mistake overloads the 0603 R51x (0.1 W); an 0805/1206
R51x would take it. Not changed: a supply rail on an output jack is a misuse.

## 2. Gate outputs (sheet `cv_gate`)

```
Vgpio gpio 0 PWL(0 -> 3.3 V, 2 ns edges, 200 us high)
R531  gpio 0 100k
Xu504 gpio buf vbus 0 CMOSBUF VTH=1.5 TE=3n ROUT=40 | 70   ; 74AHCT125 on VBUS
R541  buf jack 1k
Rload jack 0 100k | 10k ; Ccable jack 0 0 | 1n | 10n
```

| VBUS | Load | Cable | Gate high | Rise 10-90 % | Up to 2.5 V | Down to 1 V |
| --- | --- | --- | --- | --- | --- | --- |
| 5.0 V | 100 k | 0 / 1 nF / 10 nF | 4.95 V | ns / 2.3 us / 22.6 us | ns / 0.7 us / 7.3 us | ns / 1.7 us / 16.5 us |
| 5.0 V | 10 k | 0 / 1 nF / 10 nF | 4.53 V | ns / 2.1 us / 20.7 us | ns / 0.8 us / 7.6 us | ns / 1.4 us / 14.2 us |
| 4.5 V, ROUT 70 R | 10 k | 0 / 10 nF | 4.07 V | ns / 21.3 us | ns / 9.2 us | ns / 13.6 us |

Shorted jack: 4.8 mA. Gate inputs switch at 1-3 V; triggers are >= 1 ms. OK.

## 3. Encoder RC filters (sheet `top_strip`, `encoder_block`)

```
V33  v33 0 3.3
R601 v33 raw 10k
Sc   raw 0 (contact, 1 R closed)
R602 raw enc 10k
C603 enc 0 10n
Rpu  enc v33 50k | 80k | none      ; RP2040 pull-up, on in firmware
```

The GPIO voltage is checked against the RP2040 limits. The slowest thresholds (2.0 V
up, 0.8 V down) give the longest delays; the most sensitive ones (1.0 V up from a low
input, 1.8 V down from a high input) let the shortest spikes through.

| GPIO pull-up | Low level | Contact closes -> GPIO low | Contact opens -> GPIO high | Shortest spike that passes (most sensitive / mid thresholds) |
| --- | --- | --- | --- | --- |
| 50 k | 0.55 V | 66-200 us | 26-108 us | closing 75 / 100 us; opening 30 / 100 us |
| 80 k | 0.37 V | 64-171 us | 39-131 us | closing 75 / 100 us; opening 50 / 150 us |
| off | 0 V | 61-142 us | 73-187 us | closing 75 / 100 us; opening 75 / 200 us |

Fast turn: a contact edge every 1 ms (an EC11 with 15 pulses at ~17 rev/s) and 200 us
of bounce (three extra changes) on every edge, decoded with the firmware's
`quad_table`: 24 of 24 steps, no invalid transitions, for every threshold set and for
polling every 0 / 100 / 250 us. The filter adds at most 0.2 ms; the EC11's own bounce
(2-3 ms at 360 deg/s) is handled by the Gray-code table, not by the filter.

The low level with the GPIO pull-up on is 0.55 V against VIL 0.8 V: within the
limit, 0.25 V margin. Turning the internal pull-ups off on GPIO 18/19/21/22 (the
10 k pull-ups suffice) would give 0 V and, at worst-case thresholds, reject opening
spikes shorter than 75 us instead of 30 us; a firmware choice, not needed.

kicad-happy's automatic RC check agrees: R602/C603 (and the other three) 1.588 kHz
simulated vs 1.592 kHz calculated, tau 100 us.

## 4. MIDI (sheet `midi`)

**MIDI OUT** into a receiver drawn as in the MIDI 1.0 specification (220 R, opto LED,
1N4148 across it), UART byte 0x55 (alternating bits) at 31 250 baud:

```
Xu401 tx txb v33 0 CMOSBUF VTH=1.65 TE=2n ROUT=12 | 25   ; 74LVC1G17
R402 txb o5 10 ; Xfb401 o5 p5 FB1K       ; pin 5
R403 v33 o4 33 ; Xfb402 o4 p4 FB1K       ; pin 4
cable 15 m ; Rd r4 la 220 ; Dled la r5 LED6N138 ; Dprot r5 la D1N4148
```

| Case | LED current on | Peak | Rise / fall 10-90 % | Bit width error | LED reverse |
| --- | --- | --- | --- | --- | --- |
| 3.3 V, VF typ, 15 m | 6.54 mA | 9.1 mA | 50 / 44 ns | 23 ns | -0.57 V |
| 3.3 V, VF typ, no cable | 6.60 mA | 6.6 mA | 12 / 8 ns | 1 ns | -0.27 V |
| 3.135 V, VF max, resistors +5 %, ROL 25 R, 15 m | 4.53 mA | 6.2 mA | 57 / 41 ns | 24 ns | -0.50 V |

4.5 mA worst case is 2.8x the 1.6 mA that 6N138/H11L1-type receivers need. Off current 0.

**MIDI IN** (J402 - FB403 - R404 220 R - H11L1 - FB404, D401 across the LED,
R405 1 k pull-up, 15 m cable from the sender):

| Sender | LED current | vs 1.6 mA | Delay fall / rise | Bit width error |
| --- | --- | --- | --- | --- |
| 5 V, 220 R / 220 R (MIDI 1.0) | 5.56 mA | 3.5x | 1.4 / 1.6 us | 0.6 % |
| 3.3 V CA-033, 33 R / 10 R (this board's MIDI OUT) | 7.55 mA | 4.7x | 1.2 / 1.6 us | 1.2 % |
| 3.3 V with old 220 R / 220 R | 3.14 mA | 2.0x | 1.6 / 1.4 us | 0.4 % |
| same, VF max, IF(on) 1.6 mA, ratio 0.5, 4 us delay | 2.77 mA | 1.7x | 4.6 / 4.2 us | 1.0 % |

MIDI_RX low level 0.10 V (VIL 0.8 V). The lossless cable model rings at the first
edge: the LED current dips to 0.3-0.5 mA for 100-200 ns. Without the 0.2 us detector
bandwidth in the H11L1 model the output toggled three times on each edge; the real
part (1 MHz class) and the UART's mid-bit sampling both ignore it, but this is a model
assumption to look at on the bench with a long cable.

## 5. +5V_LED inrush through U201 (sheets `power`, `rgb_chain`)

```
Vusb usb 0 5 | 4.75 ; Rcab usb vbus 0.25 ; Cvbus vbus 0 11u      ; C201 + the Pico's
Bu201 vbus led   ; soft-start ramp, then IOS 234 | 267 | 303 mA, 85 mR
C202 10u ; C983 100n ; 82 x 100n (C901-C982) ; C984 100u (ESR 0.3 R)
Bidle led 0 82 mA | 0 (82 x SK6812MINI-E idle, assumed 1 mA each)
```

| IOS | Rail at 90 % after EN | Time in current limit | VBUS dip (0.25 R cable) | Switch energy / peak power |
| --- | --- | --- | --- | --- |
| 234 mA | 2.1-3.0 ms | 2.3-3.3 ms | 59 mV | 1.3-1.8 mJ / 1.1 W |
| 267 mA | 1.9-2.5 ms | 2.0-2.8 ms | 67 mV | 1.3-1.7 mJ / 1.3 W |
| 303 mA | 1.7-2.1 ms | 1.8-2.3 ms | 76 mV | 1.3-1.7 mJ / 1.5 W |

VBUS never supplies more than IOS to the chain. The longest stay in current limit,
3.3 ms, is below the TPS2553's 5 ms minimum overcurrent deglitch, so LED_FAULT_N (TP201)
does not flag a normal power-up; a chain that already draws much more than 82 mA when
the rail comes up (the firmware lighting LEDs before U201 is on) would.

## kicad-happy automatic pass

`analyze_schematic.py` + `simulate_subcircuits.py` (offline, local ngspice) on the
schematic: 16 subcircuits, 15 pass, 1 info. The encoder filters are confirmed (above);
the three decoupling groups have |Z| <= 0.135 R at 1 MHz. The R51x/C51x and R52x/C51x
"RC filters" are the op-amp loop read as filters, and its inrush figure (1.18 A) assumes
a generic soft-start without U201's current limit; the benches above replace both.

## Not covered

- Vendor models for TPS2553, H11L1 and the MCP4822 (the MCP6004 has Microchip's). The
  generic models carry the datasheet limits; the bench should confirm the CV step
  into the real synth with a 1-2 m cable, MIDI IN with a long cable, and TP201 at
  power-up.
- The MCP4822's own errors (offset, gain, INL, 10 mV floor) beyond the note above.
- PCB parasitics, ground bounce from the LED chain into the CV ground, temperature.
- The firmware's encoder poll interval: the benches show that the hardware needs no
  more than 0.25 ms polling for a very fast turn; the main loop's gaps while it
  redraws the OLED were not measured.
