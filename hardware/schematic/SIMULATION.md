# SPICE simulation of the analog parts

Benches for the CV outputs, gate outputs, encoder RC filters, MIDI OUT/IN and the
+5V_LED inrush of the schematic in this folder (generator `gen/build.py`), as it stands
after the multi-model review (MIDI OUT open collector, C984 470 uF, the added pull-downs)
and the third MCP23017 (no analog change). The benches' part values were checked against
`gen/build.py` at that state (merge dfba9fd). ngspice 46, 2026-11-16.

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
| Gate outputs | OK | high 4.90 V (100 k) / 4.49 V (10 k), 4.03 V worst; rise 2.1-2.2 us into 1 nF, 21-22 us into 10 nF |
| Encoder RC filters | OK | GPIO follows the contact in <= 0.2 ms; spikes shorter than 30 us (worst-case thresholds) to 100 us (mid) rejected; fast turn with bounce decodes 24 of 24 |
| MIDI OUT (open collector) | OK, meets CA-033 | loop current 6.7 mA, 4.3 mA at CA-033's worst corner (spec: 4.47 mA); RA short 109 mA / 0.375 W; bit width error <= 1.2 % |
| MIDI IN (H11L1) | OK, meets CA-033 | 1.7-4.8x the H11L1 turn-on current for 5 V, CA-033 and old 3.3 V senders; bit width error <= 3.2 % |
| +5V_LED inrush (U201) | OK with TPS2553 (not -1) | charged in 6.8-12.7 ms at the current limit (C984 470 uF): longer than the 3-11 ms FAULT deglitch, so TP201 goes low briefly at power-up (a TPS2553-1 would latch off); junction rise <= 44 C |

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
- **74AHCT125 (`CMOSBUF`), 74LVC1G14 (`CMOSINV`)**: threshold, 2-3 ns edge, output
  resistance from the VOH/VOL limits (AHCT at 4.5 V: <= 70 R / 45 R; LVC at 3 V: <= 23 R
  sinking). **MMBT3904**: the classic 2N3904 model (same die).
- **Opto LEDs**: diodes fitted to VF = 1.4 V typ / 1.7 V max at 1.6 mA (6N138),
  1.2 V typ / 1.5 V max at 10 mA (H11L1), and CA-033's worst receiver, 1.9 V. **H11L1 output**: LED current -> 0.2 us
  detector low-pass -> delay (1.2 us typ, 4 us max) -> Schmitt at IF(on) 1.2 mA typ /
  1.6 mA max with IF(off)/IF(on) 0.75 typ / 0.5 worst -> open collector.
- **MIDI cable**: 15 m as three 5 m LC sections (0.5 uH/m loop, 80 pF/m, 0.17 R/m), no
  loss beyond DC resistance, so it rings more than a real cable.
- **TPS2553**: behavioural: soft-start ramp (tr 1.1 ms typ), then constant current at
  IOS = 232 / 267 / 306 mA (R201 = 100 k 1 %, datasheet equations), rDS(on) 85 mR.
  FAULT deglitch, thermal limits and RthJB / RthJA from SLVS841G (rev. October 2026).
- **RP2040 inputs**: VIH 2.0 V, VIL 0.8 V, Schmitt hysteresis >= 0.2 V, pull-up 50-80 k
  (RP2040 datasheet, "Pin specifications"). The real thresholds lie anywhere inside these
  limits, so the encoder bench checks the extremes.

## 1. CV output stage (sheet `cv_gate`, `cv_stage`)

```
Vbus  vbus 0 5                        ; also 4.5 V
Vdac  dac 0 ...                       ; MCP4822 output, ideal, slew 0.55 V/us for large steps
Xu503 dac fb vbus 0 amp MCP6001       ; U503A-D, Microchip model (cvv)
*     dac fb amp vbus 0 MCP6004G      ; generic model (cv)
R515  dac 0 100k                      ; input pull-down (no effect on an ideal DAC source)
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
R551  jack 0 100k
Rload jack 0 100k | 10k ; Ccable jack 0 0 | 1n | 10n
```

| VBUS | Load | Cable | Gate high | Rise 10-90 % | Up to 2.5 V | Down to 1 V |
| --- | --- | --- | --- | --- | --- | --- |
| 5.0 V | 100 k | 0 / 1 nF / 10 nF | 4.90 V | ns / 2.2 us / 22.4 us | ns / 0.7 us / 7.3 us | ns / 1.6 us / 16.2 us |
| 5.0 V | 10 k | 0 / 1 nF / 10 nF | 4.49 V | ns / 2.1 us / 20.5 us | ns / 0.8 us / 7.6 us | ns / 1.4 us / 14.0 us |
| 4.5 V, ROUT 70 R | 10 k | 0 / 10 nF | 4.03 V | ns / 21.1 us | ns / 9.3 us | ns / 13.4 us |

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

**MIDI OUT** (open collector, as drawn after the review) into a receiver drawn as in
CA-033 Figure 2 (220 R, opto LED, 1N4148 across it), UART byte 0x55 (alternating bits)
at 31 250 baud:

```
R401  tx v33 10k
Xu401 tx txn v33 0 CMOSINV VTH=1.65 TE=2n ROUT=20   ; 74LVC1G14
R406  txn base 470 ; R407 base 0 10k
Q401  sink base 0 Q2N3904                          ; MMBT3904
R402  sink o5 10 ; Xfb401 o5 p5 FB1K               ; pin 5 (RC)
R403  v33 o4 33  ; Xfb402 o4 p4 FB1K               ; pin 4 (RA)
cable 15 m ; Rd r4 la 220 ; Dled la r5 LED... ; Dprot r5 la D1N4148
```

| Case | LED current on | Peak | Rise / fall 10-90 % | Delay on / off | Bit width error |
| --- | --- | --- | --- | --- | --- |
| 3.3 V, 6N138 VF typ, 15 m | 6.67 mA | 9.9 mA | 49 / 505 ns | 0.14 / 0.41 us | 0.27 us (0.9 %) |
| 3.3 V, 6N138 VF typ, no cable | 6.73 mA | 6.8 mA | 14 / 16 ns | 0.01 / 0.27 us | 0.26 us (0.8 %) |
| 3.135 V, R +5 %, 6N138 VF max 1.7 V, 15 m | 4.79 mA | 7.8 mA | 54 / 619 ns | 0.13 / 0.53 us | 0.40 us (1.2 %) |
| CA-033 worst: 3.135 V, R +5 %, VF 1.9 V, 15 m | 4.27 mA | 7.3 mA | 54 / 628 ns | 0.13 / 0.53 us | 0.40 us (1.2 %) |

The turn-off delay is Q401's storage time (base overdriven: ~5 mA base for ~7 mA
collector); the slower fall with a cable is the cable capacitance discharging through
the receiver once the open collector lets go. No reverse voltage reaches the LED.

**MIDI OUT faults** (DC, TX low so Q401 conducts; CA-033's corner: 3.465 V, resistors -5 %):

| Fault | Current | Power |
| --- | --- | --- |
| pin 4 to GND | 109 mA in RA | RA 0.375 W (R403 is 0.5 W) |
| pins 4 and 5 shorted | 79 mA | RA 0.195 W, RC 0.059 W (R402 0.25 W), Q401 15 mW at Vce 0.19 V |
| pin 5 to GND | ~0 | nothing |

**MIDI IN** (J402 - FB403 - R404 220 R - H11L1 - FB404, D401 across the LED,
R405 1 k pull-up, 15 m cable from the sender):

| Sender | LED current | vs 1.6 mA | Delay fall / rise | Bit width error |
| --- | --- | --- | --- | --- |
| 5 V, 220 R / 220 R (MIDI 1.0) | 5.56 mA | 3.5x | 1.4 / 1.6 us | 0.6 % |
| 3.3 V CA-033, this board's MIDI OUT | 7.72 mA | 4.8x | 1.2 / 2.2 us | 3.2 % |
| 3.3 V with old 220 R / 220 R | 3.14 mA | 2.0x | 1.6 / 1.4 us | 0.4 % |
| same, VF max, IF(on) 1.6 mA, ratio 0.5, 4 us delay | 2.77 mA | 1.7x | 4.6 / 4.2 us | 1.0 % |

MIDI_RX low level 0.10 V (VIL 0.8 V), edges at MIDI_RX 9 / 35 ns in the model (the
H11L1 datasheet: 0.1 us typ). With this board's own MIDI OUT as the sender the slower
release (Q401 storage plus the cable tail, reaching the H11L1's lower off threshold
later) stretches the current-on bits by 1 us, 3.2 % of a bit: the UART samples
mid-bit. The lossless cable model rings at the first edge: the LED current dips to
0.3-0.5 mA for 100-200 ns. Without the 0.2 us detector bandwidth in the H11L1 model the
output toggled three times on each edge; the real part (1 MHz class) and the UART's
mid-bit sampling both ignore it, but this is a model assumption to look at on the bench
with a long cable.

### Against CA-033 (MIDI 1.0 Electrical Specification Update, 2014)

| CA-033 says | This board | Status |
| --- | --- | --- |
| 31.25 kbaud +-1 %, 8-N-1, LSB first; logical 0 = current on | UART0 at 31 250 baud; TX low -> U401 high -> Q401 on -> current on | OK (baud and format are firmware) |
| 3.3 V +-5 % transmitter: RA 33 R 5 % 0.5 W, RC 10 R 5 % 0.25 W | R403 33 R 0.5 W (1210), R402 10 R 0.25 W (1206) | OK |
| Buffer before RC optional, "IC gate or transistor"; at 3.3 V assumed open collector/drain, and its short-circuit current must be safe | U401 + Q401 open collector; 79 mA with pins 4-5 shorted (MMBT3904 200 mA) | OK |
| RA short to GND: 111 mA, 0.383 W at 3.465 V and -5 % resistors | 109 mA, 0.375 W (ferrite DC resistance in series) | OK |
| Worst-case loop current 4.472 mA (3.135 V, +5 % resistors, VF 1.9 V, ideal buffer) | 4.27 mA (Q401 Vce(sat) and ferrites) | OK, 0.2 mA below the spec's own figure; PC900V needs 4 mA in its worst case |
| Typical ~7.2 mA (3.3 V, VF 1.4 V) | 6.67-6.73 mA | OK |
| Pin 2 to GND on the transmitter only; pins 1, 3 open | J401 pin 2 GND; J402 pin 2 open; pins 1/3 open on both | OK |
| MIDI IN: no pin and no shield with a DC path to ground; optional 0.1 uF from pin 2 / shield | J402 pin 2 and shell open; the optional capacitor is not fitted | OK (option not used) |
| MIDI OUT shield: N/C or ground | open | OK |
| Optional ferrite beads 1 k at 100 MHz (MMZ1608Y102BT) close to the jacks | FB401-FB404 1k@100MHz | OK; place at the jacks on the PCB |
| Receiver: opto, reverse diode (1N914), RB 220 R, RD by opto and VRX | H11L1, D401 1N4148W, R404 220 R, R405 1 k at 3.3 V | OK |
| Receiver must need < 5 mA to turn on | H11L1 IF(on) <= 1.6 mA | OK |
| Rise and fall times < 2 us | MIDI_RX 9 / 35 ns (model; datasheet 0.1 us); LED current at a receiver <= 0.63 us | OK |
| One output drives one input; cable <= 15 m | one MIDI OUT; benches use 15 m | OK |
| DIN-5 180 deg female jacks labelled MIDI OUT / MIDI IN | J401 / J402 DIN-5_180degree; labels on the panel | OK; footprint still missing (README) |
| MIDI THRU optional | none | not required |

Nothing on this board departs from CA-033. Two limits come from the specification
itself: at its worst corner (4.3-4.5 mA) a receiver that really needs 5 mA would not be
guaranteed, and receivers that take power from pin 4 or are not opto-isolated may not
work with any 3.3 V sender (CA-033, "Low-Voltage Signaling Interoperability Issues").

## 5. +5V_LED inrush through U201 (sheets `power`, `rgb_chain`)

```
Vusb usb 0 5 | 4.75 ; Rcab usb vbus 0.25 ; Cvbus vbus 0 21u   ; C201 1u + C507 10u + the Pico's (~10u)
Bu201 vbus led    ; soft-start ramp, then IOS 232 | 267 | 306 mA, 85 mR
C202 10u ; C983 100n ; 82 x 100n (C901-C982) ; C984 470u (ESR 0.1 R)
Bidle led 0 82 mA | 0 (82 x SK6812MINI-E idle, assumed 1 mA each)
```

| IOS | Rail at 90 % after EN | Time in current limit | VBUS dip (0.25 R cable) | Switch energy | Switch power peak / mean | Junction rise (bound) |
| --- | --- | --- | --- | --- | --- | --- |
| 232 mA | 9.0-12.7 ms | 9.8-14.0 ms | 58 mV | 5.3-7.4 mJ | 1.1 / 0.5-0.6 W | <= 34 C |
| 267 mA | 7.8-10.5 ms | 8.5-11.6 ms | 67 mV | 5.3-7.1 mJ | 1.3 / 0.6-0.7 W | <= 39 C |
| 306 mA | 6.8-8.8 ms | 7.4-9.7 ms | 77 mV | 5.3-6.9 mJ | 1.5 / 0.7-0.8 W | <= 44 C |

VBUS never supplies more than IOS to the chain. With C984 at 470 uF (100 uF before the
review: 2-3 ms) the rail spends 7.4-14 ms in current limit at power-up. The TPS2553's
overcurrent FAULT deglitch is 3 / 7.5 / 11 ms (min / typ / max, SLVS841G), so
LED_FAULT_N (TP201) goes low for a few milliseconds at every power-up: in every case at
the minimum deglitch, in all but one at the typical, and with the chain's idle load at
IOS <= 267 mA even at the maximum. What that means depends on the variant:

- **TPS2553 (as drawn; "auto retry" in TI's comparison table)**: after the deglitch it
  only asserts FAULT and keeps regulating at IOS (datasheet 9.1.1); it switches off only
  if the die passes 135 C in current limit, and then cycles back on 10 C lower. The
  charge simply finishes. TP201 is a test point and nothing reacts to it; read it on the
  bench as "charging", not as a fault.
- **TPS2553-1 (latch-off) must not be fitted**: it turns off when the overcurrent lasts
  past the deglitch (7.5 ms typ) and stays off until EN or power is cycled, so with
  470 uF the LED rail would not come up at most power-ups. The BOM needs TPS2553DBV
  without -1.

Temperature (estimate; the model has no thermal part): the switch dissipates 1.1-1.5 W
at the start, falling to zero as the rail rises, 0.5-0.8 W on average over the charge.
For these few milliseconds the board copper stays at ambient, so the junction rise is
at most peak power x RthJB (29.4 C/W, DBV): <= 44 C, i.e. <= 69 C at 25 C ambient and
<= 104 C at 60 C, below the 135 C current-limit shutdown. RthJA (182.6 C/W) would only
apply to a charge lasting seconds (it would give 92-138 C of rise), which this is not.

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
- PCB parasitics, ground bounce from the LED chain into the CV ground, temperature,
  the TPS2553's junction temperature during the inrush.
- The firmware's encoder poll interval: the benches show that the hardware needs no
  more than 0.25 ms polling for a very fast turn; the main loop's gaps while it
  redraws the OLED were not measured.
