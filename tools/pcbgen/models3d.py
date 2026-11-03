"""Simple 3D models for footprints whose KiCad library model doesn't exist.

KiCad 10 ships no STEP files for the Amphenol RJ12/RJ45 jacks and the 1812 / 2920
fuse footprint, so they render as bare pads. This writes plain VRML models
(boxes, sized from the footprints' fab outlines and pads) into
<board dir>/3dmodels/ and points those footprints at them via ${KIPRJMOD}.

    python3 models3d.py <board.kicad_pcb>       (KiCad's bundled Python)

VRML units are 0.1 inch (KiCad convention); model x = footprint x,
model y = -footprint y, z = height above the board.
"""
import os
import sys

import pcbnew

MODEL_DIR = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/3dmodels"

BLACK = (0.08, 0.08, 0.09)
DARK = (0.02, 0.02, 0.02)
GOLD = (0.85, 0.66, 0.22)
TIN = (0.78, 0.78, 0.80)
PTC = (0.70, 0.56, 0.28)
FUSE_CERAMIC = (0.84, 0.79, 0.63)
IC_BODY = (0.12, 0.12, 0.13)


def box(x0, x1, y0, y1, z0, z1, color, shininess=0.2):
    """Axis-aligned box in footprint mm coordinates (y down) -> VRML shape."""
    s = 1 / 2.54
    xs, ys, zs = (x0 * s, x1 * s), (-y1 * s, -y0 * s), (z0 * s, z1 * s)
    pts = [(xs[i & 1], ys[(i >> 1) & 1], zs[(i >> 2) & 1]) for i in range(8)]
    faces = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    return ("Shape { appearance Appearance { material Material { diffuseColor %.3f %.3f %.3f "
            "specularColor 0.3 0.3 0.3 shininess %.2f } }\n"
            "  geometry IndexedFaceSet { solid FALSE coord Coordinate { point [ %s ] }\n"
            "    coordIndex [ %s ] } }\n" % (
                *color, shininess,
                ", ".join("%.4f %.4f %.4f" % p for p in pts),
                " ".join("%d,%d,%d,%d,-1," % f for f in faces)))


def rj_jack(x0, x1, y0, y1, height, opening_w, opening_z, depth, contacts, pins, pegs):
    """Horizontal modular jack: body with the plug opening at y1 (front)."""
    cx = (x0 + x1) / 2
    oa, ob = cx - opening_w / 2, cx + opening_w / 2
    oz0, oz1 = opening_z
    yb = y1 - depth
    out = [
        box(x0, x1, y0, yb, 0, height, BLACK),            # back block
        box(x0, oa, yb, y1, 0, height, BLACK),            # side walls
        box(ob, x1, yb, y1, 0, height, BLACK),
        box(oa, ob, yb, y1, 0, oz0, BLACK),               # below the opening
        box(oa, ob, yb, y1, oz1, height, BLACK),          # above the opening
        box(oa, ob, yb, yb + 0.2, oz0, oz1, DARK),        # back wall of the opening
        box(cx - 1.7, cx + 1.7, y1 - 2.0, y1, oz0 - 0.9, oz0, DARK),   # latch notch
    ]
    pitch = 1.02
    for i in range(contacts):                             # spring contacts under the roof
        x = cx + (i - (contacts - 1) / 2) * pitch
        out.append(box(x - 0.2, x + 0.2, yb, yb + 6.0, oz1 - 0.5, oz1, GOLD, 0.6))
    for (px, py) in pins:                                 # solder pins
        out.append(box(px - 0.25, px + 0.25, py - 0.25, py + 0.25, -3.0, 0, GOLD, 0.6))
    for (px, py, d) in pegs:                              # plastic locating pegs
        r = d / 2 - 0.15
        out.append(box(px - r, px + r, py - r, py + r, -2.6, 0, BLACK))
    return out


def fuse(length, width, height, end):
    """SMD PTC polyfuse with tin end terminations, centred on the footprint origin."""
    a, b = length / 2, width / 2
    return [box(-a + end, a - end, -b, b, 0.05, height, PTC, 0.1),
            box(-a, -a + end, -b, b, 0.0, height + 0.05, TIN, 0.6),
            box(a - end, a, -b, b, 0.0, height + 0.05, TIN, 0.6)]


def chip_fuse(length=6.3, width=3.2, height=1.5, end=0.85):
    """Approximate ceramic-body SMD fuse with tin end caps."""
    a, b = length / 2, width / 2
    return [box(-a + end, a - end, -b, b, 0.05, height, FUSE_CERAMIC, 0.15),
            box(-a, -a + end, -b, b, 0.0, height + 0.05, TIN, 0.6),
            box(a - end, a, -b, b, 0.0, height + 0.05, TIN, 0.6)]


def soic8_ep():
    """Approximate TI SOIC-8 PowerPAD body and gull-wing leads."""
    shapes = [
        box(-1.95, 1.95, -2.45, 2.45, 0.22, 1.72, IC_BODY, 0.25),
        box(-1.55, 1.55, -0.15, 0.15, 0.03, 0.09, TIN, 0.6),
        box(-1.50, -1.18, -2.05, -1.73, 1.72, 1.74, TIN, 0.6),
    ]
    for side in (-1, 1):
        x0, x1 = (1.55, 2.95) if side > 0 else (-2.95, -1.55)
        for y in (-1.905, -0.635, 0.635, 1.905):
            shapes.append(box(x0, x1, y - 0.22, y + 0.22, 0.07, 0.22, TIN, 0.6))
    return shapes


def fab_bbox(fp):
    xs, ys = [], []
    for g in fp.GraphicalItems():
        if g.GetLayerName() == "F.Fab" and isinstance(g, pcbnew.PCB_SHAPE):
            for p in (g.GetStart(), g.GetEnd()):
                xs.append(pcbnew.ToMM(p.x))
                ys.append(pcbnew.ToMM(p.y))
    return min(xs), max(xs), min(ys), max(ys)


def library_footprint(fp):
    fid = fp.GetFPID()
    return pcbnew.FootprintLoad(os.path.join(MODEL_DIR.replace("3dmodels", "footprints"),
                                             fid.GetLibNickname().wx_str() + ".pretty"),
                                fid.GetLibItemName().wx_str())


def model_for(fp):
    """(file name, VRML shapes) for footprints we have a model for, else None."""
    name = fp.GetFPID().GetLibItemName().wx_str()
    if name == "Fuse_2512_6332Metric":
        return "Fuse_2512.wrl", chip_fuse()
    if name == "Fuse_Littelfuse-NANO2-451_453":
        if "SCT6125" in fp.GetFieldText("MPN"):
            return "Fuse_SCT6125.wrl", chip_fuse(6.3, 2.7, 2.7, 1.6)
        return "Fuse_NANO2_451.wrl", chip_fuse(6.1, 2.69, 2.69, 1.45)
    if name == "SOIC-8-1EP_3.9x4.9mm_P1.27mm_EP2.514x3.2mm":
        return "LMR36520_SOIC-8-EP.wrl", soic8_ep()
    if name == "Fuse_1812_4532Metric":
        return "Fuse_PTC_1812.wrl", fuse(4.5, 3.2, 1.2, 0.5)
    if name == "Vishay_PowerPAK_1212-8_Single":      # also AOS DFN 3.3x3.3 (AON7264E)
        return "PowerPAK_1212-8.wrl", [box(-1.65, 1.65, -1.65, 1.65, 0.0, 0.95, BLACK, 0.3),
                                       box(-1.2, -0.9, -1.25, -0.95, 0.95, 0.96, TIN)]   # pin-1 mark
    if name == "Fuse_2920_7451Metric":
        return "Fuse_PTC_2920.wrl", fuse(7.4, 5.1, 1.5, 0.8)
    if name.startswith(("RJ12_Amphenol_54601", "RJ45_Amphenol_54602")):
        lib = library_footprint(fp)                       # unrotated, unmirrored geometry
        x0, x1, y0, y1 = fab_bbox(lib)
        pads = [(pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y), pcbnew.ToMM(p.GetDrillSize().x))
                for p in lib.Pads()]
        pins = [(x, y) for x, y, d in pads if d < 1.5]
        pegs = [(x, y, d) for x, y, d in pads if d >= 1.5]
        rj45 = name.startswith("RJ45")
        shapes = rj_jack(x0, x1, y0, y1, height=13.4 if rj45 else 11.6,
                         opening_w=11.9 if rj45 else 9.8, opening_z=(1.6, 9.6) if rj45 else (1.6, 8.4),
                         depth=13.0, contacts=len(pins), pins=pins, pegs=pegs)
        return ("RJ45_Amphenol_54602.wrl" if rj45 else "RJ12_Amphenol_54601.wrl"), shapes
    return None


def model_exists(path, board_dir):
    path = path.replace("${KIPRJMOD}", board_dir)
    for var in ("${KICAD10_3DMODEL_DIR}", "${KICAD9_3DMODEL_DIR}", "${KICAD8_3DMODEL_DIR}"):
        path = path.replace(var, MODEL_DIR)
    return os.path.exists(path) or os.path.exists(os.path.splitext(path)[0] + ".step")


def main(board_path):
    board = pcbnew.LoadBoard(board_path)
    out_dir = os.path.join(os.path.dirname(os.path.abspath(board_path)), "3dmodels")
    written, patched = set(), []
    for fp in board.GetFootprints():
        models = list(fp.Models())
        name = fp.GetFPID().GetLibItemName().wx_str()
        custom_override = name in {
            "Fuse_2512_6332Metric",
            "SOIC-8-1EP_3.9x4.9mm_P1.27mm_EP2.514x3.2mm",
        }
        if not custom_override and models and any(
                model_exists(m.m_Filename, os.path.dirname(os.path.abspath(board_path))) for m in models):
            continue
        made = model_for(fp)
        if not made:
            continue
        fname, shapes = made
        if fname not in written:
            os.makedirs(out_dir, exist_ok=True)
            with open(os.path.join(out_dir, fname), "w") as fh:
                fh.write("#VRML V2.0 utf8\n# generated by tools/pcbgen/models3d.py\n" + "".join(shapes))
            written.add(fname)
        path = "${KIPRJMOD}/3dmodels/" + fname
        if models:
            fp.Models()[0].m_Filename = path
        else:
            m = pcbnew.FP_3DMODEL()
            m.m_Filename = path
            fp.Models().push_back(m)
        patched.append(fp.GetReference())
    if patched:
        board.Save(board_path)
    print("3D models:", sorted(written), "footprints:", sorted(patched))


if __name__ == "__main__":
    main(sys.argv[1])
