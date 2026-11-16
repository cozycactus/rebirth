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
| 8 | `key_matrix.kicad_sch` | 8 x 10 key matrix: 64 pads, 8 SCENE, 8 MUTE, one BAT54J per key; ROW/COL ports |
| 9 | `rgb_chain.kicad_sch` | U901 74AHCT1G125, 82 x SK6812MINI-E with 100 nF each, bulk C984 (A2 sheet) |
| 10 | `key_scanner.kicad_sch` | Key scanner: U801/U802 MCP23017 at 0x22/0x23 driving the matrix columns and reading its rows |

Reference designators: the hundreds digit is the sheet (sheet 10 uses 8xx, it belongs to
the key matrix). In the key matrix SW8kk and D8kk carry the key number kk
(= 10 x row + column + 1, as in the plan's table). In the RGB chain D9nn is chain
position nn, which equals firmware logical index nn-1.

## Interfaces the firmware relies on

- GPIO map exactly as in the design: MIDI 0/1, OLED SPI0 2-6, jitter probe 7, I2C0 8/9,
  DAC SPI1 10-13 (CS 12 = U501 = CV1/CV2, CS 13 = U502 = CV3/CV4, channel A = odd CV),
  gates 14-17, encoder 1 = 18/19/20, encoder 2 = 21/22/26, LED data 27, PLAY 28.
- I2C: 0x20 buttons 1-7 and 9-15, 0x24 buttons 8 and 16, 0x21 LEDs, 0x22 + 0x23 key
  scanner; external 2.2 k pull-ups. **No TCA8418 (0x34) on this board** (decision 11):
  0x24, 0x22 and 0x23 need firmware support (decisions 10 and 11).
- Key matrix: ROW r = slot r+1; COL 0-7 = track 1-8, COL 8 = SCENE (row), COL 9 = MUTE
  (track = row + 1), as in the plan's table. Scanner pins: U801 (0x22) GPA0-6 = ROW0-6,
  GPB0 = ROW7, GPB1-7 = COL0-6, GPA7 = COL7; U802 (0x23) GPA0 = COL8, GPA1 = COL9.
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
| Key scanner | 2 x MCP23017 (SOIC-28W) at 0x22/0x23 | the design's fallback, chosen for latency (decision 11) |
| RGB LEDs | SK6812MINI-E | design; reverse-mount part under MX-style keycaps |
| LED data buffer | 74AHCT1G125 on +5V_LED, OE low, 10 k pull-downs before and after, 390 R series | design (330-470 R) |
| LED rail bulk | C984 470 uF 10 V low-ESR | PWM current peaks; final value from a scope check |
| Key diodes | BAT54J Schottky (SOD-323F) | low Vf keeps a pressed row well below the MCP23017's VIL |
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
   jack side, C51x 1 nF C0G closing the loop at HF): no DC error from the load, damped
   into cable capacitance (6 % overshoot at 1 nF, 28 % at 10 nF; with 100 pF it rang at
   34-38 % / 66-68 %, see SIMULATION.md), a short to GND draws at most ~5 mA. R515-R518
   (100 k) hold the buffer inputs at 0 V while the DACs are unpowered or in their power-on
   shutdown. Accuracy: near 0 V the jack follows the DAC only above ~10 mV (MCP4822 swing;
   the MCP6004 itself reaches ~0.2 mV into a grounded load), up to ~17 cents sharp at
   note 24 with the op-amp offset, and the DAC's gain/offset errors (about +-2 % FSR)
   need a per-channel calibration in firmware, which does not exist yet.
3. **Gates are 5 V** (74AHCT125 on VBUS, 1 k series, 100 k pull-downs at the inputs and at
   the jacks). ARCHITECTURE.md only says the GPIOs are 3.3 V logic; see open point 2.
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
9. **Defined states**: R903 holds the first LED's DIN low while U901 is unpowered, C506/C507 add local bulk at U502 and
   the VBUS buffers.
10. **Steps 8 and 16 on a third MCP23017 (U703, 0x24; decided by the owner).** The current
    MCP23017 datasheet (DS20001952 rev. D, 2022, and later) makes GPA7/GPB7 output-only;
    Microchip's support note, as quoted on EEVblog, says a level change on them during a
    read can corrupt SDA, so buttons there could upset the whole I2C bus. SW708/SW716 now
    sit on U703 GPA0/GPA1 (A2 = 1, A1 = A0 = 0; 0x22/0x23 are the key scanner of
    decision 11). U701 GPA7/GPB7 and all unused U703 pins are left open.
    **Firmware change needed** (the firmware is not touched here): read U703 GPA0/GPA1 as
    steps 8/16 (same register setup as 0x20: inputs, pull-ups, inverted polarity), stop
    using U701 bits 7 and 15 as buttons, and configure GPA7/GPB7 of U701 and U703 as
    outputs. U703's spare pins can later drive U201 EN (open point 1).
11. **Key matrix scanned by two MCP23017, no TCA8418 (decided by the owner: low latency
    first).** TI's datasheet (SCPS215G, 8.6.2.15) scans the keypad every 25 ms and reports
    a press after 25-40 ms, debounce disabled or not; a faster I2C clock does not change
    that. The design asks for <= 15 ms press-to-event, so the board uses the design's own
    fallback: U801 (0x22) + U802 (0x23) MCP23017 on ROW0-7/COL0-9 (sheet 10). This is a
    change to the approved design ("Grid electronics" and "Hardware interface" name the
    TCA8418) and the firmware's TCA8418 driver is not used on this board.
    Pin map: U801 GPA0-6 = ROW0-6, GPB0 = ROW7 (all rows in one 2-byte read), GPB1-7 =
    COL0-6, GPA7 = COL7; U802 GPA0/GPA1 = COL8/COL9. Rows are inputs with the internal
    pull-ups, columns push-pull outputs, one low at a time, the others high; the
    output-only GPA7/GPB7 only drive columns. Key diodes are BAT54J Schottky: a pressed
    key pulls its row to ~0.25 V against the MCP23017's VIL of 0.2 VDD = 0.66 V
    (DS20001952, D031); a silicon 1N4148 at the 40-115 uA pull-up current would leave only
    ~0.1 V of margin. **Firmware change needed**: a scan driver on Core 0. One column step
    is a 3-byte write plus a 2-byte read, about 0.2 ms at 400 kHz, so a full scan takes
    2-3 ms; scanning every ~4 ms keeps press-to-event (scan + 5 ms debounce) near 10 ms.
    The I2C bus then carries the scan next to the step-row traffic.

## Open points for the owner (design-level, not changed here)

1. **LED rail before enumeration.** EN = +3V3 turns the chain on before any firmware runs;
   82 x ~1 mA idle plus the Pico exceed the 100 mA an unconfigured USB device may draw.
   If the design's fallback (minimal USB enumeration asking for 500 mA) is ever needed,
   EN must be firmware-controlled (a spare U703 output with a pull-down). Enumeration
   goes through the Pico's own micro-USB (D+/D- are not on its header).
2. **Gate level.** Gates are 5 V here; confirm that the target synths want 5 V gates
   (most do), or drop U504 for 3.3 V gates through 1 k.
3. **DAC gain 2x is not possible on this board.** With the DACs on +3V3 only the default
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
- Pin numbers checked against datasheets for TPS2553 (SLVS841, DBV), SK6812MINI-E (see
  above), MCP23017 thresholds (DS20001952) and the CA-033 resistor values; the KiCad
  library symbols of MCP4822, MCP23017, MCP6004, 74AHCT125, 74AHCT1G125, 74LVC1G14,
  MMBT3904, BAT54J, H11L1 and the Pico match their standard pinouts.
- Independent review (2026-11-16) by Codex and Grok, plus DeepSeek's complete answer
  from a run that ended with an error afterwards; Gemini could not run (API credits).
  All three found the MCP23017 GPA7/GPB7 problem, two the TCA8418 latency, all three the
  MIDI OUT short-circuit current; the fixes and open points above come from that review,
  checked against the datasheets. A reported reversed SK6812 pinout was a documentation
  numbering difference, not a wiring error.
- SPICE simulation of the CV outputs, gates, encoder filters, MIDI OUT/IN and the
  +5V_LED inrush: `SIMULATION.md` (benches in `sim/`). It changed C511-C514 from 100 pF
  to 1 nF.
- Not done: PCB, BOM with MPNs.
