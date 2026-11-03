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
The 5 V source is selected manually with JP8: 1–2 for the VIN buck, 2–3 for
external 5 V, or no shunt for USB-only operation. Never connect USB while an
external source is powering the board. There is no automatic source isolation.

VIN uses Q7 with a 12 V gate-source clamp; D1 is the **bidirectional SMBJ30CA**
(C83847), upstream of Q7. External 5 V uses F2, **bidirectional SMBJ5.0CA**
D4 (C83323), then Q9 AO3401A with R56 10k gate pull-down. Q9 pin 3 is the
raw fused input, pin 2 is `5V_PROT` to JP8 pin 3, pin 1 is `5V_GATE`.
The former crowbar D3 is removed. Do not substitute unidirectional TVS parts:
they would recreate the reverse-polarity short circuit. Bidirectional D1/D4
have no polarity; ignore the standard SMB footprint's cathode silk bar.
Neither single-MOSFET circuit provides reverse-current isolation between
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
leaving them to the autorouter. Four 0.6/0.3 mm EP thermal vias and local
capacitor ground returns are also generated. Recheck the fully routed
result: the generator's placement test is not a full routing/thermal test.

F7 now maps to the actual SMD Littelfuse 045106.3MRL (C178982), using the
standard NANO2-451/453 footprint. F5 remains SETsafe SCT6125T4A250V
(C19712551); the same pad geometry closely matches its recommended
6.9 mm overall / 2.0×3.0 mm pads. Do not restore the former cartridge
mapping or assume a footprint name changes the ordered fuse.

Optional AUX divider R25 is **15k**, DNP by default. With R24 10k,
5 V becomes 3.0 V at the ADC; input scale is 1.66667 (downlink 1667).
Existing firmware defaults and stored calibration remain unchanged.

D22/D23 use B360A-13-F (C94193), 3 A / 60 V in the existing SMA footprints.
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
