# Schematic: RP2040 sequencer with the session-style live panel

KiCad 10 project, hierarchical. Open `rebirth_panel.kicad_pro`.
Sources: `docs/superpowers/specs/2026-11-16-session-panel-design.md` ("Hardware
interface", "Power"), `ARCHITECTURE.md` ("Hardware Interfaces", "MIDI Circuit",
"CV Circuit") and the pin/address constants in `include/*.h`.

The sheets are generated. Edit `gen/build.py` (layout and parts) or `gen/sch.py`
(the writer), never the `.kicad_sch` files, then run:

```bash
python3 hardware/schematic/gen/build.py --check
```

`--check` needs KiCad 10 (`kicad-cli`, `KICAD_CLI` to override the path) and the
KiCad symbol library (`KICAD_SYMBOL_DIR`). Every part in the generator declares
the net of each of its pins; the check exports the netlist with `kicad-cli` and
fails if the drawing connects anything differently, then runs ERC.

## Sheets

| # | File | Contents |
| --- | --- | --- |
| 1 | `rebirth_panel.kicad_sch` | Root: sheet boxes wired in signal order, GPIO and I2C address map |
| 2 | `power.kicad_sch` | VBUS entry, +3V3 summary, U201 TPS2553 current-limited switch making +5V_LED, power budget |
| 3 | `mcu.kicad_sch` | U301 Raspberry Pi Pico, every used GPIO, I2C pull-ups R301/R302, jitter test point TP301 |
| 4 | `midi.kicad_sch` | MIDI OUT (CA-033, 3.3 V: 33 R / 10 R, buffer U401), MIDI IN (H11L1 opto, reserved) |
| 5 | `cv_gate.kicad_sch` | U501/U502 MCP4822, U503 MCP6004 buffers, CV 1-4 jacks; U504 74AHCT125 gates 1-4 |
| 6 | `top_strip.kicad_sch` | OLED header J601 (SSD1309, SPI0), encoders TRACK/VALUE with RC filters, PLAY |
| 7 | `step_row.kicad_sch` | MCP23017 0x20 (step buttons 1-7, 9-15), 0x24 (steps 8, 16), 0x21 (16 step LEDs) |
| 8 | `key_matrix.kicad_sch` | U801 TCA8418 (0x34), 8 x 10 matrix: 64 pads, 8 SCENE, 8 MUTE, one 1N4148W per key |
| 9 | `rgb_chain.kicad_sch` | U901 74AHCT1G125, 82 x SK6812MINI-E with 100 nF each, bulk C984 (A2 sheet) |

Reference designators: the hundreds digit is the sheet. In the key matrix SW8kk and
D8kk carry the TCA8418 key event code kk (= 10 x row + column + 1). In the RGB chain
D9nn is chain position nn, which equals firmware logical index nn-1.

## Interfaces the firmware relies on

- GPIO map exactly as in the design: MIDI 0/1, OLED SPI0 2-6, jitter probe 7, I2C0 8/9,
  DAC SPI1 10-13 (CS 12 = U501 = CV1/CV2, CS 13 = U502 = CV3/CV4, channel A = odd CV),
  gates 14-17, encoder 1 = 18/19/20, encoder 2 = 21/22/26, LED data 27, PLAY 28.
- I2C: 0x20 buttons 1-7 and 9-15, 0x24 buttons 8 and 16, 0x21 LEDs, 0x34 TCA8418;
  external 2.2 k pull-ups. The 0x24 expander is new and needs firmware support
  (decision 10).
- Key matrix: ROW r = slot r+1; COL 0-7 = track 1-8, COL 8 = SCENE (row), COL 9 = MUTE
  (track = row + 1), as in the plan's table. TCA8418 INT not connected (polled).
- RGB chain order is the plan's logical order (`LED_PAD(track,row) = row*8 + track`,
  scene 64-71, mute 72-79, PLAY 80, BEAT 81), so `chain_order[]` can stay the identity.

## Part choices

| Function | Part | Why |
| --- | --- | --- |
| MCU | Raspberry Pi Pico on headers | as in the design |
| LED supply switch | TPS2553DBV, R201 = 100 k 1% | see "Decisions" |
| MIDI OUT driver | 74LVC1G14 inverter + MMBT3904 open collector, 470 R / 10 k base | CA-033 assumes an open-collector driver at 3.3 V; survives cable shorts |
| MIDI IN opto | H11L1 (3-15 V, Schmitt output, 1.6 mA turn-on) | works from 3.3 V, unlike the 6N138 |
| DACs | 2 x MCP4822 on +3V3, LDAC tied low | design; outputs update on CS rising edge |
| CV buffers | MCP6004 (RRIO, 1.8-6 V) on VBUS | 0-2.048 V with headroom, quad = one package |
| Gate buffer | 74AHCT125 on VBUS | TTL inputs take 3.3 V, outputs give 5 V gates |
| Key scanner | TCA8418RTW (WQFN-24, footprint QFN-24 4x4 EP 2.5) | design; project symbol `rebirth:TCA8418RTW` |
| RGB LEDs | SK6812MINI-E | design; reverse-mount part under MX-style keycaps |
| LED data buffer | 74AHCT1G125 on +5V_LED, OE low, 10 k pull-downs before and after, 390 R series | design (330-470 R) |
| LED rail bulk | C984 470 uF 10 V low-ESR | PWM current peaks; final value from a scope check |
| Key diodes | 1N4148W (SOD-123) | any small-signal diode |
| Keys | MX-type switches (footprint SW_Cherry_MX_1.00u_PCB) | assumption; any momentary switch works |
| Step buttons / LEDs | 6 mm tactile, red 3 mm LEDs with 470 R (~3 mA) | unchanged hardware; values are a choice |
| Encoders | EC11 with switch | unchanged hardware |
| Jacks | 3.5 mm TS (PJ398SM footprint) | rear panel |

Footprints are a starting point for the PCB step; no MPNs are set yet. The MIDI DIN
sockets have no footprint: KiCad's library has none for a panel DIN-5.

## Decisions the design leaves open

1. **U201 current-limited switch for +5V_LED.** The design powers the chain from VBUS
   with a bulk capacitor and leaves inrush to the bench. 82 x 100 nF + 470 uF is far
   above the 10 uF that USB allows at plug-in, and a firmware error at full white
   would ask for ~5 A. U201 charges the chain at its current limit (232-306 mA with
   R201 = 100 k 1 %), caps the chain current in hardware, and switches on with +3V3
   (EN threshold 0.66-1.1 V: it follows the regulator, not the firmware). The LED data
   buffer U901 runs from the same switched rail, so it never drives a data pin above an
   unpowered chain. R201 must stay >= 82 k (limit max 369 mA); if the bench raises the
   firmware budget, recompute R201 from TPS2553 eq. 1-3.
2. **CV buffers keep the 1 k output resistor inside the loop** (R52x feedback from the
   jack side, C51x closing the loop at HF): no DC error from the load, stable into cable
   capacitance, a short to GND draws at most ~5 mA. R515-R518 (100 k) hold the buffer inputs at
   0 V while the DACs are unpowered or in their power-on shutdown. Accuracy: the MCP6004
   cannot reach exactly 0 V (~25 mV at light load = up to 30 cents sharp at note 24), and
   the DAC's gain/offset errors (about +-2 % FSR) need a per-channel calibration in
   firmware, which does not exist yet.
3. **Gates are 5 V** (74AHCT125 on VBUS, 1 k series, 100 k pull-downs at the inputs and at
   the jacks). ARCHITECTURE.md only says the GPIOs are 3.3 V logic; see open point 3.
4. **Encoder RC filters** (10 k pull-up, 10 k series, 10 nF) on A/B; the internal
   pull-ups stay on in firmware and do no harm.
5. **AGND** of the Pico joins GND through R303 (0 R) at one point. The Pico symbol
   declares both pins as power outputs; the link keeps that explicit and ERC clean.
6. **OLED** as a 7-pin header to a module (GND VCC SCK SDA RES DC CS, the common order);
   check the actual module.
7. **MIDI ferrite beads** (CA-033 optional) are drawn; fit 0 R links if not wanted.
   MIDI IN pin 2 and both shells are left open.
8. **MIDI OUT open collector.** CA-033 assumes an open-collector/open-drain driver at
   3.3 V and warns about the driver's short-circuit current. A push-pull gate would see
   3.3 V / 10 R into its output with pin 5 shorted to ground. U401 (74LVC1G14) inverts
   the UART, Q401 (MMBT3904) sinks the loop: pin 5 to GND costs nothing, pins 4-5 shorted
   pass <= 85 mA through Q401 (200 mA max). Same polarity as before, no firmware change.
9. **Defined states**: R802 pulls the TCA8418's open-drain INT up (datasheet), R903 holds
   the first LED's DIN low while U901 is unpowered, C506/C507 add local bulk at U502 and
   the VBUS buffers.
10. **Steps 8 and 16 on a third MCP23017 (U703, 0x24; decided by the owner).** The current
    MCP23017 datasheet (DS20001952 rev. D, 2022, and later) makes GPA7/GPB7 output-only;
    Microchip's support note, as quoted on EEVblog, says a level change on them during a
    read can corrupt SDA, so buttons there could upset the whole I2C bus. SW708/SW716 now
    sit on U703 GPA0/GPA1 (A2 = 1, A1 = A0 = 0; 0x22/0x23 stay free for the design's
    matrix fallback). U701 GPA7/GPB7 and all unused U703 pins are left open.
    **Firmware change needed** (the firmware is not touched here): read U703 GPA0/GPA1 as
    steps 8/16 (same register setup as 0x20: inputs, pull-ups, inverted polarity), stop
    using U701 bits 7 and 15 as buttons, and configure GPA7/GPB7 of U701 and U703 as
    outputs. U703's spare pins can later drive U201 EN or TCA8418 RESET (open point 2).

## Open points for the owner (design-level, not changed here)

1. **TCA8418 latency.** TI's datasheet (SCPS215G, 8.6.2.15) scans the keypad every 25 ms
   and reports a press after 25-40 ms; disabling debounce does not change that. The design
   asks for <= 15 ms press-to-event. Either accept 25-40 ms (plus firmware debounce), or
   switch the matrix to the design's fallback scanner (two MCP23017), or lay out both on
   the board and fit one after a bench test.
2. **LED rail before enumeration.** EN = +3V3 turns the chain on before any firmware runs;
   82 x ~1 mA idle plus the Pico exceed the 100 mA an unconfigured USB device may draw.
   If the design's fallback (minimal USB enumeration asking for 500 mA) is ever needed,
   EN must be firmware-controlled (a spare U703 output with a pull-down). Enumeration
   goes through the Pico's own micro-USB (D+/D- are not on its header).
3. **Gate level.** Gates are 5 V here; confirm that the target synths want 5 V gates
   (most do), or drop U504 for 3.3 V gates through 1 k.
4. **DAC gain 2x is not possible on this board.** With the DACs on +3V3 only the default
   1x (0-2.048 V, about two octaves) works. `-DREBIRTH_CV_DAC_GAIN=2` would need the DACs
   on VBUS and 3.3 V -> 5 V buffers on their SPI lines (ARCHITECTURE.md already says so);
   a firmware `#error` for gain 2 on this board would prevent a wrong build.

## Notes for the PCB step

- **SK6812MINI-E pin numbers** differ between OPSCO documents: the current LCSC datasheet
  (SK6812MINI-E-012 Rev. B/1, LCSC C5149201) numbers 1 GND, 2 DIN, 3 VDD, 4 DOUT, like
  KiCad's symbol and footprint; the older OPSCO spec rev. 02/04 numbers 1 VDD, 2 DOUT,
  3 GND, 4 DIN. The pad functions are the same geometrically. Order the C5149201 part
  and check the reel's drawing against the footprint before layout.
- Fix MPNs and datasheets for the OLED module (pin order, 3.3 V, current), the DIN-5
  sockets (no KiCad footprint) and the jacks (PJ398SM's TN contact stays unconnected).
- C984 voltage rating is in its value (10 V); keep it, a 6.3 V part is too close to VBUS.
- Step LEDs are driven high-side by the MCP23017 (VOH = VDD - 0.7 V at 3 mA): expect
  ~1.5-3 mA each; adjust R701-R716 on the bench.
- Optional: 10 k pull-ups on DAC_CS1_N/DAC_CS2_N (not drawn: the DACs power up in
  shutdown and the firmware writes them at start).

## Verification

- Netlist exported from the root equals the generator's declared nets (307 nets, every
  pin of every part, 0 differences); `kicad-cli sch erc`: 0 violations (with KiCad's
  default symbol library table installed; without it ERC reports library warnings only).
- `readable-schematic` lint: 0 errors. Advisories accepted: `flag_side` on the Pico's
  pin list (labels sit where the symbol's pins are), `sheet_jump` for the SPI labels
  repeated at each DAC and the I2C labels repeated at each expander, `sheet_fill` on the
  power sheet.
- kicad-happy schematic analyzer: detects the 10 x 8 key matrix (80 keys) and the
  82-LED chain. Its findings triaged: J402 without ground pins is required by CA-033;
  "LED_DATA 3.3 V/5 V crossing without level shifter" is wrong, U901 is the shifter;
  no MPNs/datasheets (DS-001, SS-001) is expected at this stage; no ESD parts on the
  jacks and MIDI sockets (protection is series resistors and the opto) is a PCB-stage
  choice.
- Pin numbers checked against datasheets for TCA8418 (TI SCPS215G, RTW), TPS2553
  (SLVS841, DBV), SK6812MINI-E (see above) and the CA-033 resistor values; the KiCad
  library symbols of MCP4822, MCP23017, MCP6004, 74AHCT125, 74AHCT1G125, 74LVC1G14,
  MMBT3904, H11L1 and the Pico match their standard pinouts.
- Independent review (2026-11-16) by Codex and Grok, plus DeepSeek's complete answer
  from a run that ended with an error afterwards; Gemini could not run (API credits).
  All three found the MCP23017 GPA7/GPB7 problem, two the TCA8418 latency, all three the
  MIDI OUT short-circuit current; the fixes and open points above come from that review,
  checked against the datasheets. A reported reversed SK6812 pinout was a documentation
  numbering difference, not a wiring error.
- Not done: SPICE simulation of the CV buffers and the RC filters, PCB, BOM with MPNs.
