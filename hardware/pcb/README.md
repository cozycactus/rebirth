# PCB: main board and panel board

Two KiCad 10 projects, one per board, generated from the schematic
(`hardware/schematic/`, its README first):

| Board | Project | Size | Contents |
| --- | --- | --- | --- |
| Panel board | `panel/panel.kicad_pro` | 248 x 251 mm | 8x8 pads, 8 SCENE, 8 MUTE, PLAY (81 MX switches), 82 SK6812MINI-E, 80 BAT54J, 16 step buttons and LEDs, 5 MCP23017, encoders, OLED socket, LED data buffer, cable header J1201 |
| Main board | `main/main.kicad_pro` | 244 x 57.5 mm | Raspberry Pi Pico, LED-rail switch U201, MIDI OUT/IN, 2 x MCP4822 + MCP6004, gate buffer, 8 jacks, 2 DIN-5, cable header J1101 |

The two boards are joined by a 26-wire IDC ribbon, J1101 pin n to J1201 pin n.

## Decisions (owner, 2026-11-16)

| Question | Decision |
| --- | --- |
| One board or two | Two: panel board under the top plate, main board on the case floor at the rear wall |
| Copper layers | 2 on both boards, 1.6 mm FR4, 1 oz |
| Fab and assembly | JLCPCB, their SMT assembly with LCSC parts; through-hole parts soldered by hand |
| Pads | MX-type switches soldered to the panel board, translucent 17 mm keycaps through windows in the 2 mm aluminium top plate, board on 15 mm standoffs |
| LED rail enable | JP201: EN = +3V3 as designed, GPIO 7 by moving a solder jumper |
| Jack protection | 5 V TVS on all eight jacks |
| Case height | 28 mm at the front, 50 mm at the rear (mockup: 20/45 mm) |

The board plan was reviewed independently (Codex, DeepSeek, Grok; Gemini could not run) before
layout. What came out of it is in the schematic README (decisions 12-14) and below.

## Regenerating

The boards are generated, like the schematic. With KiCad 10 installed:

```bash
python3 hardware/schematic/gen/build.py --check          # schematics of all three projects
PY=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
$PY hardware/pcb/gen/build_pcb.py panel place            # footprints, outline, holes, regular routing
$PY hardware/pcb/gen/build_pcb.py panel maze             # grid router for the rest (panel)
$PY hardware/pcb/gen/build_pcb.py panel finish           # GND pours, stitching vias, DRC + parity
```

For the main board: `place`, then
`FREEROUTING_JAR=/path/to/freerouting-2.4.1.jar $PY hardware/pcb/gen/build_pcb.py main route`,
then `maze` and `finish`. `place` overwrites the board file; `route` hands the unrouted
connections to Freerouting (GND is left out, it is poured); `maze` joins what is still open
with the grid router in `gen/maze.py`; `finish` adds the GND pours on both layers and stitching
vias, joins GND islands, refills and runs `kicad-cli pcb drc --schematic-parity`. Run `finish`
again until the DRC summary it prints stops changing (two or three times). The panel takes
about half an hour, the main board a few minutes. Placement is
in `gen/panel.py` and `gen/mainboard.py`, the panel geometry in `gen/geometry.py` (from the
Blender mockup `hardware/panel/build_panel.py`).

## Mechanics (case frame: x to the right, y toward the rear wall, z up)

The Blender model `hardware/panel/build_internals.py` shows all of this and animates it
(scene "Sequencer Internals": the plate and caps lift off, the lid with the panel board opens).

- **Stack under the top plate.** Plate 2 mm; panel board top 15 mm below the plate's underside on
  M3 standoffs. Cherry MX: housing top 11.6 mm, stem top 15.2 mm above the board (MX2A datasheet:
  18.5 mm total with 3.3 mm pins); a DSA-height cap reaches ~20 mm, ~3 mm above the plate, and
  goes ~1 mm below the plate surface at full travel. Plate windows: caps + 0.8 mm. Change the
  standoffs with the keycap profile.
- **Case height** 28 mm at the front, 50 mm at the rear (tilt 4.7 deg). At the front edge the
  panel board's underside is ~12 mm above the 3 mm floor; at the rear ~2 mm stay between the
  DIN sockets and the panel board.
- **Panel board holes** (17 x M3, plated, on GND) sit around the grid block, between the step
  groups and beside the top strip; none inside the 8x8 grid (the caps leave no room for a
  standoff). Centre deflection of the 1.6 mm board between the supports is a few tenths of a
  millimetre for a 10 N press; if the grid feels soft, add a non-conductive support from the
  case floor under the grid centre. The aluminium plate is grounded through the standoffs only
  if it is bare or chromated at the screw faces (anodising insulates): use toothed washers or
  a ground tab and check < 1 ohm from the plate to GND.
- **Top strip.** EC11 encoders need a ~30 mm shaft from the board (the KiCad footprint name
  says H20mm; the pinout is the same) and pass the plate without a nut; the OLED module sits on
  ~11 mm spacers so the glass is just under the 57 x 29 mm window, plugged into the 7-pin
  socket J601 at its top edge (check the module's header position and pin order before
  ordering, or join them with a short cable). BEAT (D982) lights through a light pipe; the
  step buttons are tall 6 x 6 tactiles (~15 mm) with caps, the step LEDs stand on long legs up
  to Ø4 mm plate holes (sleeve the legs: the plate is on GND).
- **Main board** on 5 mm standoffs (7 x M3, plated, on GND, one beside the DIN sockets to take
  their up-to-5-kgf plug force). The Pico's edge is flush with the board's rear edge, so its
  micro-USB receptacle sits ~2 mm inside the 3 mm rear wall: thin the wall around the USB
  opening to ~1 mm, or use a short panel-mount extension. The jacks (CUI SJ1-3513N, no thread)
  and the DIN-5 sockets (SDS-50J) have their faces at the board's rear edge.
- **Cable.** Both headers are right angle and open toward the front: J1201 under the panel
  board above J1101 at the main board's front edge; the ribbon leaves both plugs forward and
  folds back on itself (~100 mm). Keyed headers and keyed IDC sockets, stripe = pin 1 on both
  ends.

## Cable J1101 - J1201 (wire n = pin n)

| Pin | Net | Pin | Net |
| --- | --- | --- | --- |
| 1 | +5V_LED | 2 | +5V_LED |
| 3 | GND | 4 | LED_DATA |
| 5 | GND | 6 | OLED_SCK |
| 7 | GND | 8 | OLED_MOSI |
| 9 | OLED_DC | 10 | OLED_CS_N |
| 11 | OLED_RST_N | 12 | PLAY_N |
| 13 | +3V3 | 14 | +3V3 |
| 15 | GND | 16 | I2C_SCL |
| 17 | GND | 18 | I2C_SDA |
| 19 | ENC1_A | 20 | ENC1_B |
| 21 | ENC1_SW_N | 22 | ENC2_A |
| 23 | ENC2_B | 24 | ENC2_SW_N |
| 25 | GND | 26 | GND |

A ribbon crimped the wrong way round joins pin n to pin 27 - n: +5V_LED meets GND (U201 limits
it to ~0.3 A) and +3V3 meets +3V3; no rail reaches another rail. Ring pin 1 to pin 1 before
the first power-up anyway. Up to 306 mA flows on the two +5V_LED wires and back on seven GND
wires (well within a 28 AWG ribbon and IDC contacts).

## Panel board

- **Sides.** Every SMD part is on the bottom (B), so the board needs one SMT pass; through-hole
  parts (81 MX switches, 16 tactiles, 16 step LEDs, 2 encoders, OLED socket J601, cable header
  J1201) are hand-soldered, the switches from the top. The SK6812MINI-E are reverse mounted:
  KiCad's `LED_SK6812MINI-E_3.2x2.8mm_P1.5mm_ReverseMount` placed on B.Cu, the lens in the
  board cutout under the MX LED window, 5.08 mm from the switch centre on the side away from
  the switch pins. Their pads are 0.2-0.25 mm from that cutout; `panel.kicad_dru` allows 0.2 mm
  for them only (JLCPCB routes to 0.2 mm), the rest of the board keeps 0.25 mm. JLCPCB lists
  C5149201 as Standard PCBA only; check the reel's pin 1 against the footprint (schematic
  README, notes for the PCB step) and the bottom-side rotation in the CPL before ordering.
- **Key cell** (19.5 mm pitch, KiCad coordinates relative to the switch centre, +y toward the
  front): MX footprint origin at (+2.54, -5.08); LED at (0, +5.08); its 100 nF at (+4.6, +5.8),
  vertical; BAT54J at (+4.9, -7.6), anode toward switch pin 1. Copper: ROW line on B.Cu through
  switch pin 2 at y -2.54, COL line on F.Cu at x +7.0 with the diode cathode on a via, LED chain
  on B.Cu above the LED pads, +5V_LED bus (0.6 mm) on B.Cu at y +8.6, two GND vias per cell.
- **+5V_LED**: J1201 pins 1-2 -> 1 mm F.Cu under the header body (clear of both pin rows, so
  the cable pins can leave downward on either layer) -> via -> C984 (470 uF) -> 1 mm feed along
  the top of the grid -> 1 mm spine on F.Cu down the right side (x = 95.5) -> a via onto every
  row bus and the mute bus.
- **Expanders.** Step row: U701 (0x20) and U703 (0x24) at the left margin, U702 (0x21, LEDs) at
  the right; key scanner U801 (0x22) and U802 (0x23) at the right margin next to the ROW line
  ends. Each expander's RESET pull-up sits right of its pin 18.
- **Step row buses.** Between the step LEDs (with their 470 R underneath) and the step buttons
  is a channel ~8 mm high. B.Cu carries U702's 16 LED lines to the 470 R, F.Cu the 16 button
  lines from U701/U703, 0.5 mm pitch, both drawn by `gen/panel.py`. On both expanders one pin
  column comes out in the reverse order of the row; the LED lines solve it by leaving U702 on
  both sides (GPB7 takes F.Cu round H12), the button lines by a diagonal of vias on U701's
  right column and a B.Cu loop under the chip to a staircase of vias for its left column. They
  come down between D701 and D702. U901 (LED data buffer) and C984 sit next to J1201, so the chain starts and the LED
  current enters at the same corner.
- **Routing.** The key grid, the LED supply and the step-row buses are drawn by `gen/panel.py`;
  the rest by a grid
  router (`gen/maze.py`: A* on a 0.2 mm grid on both layers, KiCad's clearances grown into the
  grid, no vias in SMD pads or next to holes). A connection that finds no way gets a trial path
  that may cross other signal nets at a cost; only the nets it crosses are ripped up and routed
  again after it, and the best of up to eight rounds is kept. Freerouting made no progress on this board (182 of 324
  connections still open after two passes and 30 minutes). Many long runs cross the grid
  diagonally: electrically fine, tidier by hand later if wanted.

## Main board

- **Left:** the Pico U301 on 8.5 mm sockets, its USB end 0.5 mm inside the rear edge; the
  source terminations R1101-R1105 and R561-R564 right at its GPIO pins (R1106 for LED_DATA on
  the other side); U201 (LED-rail switch), C201/C202, JP201 and TP201 beside its VBUS pin.
- **J1101** in front of the Pico, opening to the front edge, so the LED supply and its return
  stay at the Pico end of the board.
- **Middle:** MIDI OUT behind J401, MIDI IN (H11L1, DIP-6) behind J402.
- **Right:** gate buffer U504 between the gate jacks, op-amp U503 between the CV jacks, the two
  DACs in front of it; the 1 k / 10 k / 1 nF network and the TVS of each channel in front of
  its jack. The DAC SPI lines (~170 mm, 33 R at the source) run along the front of the board.
- All SMD parts on the top (one SMT pass). JLCPCB's Standard PCBA asks for at least 70 mm on
  the short side: order the 57.5 mm board with edge rails, or use Economic PCBA if every part
  qualifies.
- **Routing:** Freerouting, then `gen/maze.py` for the few connections it leaves (with rip-up of
  the nets that wall a pad in). GND poured on both layers, stitched.

## Fabrication (JLCPCB)

- 2 layers, FR4 1.6 mm, 1 oz copper, ENIG preferred (fine reverse-mount LED pads; HASL works).
- Rules: routed at 0.2 mm track/space; the DRC holds 0.15 mm (JLCPCB minimum 0.127 mm), vias
  0.6/0.3 mm (power 0.8/0.4 mm), copper 0.25 mm from the outline (0.2 mm for the LED pads at
  their cutouts), hole to hole 0.5 mm.
- Fiducials: three per assembly side (panel: bottom, main: top). Mounting holes are plated
  and on GND. Reference designators of the dense parts are on the fab layers, so the
  silkscreen stays readable; the assembly drawing comes from F.Fab/B.Fab.
- Not prepared here: BOM with LCSC numbers and the CPL (the schematic has no MPNs yet).

## Verification (2026-11-16, KiCad 10.0.7)

| Check | Main board | Panel board |
| --- | --- | --- |
| `kicad-cli pcb drc --schematic-parity --severity-all` against the rules above | 0 violations, 0 unconnected, 0 parity issues | 0 violations, 0 unconnected, 0 parity issues |
| Footprints / nets on the board = schematic | 109 / 91 | 437 / 307 |
| Copper connectivity (kicad-happy `analyze_pcb.py --full`, union-find over pads, tracks, vias and fills) | every net one island | every net one island |
| kicad-happy `cross_analysis.py` | 2 info (MIDI DIN pins without ESD parts) | 0 findings |

Schematics: `python3 hardware/schematic/gen/build.py --check` gives 0 problems and 0 ERC
violations for all three projects (380 / 91 / 307 nets) and 26 of 26 cable pins matched; the
generated sheets are unchanged by the PCB work. Copper pictures: `renders/panel_copper.jpg`,
`renders/main_copper.jpg` (F.Cu pour drawn over B.Cu).

kicad-happy findings that stay, and why:

- Main, PM-002 "courtyard overhangs the board edge": J401/J402 (0.5 mm), J501-J508 (4 mm) and
  U301 (1.06 mm). On purpose: the jack and DIN faces and the Pico's USB end sit at the rear edge.
- Main, CC-002: two short MIDI_TX segments (under 3 mm together) at 0.15 mm, where Freerouting
  necked down between pins; above JLCPCB's 0.127 mm, and the line carries ~5 mA. Everything else
  is 0.2 mm or wider.
- Both, TE-001 test-point coverage (2 of 88 nets on main, none on the panel): bring-up uses the
  Pico pins, the jacks and the cable header; TP201 and TP301 are the only test points.
- Main schematic, CG-AUD "J402 has no ground pin": MIDI IN leaves pin 2 open by the MIDI spec.
- Main schematic, PU-001 "U201 EN without pull-up": EN has R203 to GND and JP201 to +3V3 (or
  GPIO 7), as designed.
- Both schematics, DS-001 / SS-001: no datasheets folder and no MPNs, so the analyzer's checks
  are consistency checks only, not checks against the parts' datasheets.

Not done here: Gerbers, drill files, BOM with LCSC numbers and the CPL; the EMC, thermal and
SPICE passes of kicad-happy; a readable-schematic lint of the regenerated sheets (it timed out
after 600 s; the sheets were checked by eye on renders). Freerouting results vary from run to
run, so a regenerated main board is not identical to the committed one; the panel's routing
comes out the same every time.
