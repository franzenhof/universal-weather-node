# Enclosure

| PCB revision | Files | Source |
|---|---|---|
| Rev 1.2 | `heltec weather node pcb v1.2 Case-body.stl`, `… Case-top.stl`, `… button.stl` | `heltec weather node pcb v1.2.f3d` (Fusion 360) |
| **Rev 1.3** | `rev13/heltec weather node pcb v1.3 case-body.stl`, `… case-lid.stl`, `… button-pin.stl` (+ STEP, assembly STEP) | [`case_rev13.py`](case_rev13.py) (build123d) |
| **v2.0** | [`v20/`](v20/): body, lid, button pin, optional gland blank (STL + STEP), assembly STEP and preview | [`case_v20.py`](case_v20.py) (build123d; reads current KiCad PCB) |
| **Minimal wireless build** (no PCB) | [`minimal/`](minimal/): body, two lids, PRG/RST pins (STL + STEP), assembly STEP and preview | [`case_minimal.py`](case_minimal.py) (build123d) |

## Minimal wireless build enclosure (bare Heltec V3 + BME280, no PCB)

For the [Minimal Wireless Build](../README.md#minimal-wireless-build-no-custom-pcb): a bare
**Heltec WiFi LoRa 32 V3** (pin headers soldered, pins pointing down) and a 4-pin
**GY-BME280** module on four female–female jumper wires (J3 pins 1 GND, 2 3V3, 7 GPIO42,
8 GPIO41). Wall-mounted, **for a sheltered place** (under a roof or eave) — the louvres
shed rain, but the case is **not waterproof** (no IP rating).

![Minimal build enclosure](minimal/preview.png)

**Size:** case 64.3 × 76.1 × 35.0 mm (W × H × D, closed); 75 × 104 mm including the
screw lobes and the mounting ears.

**Concept — two chambers, one above the other**

- **Top: electronics chamber.** The Heltec sits display-forward, USB-C on the left. It
  needs no screws (it has no mounting holes): four **saddles** carry the pin-header plastic,
  the pins drop into end-capped slots (this locates the board in X and Y), and four ribs on
  the lid end 2.2 mm above the board, just above the soldered pin tips, so it cannot lift off. 21 mm below
  the board leave room for the Dupont housings (2.5 + 14 mm) and the wire bend; the saddles
  avoid the wired pins J3 1, 2, 7, 8. Louvres in the side walls (left: rear half, below the USB opening;
  right: front half, above the SMA nut) — none in the floor, i.e. none towards the sensor.
- **Air gap.** The electronics floor and the sensor chamber ceiling are two separate walls
  with an **8 mm air gap**, open to the front, both sides and (between three narrow bridge
  strips) the back, so wind flushes it. Because the warm chamber is on top, the air in a
  horizontal gap is stably layered — warm air cannot sink into the gap, heat can only cross
  ~8 mm of still air and the thin bridges. (A vertical "chimney" gap would only work with the
  chambers side by side; then rising warm air from the electronics would pass the sensor.)
- **Bottom: BME280 chamber.** 45° louvres falling to the outside on both side walls and
  in the lid, straight slots in the floor. The module lies horizontally in two card guides,
  header at the rear edge with the pins pointing up, ≥ 6 mm from every wall; a finger on the
  lid stops it from sliding out. The **only** connection between the chambers is a
  12.5 × 4.5 mm cable slot (a 1×4 Dupont housing, or four single housings one after another
  beside the wires already threaded, pass through) — **seal it with hot glue or a
  foam plug** after wiring so warm air cannot flow down. `case_minimal.py` checks this: the
  separating walls are solid except for the conduit bore.

**Antenna:** Ø 6.5 mm hole in the right wall for an **SMA bulkhead** with an IPEX (U.FL)
→ SMA pigtail (10–15 cm) and an external **868 MHz** antenna (recommended; a hinged
antenna pointing up). Alternative: the small antenna supplied with the Heltec stuck inside
the electronics chamber (lower range); close the SMA hole with a screw + nut or hot glue.
Never power the Heltec without an antenna.

**Print** (PETG or ASA, 0.2 mm layers, 3–4 perimeters, ≥ 2.4 mm walls, **no supports**):

| Part | Count | Orientation | Notes |
|---|---|---|---|
| [case-body](minimal/universal-weather-node-minimal-case-body.stl) | 1 | back on the bed (as exported) | USB window bridges 13.5 mm; louvres, saddles and card guides are vertical |
| [lid-electronics](minimal/universal-weather-node-minimal-lid-electronics.stl) | 1 | outer face on the bed | window, pusher guides, hold-down ribs |
| [lid-sensor](minimal/universal-weather-node-minimal-lid-sensor.stl) | 1 | outer face on the bed | 45° louvres print without support |
| [button-pin-prg](minimal/universal-weather-node-minimal-button-pin-prg.stl) (9.2 mm) | 1 | foot down | protrudes 1.8 mm |
| [button-pin-rst](minimal/universal-weather-node-minimal-button-pin-rst.stl) (7.7 mm) | 1 | foot down | nearly flush — press with a pen tip, no accidental resets |

STEP files, an [assembly STEP](minimal/universal-weather-node-minimal-assembly.step) (with
Heltec/BME280 clearance envelopes), [validation.json](minimal/validation.json) and a
[display-cover template](minimal/display-cover.svg) are in [`minimal/`](minimal/).

**Hardware**

- 8 × **M3 × 10 (–12) mm screws for plastic** (self-tapping, e.g. ST2.9 × 13 or PT/Delta
  type, pan head) — 4 per lid, into Ø 2.5 mm pilot holes (12 mm deep).
- 2 × wall screws Ø 3.5–4 mm (pan head ≤ 8 mm, head height ≤ 2.4 mm) + plugs: top ear =
  keyhole (hang), bottom ear = Ø 4.4 mm hole (fix).
- SMA female bulkhead with IPEX/U.FL pigtail (thread 1/4"-36, ≥ 5 mm thread length),
  868 MHz antenna.
- Optional: 1 mm clear PETG/acrylic sheet 30.6 × 19.6 mm, glued into the 0.8 mm recess
  behind the display window (recommended outdoors).
- Hot glue or a bit of foam for the cable conduit; optionally 1 mm foam pads on the lid ribs.

**Assembly**

1. Insert both button pins into the lid guides from the inside (foot towards the Heltec).
2. Push the four jumper wires onto the BME280 (VIN, GND, SCL, SDA), feed the free ends one by
   one up through the conduit, slide the module into its card guides from the front (header
   at the back, pins up) until it stops.
3. Plug the wires onto the Heltec J3 pins 1 (GND), 2 (3V3), 7 (GPIO42 → SCL), 8 (GPIO41 →
   SDA); lay the slack behind the board. Fit the SMA bulkhead and connect the pigtail to the
   Heltec's IPEX socket (press straight down, do not pull on the thin cable).
4. Lower the Heltec onto the saddles (USB-C to the left, display to the front); the pins must
   drop into the saddle slots. Seal the conduit with hot glue / foam.
5. Screw on both lids; check that PRG/RST are not held down and the USB-C plug fits.

**Please check on the first print** (estimates, adjust in `case_minimal.py`): Heltec PCB
thickness `HT` (1.2 mm assumed), switch height `BTN_H` (2.5 mm) and positions in `BUTTONS`,
OLED `WINDOW`, `USB_OPEN` (13.5 × 9 mm — enough for typical USB-C overmoulds, not for very
bulky ones), and the BME280 size `BME_W` × `BME_L` (10.5 × 13 mm, header on the 10.5 mm
edge — **measure your module**, sizes vary between 10.5 × 13 and 11.5 × 15 mm). Heltec outline
50.2 × 25.5 mm (Heltec spec) and header layout (pitch, 22.86 mm row spacing, pin 1 2.7 mm
from the USB end) match the Heltec sockets in `pcb/v2.0`; button, USB and OLED positions are
read from the Heltec V3 pin map and the v2.0 case — not measured on a real board.

**Regenerate:** `.venv-cad/bin/python 3dprint/case_minimal.py` (same venv as below). The
script checks single valid solids, watertight STL, part/part collisions, collisions with the
Heltec/USB plug/SMA/BME280/Dupont envelopes, a free cable conduit, the wire bend room and
the chamber separation, then writes `minimal/` and `pictures/case-minimal.png`.

## v2.0 enclosure (indoor)

Designed for the current **100 × 100 × 1.6 mm** PCB, with four M3 mounts on a
**92 × 92 mm** pattern. It follows the Rev 1.3 concept (body, lid, captive button,
display-cover recess, connector access and wall-mount ears), but is a new
construction for the different board size and layout. Older cases do not fit.

**Indoor/workshop use only.** Open USB, jack and terminal windows mean this is
not rain-, dust- or condensation-proof. Cable glands and a display cover do not
make the remaining openings watertight. No IP rating, physical print fit or
thermal qualification is claimed.

![v2.0 enclosure](v20/preview.png)

### Download / print

| File | Purpose |
|------|---------|
| [Body STL](v20/universal-weather-node-v2.0-case-body.stl) / [STEP](v20/universal-weather-node-v2.0-case-body.step) | Four PCB supports, connector windows, cable bay, mounting ears and lid screw towers; Ø 10 mm knock-out (0.6 mm outer skin) in the left wall for the optional DC jack J18 — drill or cut it out when J18 is fitted |
| [Lid STL](v20/universal-weather-node-v2.0-case-lid.stl) / [STEP](v20/universal-weather-node-v2.0-case-lid.step) | Display window, locating lip, button guide and screw holes |
| [Button STL](v20/universal-weather-node-v2.0-button-pin.stl) / [STEP](v20/universal-weather-node-v2.0-button-pin.step) | Captive push pin for the Heltec PRG button |
| [Gland blank STL](v20/universal-weather-node-v2.0-gland-blank.stl) / [STEP](v20/universal-weather-node-v2.0-gland-blank.step) | Print up to three if M12 gland openings are unused; glue in place, not a sealing certification |
| [Assembly STEP](v20/universal-weather-node-v2.0-assembly.step) | Closed enclosure with PCB/component **clearance envelopes**, not detailed electronic models |
| [Display-cover template](v20/display-cover.svg) | 37 × 22 mm clear sheet, print at 100% / verify dimensions |
| [CAD validation](v20/validation.json) | Dimensions, PCB mounts, opening positions and exported mesh checks |

Overall footprint including ears/towers is approximately **122 × 158 mm**;
closed height **42.4 mm**, plus about 1.5 mm button protrusion. The rectangular
body is 108 × 134 mm. Walls/floor are 2.4 mm; PCB underside is 8.4 mm above
the outer floor, leaving 6 mm below the PCB for solder tails.

- Six cable windows on the bottom side align with POWER, HEATER, FAN, AUX,
  1-WIRE and I²C. A 27.6 mm-deep internal bay between PCB edge and wall provides
  wire/bend space. Wiring and terminal screws are accessed with the lid removed;
  these windows are not tightly fitted terminal-body cutouts.
- Right-side windows give access to Davis rain/wind jacks, their parallel
  terminals and J17 UV. Left-side windows serve USB and J14 VE.Direct; J15's
  cable exits via the VE.Direct opening. Each Davis jack/parallel-terminal pair
  has a combined opening rather than a fragile narrow dividing bridge.
- Three **Ø12.5 mm** bottom-side holes accept selected M12 cable glands.
  Choose glands with nut envelope **no larger than Ø18 × 4 mm** inside, and
  sufficient threaded length for the 2.4 mm wall. Check the actual gland drawing;
  M12 does not define its flange/nut size or supported cable diameter.
- **Ø6.5 mm SMA** opening on the left accepts a suitable bulkhead pigtail.
  Check thread/flange dimensions before ordering; route the thin IPEX lead
  without pulling on the Heltec RF socket.
- Four mounting ears have Ø4.6 mm holes on an **80 × 146 mm** pattern.

Use PETG or ASA, 0.2 mm layers, approximately 4 perimeter lines and 4–5
top/bottom layers. At least 180 × 180 mm usable bed area is recommended.
All STLs are already in print orientation at Z=0:

| Part | Orientation | First-print notes |
|------|-------------|-------------------|
| Body | Floor down | Closed side windows bridge up to about 16 mm; tune bridging or add local supports if your printer needs them |
| Lid | Outer face down, lip/guide up | Keep the display recess and screw holes free of support residue |
| Button | Foot down | Slide freely in Ø3.9 mm guide; lightly deburr if necessary |
| Gland blank | Flange down | Glue optional blanks into unused gland holes |

### Hardware and assembly

1. Fit four **M3 nuts** into the floor's hex pockets (approximately 5.8 mm
   across flats, 2.5 mm deep). Set the unpowered PCB onto the four supports.
   Secure with **M3 × 10 mm screws** and suitable thin washers; check that
   screws engage the nuts without protruding underneath or touching nearby
   joints. Keep washers within the board's ≤7 mm outside-diameter limit.
2. Fit glands/pigtail if used; wire terminals with the lid off. Add strain
   relief so cables cannot load the PCB connectors. Do not route cables over
   hot power components or the display/button.
3. Cut a **37 × 22 × 1 mm** clear acrylic/PETG cover and bond it into the lid's
   0.8 mm-deep inside recess. The remaining 0.2 mm projects into the enclosure.
4. Insert the button from inside, with its wide foot towards the PCB. Close the
   lid using four **M3 × 12 mm screws suitable for plastic** into the Ø2.5 mm
   pilot holes. Do not overtighten; holes may need drilling/fit adjustment for
   the particular screw and material.
5. Test display visibility, PRG movement, USB plug clearance and all sensor
   plugs **before** permanent mounting. On v2.0 always disconnect external
   supplies and remove JP8 before plugging in USB.

**Mandatory first-print fit check:** PCB outline, mounting pattern and connector
XY are read from the actual KiCad board. Connector/module heights and the
display/button/USB dimensions are estimates carried over from the earlier case
and Heltec pin layout, not measured on an assembled v2.0 board.

The estimated button is **14 mm** tall. It should have a small rest clearance,
not hold PRG down at boot. If it does, shorten it; if travel is insufficient,
adjust `BUTTON_TOP` / `BUTTON_GAP` in the source. Also check the window/guide
position (`DISPLAY`, `BUTTON`), header stack height and USB cable overmould.
Do not force a lid that presses on the module. This is not a measured
mechanical fit guarantee.

### Regenerate / validate

Python 3.11–3.14 is required; the supplied tool versions were tested with
Python 3.14 on macOS. A working build123d/OCP package for your OS is required.

```bash
python3 -m venv .venv-cad
.venv-cad/bin/pip install -r 3dprint/requirements.txt
.venv-cad/bin/python 3dprint/case_v20.py
```

The source reuses the repository's KiCad S-expression reader; KiCad itself is
not needed to generate the case. It refuses an unexpected board size, mounting
pattern or moved key connectors. All four exported print parts are checked for
one valid CAD solid, a watertight single-volume STL, matching CAD/mesh volume
and Z=0 print placement. Body/lid/button intersections, PCB and estimated
component-envelope collisions, M12 nut/lid clearance and support clearance
from through-hole pads are checked too.

Coordinates in `validation.json` use PCB-local X right/Y down; STEP assembly
uses X right/Y up/Z up (`CAD Y = -PCB Y`) so the enclosure is not mirrored
relative to the physical board. Individual lid STEP/STL files are print-oriented;
use the assembly STEP for installed positions. CAD checks are not physical
fit, heat, durability or weatherproofing tests.

The Rev 1.2 case does **not** fit Rev 1.3: the Heltec sits 5.1 mm further towards the RJ jacks (display window, button and
USB-C opening would be off), the wind RJ11 jack is 1.27 mm lower, and the three Rev 1.2 mounting bosses would land on
Rev 1.3 solder joints (Rev 1.3 has a single mounting hole).

![Rev 1.3 enclosure](rev13/preview.png)

## Rev 1.3 enclosure

Outer size 103.8 × 56.7 × 26.2 mm (closed, without the mounting ears). Board, display, USB-C and jack positions are taken
from the Rev 1.3 gerbers; the heights above the board (jack windows, USB-C opening, lid, button pin) are the same as in
the proven Rev 1.2 case.

**What is new**

- **Supports generated from the drill file** — the script reads `pcb/v1.3/…rev13.zip` and places the edge ledge and
  seven posts so that none of them touches one of the 185 solder joints.
- **Board fixation for the single mounting hole** — one M2 screw at the RJ end (slim boss: an R6 joint is only
  2.54 mm away, so use a **nylon** screw or a plastic washer), two hold-down ribs on the lid press the cable end of the
  board down. The RJ jacks stick out through their windows.
- **Cable entries** for the 5 V input and the 1-Wire header: U-notches in the cable end wall; tongues on the lid close
  them to a round Ø 5.6 mm passage (fill with a little hot glue / silicone for strain relief and dust).
- **Antenna hole** Ø 6.5 mm at the cable end: fits an SMA bulkhead (IPEX → SMA pigtail, external antenna for better
  range) or simply lets the Heltec spring antenna out.
- **Mounting ears** (4 × Ø 4.5 mm) for screwing the case to a wall or board.
- **Engraved labels** RAIN / BME280 / WIND under the jack windows, 5V / 1-WIRE / ANT at the cable end.
- **Captive button pin** — the foot (Ø 5.6) cannot pass the Ø 3.8 mm hole, so the pin cannot fall out of the closed lid.
- **Recess for a display cover** — 0.8 mm pocket around the window on the inside for a 1 mm clear acrylic/PETG sheet.
- Walls 1.6 mm (Rev 1.2: 0.8 mm), rounded corners.

**Printing** (PETG or ASA, 0.2 mm layers, no supports)

| Part | Orientation | Notes |
|---|---|---|
| case-body | floor on the bed | jack/USB windows bridge 13–16 mm |
| case-lid | as exported (outer face on the bed) | ribs, tongues and lip point up |
| button-pin | standing on its foot | |

**Assembly**

1. Plug the Heltec onto the board. Tilt the board and insert it RJ jacks first, so that the jacks go into their
   windows, then lower the cable end onto the supports.
2. Fix the RJ end with an M2 × 6 **nylon** screw (or steel screw + plastic washer) through the mounting hole into
   the boss.
3. Lay the 5 V and 1-Wire cables into their notches; feed the antenna through the antenna hole.
4. Put the button pin into the guide from the inside (foot down), close the lid.

**Please check on the first print** — these values are estimated, adjust in `case_rev13.py` and re-run:

- `PIN_LEN` (button pin, derived from the 12 mm Rev 1.2 pin) — the pin should rest on the PRG button and protrude
  a little.
- `HOLD_PRESS` (0.1 mm preload of the lid ribs) — if the lid does not close, set it to 0.
- `GAP_SIDE`, `LIP_GAP` (fits) and `JACK_Z` / `USB_Z` (windows) if your jacks or headers differ.

**Regenerate**

```bash
python3 -m venv .venv-cad && .venv-cad/bin/pip install build123d
.venv-cad/bin/python 3dprint/case_rev13.py      # → 3dprint/rev13/*.stl, *.step
```
