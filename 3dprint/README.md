# Enclosure

| PCB revision | Files | Source |
|---|---|---|
| Rev 1.2 | `heltec weather node pcb v1.2 Case-body.stl`, `… Case-top.stl`, `… button.stl` | `heltec weather node pcb v1.2.f3d` (Fusion 360) |
| **Rev 1.3** | `rev13/heltec weather node pcb v1.3 case-body.stl`, `… case-lid.stl`, `… button-pin.stl` (+ STEP, assembly STEP) | [`case_rev13.py`](case_rev13.py) (build123d) |
| **v2.0** | [`v20/`](v20/): body, lid, button pin, optional gland blank (STL + STEP), assembly STEP and preview | [`case_v20.py`](case_v20.py) (build123d; reads current KiCad PCB) |

## v2.0 enclosure (indoor prototype)

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
and Heltec pin layout, not measured on an assembled v2.0 prototype.

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
