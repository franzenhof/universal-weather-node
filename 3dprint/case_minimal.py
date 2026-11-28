"""Generate the enclosure for the minimal wireless build (bare Heltec V4 + BME280 module, optional GNSS module).

Two chambers, wall-mounted: the electronics chamber on top (Heltec V4 at the bottom of it, an
optional GNSS module in a bay above the board, patch antenna facing the lid) and a louvred BME280
chamber below it, separated by a double wall with a ventilated air gap. The top wall carries a
knock-out for an optional R-SMA bulkhead (WiFi antenna). Dimensions in mm.

Assembly frame (= body print frame): X right, Y up (when wall-mounted), Z out of the wall.
The back of the case lies on the wall at Z = 0.
"""

import argparse
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT = HERE / "minimal"
PICTURE = ROOT / "pictures" / "case-minimal.png"
STEM = "universal-weather-node-minimal"

# --- case ---------------------------------------------------------------------------------
WALL = 2.4          # side walls and back plate
LID_T = 2.4
LIP_H, LIP_T, LIP_GAP = 2.0, 1.6, 0.3
CORNER_R = 2.0

# --- Heltec WiFi LoRa 32 V4 (board, USB-C end on the left, 3V3 header J3 on top) ----------
# Outline 51.69 x 25.4 mm, sockets 22.86 mm apart, pin 1 3.43 mm from the USB end: the Heltec V4
# STEP model as used in case_v4_full.py (module X -1.27..50.42 around the rows, pin 1 at 2.16).
# Header naming follows the V3 layout of this file: "J3" = 3V3 header (GND, 3V3, ..., display and
# PRG side), "J2" = 5V header (GPIO47 / 48 = pins 13 / 14).
# PRG button: V4 STEP model (3.43 from the USB end, 7.91 from the module axis = 4.79 from the edge).
# Everything marked TODO is not in the repo and must be measured on a real V4.
HL, HW = 51.69, 25.4
HT = 1.2            # Heltec PCB thickness (assumed; 1.0-1.6 fits)
PIN1_X = 3.43       # pin 1 centre from the USB end of the board
ROW_IN = 1.27       # pin row centre from the long board edge
PITCH = 2.54
HDR_PLASTIC = 2.5   # pin header plastic below the board (pins point down, to the wall)
PIN_LONG = 6.0      # header pin below the plastic
DUPONT = 14.0       # female Dupont housing length
BEND = 4.5          # wire exit + bend below the Dupont housings
USB_GAP = 1.3       # board USB end to inner left wall
RIGHT_GAP = 8.0     # board antenna end to inner right wall (IPEX pigtail)
SIDE_GAP = 1.5      # long board edges to inner walls
ABOVE = 8.0         # PCB top to lid inner face (OLED ~3.5, USB 3.3, WiFi spring antenna)
WIRED = (("J3", 1), ("J3", 2), ("J2", 13), ("J2", 14))   # GND, 3V3 (3V3 header), GPIO47 SDA, GPIO48 SCL (5V header)
SADDLES = (("J2", 1, 3), ("J2", 7, 9), ("J2", 16, 18), ("J3", 5, 7), ("J3", 11, 13), ("J3", 16, 18))
SLOT_W, SLOT_CLEAR, CAP = 1.3, 0.72, 1.2
SADDLE_W = 3.2
RIB_GAP = 2.2       # lid hold-down ribs above the PCB top (above the soldered pin tips)
# from USB end, from long edge, header side, pusher protrusion at rest (RST nearly flush:
# no accidental resets, press it with a pen tip)
# RST position on the V4: TODO - not in the repo, assumed mirrored to PRG on the 5V header side
BUTTONS = {"PRG": (3.43, 4.79, "J3", 1.8), "RST": (3.43, 4.79, "J2", 0.3)}
BTN_H = 2.5         # tact switch top above PCB top (estimate)
BTN_REST = 0.5      # pusher foot above the switch
PIN_D, PIN_HOLE, FOOT_D, FOOT_H = 3.2, 3.8, 5.6, 1.0
GUIDE_OD, GUIDE_H = 6.4, 2.0
WINDOW = (30.73, 12.7, 33.0, 18.0)   # OLED window centre (from USB end, from J3 edge), size: as in the V4 Full case
WINDOW_CHAMFER = 1.2
COVER_POCKET = (1.5, 0.8)            # inside recess for an optional 1 mm clear sheet
USB_OPEN = (13.5, 9.0)               # Y x Z, room for a typical USB-C plug overmould
USB_Z_ABOVE = 1.6                    # USB-C axis above PCB top

# --- SMA bulkhead (IPEX -> SMA pigtail) in the right wall --------------------------------
SMA_D, SMA_Z = 6.5, 11.0
SMA_NUT = 11.0      # nut + washer clearance diameter
IPEX_END = (5.0, 1.0)   # TODO: V4 u.FL position not in the repo; envelope only (distance from the antenna end, size)

# --- optional R-SMA bulkhead for the WiFi antenna (u.FL -> R-SMA pigtail inside) --------------
# Round knock-out in the top wall, closed by a thin outer skin; a groove marks the ring outside.
# 1/4"-36 thread -> 6.4 mm hole; the nut / washer sit on the flat inner wall. The antenna screws on
# outside, pointing up. TODO: the V4's WiFi u.FL position is unknown (pigtail length is free).
WIFI_HOLE_D, WIFI_SKIN = 6.4, 0.6
WIFI_RING = (9.6, 10.4, 0.3)       # marking groove: inner D, outer D, depth
WIFI_X_FROM_RIGHT, WIFI_Z = 10.0, 14.0
WIFI_NUT = 11.0                    # nut + washer clearance diameter

# --- optional Heltec GNSS module (patch antenna up, only plastic above) -----------------------
# TODO: dimensions of the Heltec GNSS module are not in the repo (docs only name UC6580 / L76K).
# Placeholders - measure the module you have: footprint, thickness incl. patch, cable to the V4's
# GNSS connector (position on the V4 also unknown; the cable is free-hanging).
GNSS_L, GNSS_W, GNSS_T = 25.0, 25.0, 8.0   # X, Y, height incl. patch antenna (TODO)
GNSS_X0 = 4.0                              # module left edge from the inner left wall
GNSS_LID_GAP = 1.0                         # patch top to the lid inner face
BAY_PAD = 1.5                              # bay clearance around the module
POST_R, POST_INSET = 1.8, 3.0              # support posts under the module corners

# --- air gap / BME280 chamber -------------------------------------------------------------
GAP = 8.0           # ventilated air gap between the two chamber walls
POD_H = 30.0        # BME chamber inner height
# Cable slot through the double wall (stadium shape along X): a 1x4 Dupont housing
# (10.2 x 2.5 mm) or four single housings (2.54 x 2.54 mm, 3.6 mm across the corners)
# pass one after another next to the wires already threaded. Seal with hot glue.
CONDUIT_X, CONDUIT_Z = 20.0, 6.8   # between the 3V3-header wires (x 3-6) and 47/48 (x 34-36)
CONDUIT_W, CONDUIT_H = 12.5, 4.5     # inner slot
CONDUIT_WALL = 2.0                   # tube wall in the air gap
BRIDGES = ((-WALL, 4.0), (15.0, 25.0), (55.5, None))   # back plate strips across the gap
BME_W, BME_L, BME_T = 10.5, 13.0, 1.6   # GY-BME280: header edge (X) x depth (Z) x PCB
BME_Y = 6.0         # board underside above the chamber floor
BME_STOP_Z = 9.0    # board rear edge (header side) stop: sensor ~mid-depth, away from the back
GROOVE_D, GROOVE_CLEAR = 1.2, 0.2

# --- louvres ------------------------------------------------------------------------------
LOUVRE_T = 1.5      # slot width perpendicular to the 45 deg channel (2.1 mm vertical)
LOUVRE_Y = (10.0, 14.5, 19.0, 23.5)  # inner slot centres above each chamber floor

# --- screws / mounting --------------------------------------------------------------------
LOBE_R, LOBE_OFF, LOBE_Y = 3.8, 1.6, 2.6
PILOT_D, PILOT_DEPTH, SCREW_CLEAR = 2.5, 12.0, 3.4
EAR_T, EAR_W = 5.0, 18.0
KEY_HEAD, KEY_SHANK, KEY_SLIDE = 8.5, 4.4, 6.0

# --- derived ------------------------------------------------------------------------------
EW = USB_GAP + HL + RIGHT_GAP            # electronics chamber inner width
EH0 = HW + 2 * SIDE_GAP                  # Heltec section inner height
GY0 = EH0 + BAY_PAD                      # GNSS module Y range
GY1 = GY0 + GNSS_W
EH = GY1 + BAY_PAD                       # electronics chamber inner height (Heltec + GNSS bay)
HZ = WALL + HDR_PLASTIC + DUPONT + BEND  # Heltec PCB underside
D = HZ + HT + ABOVE                      # body depth = lid inner face
BX0, BX1 = USB_GAP, USB_GAP + HL
BY0, BY1 = SIDE_GAP, SIDE_GAP + HW
ROWS = {"J3": BY1 - ROW_IN, "J2": BY0 + ROW_IN}
PY1 = -WALL - GAP - WALL                 # BME chamber inner top
PY0 = PY1 - POD_H
BME_CX = CONDUIT_X
BME_YB = PY0 + BME_Y
E_OUT = (-WALL, EW + WALL, -WALL, EH + WALL)
P_OUT = (-WALL, EW + WALL, PY0 - WALL, PY1 + WALL)
USB_Y = (BY0 + BY1) / 2
USB_ZC = HZ + HT + USB_Z_ABOVE
SMA_Y = EH0 / 2
WIFI_X = EW - WIFI_X_FROM_RIGHT
GNSS_TOP = D - GNSS_LID_GAP
GNSS_BOT = GNSS_TOP - GNSS_T
FOOT_Z = HZ + HT + BTN_H + BTN_REST
PIN_LEN = {name: D + LID_T + spec[3] - FOOT_Z for name, spec in BUTTONS.items()}


def pin_x(n):
    return BX0 + PIN1_X + (n - 1) * PITCH


def button_xy(name):
    dx, dy, side, _ = BUTTONS[name]
    return BX0 + dx, (BY1 - dy) if side == "J3" else (BY0 + dy)


def lobes(y0, y1):
    """Screw lobe centres on the outside of both side walls, near the top and bottom."""
    return [(x, y) for x in (-WALL - LOBE_OFF, EW + WALL + LOBE_OFF)
            for y in (y0 + LOBE_Y, y1 - LOBE_Y)]


def slot_area(w, h):
    """Cross-section of a stadium slot (width w along X, height h along Z)."""
    return (w - h) * h + math.pi * (h / 2) ** 2


def generate(output, picture):
    from build123d import (Align, Axis, Box, Compound, Cylinder, Location, Polygon,
                          Rectangle, export_step, export_stl, extrude, fillet)
    import trimesh

    def block(x0, x1, y0, y1, z0, z1):
        return Box(x1 - x0, y1 - y0, z1 - z0,
                   align=(Align.MIN, Align.MIN, Align.MIN)).moved(Location((x0, y0, z0)))

    def zcyl(x, y, r, z0, z1):
        return Cylinder(r, z1 - z0, align=(Align.CENTER, Align.CENTER, Align.MIN)).moved(
            Location((x, y, z0)))

    def xcyl(x0, x1, y, z, r):
        c = Cylinder(r, x1 - x0, align=(Align.CENTER, Align.CENTER, Align.MIN))
        return c.rotate(Axis.Y, 90).moved(Location((x0, y, z)))

    def ycyl(y0, y1, x, z, r):
        c = Cylinder(r, y1 - y0, align=(Align.CENTER, Align.CENTER, Align.MIN))
        return c.rotate(Axis.X, -90).moved(Location((x, y0, z)))

    def yslot(y0, y1, cx, cz, w, h):
        """Stadium-shaped prism along Y (width w along X, height h along Z, round ends)."""
        s = block(cx - (w - h) / 2, cx + (w - h) / 2, y0, y1, cz - h / 2, cz + h / 2)
        for x in (cx - (w - h) / 2, cx + (w - h) / 2):
            s += ycyl(y0, y1, x, cz, h / 2)
        return s

    def rbox(outline, z0, z1, r=CORNER_R):
        x0, x1, y0, y1 = outline
        return fillet(block(x0, x1, y0, y1, z0, z1).edges().filter_by(Axis.Z), r)

    def side_louvre(wall_x, outward, y_in, z0, z1):
        """45 deg slot through a side wall, falling towards the outside (rain-shedding)."""
        h = LOUVRE_T * math.sqrt(2) / 2   # half vertical opening
        xi, xo = wall_x - outward * 0.05, wall_x + outward * (WALL + 0.05)
        yi, yo = y_in + 0.05, y_in - WALL - 0.05
        pts = [(xi, yi - h), (xi, yi + h), (xo, yo + h), (xo, yo - h)]
        return extrude(Polygon(*pts, align=None), z1 - z0).moved(Location((0, 0, z0)))

    def front_louvre(x0, x1, y_in, z_in):
        """45 deg slot through a lid, falling towards the outside (+Z)."""
        slot = Box(x1 - x0, LOUVRE_T, 4 * LID_T, align=(Align.MIN, Align.CENTER, Align.CENTER))
        slot = slot.rotate(Axis.X, 45)
        return slot.moved(Location((x0, y_in - LID_T / 2, z_in + LID_T / 2)))

    # ---------------------------------------------------------------- body
    body = rbox(E_OUT, 0, D) + rbox(P_OUT, 0, D)
    for x, y in lobes(0, EH) + lobes(PY0, PY1):
        body += zcyl(x, y, LOBE_R, 0, D)
    body -= block(0, EW, 0, EH, WALL, D + 1)
    body -= block(0, EW, PY0, PY1, WALL, D + 1)
    for x, y in lobes(0, EH) + lobes(PY0, PY1):
        body -= zcyl(x, y, PILOT_D / 2, D - PILOT_DEPTH, D + 1)
    # back plate strips bridging the air gap (the gap is open front, back, left and right)
    for x0, x1 in BRIDGES:
        body += block(x0, EW + WALL if x1 is None else x1, PY1 + WALL - 0.01, -WALL + 0.01, 0, WALL)
    # cable conduit through the double wall (seal with hot glue / foam after wiring)
    body += yslot(PY1 + WALL - 0.01, -WALL + 0.01, CONDUIT_X, CONDUIT_Z,
                  CONDUIT_W + 2 * CONDUIT_WALL, CONDUIT_H + 2 * CONDUIT_WALL)
    body -= yslot(PY1 - 1, 1, CONDUIT_X, CONDUIT_Z, CONDUIT_W, CONDUIT_H)

    # Heltec saddles: carry the pin-header plastic, pins drop into end-capped slots
    saddle_top = HZ - HDR_PLASTIC
    saddle_info = []
    for row, first, last in SADDLES:
        y = ROWS[row]
        xa, xb = pin_x(first) - SLOT_CLEAR, pin_x(last) + SLOT_CLEAR
        s = block(xa - CAP, xb + CAP, y - SADDLE_W / 2, y + SADDLE_W / 2, WALL - 0.01, saddle_top)
        s -= block(xa, xb, y - SLOT_W / 2, y + SLOT_W / 2, saddle_top - PIN_LONG - 1.5, saddle_top + 1)
        body += s
        saddle_info.append({"row": row, "pins": [first, last], "x": [xa - CAP, xb + CAP], "y": y})

    # USB-C opening (left wall) with a printable 45 deg drip hood above it
    uw, uh = USB_OPEN
    body -= block(-WALL - 1, 0.5, USB_Y - uw / 2, USB_Y + uw / 2, USB_ZC - uh / 2, USB_ZC + uh / 2)
    hood_z0 = USB_ZC - uh / 2 - 4
    hood = Polygon((-WALL + 0.01, hood_z0), (-WALL - 4, hood_z0 + 4), (-WALL - 4, D),
                   (-WALL + 0.01, D), align=None)
    hood = extrude(hood, 1.6).rotate(Axis.X, 90).moved(Location((0, USB_Y + uw / 2 + 0.6 + 1.6, 0)))
    body += hood
    # SMA bulkhead (right wall)
    body -= xcyl(EW - 1, EW + WALL + 1, SMA_Y, SMA_Z, SMA_D / 2)
    # WiFi R-SMA knock-out (top wall): hole from the inside up to WIFI_SKIN below the outer face
    body -= ycyl(EH - 1, EH + WALL - WIFI_SKIN, WIFI_X, WIFI_Z, WIFI_HOLE_D / 2)
    ring = ycyl(EH + WALL - WIFI_RING[2], EH + WALL + 0.1, WIFI_X, WIFI_Z, WIFI_RING[1] / 2)
    ring -= ycyl(EH + WALL - WIFI_RING[2] - 0.1, EH + WALL + 0.2, WIFI_X, WIFI_Z, WIFI_RING[0] / 2)
    body -= ring
    # GNSS module: four posts carry it under the lid (patch up)
    for px in (GNSS_X0 + POST_INSET, GNSS_X0 + GNSS_L - POST_INSET):
        for py in (GY0 + POST_INSET, GY1 - POST_INSET):
            body += zcyl(px, py, POST_R, WALL - 0.01, GNSS_BOT)

    # electronics chamber louvres: low and deep on the left (inlet, below USB),
    # high on the right (outlet, above the SMA nut); none towards the BME chamber
    for y in LOUVRE_Y:
        body -= side_louvre(0, -1, y, 5.0, 16.0)
        body -= side_louvre(EW, 1, y, 19.5, D - LIP_H - 0.5)
    for k in range(4):    # GNSS bay: same styling, inlet low on the left, outlet high on the right
        y_in = GY0 + 4.5 + 4.5 * k
        body -= side_louvre(0, -1, y_in, 5.0, 16.0)
        body -= side_louvre(EW, 1, y_in, 19.5, D - LIP_H - 0.5)
    # BME chamber louvres: both side walls + straight slots in the floor
    for y in LOUVRE_Y:
        for wall_x, out in ((0, -1), (EW, 1)):
            body -= side_louvre(wall_x, out, PY0 + y, 5.0, D - LIP_H - 0.5)
    floor_slots = [x for x in range(6, int(EW) - 3, 5)]
    for x in floor_slots:
        body -= block(x - 1.0, x + 1.0, PY0 - WALL - 1, PY0 + 0.5, 5.0, D - LIP_H - 0.5)

    # BME280 card guides: board horizontal, header at the rear edge, pins up to the conduit
    gx = BME_W / 2 + GROOVE_CLEAR
    for side in (-1, 1):
        inner, outer = BME_CX + side * (gx - GROOVE_D), BME_CX + side * (gx + 1.8)
        rail = block(min(inner, outer), max(inner, outer), BME_YB - 3.0, BME_YB + BME_T + 1.6,
                     WALL - 0.01, BME_STOP_Z + BME_L + 1.6)
        g0, g1 = BME_CX + side * gx, BME_CX + side * (gx - GROOVE_D - 1)
        rail -= block(min(g0, g1), max(g0, g1), BME_YB - 0.15, BME_YB + BME_T + 0.15,
                      BME_STOP_Z, D)
        # no upper lip next to the header plastic at the rear edge
        rail -= block(min(g0, g1), max(g0, g1), BME_YB + BME_T - 0.01, BME_YB + 10,
                      WALL + 0.5, BME_STOP_Z + 4.5)
        body += rail

    # wall mounting: keyhole ear on top (hang), screw ear at the bottom (fix)
    top_y = EH + WALL
    ear = rbox((EW / 2 - EAR_W / 2, EW / 2 + EAR_W / 2, top_y - 3, top_y + 16), 0, EAR_T, 3)
    ky = top_y + 5.5
    ear -= zcyl(EW / 2, ky, KEY_HEAD / 2, -1, EAR_T + 1)
    ear -= block(EW / 2 - KEY_SHANK / 2, EW / 2 + KEY_SHANK / 2, ky, ky + KEY_SLIDE, -1, EAR_T + 1)
    ear -= zcyl(EW / 2, ky + KEY_SLIDE, KEY_SHANK / 2, -1, EAR_T + 1)
    ear -= block(EW / 2 - KEY_HEAD / 2, EW / 2 + KEY_HEAD / 2, ky, ky + KEY_SLIDE, -1, 2.6)
    ear -= zcyl(EW / 2, ky + KEY_SLIDE, KEY_HEAD / 2, -1, 2.6)
    body += ear
    bot_y = PY0 - WALL
    ear = rbox((EW / 2 - EAR_W / 2, EW / 2 + EAR_W / 2, bot_y - 12, bot_y + 3), 0, 3.2, 3)
    ear -= zcyl(EW / 2, bot_y - 6, 2.2, -1, 5)
    body += ear

    # ---------------------------------------------------------------- lids
    def lid_plate(outline, y0, y1, ex0, ex1, ey0, ey1):
        lid = rbox(outline, D, D + LID_T)
        for x, y in lobes(y0, y1):
            lid += zcyl(x, y, LOBE_R, D, D + LID_T)
            lid -= zcyl(x, y, SCREW_CLEAR / 2, D - 1, D + LID_T + 1)
        lip = block(ex0 + LIP_GAP, ex1 - LIP_GAP, ey0 + LIP_GAP, ey1 - LIP_GAP, D - LIP_H, D + 0.01)
        lip -= block(ex0 + LIP_GAP + LIP_T, ex1 - LIP_GAP - LIP_T,
                     ey0 + LIP_GAP + LIP_T, ey1 - LIP_GAP - LIP_T, D - LIP_H - 1, D + 1)
        return lid + lip

    lid_e = lid_plate(E_OUT, 0, EH, 0, EW, 0, EH)
    wcx, wcy, ww, wh = WINDOW
    wx, wy = BX0 + wcx, BY1 - wcy
    lid_e -= block(wx - ww / 2, wx + ww / 2, wy - wh / 2, wy + wh / 2, D - 1, D + LID_T + 1)
    c = WINDOW_CHAMFER
    chamfer = extrude(Rectangle(ww + 2 * c, wh + 2 * c), c + 0.01, dir=(0, 0, -1), taper=45)
    lid_e -= chamfer.moved(Location((wx, wy, D + LID_T + 0.01)))
    m, d = COVER_POCKET
    lid_e -= block(wx - ww / 2 - m, wx + ww / 2 + m, wy - wh / 2 - m, wy + wh / 2 + m, D - 1, D + d)
    for name in BUTTONS:
        bx, by = button_xy(name)
        lid_e += zcyl(bx, by, GUIDE_OD / 2, D - GUIDE_H, D + 0.01)
        lid_e -= zcyl(bx, by, PIN_HOLE / 2, D - GUIDE_H - 1, D + LID_T + 1)
    for row, first, last in SADDLES:   # hold-down ribs above the saddles (pin tips)
        y = ROWS[row]
        rib = block(pin_x(first) - 1.0, pin_x(last) + 1.0, y - 0.8, y + 0.8, HZ + HT + RIB_GAP, D + 0.01)
        for name in BUTTONS:   # keep clear of the pusher feet
            rib -= zcyl(*button_xy(name), FOOT_D / 2 + 0.6, HZ, D + 1)
        lid_e += rib

    for px in (GNSS_X0 + 1.5, GNSS_X0 + GNSS_L - 3.5):   # hold-down ribs on the module corners
        for py in (GY0 + 1.5, GY1 - 3.5):
            lid_e += block(px, px + 2.0, py, py + 2.0, GNSS_TOP + 0.3, D + 0.01)

    lid_p = lid_plate(P_OUT, PY0, PY1, 0, EW, PY0, PY1)
    finger_y = BME_YB + BME_T / 2
    lid_p += block(BME_CX - 2, BME_CX + 2, finger_y - 2, finger_y + 2,
                   BME_STOP_Z + BME_L + 0.5, D + 0.01)
    for y in (4.0, 9.0, 14.0, 19.0, 24.0):
        y_in = PY0 + y
        if abs(y_in - finger_y) < 4.5:
            spans = ((5.0, BME_CX - 4.5), (BME_CX + 4.5, EW - 5.0))
        else:
            spans = ((5.0, EW - 5.0),)
        for x0, x1 in spans:
            lid_p -= front_louvre(x0, x1, y_in, D)

    # ---------------------------------------------------------------- pushers
    pusher = {n: zcyl(0, 0, FOOT_D / 2, 0, FOOT_H) + zcyl(0, 0, PIN_D / 2, 0, PIN_LEN[n])
              for n in BUTTONS}
    pushers = {f"button-pin-{n.lower()}": pusher[n].moved(Location((*button_xy(n), FOOT_Z)))
               for n in BUTTONS}

    # ---------------------------------------------------------------- envelopes (not printed)
    top = HZ + HT
    heltec = {"heltec-pcb": block(BX0, BX1, BY0, BY1, HZ, top)}
    heltec["oled"] = block(BX0 + 14.23, BX0 + 47.23, BY1 - 12.7 - 9, BY1 - 12.7 + 9, top, top + 3.8)
    heltec["usb-c"] = block(BX0 - 0.8, BX0 + 6.8, USB_Y - 4.5, USB_Y + 4.5, top, top + 3.3)
    heltec["wifi-spring"] = zcyl(BX0 + 10, BY1 - 5, 1.8, top, top + 5.5)
    heltec["ipex"] = block(BX1 - IPEX_END[0], BX1 - IPEX_END[1], USB_Y - 2, USB_Y + 2, top, top + 2.5)
    for name in BUTTONS:
        bx, by = button_xy(name)
        heltec[f"switch-{name}"] = block(bx - 2, bx + 2, by - 1.75, by + 1.75, top, top + BTN_H)
    for row, y in ROWS.items():
        heltec[f"header-{row}"] = block(pin_x(1) - 1.27, pin_x(18) + 1.27, y - 1.27, y + 1.27,
                                        HZ - HDR_PLASTIC, HZ)
        for n in range(1, 19):
            x = pin_x(n)
            heltec[f"pintip-{row}-{n}"] = block(x - 0.5, x + 0.5, y - 0.5, y + 0.5, top, top + 1.5)
            if (row, n) in WIRED:
                heltec[f"dupont-{row}-{n}"] = block(x - 1.27, x + 1.27, y - 1.27, y + 1.27,
                                              HZ - HDR_PLASTIC - DUPONT, HZ - HDR_PLASTIC)
            else:
                heltec[f"pin-{row}-{n}"] = block(x - 0.32, x + 0.32, y - 0.32, y + 0.32,
                                                 HZ - HDR_PLASTIC - PIN_LONG, HZ - HDR_PLASTIC)
    plug = block(-WALL - 25, BX0 - 0.8 - 0.4, USB_Y - 6.1, USB_Y + 6.1, USB_ZC - 3.3, USB_ZC + 3.3)
    sma_in = xcyl(EW - 11, EW, SMA_Y, SMA_Z, SMA_NUT / 2)
    wifi_in = ycyl(EH - 4, EH, WIFI_X, WIFI_Z, WIFI_NUT / 2)
    gnss = {"gnss-pcb": block(GNSS_X0, GNSS_X0 + GNSS_L, GY0, GY1, GNSS_BOT, GNSS_BOT + 1.6),
            "gnss-patch": block(GNSS_X0 + 3, GNSS_X0 + GNSS_L - 3, GY0 + 3, GY1 - 3, GNSS_BOT + 1.6, GNSS_TOP)}
    bme = {"bme-pcb": block(BME_CX - BME_W / 2, BME_CX + BME_W / 2, BME_YB, BME_YB + BME_T,
                            BME_STOP_Z, BME_STOP_Z + BME_L)}
    hz = BME_STOP_Z + 1.27
    bme["bme-header"] = block(BME_CX - 5.08, BME_CX + 5.08, BME_YB + BME_T, BME_YB + BME_T + 2.5,
                              hz - 1.27, hz + 1.27)
    bme["bme-dupont"] = block(BME_CX - 5.08, BME_CX + 5.08, BME_YB + BME_T + 2.5,
                              BME_YB + BME_T + 2.5 + DUPONT, hz - 1.27, hz + 1.27)
    # the slot through both walls and the gap must be clear (inside the chambers the wires
    # bend to the BME header / Heltec pins, so card guides and saddles beside the axis are fine)
    conduit_path = yslot(PY1 - 2, 1.0, CONDUIT_X, CONDUIT_Z, CONDUIT_W - 0.1, CONDUIT_H - 0.1)

    # ---------------------------------------------------------------- checks
    parts = {"case-body": body, "lid-electronics": lid_e, "lid-sensor": lid_p, **pushers}
    for name, shape in parts.items():
        if not shape.is_valid or len(shape.solids()) != 1:
            raise ValueError(f"{name}: invalid or disconnected solid ({len(shape.solids())} solids)")
    names = list(parts)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            overlap = (parts[a] & parts[b]).volume
            if overlap > 0.001:
                raise ValueError(f"Collision {a}/{b}: {overlap:.3f} mm3")
    envelopes = {**heltec, **bme, **gnss, "usb-plug": plug, "sma-inside": sma_in, "wifi-inside": wifi_in}
    for env_name, env in envelopes.items():
        for name, shape in parts.items():
            if (shape & env).volume > 0.001:
                raise ValueError(f"{name} collides with envelope {env_name}")
    for env_name in ("heltec-pcb", "header-J3", "header-J2"):
        if (sma_in & heltec[env_name]).volume > 0.001:
            raise ValueError(f"SMA envelope collides with {env_name}")
    if (conduit_path & body).volume > 0.001:
        raise ValueError("Cable conduit is obstructed")
    # Separation: the electronics floor and the BME ceiling must be solid except for the
    # conduit bore, i.e. no other opening between the two chambers.
    bore = slot_area(CONDUIT_W, CONDUIT_H) * WALL
    separation = {"conduit_bore_mm3": round(bore, 2)}
    for label, y0 in (("electronics floor", -WALL), ("BME ceiling", PY1)):
        region = block(0, EW, y0, y0 + WALL, WALL, D)
        missing = region.volume - (region & body).volume
        if abs(missing - bore) > 0.5:
            raise ValueError(f"{label}: unexpected opening ({missing:.2f} vs bore {bore:.2f} mm3)")
        separation[f"{label.replace(' ', '_')}_opening_mm3"] = round(missing, 2)
    # the gap itself must be free of material except the conduit tube and back bridges
    gap_region = block(-WALL - 10, EW + WALL + 10, -WALL - GAP + 0.01, -WALL - 0.01, WALL + 0.01, D + 10)
    tube = (slot_area(CONDUIT_W + 2 * CONDUIT_WALL, CONDUIT_H + 2 * CONDUIT_WALL)
            - slot_area(CONDUIT_W, CONDUIT_H)) * (GAP - 0.02)
    in_gap = sum((gap_region & p).volume for p in parts.values())
    if in_gap > tube * 1.05:
        raise ValueError(f"Air gap obstructed: {in_gap:.1f} mm3 of material (tube {tube:.1f})")
    separation["material_in_air_gap_mm3"] = round(in_gap, 1)
    separation["conduit_tube_mm3"] = round(tube, 1)
    # the Dupont housings of the wired pins leave room for the wire bend above the back plate
    for row, n in WIRED:
        if heltec[f"dupont-{row}-{n}"].bounding_box().min.Z - WALL < BEND - 0.01:
            raise ValueError("No bend room below the Dupont housings")
    # pins sit in the saddle slots (X/Y location)
    for row, first, last in SADDLES:
        for n in range(first, last + 1):
            if (heltec[f"pin-{row}-{n}"] & body).volume > 0.001:
                raise ValueError(f"Pin {row}.{n} hits a saddle")

    # ---------------------------------------------------------------- export
    flip = lambda shape: shape.rotate(Axis.X, 180).moved(Location((0, 0, D + LID_T)))
    printable = {"case-body": body, "lid-electronics": flip(lid_e), "lid-sensor": flip(lid_p),
                 **{f"button-pin-{n.lower()}": pusher[n] for n in BUTTONS}}
    output.mkdir(parents=True, exist_ok=True)
    report = {"frame": "X right, Y up (wall-mounted), Z out of the wall; back on the wall at Z=0",
              "heltec_board_mm": [BX0, BY0, HZ, HL, HW, HT],
              "heltec_underside_clearance_mm": HZ - WALL,
              "body_depth_mm": D, "closed_depth_mm": D + LID_T,
              "electronics_chamber_inner_mm": [EW, EH, D - WALL],
              "bme_chamber_inner_mm": [EW, POD_H, D - WALL],
              "air_gap_mm": GAP, "conduit": {"x": CONDUIT_X, "z": CONDUIT_Z, "w": CONDUIT_W, "h": CONDUIT_H},
              "saddles": saddle_info, "button_pin_length_mm": {n: round(v, 2) for n, v in PIN_LEN.items()},
              "usb_opening": {"y": USB_Y, "z": USB_ZC, "size_yz": list(USB_OPEN)},
              "sma": {"y": SMA_Y, "z": SMA_Z, "d": SMA_D},
              "wifi_rsma_knockout": {"x": WIFI_X, "z": WIFI_Z, "hole_d": WIFI_HOLE_D, "skin": WIFI_SKIN,
                                     "ring_d": list(WIFI_RING[:2])},
              "gnss_bay": {"module_mm": [GNSS_L, GNSS_W, GNSS_T], "x0": GNSS_X0, "y": [GY0, GY1],
                           "top_z": GNSS_TOP, "estimated": "module size is a placeholder (TODO)"},
              "separation": separation, "parts": {},
              "qualification": "CAD checks only; Heltec/BME/GNSS heights and sizes are estimates, not measured"}
    for name, shape in printable.items():
        if not shape.is_valid or len(shape.solids()) != 1:
            raise ValueError(f"Invalid printable {name}")
        stem = output / f"{STEM}-{name}"
        export_step(shape, str(stem) + ".step")
        export_stl(shape, str(stem) + ".stl", tolerance=0.03, angular_tolerance=0.15)
        mesh = trimesh.load_mesh(str(stem) + ".stl")
        if not mesh.is_watertight or not mesh.is_volume or len(mesh.split()) != 1:
            raise ValueError(f"{name}: exported mesh is not a single closed volume")
        if abs(mesh.volume - shape.volume) / shape.volume > 0.005:
            raise ValueError(f"{name}: STL/CAD volume mismatch")
        if abs(mesh.bounds[0, 2]) > 0.001:
            raise ValueError(f"{name}: print does not start at Z=0")
        report["parts"][name] = {"size_mm": [round(v, 2) for v in mesh.extents.tolist()],
                                "volume_mm3": round(shape.volume, 1),
                                "watertight": True, "single_solid": True}
        print(f"PASS {name}: {mesh.extents.round(2)}, watertight, single solid")
    overall = Compound([body, lid_e, lid_p]).bounding_box()
    report["overall_mm"] = [round(overall.size.X, 1), round(overall.size.Y, 1), round(overall.size.Z, 1)]
    print("overall (with lobes and ears):", report["overall_mm"])
    export_step(Compound([body, lid_e, lid_p, *pushers.values(), *heltec.values(), *bme.values()]),
                str(output / f"{STEM}-assembly.step"))
    (output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    cw, ch = WINDOW[2] + 2 * COVER_POCKET[0] - 0.4, WINDOW[3] + 2 * COVER_POCKET[0] - 0.4
    (output / "display-cover.svg").write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{cw:g}mm" height="{ch:g}mm" '
        f'viewBox="0 0 {cw:g} {ch:g}">\n'
        f'  <rect x="0" y="0" width="{cw:g}" height="{ch:g}" fill="none" stroke="black" stroke-width="0.2"/>\n'
        '</svg>\n')
    shown = [(heltec["heltec-pcb"], "#2f7a4a"), (heltec["oled"], "#1d1d1d"),
             (heltec["usb-c"], "#b9b9b9")]
    shown += [(heltec[k], "#2a2a2a") for k in ("header-J3", "header-J2")]
    shown += [(heltec[f"dupont-{r}-{n}"], "#1d1d1d") for r, n in WIRED]
    shown += [(gnss["gnss-pcb"], "#2f7a4a"), (gnss["gnss-patch"], "#c9c9a8")]
    shown += [(heltec[k], "#c8a24a") for k in heltec if k.startswith("pin-")]
    shown += [(bme["bme-pcb"], "#7b46a8"), (bme["bme-header"], "#2a2a2a"), (bme["bme-dupont"], "#1d1d1d")]
    shown += [(sma_in, "#c8a24a")]
    sma_x = EW + WALL
    antenna = [xcyl(sma_x, sma_x + 9, SMA_Y, SMA_Z, 4.0), xcyl(sma_x + 9, sma_x + 16, SMA_Y, SMA_Z, 5.0),
               ycyl(SMA_Y - 5, SMA_Y + 70, sma_x + 21, SMA_Z, 5.0)]
    antenna.append(ycyl(EH + WALL, EH + WALL + 60, WIFI_X, WIFI_Z, 4.5))
    preview(output, picture, parts, shown, antenna, CONDUIT_X)




def render(items, camera, size=(900, 1000), margin=0.06):
    """Small z-buffer rasteriser (flat shading + outline), returns an RGB image array.

    matplotlib's 3D painter sorting mis-orders the long fillet triangles, so the preview is
    rasterised here and only shown with imshow.
    """
    import numpy as np
    from matplotlib.colors import to_rgb

    c = np.array(camera, float)
    c /= np.linalg.norm(c)
    right = np.cross((0, 1, 0), c)
    right /= np.linalg.norm(right)
    up = np.cross(c, right)
    light = np.array((-0.45, 0.65, 0.6))
    light /= np.linalg.norm(light)
    tris, cols, ids = [], [], []
    for index, (shape, color) in enumerate(items):
        vertices, triangles = shape.tessellate(0.05, 0.2)
        points = np.array([tuple(v) for v in vertices])
        tri = points[np.asarray(triangles)]
        normals = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        normals /= np.maximum(np.linalg.norm(normals, axis=1)[:, None], 1e-12)
        shade = 0.42 + 0.5 * np.clip(normals @ light, 0, 1) + 0.12 * np.clip(normals @ c, 0, 1)
        tris.append(tri)
        cols.append(np.clip(shade[:, None] * np.array(to_rgb(color)), 0, 1))
        ids.append(np.full(len(tri), index))
    tri, col, ident = np.concatenate(tris), np.concatenate(cols), np.concatenate(ids)
    view = np.stack([tri @ right, tri @ up, tri @ c], axis=-1)
    width, height = size
    lo, hi = view[..., :2].reshape(-1, 2).min(0), view[..., :2].reshape(-1, 2).max(0)
    scale = min(width * (1 - 2 * margin) / (hi[0] - lo[0]), height * (1 - 2 * margin) / (hi[1] - lo[1]))
    offset = np.array((width, height)) / 2 - (lo + hi) / 2 * scale
    px = view[..., 0] * scale + offset[0]
    py = height - (view[..., 1] * scale + offset[1])
    depth = view[..., 2]
    zbuf = np.full((height, width), -np.inf)
    image = np.ones((height, width, 3))
    owner = np.full((height, width), -1)
    for k in range(len(tri)):
        x, y, z = px[k], py[k], depth[k]
        x0, x1 = max(int(np.floor(x.min())), 0), min(int(np.ceil(x.max())), width - 1)
        y0, y1 = max(int(np.floor(y.min())), 0), min(int(np.ceil(y.max())), height - 1)
        if x0 > x1 or y0 > y1:
            continue
        det = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
        if abs(det) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        l0 = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / det
        l1 = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / det
        l2 = 1 - l0 - l1
        inside = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
        if not inside.any():
            continue
        zz = l0 * z[0] + l1 * z[1] + l2 * z[2]
        region = zbuf[y0:y1 + 1, x0:x1 + 1]
        better = inside & (zz > region)
        region[better] = zz[better]
        image[y0:y1 + 1, x0:x1 + 1][better] = col[k]
        owner[y0:y1 + 1, x0:x1 + 1][better] = ident[k]
    # outlines: depth steps and part boundaries
    filled = np.isfinite(zbuf)
    z = np.where(filled, zbuf, zbuf[filled].min() - 50)
    edge = np.zeros_like(filled)
    for axis in (0, 1):
        dz = np.abs(np.diff(z, axis=axis)) > 1.2
        do = np.diff(owner, axis=axis) != 0
        e = dz | do
        if axis == 0:
            edge[1:] |= e
        else:
            edge[:, 1:] |= e
    image[edge] *= 0.35
    return image


def preview(output, picture, parts, inner, antenna, section_x):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from build123d import Align, Box, Location

    body, lids = parts["case-body"], [parts["lid-electronics"], parts["lid-sensor"]]
    pins = [p for k, p in parts.items() if k.startswith("button")]
    closed = [(body, "#5d8aa0")] + [(lid, "#a9c4d0") for lid in lids] + [(p, "#22323a") for p in pins]
    closed += [(a, "#3a3a3a") for a in antenna] + inner[:2]
    opened = [(body, "#5d8aa0")] + [(shape, colour) for shape, colour in inner]
    keep = Box(300, 300, 300, align=(Align.MAX, Align.CENTER, Align.CENTER)).moved(
        Location((section_x, 0, 0)))
    section = [(shape & keep, colour) for shape, colour in
               [(body, "#5d8aa0")] + [(lid, "#a9c4d0") for lid in lids] + inner]
    section = [(shape, colour) for shape, colour in section if shape.volume > 0.01]
    fig, axes = plt.subplots(1, 3, figsize=(15, 7.2), gridspec_kw={"width_ratios": (1.05, 0.9, 0.9)})
    views = (("Closed, wall-mounted (antenna illustrative)", closed, (-0.55, 0.42, 0.85)),
             ("Lids removed: Heltec V4, GNSS, Dupont wires, BME280", opened, (-0.55, 0.42, 0.85)),
             (f"Section X = {section_x:g} mm through the cable conduit", section, (0.93, 0.18, 0.32)))
    for ax, (title, items, camera) in zip(axes, views):
        ax.imshow(render(items, camera))
        ax.set_title(title, fontsize=10)
        ax.set_axis_off()
    fig.suptitle("Universal Weather Node - minimal wireless build enclosure: Heltec V4 + GNSS chamber on top, "
                 "ventilated air gap, louvred BME280 chamber below")
    fig.subplots_adjust(left=0.01, right=0.99, bottom=0.01, top=0.9, wspace=0.03)
    fig.savefig(output / "preview.png", dpi=130)
    plt.close(fig)
    plt.imsave(picture, render(closed, (-0.55, 0.42, 0.85), size=(760, 1000), margin=0.04))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUT)
    parser.add_argument("--picture", type=Path, default=PICTURE)
    args = parser.parse_args()
    generate(args.output, args.picture)
