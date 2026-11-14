# pcbgen – KiCad generator for hardware v2.0

Scripts that produced `pcb/v2.0/` (schematic, placement, routing, silkscreen, production files).
The KiCad files in `pcb/v2.0/` are the reference; use this generator to rebuild them after a
circuit or layout change (or edit the board directly in KiCad for small changes).

| File | Purpose |
|------|---------|
| `design.py` | parts, values, footprints and nets (the netlist) |
| `lcsc.py` | LCSC / JLCPCB part numbers |
| `schem_*.py` | schematic placement per function block (`gen_sch2.py` draws the wiring) |
| `layout_*.py` | board outline, fixed parts, placement zones, labels, protection positions |
| `gen_sch2.py` | drawn schematic (wires, power symbols, labels) |
| `gen_pcb2.py` | board: footprints, connectivity-aware placement, rotation pass, keep-outs |
| `finish.py`, `bridge.py` | session import, ground pours, stitching/island vias, ground bridges |
| `brand.py`, `fixrefs.py` | silkscreen: terminal titles, notes, reference texts |
| `backdoc.py` | standalone back-side operating reference, 0.8 mm text, preserving pin labels |
| `checknet.py`, `domaincheck.py`, `drc.py` | checks: netlist vs. design, isolation, DRC summary |
| `build2.sh` | full flow: schematic → ERC → netlist → placement → parallel Freerouting attempts → best board |
| `export.sh` | Gerber/drill ZIP, JLCPCB BOM/CPL, schematic PDF, renders, final DRC |
| `jlcpcb_cpl.py` | Corrected JLCPCB CPL from temporary raw positions, with article/angle guards |
| `simulate_fan_pwm.py` | Bounded Q4 PWM subcircuit regression with an externally downloaded vendor model |

Requirements: KiCad 10 (`kicad-cli` and its bundled Python with `pcbnew`), Java and
[Freerouting](https://github.com/freerouting/freerouting) 2.5 (`FREEROUTING_JAR=/path/freerouting-2.5.0.jar`).

```bash
mkdir -p tools/pcbgen/proj && cp pcb/v2.0/*.kicad_pro tools/pcbgen/proj/
FREEROUTING_JAR=~/freerouting-2.5.0.jar ATTEMPTS=12 PARALLEL=4 bash tools/pcbgen/build2.sh tools/pcbgen/proj
bash tools/pcbgen/export.sh tools/pcbgen pcb/v2.0 universal-weather-node
```

Routing: every attempt gets a slightly varied placement (`JITTER=1`), ground is routed as a normal
net before the pours are added (`NOPLANE=1`, avoids isolated ground islands); the attempt with the
fewest DRC errors is kept. A few silkscreen texts of the released board were touched up by hand
afterwards (test-point labels, brand text position) – check the top render after a rebuild.
`brand.py` also invokes `backdoc.py` when `BACK_DOCUMENTATION` is enabled.
The back reference uses 0.8 mm mirrored B.Silkscreen text, reads normally from
the back, and documents power/USB restrictions, every field connector, jumper
positions, limits, ADC scaling, GPIOs, test points, startup and hardware mounting.
Pinouts are by connector pin number, not left-to-right in the reversed view.
Existing labels beside the pins remain. To update only this documentation,
run `backdoc.py <board.kicad_pcb>` with KiCad Python; it replaces the previous
summary blocks and fails without saving if every block cannot fit. Re-run DRC
and inspect the bottom render after any layout change. It does not modify
electrical copper, footprints or the schematic.
J3 is now a two-pin VIN/GND terminal (6–29.2 V); the external 5 V branch is
removed. JP8 is a two-pin buck disconnect: fit for VIN operation, **remove
before USB** and disconnect VIN. There is no automatic source isolation.
USB powers logic and sensors; a 5 V fan shares the Heltec/USB current budget.
Heater and VIN-mode fan loads require VIN.
The [USB-C/VIN function matrix](../../README.md#functions-by-power-source-v20)
lists supported interfaces, jumper settings, firmware-update modes and shared
current limits. The [bring-up checklist](../../pcb/v2.0/CHECKLIST.md#b-bring-up-of-the-first-prototypes)
includes the corresponding prototype checks for both power modes.

## JLCPCB orientation review

`export.sh` uses temporary raw KiCad positions under `tools/pcbgen/out/`,
generates the corrected `*-cpl-jlcpcb.csv` and removes the temporary files.
Only one order BOM and one corrected CPL are published. Rotation rules and
check statuses live in the helper, not additional order-like CSV files.

The numeric SOIC +270 and SOT-23 +180 rules were initially researched in
[Fabrication-Toolkit](https://github.com/bennymeg/Fabrication-Toolkit/blob/7f627b6be76081b962161b9eb9fe3a326a18c915/plugins/transformations.csv).
The helper is independently implemented; it does not copy toolkit source.
Successive supplied JLCPCB previews were then checked against PCB pads:

- U1-U5 and Q3: +270 degrees. U2/U3, U5 and Q3 offsets were determined from
  their selected library pin-1 marks, not a blanket package rule.
- SOT-23 parts C15127, C20917, C37130, C20526, C8592 and C49338: +180 degrees. BAT54S is now C7420333 (R+O); +180 degrees confirmed in the 2026-11-06 10:19:27 UTC preview.
- C6: no offset; plus already agrees with the PCB, unlike the generic
  electrolytic rule.
- The former Q4, Q7 and the two-terminal diodes: no offset; tab/gate and cathode bands
  agree in the final preview.

The 2026-11-04 changes replace Q4 with DMP6023LE (C154901, SOT-223).
This article is **not** covered by the old preview. The supplied
2026-11-04 preview (19:26:57 UTC) showed Q4 pin 1
upper right instead of lower left: the CPL now applies **+180 degrees**
(raw 90 -> CPL 270) with identity/angle guards. The subsequent 19:31:57 UTC
preview confirmed gate lower left, source lower right and drain tab top
(`PREVIEW_MATCHED_SOT223`). U6 was subsequently removed together with the
external 5 V input; do not reuse the earlier manufacturing files.
Check all pads in the final order; preview status is not hardware or assembly approval.

For Q3, the user confirmed the purple dot as library pin 1 and the
1-3 source / 4 gate / 5-8 drain mapping. The final supplied preview at
21:39:32 UTC shows pin 1 upper right, gate upper left and drain below.
All positions, sides and references remain unchanged.

```bash
python3 -B tools/pcbgen/jlcpcb_cpl.py \
  /path/to/temporary-raw-cpl.csv \
  pcb/v2.0/production/universal-weather-node-v2.0-bom-jlcpcb.csv \
  pcb/v2.0/production/universal-weather-node-v2.0-cpl-jlcpcb.csv
node --test test/pcb_cpl.test.js
```

The helper refuses changed corrected IC/Q3 parts, footprints or source angles,
duplicate references, missing corrected parts, bottom-side placements and
overlapping input/output paths. These guards prevent accidental double correction.
If the board or article selection changes, reassess the final preview instead of
bypassing the guards. A preview-matched status records this design's supplied
images, not proof that future orders or library models are unchanged.

Final order check (top view, sensor terminals at bottom): U1 pin 1 upper left,
U4 pin 1 lower right, U2 pin 1 lower left, U3 pin 1 lower right, U5/Q3 pin 1
upper right. Recheck all remaining pin-1 and polarity marks before paying.
Visual checks do not
qualify the hardware or authorize payment/production on the user's behalf.

The 2026-11-05 11:10:52 UTC preview shows C31 (47 uF/63 V, C445452)
reversed: the part's plus is left, but PCB plus/pad 1 is right. Its
identity/angle-guarded CPL entry now adds **180 degrees** (raw 180 -> CPL 0).
The subsequent 2026-11-05 11:15:14 UTC preview confirms plus right,
minus left (`PREVIEW_MATCHED_POLARITY`). C6's previously reviewed zero offset
is unchanged. This visual confirmation is not hardware qualification.

VIN uses Q7 with a 12 V gate-source clamp; D1 is the **bidirectional SMBJ30CA**
(C83847), upstream of Q7. F2/D4/Q9/R56/U6/R57–R60/C28–C30 and their
external-5V nets are removed from the schematic, PCB and production files.
Q4 uses DMP6023LE with R32=75k and the gate-driver bank
`(R33 + R62) || (R63 + R64) = 2k`. All four are existing C17944
2k/1%/0.25 W/1206 parts: no new article/feeder, three extra placements;
actual setup/assembly savings require a supplier quote.
The conservative 29.2 V estimate is 0.171 W total, at most 0.043 W each
over all sixteen individual resistor corners. The nominal 1 W sum does
not qualify thermal coupling, ambient derating or surge-pulse endurance.
The explicit four-resistor model in `simulate_fan_pwm.py` retains the 1 ms
turn-off check; it is not a whole-board or real-fan qualification.
R61=10k and JP12
provide an optional +5V tacho pull-up; fit JP12 for open collector, remove it
for an externally pulled-up or push-pull signal.
The former crowbar D3 is removed. Do not substitute a unidirectional D1:
it would recreate the reverse-polarity short circuit. Bidirectional D1
has no polarity; ignore the standard SMB footprint's cathode silk bar.
The single-MOSFET VIN circuit does not provide reverse-current isolation between
simultaneously powered sources. Rewire only with all rails discharged.

Hand touch-ups after the design review (repeat them after a rebuild; verify against the
current source before applying, because the power tree has changed):

- high-current input path: two extra 1.0/0.5 mm vias next to every original via of
  VIN_IN, VIN_F and +VLOAD, connected on both copper layers, with a few
  +VLOAD segments widened. The former D21 series diode was removed when Q7 became the common
  reverse-polarity switch; do not restore the old D21 copper/pour edits;
- removed dangling vias/track stubs and joined newly split ground islands; the
  final routed board has no dangling-item or unconnected-item warnings;
- 5 V gate driver U5 (74AHCT1G125, SC-70-5) + input pull-down R39 (0402) under the Heltec module: the HEAT_PWM
  trace was cut on its straight B.Cu segment, the load side renamed to HEAT_DRV, gaps routed by a grid router;
- silkscreen: JP1/JP11 labels beside the SDA/SCL headers, JP2 pin names re-placed, I2C pull-up notes,
  `R25 15k: AIN 0-5V x1.667`; back side: quick reference and jumper table,
  JP8/USB warning, and moved notes that previously sat on pads. The redundant
  Heltec silk envelope is on F.Fab; standard terminal pin-1 triangles still
  produce nine cosmetic edge warnings.

The buck uses the LMR36520's **400 kHz** design, not AP63205 values: L1 is
10 µH Bourns SRP5030T-100M, C26 is 1 µF/50 V VCC bypass, C2 is
220 nF/100 V X7R. `layout_w1.py` fixes F1 and the complete buck cluster;
`BUCK_PRE_ROUTE` preserves its front-only input, SW, BOOT and VCC routes.
These critical pre-routes fail explicitly if blocked instead of silently
leaving them to the autorouter. `LOCAL_PRE_ROUTE` similarly preserves the
small 0.3 mm U5/C32 supply and ground connections; do not use the wider
connector pre-route width between the SC-70 pins. Four 0.6/0.3 mm EP thermal vias and local
capacitor ground returns are also generated. Recheck the fully routed
result: the generator's placement test is not a full routing/thermal test.

Stability changes add C31 (47 uF/63 V on protected VIN), C32 (100 nF local
U5 bypass), C33 (second 10 uF/50 V Murata GRM32ER71H106KA12L input ceramic; the 100 V part became unavailable), and C34
(specified 22 uF/25 V 1210 output ceramic). C1 is the same specified Murata
article as C33; C4/C5 and USB-side C6 remain. The original C1/C2 hot loop
stays front-only; C33's reservoir branch has a short back-layer link.
Keep the added ground stitch near board-local (48.5, 37) mm, the readable
JP2 note and C31's reference outside its body after manual touch-ups.
See the [capacitor evidence and qualification limits](../../pcb/v2.0/CHECKLIST.md#electrical-stability-improvements).

Reproduce the fan check on macOS with KiCad's ngspice shared library:

```bash
mkdir -p tools/pcbgen/out
curl --fail --location \
  https://www.diodes.com/spice/download/2345/DMP6023LE.spice.txt \
  --output tools/pcbgen/out/DMP6023LE.spice.txt
python3 -B tools/pcbgen/simulate_fan_pwm.py
```

The third-party model is cached under ignored `out/`, not redistributed in
the sources. Results go to `pcb/v2.0/fan-pwm-stability.json`, outside the
three manufacturing order files. The check covers a resistive fan substitute,
an ideal Q5 and an approximate zener; it does not qualify thermal, transient,
EMI or real-fan behavior. The maximum simulated turn-off delay is 864 us
within a 1 ms PWM off-window, not a guaranteed component-corner maximum.

F7 now maps to the actual SMD Littelfuse 045106.3MRL (C178982), using the
standard NANO2-451/453 footprint. F5 remains SETsafe SCT6125T4A250V
(C19712551); the same pad geometry closely matches its recommended
6.9 mm overall / 2.0×3.0 mm pads. Do not restore the former cartridge
mapping or assume a footprint name changes the ordered fuse.

Optional AUX divider R25 is **15k**, DNP by default. With R24 10k,
5 V becomes 3.0 V at the ADC; input scale is 1.66667 (downlink 1667).
Existing firmware defaults and stored calibration remain unchanged.

D22/D23 use SS210 (C14996, basic), 2 A / 100 V in the existing SMA footprints (B360A-13-F before).
The former 40 V SS34 reverse rating was below D1's 48.4 V maximum pulse clamp.
Cathode/anode nets and the freewheel function are unchanged; PWM heating and
cable ringing still require prototype tests.

I2C pull-ups use two separate standard 1×2, 2.54 mm headers: JP1 for SDA and
JP11 for SCL (both pins 1–2). The parallel jumper rows are 5.08 mm apart, leaving
one empty 2.54 mm grid row and a 2.54 mm gap between their nominal plastic bodies.
A normal 2.54 mm shunt cannot bridge the rows. Both headers use the existing
`HDR2` JLCPCB part, not the former `HDR2X2` block.

UV input protection includes D32 PESD5V0S2BT (existing `PESD` part, C49338)
at local (88.75, 82) mm beside J17: pin 1 to UV_SIGNAL, pin 3 to GND with a
short local ground via, pin 2 unused. The connector-to-TVS signal connection
is pre-routed. R51, the JP10 range divider, D30 rail clamps and C27 remain in
place. The TVS has 5 V standoff and does not replace those ADC protections.
The routed board's nearby UV labels were adjusted by hand; preserve their
clearance after a rebuild.

The sensor-rail LDO U7 (AP2112K-3.3, C51118) with C35 (input), C36 (output)
and the Heltec bulk capacitor C37 (22 µF at J2.2/J2.3) were added to the routed
board by hand, under the module between the sockets: U7 at local (13, 18.5) mm,
VIN/EN tapped from the +5V diagonal, VOUT routed between J2 pins 3 and 4 to F3
(0.3 mm in the socket gap, 0.27 mm clearance). Four stitching vias there were
removed. A full rebuild places these parts automatically; keep the LDO output
capacitor directly at U7 and C37 at J2.2.

Final review fixes, also edited by hand: F8 (PTC 0.1 A, 1206) sits above J17 at
(144, 116.6) mm board coordinates; the +5V B.Cu diagonal to J17 now ends at a
via beside F8.1 and F8.2 feeds J17.4 (net UV_5V). U4 pin 1 and C21 moved from
+3V3 to 3V3_HELTEC: a 0.4 mm B.Cu run at x 70.5 from the Heltec 3.3 V track
(y 83.84) to a via beside U4 pin 1, just right of the isolation moat; the U4
GND B.Cu link was moved to x 71.4 and two stitching vias at x 70.95 removed.
R45 is 100k (value change only).

C15 changed from 10 uF 1210 to 1 uF 0805 and now sits horizontally below the
OW_SW run at (107, 134) mm (an 0805 cannot straddle that track); C38 (22 uF
0805, +3V3 at the Q1 source) sits beside it at (103.22, 134) mm with a shared
GND link to the via at (103.77, 136.3). The C31 feed was widened to 0.8-1.0 mm
except between the Q7 pins (0.3-0.5 mm); the GND stitching via at
(58.95, 118.95) was removed for it.

The current board target is 100 × 100 mm; the four mounting holes are at
(4, 4), (96, 4), (4, 96), and (96, 96) mm (92 × 92 mm corner pattern).
`MOUNTING_COPPER_RADIUS = 3.75` reserves copper-free screw/washer/standoff
areas for hardware up to 7 mm OD plus 0.25 mm radial margin; the generated
64-sided keepouts circumscribe that radius. The extra height was
needed for complete component placement and sensor-terminal clearance. J17 is on the
right edge with pin 1 at (94.5, 81) mm, 19 mm above the bottom edge; its body stays
above the lower-right mounting hole. Q7 loss,
heater/fan copper temperatures, thermal limits in the enclosure, and JLCPCB assembly
land-pattern/orientation have not been physically validated. The protection design uses a
48.4 V max-clamp SMBJ30CA TVS with 60 V Q7 and 65 V LMR36520 ratings; fuses and PPTCs are
selected with ratings above that clamp, but fuse/PPTC derating under combined load and
ambient temperature still requires prototype validation.

## 3D models

`models3d.py` (called by `export.sh`) writes simple VRML models into `pcb/v2.0/3dmodels/` for footprints without a usable KiCad library model (Amphenol RJ12/RJ45 jacks, SMD fuses and the LMR36520 SOIC-8 PowerPAD) and points them there via `${KIPRJMOD}`. F5/F7 have separate approximate SCT6125/NANO2 bodies matching their nominal outer dimensions rather than invisible bare pads. These shapes are approximate visual models, not manufacturing drawings. The Heltec module itself is not modelled — the renders show its sockets and the silkscreen underneath.

## Pre-routed surge paths

`PRE_ROUTE` in the layout lists chains of pads and waypoints (`("@", x, y)`) that `gen_pcb2.py` draws as locked F.Cu tracks before autorouting — used for connector pin → TVS → second connector pin, so the protection sits in the surge path regardless of what Freerouting does. `RESERVED` keeps their corridors free of parts.
