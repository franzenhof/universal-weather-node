import io
import math
import re
import zipfile
from pathlib import Path

from build123d import (Align, Axis, Box, Compound, Cylinder, Location, Plane, Polygon, Pos, Text, export_step,
                       export_stl, extrude, fillet)

HERE = Path(__file__).parent
GERBER_ZIP = HERE.parent / "pcb" / "v1.3" / "universal-weather-node-pcb-gerberfile rev13.zip"
OUT = HERE / "rev13"

PCB_L, PCB_W, PCB_T = 100.0, 50.0, 1.6
MOUNT_HOLE = (77.99, 4.42)
HELTEC_X = (18.28, 43.78)
HELTEC_CX = (HELTEC_X[0] + HELTEC_X[1]) / 2
JACKS = {"RAIN": (8.20, 14.5), "BME280": (24.11, 15.9), "WIND": (40.63, 14.0)}
TERMINALS = {"1-WIRE": 19.66, "5V": 46.33}

WALL = 1.6
FLOOR = 1.6
SUPPORT_H = 3.0
Z_PCB = FLOOR + SUPPORT_H
H = Z_PCB + 20.0
GAP_SIDE = 0.3
GAP_RJ = 0.3
BULGE_X = (HELTEC_X[0] - 0.5, HELTEC_X[1] + 0.5)
BULGE_Y = PCB_W + 3.2
CORNER_R = 2.0

X0, X1 = -GAP_SIDE, PCB_L + GAP_RJ
Y0, Y1 = -GAP_SIDE, PCB_W + GAP_SIDE

JACK_Z = (PCB_T - 0.2, 18.0)
USB_W, USB_Z = 13.0, (12.0, 19.0)
CABLE_W, CABLE_D = 6.0, 5.6
CABLE_ZC = Z_PCB + 10.0
ANT_D, ANT_Y, ANT_Z = 6.5, 8.0, 18.8
HOLD_Y = ((24.5, 30.5), (34.5, 41.5))
HOLD_X, HOLD_PRESS = 1.0, 0.1

PAD_CLEAR = 1.1
LEDGE_W = 1.2
POST_R = 2.0
POST_TARGETS = ((10, 10), (10, 40), (30, 25), (50, 5), (50, 40), (65, 25), (88, 25))

LID_T = 1.6
LIP_T, LIP_H, LIP_GAP = 1.2, 2.0, 0.25
WINDOW = ((HELTEC_CX - 9.0, HELTEC_CX + 9.0), (3.5, 36.5))
WINDOW_POCKET = (2.0, 0.8)
BUTTON = (HELTEC_CX + 7.6, 47.5)
BUTTON_HOLE, GUIDE_OD, GUIDE_H = 3.8, 6.0, 3.0
PIN_D, PIN_LEN = 3.4, 12.0 + (H + LID_T - Z_PCB - 21.0)

EARS = ((12.0, -1), (88.0, -1), (8.0, 1), (92.0, 1))
EAR_W, EAR_D, EAR_T, EAR_HOLE = 12.0, 10.0, 3.0, 4.5

LABEL_H, LABEL_DEPTH = 3.0, 0.6


def drill_holes():
    with zipfile.ZipFile(GERBER_ZIP) as z:
        name = next(n for n in z.namelist() if n.endswith("_drill.txt"))
        text = io.TextIOWrapper(z.open(name), encoding="ascii").read()
    tools, cur, holes = {}, None, []
    for line in text.splitlines():
        line = line.strip()
        if m := re.match(r"^T(\d+)C([\d.]+)", line):
            tools[m.group(1)] = float(m.group(2)) * 25.4
        elif m := re.match(r"^T(\d+)$", line):
            cur = m.group(1)
        elif (m := re.match(r"^X(-?\d+)Y(-?\d+)", line)) and cur:
            holes.append((int(m.group(1)) / 10000 * 25.4, int(m.group(2)) / 10000 * 25.4, tools[cur], int(cur) >= 100))
    return holes


HOLES = drill_holes()


def clear_of_joints(x, y, r):
    return all(math.hypot(x - hx, y - hy) >= r + d / 2 + PAD_CLEAR for hx, hy, d, _ in HOLES
               if math.hypot(hx - MOUNT_HOLE[0], hy - MOUNT_HOLE[1]) > 0.1)


def block(x0, x1, y0, y1, z0, z1):
    return Box(x1 - x0, y1 - y0, z1 - z0, align=(Align.MIN, Align.MIN, Align.MIN)).moved(Location((x0, y0, z0)))


def rounded_plate(x0, x1, y0, y1, z0, z1, r):
    b = block(x0, x1, y0, y1, z0, z1)
    return fillet(b.edges().filter_by(Axis.Z), r)


def outline(z0, z1, grow=0.0, r=CORNER_R):
    g = WALL + grow
    main = rounded_plate(X0 - g, X1 + g, Y0 - g, Y1 + g, z0, z1, r + grow if r + grow > 0.1 else 0.1)
    bulge = rounded_plate(BULGE_X[0] - g, BULGE_X[1] + g, Y1, BULGE_Y + g, z0, z1, max(r + grow, 0.1))
    return main + bulge


def cavity(z0, z1, shrink=0.0):
    s = shrink
    return block(X0 + s, X1 - s, Y0 + s, Y1 - s, z0, z1) + block(BULGE_X[0] + s, BULGE_X[1] - s, Y0 + s, BULGE_Y - s, z0, z1)


def engraved(text, height):
    return extrude(Text(text, height), LABEL_DEPTH + 0.01)


def supports():
    parts = []
    step = 0.25
    for horizontal, fixed in ((True, LEDGE_W / 2), (True, PCB_W - LEDGE_W / 2), (False, LEDGE_W / 2),
                              (False, PCB_L - LEDGE_W / 2)):
        length = PCB_L if horizontal else PCB_W
        run = []
        for t in frange(0.6, length - 0.6, step) + [None]:
            ok = t is not None and clear_of_joints(*((t, fixed) if horizontal else (fixed, t)), LEDGE_W / 2)
            if ok:
                run.append(t)
                continue
            if len(run) * step >= 2.0:
                a, b = run[0], run[-1]
                lo, hi = fixed - LEDGE_W / 2, fixed + LEDGE_W / 2
                parts.append(block(a, b, lo, hi, FLOOR - 0.01, Z_PCB) if horizontal else
                             block(lo, hi, a, b, FLOOR - 0.01, Z_PCB))
            run = []
    free = [(x, y) for x in frange(3, PCB_L - 3, 0.5) for y in frange(3, PCB_W - 3, 0.5) if clear_of_joints(x, y, POST_R)]
    posts = []
    for tx, ty in POST_TARGETS:
        px, py = min(free, key=lambda p: math.hypot(p[0] - tx, p[1] - ty))
        posts.append((px, py))
        parts.append(Cylinder(POST_R, Z_PCB - FLOOR + 0.01, align=(Align.CENTER, Align.CENTER, Align.MIN)).moved(
            Location((px, py, FLOOR - 0.01))))
    mx, my = MOUNT_HOLE
    boss = Cylinder(2.6, Z_PCB - 2.0 - FLOOR + 0.01, align=(Align.CENTER, Align.CENTER, Align.MIN)).moved(
        Location((mx, my, FLOOR - 0.01)))
    boss += Cylinder(1.5, 2.0, align=(Align.CENTER, Align.CENTER, Align.MIN)).moved(Location((mx, my, Z_PCB - 2.0)))
    boss -= Cylinder(0.8, Z_PCB + 1.0, align=(Align.CENTER, Align.CENTER, Align.MIN)).moved(Location((mx, my, -0.5)))
    parts.append(boss)
    result = parts[0]
    for p_ in parts[1:]:
        result += p_
    return result, posts


def frange(a, b, s):
    n = int(round((b - a) / s))
    return [a + i * s for i in range(n + 1)]


def body():
    shell = outline(0, H) - cavity(FLOOR, H + 1)
    for name, (cy, w) in JACKS.items():
        shell -= block(X1 - 0.5, X1 + WALL + 0.5, cy - w / 2, cy + w / 2, Z_PCB + JACK_Z[0], Z_PCB + JACK_Z[1])
    shell -= block(HELTEC_CX - USB_W / 2, HELTEC_CX + USB_W / 2, BULGE_Y - 0.5, BULGE_Y + WALL + 0.5,
                   Z_PCB + USB_Z[0], Z_PCB + USB_Z[1])
    for cy in TERMINALS.values():
        shell -= block(X0 - WALL - 0.5, X0 + 0.5, cy - CABLE_W / 2, cy + CABLE_W / 2, CABLE_ZC, H + 1)
        shell -= Cylinder(CABLE_W / 2, WALL + 1, align=(Align.CENTER, Align.CENTER, Align.CENTER)).rotate(
            Axis.Y, 90).moved(Location((X0 - WALL / 2, cy, CABLE_ZC)))
    shell -= Cylinder(ANT_D / 2, WALL + 1, align=(Align.CENTER, Align.CENTER, Align.CENTER)).rotate(Axis.Y, 90).moved(
        Location((X0 - WALL / 2, ANT_Y, ANT_Z)))
    sup, _ = supports()
    shell += sup
    for ex, side in EARS:
        y_wall = (Y0 - WALL) if side < 0 else (Y1 + WALL)
        y0, y1 = (y_wall - EAR_D, y_wall + 0.5) if side < 0 else (y_wall - 0.5, y_wall + EAR_D)
        ear = rounded_plate(ex - EAR_W / 2, ex + EAR_W / 2, y0, y1, 0, EAR_T, 2.5)
        ear -= Cylinder(EAR_HOLE / 2, EAR_T + 1, align=(Align.CENTER, Align.CENTER, Align.MIN)).moved(
            Location((ex, (y0 + y1) / 2 - side * 0.5, -0.5)))
        shell += ear
    x_out = X1 + WALL
    for name, (cy, _) in JACKS.items():
        t = engraved(name, LABEL_H).rotate(Axis.X, 90).rotate(Axis.Z, 90)
        shell -= t.moved(Location((x_out - LABEL_DEPTH, cy, (Z_PCB + JACK_Z[0]) / 2 + 0.6)))
    x_in = X0 - WALL
    for name, cy in list(TERMINALS.items()) + [("ANT", ANT_Y)]:
        t = engraved(name, LABEL_H * (0.8 if name == "1-WIRE" else 1.0)).rotate(Axis.X, 90).rotate(Axis.Z, -90)
        z = (ANT_Z - ANT_D / 2 - 2.6) if name == "ANT" else (Z_PCB - 0.6)
        shell -= t.moved(Location((x_in + LABEL_DEPTH, cy, z)))
    return shell


def lid():
    plate = outline(H, H + LID_T)
    lip = cavity(H - LIP_H, H, shrink=LIP_GAP) - cavity(H - LIP_H - 1, H + 1, shrink=LIP_GAP + LIP_T)
    lip -= block(HELTEC_CX - USB_W / 2 - 0.3, HELTEC_CX + USB_W / 2 + 0.3, Y1, BULGE_Y + 1, H - LIP_H - 1, H)
    for cy in TERMINALS.values():
        lip -= block(X0 - 1, X0 + LIP_GAP + LIP_T + 1, cy - CABLE_W / 2 - 0.5, cy + CABLE_W / 2 + 0.5, H - LIP_H - 1, H)
    part = plate + lip
    for cy in TERMINALS.values():
        tongue = block(X0 - WALL, X0, cy - CABLE_W / 2 + 0.2, cy + CABLE_W / 2 - 0.2, CABLE_ZC, H + 0.01)
        tongue -= Cylinder(CABLE_D / 2, WALL + 1, align=(Align.CENTER, Align.CENTER, Align.CENTER)).rotate(
            Axis.Y, 90).moved(Location((X0 - WALL / 2, cy, CABLE_ZC)))
        part += tongue
    for y0, y1 in HOLD_Y:
        part += block(X0 + LIP_GAP, HOLD_X, y0, y1, Z_PCB + PCB_T - HOLD_PRESS, H + 0.01)
    (wx0, wx1), (wy0, wy1) = WINDOW
    part -= block(wx0, wx1, wy0, wy1, H - 1, H + LID_T + 1)
    m, d = WINDOW_POCKET
    part -= block(wx0 - m, wx1 + m, wy0 - m, wy1 + m, H - 1, H + d)
    bx, by = BUTTON
    part += Cylinder(GUIDE_OD / 2, GUIDE_H, align=(Align.CENTER, Align.CENTER, Align.MAX)).moved(Location((bx, by, H)))
    part -= Cylinder(BUTTON_HOLE / 2, LID_T + GUIDE_H + 2, align=(Align.CENTER, Align.CENTER, Align.MIN)).moved(
        Location((bx, by, H - GUIDE_H - 1)))
    part -= engraved("Rev 1.3", 3.0).moved(Location((X1 - 12.0, Y0 + 4.0, H + LID_T - LABEL_DEPTH)))
    return part


def button_pin():
    pin = Cylinder(2.8, 1.0, align=(Align.CENTER, Align.CENTER, Align.MIN))
    pin += Cylinder(PIN_D / 2, PIN_LEN, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return pin


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    parts = {"case-body": body(), "case-lid": lid(), "button-pin": button_pin()}
    printable = {
        "case-body": parts["case-body"],
        "case-lid": parts["case-lid"].rotate(Axis.X, 180).moved(Location((0, 0, H + LID_T))),
        "button-pin": parts["button-pin"],
    }
    for name, part in printable.items():
        stem = f"heltec weather node pcb v1.3 {name}"
        export_step(part, str(OUT / f"{stem}.step"))
        export_stl(part, str(OUT / f"{stem}.stl"), tolerance=0.02, angular_tolerance=0.2)
        bb = part.bounding_box()
        print(f"{name:11s} valid={part.is_valid}  volume={part.volume / 1000:5.1f} cm³  "
              f"size={bb.size.X:.1f} × {bb.size.Y:.1f} × {bb.size.Z:.1f} mm")
    _, posts = supports()
    print("support posts at:", [(round(x, 1), round(y, 1)) for x, y in posts])
    export_step(Compound([parts["case-body"], parts["case-lid"]]), str(OUT / "heltec weather node pcb v1.3 assembly.step"))
