"""Generate the Weather Node V4 Light indoor/sheltered enclosure; dimensions in mm, PCB-local XY.

Board 100 x 80 x 1.6 mm, four M3 holes (30/96, 4/76), Heltec V4 standing at the left edge with
the USB-C at the top edge. Same construction as the V4 Full case (case_v4_full.py): body with
PCB supports and nut pockets, lid with display window and captive PRG pin, wall-mount ears.

Heltec V4 geometry (PCB-local, from the Light board and the V4 STEP model used for the Full case):
module 25.4 x 51.69 mm, sockets J1 (5V side, x 1.57) / J2 (3V3 side, x 24.43), pin 1 at y 4.4, which
is 3.43 mm from the USB end; PRG button at the pin-1 position, 7.91 mm from the module axis towards
the 3V3 side; display window as in the V4 Full case, rotated with the module.
"""

import argparse
import importlib.util
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PCB = ROOT / "pcb/v4-light/universal-weather-node-v4-light.kicad_pcb"
OUT = HERE / "v4-light"
STEM = "universal-weather-node-v4-light"

PCB_W, PCB_H = 100.0, 80.0
WALL = 2.4
FLOOR = 2.4
PCB_Z = 8.4
PCB_T = 1.6
HEIGHT = 36.0         # Heltec envelope tops out at PCB_Z + 21 = 29.4 (Full case: 40, which also holds gland nuts)
LID_T = 2.4
LIP_H = 2.0
LIP_GAP = 0.35
MOUNT_R = 3.25
BAY = 16.0            # cable bay below the bottom edge (wire bend room for J8 / J9, glands)
INNER = (-1.6, 101.6, -1.6, PCB_H + BAY)       # inner cavity x0, x1, y0, y1
OUTER = (-4.0, 104.0, -4.0, PCB_H + BAY + WALL)
SCREWS = [(-6, -6), (106, -6), (-6, OUTER[3] + 2), (106, OUTER[3] + 2)]
# Heltec V4 (module envelope x0, x1, y0, y1 on the Light board)
MODULE = (0.3, 25.7, 0.97, 52.66)
MODULE_AXIS_X = 13.0
USB_X = MODULE_AXIS_X
USB_Z = (PCB_Z + 12.0, PCB_Z + 19.0)
DISPLAY = (MODULE_AXIS_X - 9.0, MODULE_AXIS_X + 9.0, 0.97 + 14.23, 0.97 + 47.23)   # x0, x1, y0, y1
BUTTON = (MODULE_AXIS_X + 7.91, 4.4)
BUTTON_TOP = PCB_Z + 21.0
BUTTON_GAP = 0.5
BUTTON_FOOT_H = 1.0
BUTTON_D = 3.4
BUTTON_HOLE = 3.9
GUIDE_H = 3.0
GLANDS = [(14.0, 25.0), (86.0, 25.0)]     # M12 gland holes in the bottom wall (x, z)
GLAND_D = 12.5
# Left wall: LoRa SMA bulkhead (open) and an optional R-SMA knock-out for the WiFi antenna.
# Both sit beyond the module's end (y > 52.7), so the nuts clear it.
ANTENNA = (60.0, 22.0)    # (y, z)
ANT_D = 6.5
WIFI_ANT = (74.0, 22.0)   # (y, z): 14 mm from the LoRa SMA, 5.5 mm nut radius clear of the H3 boss at x 30
WIFI_HOLE_D = 6.4         # 1/4"-36 bulkhead thread
WIFI_SKIN = 0.6           # left closed on the outside until the jack is fitted
WIFI_RING = (9.6, 10.4, 0.3)   # marking groove: inner D, outer D, depth
WIFI_NUT_D = 11.0


def pcb_facts():
    spec = importlib.util.spec_from_file_location("pcb_sexp", ROOT / "tools/pcbgen/sexp.py")
    if spec is None or spec.loader is None:
        raise ImportError("Cannot load the repository KiCad S-expression reader")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    get, find = module.get, module.find
    board = module.parse(PCB.read_text())[0]
    edge = [e for e in find(board, "gr_rect") if get(e, "layer")[1] == "Edge.Cuts"]
    if len(edge) != 1:
        raise ValueError("Expected one rectangular PCB outline")
    start, end = get(edge[0], "start"), get(edge[0], "end")
    x0, y0 = float(start[1]), float(start[2])
    size = (float(end[1]) - x0, float(end[2]) - y0)
    if size != (PCB_W, PCB_H):
        raise ValueError(f"Enclosure requires {PCB_W} x {PCB_H} mm PCB, got {size}")
    thickness = float(get(get(board, "general"), "thickness")[1])
    if abs(thickness - PCB_T) > 0.001:
        raise ValueError(f"Enclosure requires {PCB_T} mm PCB thickness, got {thickness}")
    footprints, mounts, joints = {}, [], []
    for f in find(board, "footprint"):
        ref = next(p[2] for p in find(f, "property") if p[1] == "Reference")
        at = get(f, "at")
        x, y = float(at[1]) - x0, float(at[2]) - y0
        angle = float(at[3]) if len(at) > 3 else 0
        footprints[ref] = (x, y, angle)
        if ref.startswith("H"):
            mounts.append((x, y))
            continue
        theta = math.radians(-angle)
        for p in find(f, "pad"):
            if p[2] not in ("thru_hole", "np_thru_hole"):
                continue
            pa, ps = get(p, "at"), get(p, "size")
            px, py = float(pa[1]), float(pa[2])
            joints.append((x + px * math.cos(theta) - py * math.sin(theta),
                           y + px * math.sin(theta) + py * math.cos(theta),
                           max(float(ps[1]), float(ps[2])) / 2))
    if sorted(mounts) != [(30, 4), (30, 76), (96, 4), (96, 76)]:
        raise ValueError(f"Unexpected mounting pattern: {mounts}")
    expected = {"J1": (1.57, 4.4, 0), "J2": (24.43, 4.4, 0), "J6": (83.1, 18.325, 90),
                "J4": (83.1, 44.325, 90), "J7": (95, 30.25, 90), "J5": (95, 65.25, 90),
                "J8": (50.5, 75, 0), "J9": (36.5, 75, 0)}
    for ref, position in expected.items():
        if any(abs(a - b) > 0.001 for a, b in zip(footprints[ref], position)):
            raise ValueError(f"{ref} moved; revise enclosure openings: {footprints[ref]}")
    for mx, my in mounts:
        if any(math.hypot(mx - x, my - y) < MOUNT_R + radius + 0.4 for x, y, radius in joints):
            raise ValueError(f"PCB support at {(mx, my)} touches a solder-pad keepout")
    return footprints, mounts


def generate(output):
    from build123d import (Align, Axis, Box, Compound, Cylinder, Location, Plane, RegularPolygon,
                          Text, export_step, export_stl, extrude, fillet)
    import trimesh
    import case_v4_full

    footprints, mounts = pcb_facts()
    ix0, ix1, iy0, iy1 = INNER
    ox0, ox1, oy0, oy1 = OUTER

    def block(x0, x1, y0, y1, z0, z1):
        return Box(x1 - x0, y1 - y0, z1 - z0,
                   align=(Align.MIN, Align.MIN, Align.MIN)).moved(Location((x0, y0, z0)))

    def cylinder(x, y, radius, z0, z1):
        return Cylinder(radius, z1 - z0, align=(Align.CENTER, Align.CENTER, Align.MIN)).moved(
            Location((x, y, z0)))

    def plate(z0, z1):
        p = block(ox0, ox1, oy0, oy1, z0, z1)
        p = fillet(p.edges().filter_by(Axis.Z), 2)
        for x, y in SCREWS:
            p += cylinder(x, y, 5, z0, z1)
        return p

    def wall_hole(x, y, z, diameter, axis):
        c = Cylinder(diameter / 2, WALL + 2, align=(Align.CENTER, Align.CENTER, Align.CENTER))
        return c.rotate(axis, 90).moved(Location((x, y, z)))

    def wifi_knockout(shape, y, z):
        """R-SMA bulkhead hole from the inside up to WIFI_SKIN below the outer face, plus a marking ring."""
        hole = Cylinder(WIFI_HOLE_D / 2, WALL - WIFI_SKIN + 0.4,
                        align=(Align.CENTER, Align.CENTER, Align.MIN)).rotate(Axis.Y, 90)
        shape -= hole.moved(Location((ox0 + WIFI_SKIN, y, z)))
        d_in, d_out, depth = WIFI_RING
        groove = Cylinder(d_out / 2, depth + 0.1, align=(Align.CENTER, Align.CENTER, Align.MIN))
        groove -= Cylinder(d_in / 2, depth + 0.1, align=(Align.CENTER, Align.CENTER, Align.MIN))
        return shape - groove.rotate(Axis.Y, 90).moved(Location((ox0 - 0.1, y, z)))

    body = plate(0, HEIGHT) - block(ix0, ix1, iy0, iy1, FLOOR, HEIGHT + 1)
    for x, y in mounts:
        body += cylinder(x, y, MOUNT_R, FLOOR - 0.01, PCB_Z)
        body -= cylinder(x, y, 1.7, -1, PCB_Z + 1)
        body -= extrude(RegularPolygon(3.35, 6), 2.5).moved(Location((x, y, -0.01)))
    for x, y in SCREWS:
        body -= cylinder(x, y, 1.25, HEIGHT - 10, HEIGHT + 1)

    openings = []
    for ref in ("J6", "J4"):          # RJ11 jacks, right wall
        cy = footprints[ref][1] - 3.175
        lo, hi = cy - 7.3, cy + 7.3
        body -= block(100.8, 105, lo, hi, PCB_Z + 1.2, PCB_Z + 15)
        openings.append((ref, "right", lo, hi, PCB_Z + 1.2, PCB_Z + 15))
    for ref, poles in (("J7", 2), ("J5", 4)):   # screw terminals, right wall
        cy = footprints[ref][1] - (poles - 1) * 1.75
        lo, hi = cy - (poles * 3.5 + 1.6) / 2, cy + (poles * 3.5 + 1.6) / 2
        body -= block(100.8, 105, lo, hi, PCB_Z + 1.1, PCB_Z + 13)
        openings.append((ref, "right", lo, hi, PCB_Z + 1.1, PCB_Z + 13))
    body -= block(USB_X - 6.5, USB_X + 6.5, -5, -0.8, *USB_Z)   # USB-C, top wall
    openings.append(("USB", "top", USB_X - 6.5, USB_X + 6.5, *USB_Z))
    # J8 / J9 (bottom terminals) open into the cable bay below the board: no wall cut-outs needed
    for x, z in GLANDS:
        body -= wall_hole(x, iy1 + WALL / 2, z, GLAND_D, Axis.X)
    body -= wall_hole(ix0 - WALL / 2 + 0.0, *ANTENNA, ANT_D, Axis.Y)
    body = wifi_knockout(body, *WIFI_ANT)
    openings.append(("LoRa SMA", "left", ANTENNA[0] - ANT_D / 2, ANTENNA[0] + ANT_D / 2,
                     ANTENNA[1] - ANT_D / 2, ANTENNA[1] + ANT_D / 2))
    openings.append(("WiFi R-SMA knock-out", "left", WIFI_ANT[0] - WIFI_HOLE_D / 2,
                     WIFI_ANT[0] + WIFI_HOLE_D / 2, WIFI_ANT[1] - WIFI_HOLE_D / 2,
                     WIFI_ANT[1] + WIFI_HOLE_D / 2))
    for x in (20, 80):    # wall-mount ears, top and bottom
        ear = block(x - 8, x + 8, oy0 - 12, oy0 + 0.5, 0, 3.2)
        ear = fillet(ear.edges().filter_by(Axis.Z), 2)
        ear -= cylinder(x, oy0 - 6, 2.3, -1, 4)
        body += ear
        ear = block(x - 8, x + 8, oy1 - 0.5, oy1 + 12, 0, 3.2)
        ear = fillet(ear.edges().filter_by(Axis.Z), 2)
        ear -= cylinder(x, oy1 + 6, 2.3, -1, 4)
        body += ear

    lid = plate(HEIGHT, HEIGHT + LID_T)
    lip = block(ix0 + LIP_GAP, ix1 - LIP_GAP, iy0 + LIP_GAP, iy1 - LIP_GAP, HEIGHT - LIP_H, HEIGHT + 0.01)
    lip -= block(ix0 + LIP_GAP + 1.75, ix1 - LIP_GAP - 1.75, iy0 + LIP_GAP + 1.75, iy1 - LIP_GAP - 1.75,
                 HEIGHT - LIP_H - 1, HEIGHT + 1)
    for x, _ in GLANDS:
        lip -= block(x - 9.5, x + 9.5, iy1 - 3, iy1 + 1, HEIGHT - LIP_H - 1, HEIGHT + 1)
    lid += lip
    for x, y in SCREWS:
        lid -= cylinder(x, y, 1.7, HEIGHT - 1, HEIGHT + LID_T + 1)
    wx0, wx1, wy0, wy1 = DISPLAY
    lid -= block(wx0, wx1, wy0, wy1, HEIGHT - 1, HEIGHT + LID_T + 1)
    lid -= block(wx0 - 2, wx1 + 2, wy0 - 2, wy1 + 2, HEIGHT - 1, HEIGHT + 0.8)
    bx, by = BUTTON
    lid += cylinder(bx, by, 3.5, HEIGHT - GUIDE_H, HEIGHT + 0.01)
    lid -= cylinder(bx, by, BUTTON_HOLE / 2, HEIGHT - GUIDE_H - 1, HEIGHT + LID_T + 1)
    lid -= extrude(Text("WEATHER NODE V4 LIGHT", 3.5), 0.6).mirror(Plane.XZ).moved(
        Location((62, 8, HEIGHT + LID_T - 0.5)))

    foot_z = BUTTON_TOP + BUTTON_GAP
    pin_length = HEIGHT + LID_T + 1.5 - foot_z
    button = cylinder(0, 0, 2.8, 0, BUTTON_FOOT_H) + cylinder(0, 0, BUTTON_D / 2, 0, pin_length)
    button_assembled = button.moved(Location((bx, by, foot_z)))
    plug = cylinder(0, 0, 8, 0, 1.6) + cylinder(0, 0, 6.05, 1.59, 4.4)
    pcb = block(0, PCB_W, 0, PCB_H, PCB_Z, PCB_Z + PCB_T)
    for x, y in mounts:
        pcb -= cylinder(x, y, 1.6, PCB_Z - 1, PCB_Z + PCB_T + 1)

    ex0, ex1, ey0, ey1 = MODULE
    envelopes = [pcb, block(ex0, ex1, ey0, ey1, PCB_Z + PCB_T + 8.5, PCB_Z + 21)]
    for ref in ("J6", "J4"):
        cy = footprints[ref][1] - 3.175
        envelopes.append(block(81.8, 100.1, cy - 6.65, cy + 6.65, PCB_Z + PCB_T, PCB_Z + PCB_T + 11.6))
    for ref, poles in (("J8", 4), ("J9", 3)):
        x, y, _ = footprints[ref]
        envelopes.append(block(x - 1.75, x + (poles - 1) * 3.5 + 1.75, y - 3.5, y + 5.1,
                               PCB_Z + PCB_T, PCB_Z + PCB_T + 10))
    for ref, poles in (("J7", 2), ("J5", 4)):
        x, y, _ = footprints[ref]
        envelopes.append(block(x - 3.5, x + 5.1, y - (poles - 1) * 3.5 - 1.75, y + 1.75,
                               PCB_Z + PCB_T, PCB_Z + PCB_T + 10))
    for z_ref, (y, z) in (("LoRa SMA nut", ANTENNA), ("WiFi R-SMA nut", WIFI_ANT)):
        nut = Cylinder(WIFI_NUT_D / 2, 4, align=(Align.CENTER, Align.CENTER, Align.MIN)).rotate(Axis.Y, 90)
        envelopes.append(nut.moved(Location((ix0 + 0.01, y, z))))
    for x, z in GLANDS:
        nut = Cylinder(9, 4, align=(Align.CENTER, Align.CENTER, Align.CENTER)).rotate(Axis.X, 90)
        nut = nut.moved(Location((x, iy1 - 2, z)))
        if (nut & lid).volume > 0.001:
            raise ValueError("M12 nut envelope collides with lid; revise gland/lip clearance")
        envelopes.append(nut)
    assembled = {"case-body": body, "case-lid": lid, "button-pin": button_assembled}
    for name, shape in assembled.items():
        if not shape.is_valid or len(shape.solids()) != 1:
            raise ValueError(f"{name}: invalid or disconnected solid")
    for i, (name, shape) in enumerate(assembled.items()):
        for other_name, other in list(assembled.items())[i + 1:]:
            overlap = (shape & other).volume
            if overlap > 0.001:
                raise ValueError(f"Collision {name}/{other_name}: {overlap} mm3")
    for index, envelope in enumerate(envelopes):
        for name, shape in assembled.items():
            if (shape & envelope).volume > 0.001:
                raise ValueError(f"{name} collides with PCB/component envelope {index}")

    # KiCad's top-view Y points down; CAD Y points up: reflect all geometry.
    body, lid = body.mirror(Plane.XZ), lid.mirror(Plane.XZ)
    button_assembled = button_assembled.mirror(Plane.XZ)
    envelopes = [e.mirror(Plane.XZ) for e in envelopes]
    assembled = {"case-body": body, "case-lid": lid, "button-pin": button_assembled}
    printable = {"case-body": body,
                 "case-lid": lid.rotate(Axis.X, 180).moved(Location((0, 0, HEIGHT + LID_T))),
                 "button-pin": button, "gland-blank": plug}
    output.mkdir(parents=True, exist_ok=True)
    report = {"pcb_mm": [PCB_W, PCB_H, PCB_T], "pcb_bottom_z_mm": PCB_Z,
              "coordinates": "Opening/mount data: PCB-local Y down; STEP assembly: X=PCB X, Y=-PCB Y, Z up",
              "mounts_mm": mounts, "openings": openings, "parts": {},
              "button_xy_mm": BUTTON, "display_window_mm": DISPLAY, "usb_x_mm": USB_X,
              "button_estimated_top_mm": BUTTON_TOP, "button_length_mm": pin_length,
              "qualification": "CAD checks only; physical fit and IP rating unverified"}
    for name, shape in printable.items():
        if not shape.is_valid or len(shape.solids()) != 1:
            raise ValueError(f"Invalid printable {name}")
        stem = output / f"{STEM}-{name}"
        step_path, stl_path = Path(str(stem) + ".step"), Path(str(stem) + ".stl")
        export_step(shape, str(step_path))
        export_stl(shape, str(stl_path), tolerance=0.03, angular_tolerance=0.15)
        mesh = trimesh.load_mesh(stl_path)
        if not mesh.is_watertight or not mesh.is_volume or len(mesh.split()) != 1:
            raise ValueError(f"{name}: exported mesh is not a single closed volume")
        if abs(mesh.volume - shape.volume) / shape.volume > 0.005:
            raise ValueError(f"{name}: STL/CAD volume mismatch")
        if abs(mesh.bounds[0, 2]) > 0.001:
            raise ValueError(f"{name}: print does not start at Z=0")
        report["parts"][name] = {"size_mm": mesh.extents.tolist(), "volume_mm3": shape.volume,
                                "watertight": True, "single_solid": True}
        print(f"PASS {name}: {mesh.extents.round(2)}, watertight, single solid")
    export_step(Compound([body, lid, button_assembled, *envelopes]), str(output / f"{STEM}-assembly.step"))
    (output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    cw, ch = wx1 - wx0 + 4 - 0.4, wy1 - wy0 + 4 - 0.4
    (output / "display-cover.svg").write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{cw:g}mm" height="{ch:g}mm" viewBox="0 0 {cw:g} {ch:g}">\n'
        f'  <rect x="0" y="0" width="{cw:g}" height="{ch:g}" fill="none" stroke="black" stroke-width="0.2"/>\n'
        '</svg>\n')
    case_v4_full.preview(output, assembled, envelopes, "V4", name="V4 Light",
                         limits=((-10, 114), (-(oy1 + 14), 14), (0, 50)))


if __name__ == "__main__":
    import sys
    sys.path.insert(0, str(HERE))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUT)
    args = parser.parse_args()
    generate(args.output)
