"""Generate the v2.0 indoor enclosure; dimensions in mm, PCB-local XY."""

import argparse
import importlib.util
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PCB = ROOT / "pcb/v2.0/universal-weather-node.kicad_pcb"
OUT = HERE / "v20"

WALL = 2.4
FLOOR = 2.4
PCB_Z = 8.4
PCB_T = 1.6
HEIGHT = 40.0
LID_T = 2.4
LIP_H = 2.0
LIP_GAP = 0.35
MOUNT_R = 3.25
SCREWS = [(-6, -6), (106, -6), (-6, 132), (106, 132)]
GLANDS = [(20, 30.5), (50, 30.5), (80, 30.5)]
GLAND_D = 12.5
ANTENNA = (61, 30)
ANT_D = 6.5
USB_Y = 20.93
USB_Z = (PCB_Z + 12.0, PCB_Z + 19.0)
DISPLAY = (15.0, 48.0, 11.93, 29.93)
BUTTON = (4.0, 13.33)
BUTTON_TOP = PCB_Z + 21.0
BUTTON_GAP = 0.5
BUTTON_FOOT_H = 1.0
BUTTON_D = 3.4
BUTTON_HOLE = 3.9
GUIDE_H = 3.0
JACK_Z = 6.5        # J18 DC jack barrel axis above the PCB top (XKB DC-005 drawing)
JACK_D = 10.0       # knock-out diameter: room for common 5.5 mm plug overmoulds
JACK_SKIN = 0.6     # left closed on the outside until J18 is fitted


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
    if size != (100.0, 100.0):
        raise ValueError(f"Enclosure requires 100 x 100 mm PCB, got {size}")
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
    if sorted(mounts) != [(4, 4), (4, 96), (96, 4), (96, 96)]:
        raise ValueError(f"Unexpected mounting pattern: {mounts}")
    expected = {"J1": (4.2, 32.36, 90), "J2": (4.2, 9.5, 90),
                "J6": (83.1, 18.325, 90), "J4": (83.1, 41.325, 90),
                "J14": (5, 38.8, -90), "J17": (94.5, 81, 90), "J18": (13.3, 79, 0)}
    for ref, position in expected.items():
        if any(abs(a - b) > 0.001 for a, b in zip(footprints[ref], position)):
            raise ValueError(f"{ref} moved; revise enclosure openings: {footprints[ref]}")
    for ref in ("J3", "J12", "J13", "J11", "J10", "J9"):
        _, y, angle = footprints[ref]
        if abs(y - 95) > 0.001 or abs(angle) > 0.001:
            raise ValueError(f"{ref}: bottom openings require Y=95 and rotation=0")
    for ref in ("J7", "J5"):
        x, _, angle = footprints[ref]
        if abs(x - 95) > 0.001 or abs(angle - 90) > 0.001:
            raise ValueError(f"{ref}: side openings require X=95 and rotation=90")
    for mx, my in mounts:
        if any(math.hypot(mx - x, my - y) < MOUNT_R + radius + 0.4 for x, y, radius in joints):
            raise ValueError(f"PCB support at {(mx, my)} touches a solder-pad keepout")
    return footprints, mounts


def generate(output):
    from build123d import (Align, Axis, Box, Compound, Cylinder, Location, Plane, RegularPolygon,
                          Text, export_step, export_stl, extrude, fillet)
    import trimesh

    footprints, mounts = pcb_facts()

    def block(x0, x1, y0, y1, z0, z1):
        return Box(x1 - x0, y1 - y0, z1 - z0,
                   align=(Align.MIN, Align.MIN, Align.MIN)).moved(Location((x0, y0, z0)))

    def cylinder(x, y, radius, z0, z1):
        return Cylinder(radius, z1 - z0, align=(Align.CENTER, Align.CENTER, Align.MIN)).moved(
            Location((x, y, z0)))

    def plate(z0, z1):
        p = block(-4, 104, -4, 130, z0, z1)
        p = fillet(p.edges().filter_by(Axis.Z), 2)
        for x, y in SCREWS:
            p += cylinder(x, y, 5, z0, z1)
        return p

    def wall_hole(x, y, z, diameter, axis):
        c = Cylinder(diameter / 2, WALL + 2,
                     align=(Align.CENTER, Align.CENTER, Align.CENTER))
        return c.rotate(axis, 90).moved(Location((x, y, z)))

    body = plate(0, HEIGHT) - block(-1.6, 101.6, -1.6, 127.6, FLOOR, HEIGHT + 1)
    for x, y in mounts:
        boss = cylinder(x, y, MOUNT_R, FLOOR - 0.01, PCB_Z)
        body += boss
        body -= cylinder(x, y, 1.7, -1, PCB_Z + 1)
        nut = extrude(RegularPolygon(3.35, 6), 2.5).moved(Location((x, y, -0.01)))
        body -= nut
    for x, y in SCREWS:
        body -= cylinder(x, y, 1.25, HEIGHT - 10, HEIGHT + 1)

    openings = []
    for ref in ("J6", "J4"):
        cy = footprints[ref][1] - 3.175
        lo, hi = cy - 7.3, cy + 7.3
        body -= block(100.8, 105, lo, hi, PCB_Z + 1.2, PCB_Z + 15)
        openings.append((ref, "right", lo, hi, PCB_Z + 1.2, PCB_Z + 15))
    for ref, poles in (("J7", 2), ("J5", 4), ("J17", 4)):
        cy = footprints[ref][1] - (poles - 1) * 1.75
        lo, hi = cy - (poles * 3.5 + 1.6) / 2, cy + (poles * 3.5 + 1.6) / 2
        body -= block(100.8, 105, lo, hi, PCB_Z + 1.1, PCB_Z + 13)
        openings.append((ref, "right", lo, hi, PCB_Z + 1.1, PCB_Z + 13))
    cy = footprints["J14"][1] + 5.25
    body -= block(-5, -0.8, cy - 8, cy + 8, PCB_Z + 1.1, PCB_Z + 13)
    openings.append(("J14", "left", cy - 8, cy + 8, PCB_Z + 1.1, PCB_Z + 13))
    # J18 DC jack (optional, hand-fitted): round knock-out in the left wall; the outer
    # JACK_SKIN stays closed so an unfitted case stays shut - drill / cut it out with J18.
    jy, jz = footprints["J18"][1], PCB_Z + PCB_T + JACK_Z
    knock = Cylinder(JACK_D / 2, WALL - JACK_SKIN + 0.4,
                     align=(Align.CENTER, Align.CENTER, Align.CENTER)).rotate(Axis.Y, 90)
    body -= knock.moved(Location((-1.6 - (WALL - JACK_SKIN) / 2 + 0.2, jy, jz)))
    openings.append(("J18 knock-out", "left", jy - JACK_D / 2, jy + JACK_D / 2, jz - JACK_D / 2, jz + JACK_D / 2))
    body -= block(-5, -0.8, USB_Y - 6.5, USB_Y + 6.5, *USB_Z)
    openings.append(("USB", "left", USB_Y - 6.5, USB_Y + 6.5, *USB_Z))
    for ref, poles in (("J3", 3), ("J12", 2), ("J13", 3), ("J11", 4), ("J10", 3), ("J9", 4)):
        cx = footprints[ref][0] + (poles - 1) * 1.75
        lo, hi = cx - (poles * 3.5 + 1.6) / 2, cx + (poles * 3.5 + 1.6) / 2
        body -= block(lo, hi, 126.8, 131, PCB_Z + 1.1, PCB_Z + 13)
        openings.append((ref, "bottom", lo, hi, PCB_Z + 1.1, PCB_Z + 13))
    for x, z in GLANDS:
        body -= wall_hole(x, 128.8, z, GLAND_D, Axis.X)
    body -= wall_hole(-2.8, *ANTENNA, ANT_D, Axis.Y)
    for x in (10, 90):
        ear = block(x - 8, x + 8, -16, -3.5, 0, 3.2)
        ear = fillet(ear.edges().filter_by(Axis.Z), 2)
        ear -= cylinder(x, -10, 2.3, -1, 4)
        body += ear
        ear = block(x - 8, x + 8, 129.5, 142, 0, 3.2)
        ear = fillet(ear.edges().filter_by(Axis.Z), 2)
        ear -= cylinder(x, 136, 2.3, -1, 4)
        body += ear

    lid = plate(HEIGHT, HEIGHT + LID_T)
    lip = block(-1.6 + LIP_GAP, 101.6 - LIP_GAP,
                -1.6 + LIP_GAP, 127.6 - LIP_GAP, HEIGHT - LIP_H, HEIGHT + 0.01)
    lip -= block(0.15, 99.85, 0.15, 125.85, HEIGHT - LIP_H - 1, HEIGHT + 1)
    for x, _ in GLANDS:
        lip -= block(x - 9.5, x + 9.5, 125, 129, HEIGHT - LIP_H - 1, HEIGHT + 1)
    lid += lip
    for x, y in SCREWS:
        lid -= cylinder(x, y, 1.7, HEIGHT - 1, HEIGHT + LID_T + 1)
    wx0, wx1, wy0, wy1 = DISPLAY
    lid -= block(wx0, wx1, wy0, wy1, HEIGHT - 1, HEIGHT + LID_T + 1)
    lid -= block(wx0 - 2, wx1 + 2, wy0 - 2, wy1 + 2, HEIGHT - 1, HEIGHT + 0.8)
    bx, by = BUTTON
    lid += cylinder(bx, by, 3.5, HEIGHT - GUIDE_H, HEIGHT + 0.01)
    lid -= cylinder(bx, by, BUTTON_HOLE / 2, HEIGHT - GUIDE_H - 1, HEIGHT + LID_T + 1)
    text = extrude(Text("WEATHER NODE v2.0", 4), 0.6).mirror(Plane.XZ).moved(
        Location((50, 67, HEIGHT + LID_T - 0.5)))
    lid -= text

    foot_z = BUTTON_TOP + BUTTON_GAP
    pin_length = HEIGHT + LID_T + 1.5 - foot_z
    button = cylinder(0, 0, 2.8, 0, BUTTON_FOOT_H) + cylinder(0, 0, BUTTON_D / 2, 0, pin_length)
    button_assembled = button.moved(Location((bx, by, foot_z)))
    plug = cylinder(0, 0, 8, 0, 1.6) + cylinder(0, 0, 6.05, 1.59, 4.4)
    pcb = block(0, 100, 0, 100, PCB_Z, PCB_Z + PCB_T)
    for x, y in mounts:
        pcb -= cylinder(x, y, 1.6, PCB_Z - 1, PCB_Z + PCB_T + 1)

    envelopes = [pcb, block(1.5, 51.7, 8.18, 33.68, PCB_Z + PCB_T + 8.5, PCB_Z + 21)]
    for ref in ("J6", "J4"):
        cy = footprints[ref][1] - 3.175
        envelopes.append(block(81.8, 100.1, cy - 6.65, cy + 6.65, PCB_Z + PCB_T, PCB_Z + PCB_T + 11.6))
    for ref, poles in (("J3", 3), ("J12", 2), ("J13", 3), ("J11", 4), ("J10", 3), ("J9", 4)):
        x, y, _ = footprints[ref]
        envelopes.append(block(x - 1.75, x + (poles - 1) * 3.5 + 1.75,
                               y - 3.5, y + 5.1, PCB_Z + PCB_T, PCB_Z + PCB_T + 10))
    for ref, poles in (("J7", 2), ("J5", 4), ("J17", 4)):
        x, y, _ = footprints[ref]
        envelopes.append(block(x - 3.5, x + 5.1, y - (poles - 1) * 3.5 - 1.75,
                               y + 1.75, PCB_Z + PCB_T, PCB_Z + PCB_T + 10))
    x, y, _ = footprints["J14"]
    envelopes.append(block(x - 5.1, x + 3.5, y - 1.75, y + 12.25, PCB_Z + PCB_T, PCB_Z + PCB_T + 10))
    x, y, _ = footprints["J18"]   # DC jack body: front 13.8 mm before pin 1, 9 mm wide, 11 mm high
    envelopes.append(block(x - 13.8, x + 0.9, y - 4.6, y + 4.6, PCB_Z + PCB_T, PCB_Z + PCB_T + 11))
    for x, z in GLANDS:
        nut = Cylinder(9, 4, align=(Align.CENTER, Align.CENTER, Align.CENTER)).rotate(Axis.X, 90)
        nut = nut.moved(Location((x, 125.6, z)))
        if (nut & lid).volume > 0.001:
            raise ValueError("M12 nut envelope collides with lid; revise gland/lip clearance")
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

    # KiCad's top-view Y points down; CAD Y points up. Reflect all geometry,
    # not just component positions, so the printed enclosure is not handed backwards.
    body, lid = body.mirror(Plane.XZ), lid.mirror(Plane.XZ)
    button_assembled = button_assembled.mirror(Plane.XZ)
    envelopes = [e.mirror(Plane.XZ) for e in envelopes]
    assembled = {"case-body": body, "case-lid": lid, "button-pin": button_assembled}
    printable = {"case-body": body,
                 "case-lid": lid.rotate(Axis.X, 180).moved(Location((0, 0, HEIGHT + LID_T))),
                 "button-pin": button, "gland-blank": plug}
    output.mkdir(parents=True, exist_ok=True)
    report = {"pcb_mm": [100, 100, PCB_T], "pcb_bottom_z_mm": PCB_Z,
              "coordinates": "Opening/mount data: PCB-local Y down; STEP assembly: X=PCB X, Y=-PCB Y, Z up",
              "mounts_mm": mounts, "openings": openings, "parts": {},
              "button_estimated_top_mm": BUTTON_TOP, "button_length_mm": pin_length,
              "qualification": "CAD checks only; physical fit and IP rating unverified"}
    for name, shape in printable.items():
        if not shape.is_valid or len(shape.solids()) != 1:
            raise ValueError(f"Invalid printable {name}")
        stem = output / f"universal-weather-node-v2.0-{name}"
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
    export_step(Compound([body, lid, button_assembled, *envelopes]),
                str(output / "universal-weather-node-v2.0-assembly.step"))
    (output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    (output / "display-cover.svg").write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" width="37mm" height="22mm" viewBox="0 0 37 22">\n'
        '  <rect x="0" y="0" width="37" height="22" fill="none" stroke="black" stroke-width="0.2"/>\n'
        '</svg>\n')
    preview(output, assembled, envelopes)


def preview(output, parts, envelopes):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    import numpy as np

    fig = plt.figure(figsize=(12, 7))
    for index, title in enumerate(("Closed enclosure / top view", "Open enclosure / PCB envelopes")):
        ax = fig.add_subplot(1, 2, index + 1, projection="3d")
        items = [(parts["case-body"], "#47768c")]
        if index == 0:
            items += [(parts["case-lid"], "#90adb9"), (parts["button-pin"], "#263a44")]
        else:
            items += [(e, "#346b4c" if i == 0 else "#454545") for i, e in enumerate(envelopes)]
        faces, colors = [], []
        from matplotlib.colors import to_rgb
        light = np.array([-0.5, -0.7, 1.0])
        light /= np.linalg.norm(light)
        for shape, color in items:
            vertices, triangles = shape.tessellate(0.15)
            points = np.array([tuple(v) for v in vertices])
            tri = points[np.asarray(triangles)]
            normals = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
            lengths = np.linalg.norm(normals, axis=1)
            normals /= np.maximum(lengths[:, None], 1e-12)
            brightness = 0.65 + 0.35 * np.abs(normals @ light)
            faces.extend(tri)
            colors.extend(brightness[:, None] * np.array(to_rgb(color)))
        ax.add_collection3d(Poly3DCollection(faces, facecolor=colors, edgecolor="none", linewidths=0))
        ax.set(xlim=(-13, 113), ylim=(-144, 18), zlim=(0, 50), title=title)
        ax.set_box_aspect((126, 162, 50))
        ax.set_proj_type("ortho")
        ax.view_init(elev=90, azim=-90)
        ax.set_axis_off()
    fig.suptitle("Universal Weather Node v2.0 - indoor prototype enclosure\n"
                 "PCB-derived XY; component heights/display/button fit require a first-print check")
    fig.tight_layout()
    fig.savefig(output / "preview.png", dpi=180)
    plt.close(fig)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUT)
    args = parser.parse_args()
    generate(args.output)
