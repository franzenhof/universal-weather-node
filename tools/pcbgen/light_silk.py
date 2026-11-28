"""V4 Light: verbose silkscreen - part values on the front, BOM + operating notes on the back.

Front: every R/C/D/Q/U/F gets its (short) value as an extra F.Silkscreen text, inside the
part body when it is free of pads, otherwise beside the part on the first free side.
Back: mirrored B.Silkscreen blocks (title, BOM grouped by value, pinouts, jumpers, GPIOs,
polarity), packed greedily into the areas free of pads, holes and other back texts.

  python3 light_silk.py <board.kicad_pcb>      (KiCad's Python; run once on a fresh build)
"""
import sys, os, importlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew
from design_light import PARTS
L = importlib.import_module(os.environ.get("LAYOUT", "layout_light"))

mm, MM = pcbnew.FromMM, pcbnew.ToMM
OX = OY = 50.0
SHORT = {"4.7k": "4k7", "100nF": "100n", "1uF": "1u", "47uF 16V": "47u", "PTC 0.17A": "PTC .17A",
         "PTC 0.1A": "PTC .1A", "SMLVT3V3": "TVS 3V3", "PESD5V0S2BT": "PESD opt.", "LP0701N3-G": "LP0701\nP-FET",
         "TN2106N3-G": "TN2106\nN-FET", "74HC14": "74HC14"}
PART = {p["ref"]: p for p in PARTS}
# shorter labels tried when the full one finds no free spot (the FET type must stay readable)
FALLBACK = {"LP0701N3-G": ["P-FET", "P"], "TN2106N3-G": ["N-FET", "N"], "SMLVT3V3": ["TVS"], "PESD5V0S2BT": ["opt."],
            "PTC 0.17A": [".17A"], "PTC 0.1A": [".1A"]}


def short(v):
    return SHORT.get(v, v + ("R" if v.replace(".", "").isdigit() else ""))


def box(o, g=0.0):
    b = o.GetBoundingBox()
    return (MM(b.GetLeft()) - g, MM(b.GetTop()) - g, MM(b.GetRight()) + g, MM(b.GetBottom()) + g)


def hit(a, b, m=0.0):
    return not (a[2] + m <= b[0] or b[2] + m <= a[0] or a[3] + m <= b[1] or b[3] + m <= a[1])


def text(b, s, layer, size, x=0, y=0, angle=0, mirror=False, just_left=False):
    t = pcbnew.PCB_TEXT(b); t.SetText(s); t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size))); t.SetTextThickness(mm(max(0.12, size * 0.15)))
    if just_left: t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
    t.SetMirrored(mirror); t.SetTextAngleDegrees(angle); t.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    return t


def front_values(b):
    fps = [f for f in b.GetFootprints() if f.GetReference()[:1] in "RCDQUF" and f.GetReference() in PART]
    pads = [box(p) for f in b.GetFootprints() for p in f.Pads()]
    silk = [box(d) for d in b.GetDrawings() if d.GetLayer() == pcbnew.F_SilkS and hasattr(d, "GetText")]
    silk += [box(f.Reference()) for f in b.GetFootprints() if f.Reference().IsVisible()]
    crt = {}
    for f in b.GetFootprints():
        c = f.GetCourtyard(pcbnew.F_CrtYd)
        if c.OutlineCount():
            bb = c.BBox(); crt[f.GetReference()] = (MM(bb.GetLeft()), MM(bb.GetTop()), MM(bb.GetRight()), MM(bb.GetBottom()))
    edge = (OX + 0.6, OY + 0.6, OX + L.W - 0.6, OY + L.H - 0.6)
    mod = (OX + L.MOD[0], OY + L.MOD[1], OX + L.MOD[2], OY + L.MOD[3])
    placed = failed = 0
    for f in fps:
        ref = f.GetReference(); labs = [short(PART[ref]["value"])] + FALLBACK.get(PART[ref]["value"], [])
        fab = [g for g in f.GraphicalItems() if g.GetLayer() == pcbnew.F_Fab and not hasattr(g, "GetText")]
        body = crt[ref]
        if fab:
            bs = [box(g) for g in fab]
            body = (min(x[0] for x in bs), min(x[1] for x in bs), max(x[2] for x in bs), max(x[3] for x in bs))
        vertical = (body[3] - body[1]) > (body[2] - body[0]) * 1.2
        cx, cy = (body[0] + body[2]) / 2, (body[1] + body[3]) / 2
        cb = crt[ref]; best = None
        for lab in labs:
            size = 0.8
            band = "DO-35" in PART[ref]["fp"]           # cathode band crosses the body text
            cands = [] if band else [(cx, cy, 90 if vertical else 0, True)]
            g = 0.25 + size / 2
            for gg in (g, g + 1.2):          # beside the part, then one text line further out
                cands += [(cx, cb[1] - gg, 0, False), (cx, cb[3] + gg, 0, False), (cb[0] - gg, cy, 90, False), (cb[2] + gg, cy, 90, False)]
            cands += [(x, y, 0, False) for x in (cb[0] - g, cb[2] + g) for y in (cb[1] - g, cb[3] + g)]   # courtyard corners
            for x, y, ang, inside in cands:
                t = text(b, lab, pcbnew.F_SilkS, size, x, y, ang)
                tb = box(t)
                if inside and not (tb[0] >= body[0] - 0.05 and tb[2] <= body[2] + 0.05 and tb[1] >= body[1] - 0.05 and tb[3] <= body[3] + 0.05):
                    continue
                if tb[0] < edge[0] or tb[1] < edge[1] or tb[2] > edge[2] or tb[3] > edge[3] or hit(tb, mod):
                    continue
                bad = sum(hit(tb, p, 0.15) for p in pads) * 10 + sum(hit(tb, s, 0.1) for s in silk) * 5
                if not inside:
                    bad += sum(hit(tb, c) for k, c in crt.items() if k != ref)
                if best is None or bad < best[0]: best = (bad, t)
                if bad == 0: break
            if best and best[0] == 0: break
        if best[0] >= 5:      # touches a pad or other silk text (a courtyard overlap is harmless) - no clean spot: leave it out (the value is in the BOM on the back)
            failed += 1; print(f"value {ref} '{labs[0]}': no free spot, only in the back BOM"); continue
        b.Add(best[1]); silk.append(box(best[1])); placed += 1
    print(f"front values: {placed} placed, {failed} only in the back BOM")


def bom_lines():
    groups = {}
    for p in PARTS:
        if p["ref"][:1] in "J": continue
        groups.setdefault((p["ref"].rstrip("0123456789"), p["value"]), []).append(p["ref"])
    key = lambda r: (r.rstrip("0123456789"), int(r[len(r.rstrip("0123456789")):]))
    rows = sorted(((sorted(v, key=key), val) for (_, val), v in groups.items()), key=lambda rv: key(rv[0][0]))
    out = {"R": [], "C": [], "D": [], "Q": []}
    for refs, val in rows:
        k = refs[0][0] if refs[0][0] in out else "Q"
        label = {"SMLVT3V3": "SMLVT3V3 (SMB TVS)", "PESD5V0S2BT": "PESD5V0S2BT opt.", "PTC 0.17A": "PTC RXEF017",
                 "PTC 0.1A": "PTC RXEF010", "47uF 16V": "47uF 16V elko", "LP0701N3-G": "LP0701 P-FET",
                 "TN2106N3-G": "TN2106 N-FET", "74HC14": "74HC14 DIP-14"}.get(val, val + ((" Ohm PR01 1W" if val in ("330", "100") else " Ohm") if refs[0][0] == "R" else ""))
        out[k].append(f"{' '.join(refs)}: {label}")
    return out


def chunks(title, lines, size=0.8, n=4):
    """split a long list into blocks of n lines (title repeated with 1/2, 2/2 ...) - fits between THT pads"""
    parts = [lines[i:i + n] for i in range(0, len(lines), n)]
    return [(f"{title}" + (f" {k + 1}/{len(parts)}" if len(parts) > 1 else "") + "\n" + "\n".join(p), size)
            for k, p in enumerate(parts)]


def back_blocks():
    o = bom_lines()
    caps = o["C"] + ["F1: PTC RXEF017 (0.17 A)", "F2: PTC RXEF010 (0.1 A)"]
    semis = o["D"] + [l for l in o["Q"] if not l.startswith("F")]
    return ([("UNIVERSAL WEATHER NODE V4 LIGHT REV 1.0\nfranzenhof.com / CERN-OHL-P v2\nHeltec WiFi LoRa 32 V4 (V4.2/V4.3) only\nPower + flash: Heltec USB-C only", 0.9)]
            + chunks("BOM RESISTORS (1%, 10.16 mm)", o["R"])
            + chunks("BOM CAPACITORS / FUSES", caps)
            + chunks("BOM DIODES / FET / IC", semis)
            + chunks("CONNECTORS", ["J1 J2: 1x18 socket 8.5 mm", "J3: 1x2 socket (V4 P4)", "J4 J6: RJ11 6P6C",
                                    "J5 J8: screw 4p 3.5 mm", "J7: screw 2p, J9: 3p", "JP1-JP3: header 1x2 + jumper"])
            + chunks("PINOUT (pin numbers)", ["J5 WIND: 1 GND 2 SPD 3 DIR 4 3V3", "J4 RJ11: 2 3V3 3 DIR 4 GND 5 SPD",
                                              "J7 RAIN: 1 GND 2 RAIN", "J6 RJ11: 3 RAIN 4 GND",
                                              "J8 I2C: 1 3V3 2 SDA 3 SCL 4 GND", "J9 1-WIRE: 1 GND 2 3V3 3 DQ"], n=3)
            + chunks("JUMPERS / GPIO", ["JP1 fit: reed vane, open: Davis pot", "JP2/JP3 fit: I2C pull-ups >0.5 m",
                                        "SPD 44 DIR 6 RAIN 16 VANE 34", "SDA 41 SCL 42 DQ 43 OW-PWR 47",
                                        "ID GPIO15 1M/470k = 1.06 V"], n=3)
            + chunks("POLARITY / ASSEMBLY", ["Diode band = cathode (K)", "C2 +: square pad", "Q1-Q3 TO-92 S-G-D: flat = silk",
                                             "U1 notch = pin 1, use a socket", "D19/D20 optional (SOT-23)",
                                             "SMD first, sockets last"], n=3))


def back_doc(b):
    obst = [box(p, 0.05) for f in b.GetFootprints() for p in f.Pads() if p.IsOnLayer(pcbnew.B_Cu)]
    # vias are tented (solder mask over them): silkscreen may cross them
    obst += [box(d, 0.2) for d in b.GetDrawings() if d.GetLayer() == pcbnew.B_SilkS]
    obst += [box(g, 0.1) for f in b.GetFootprints() for g in f.GraphicalItems() if g.GetLayer() == pcbnew.B_SilkS]
    for hx, hy in L.HOLES:
        r = L.MOUNTING_COPPER_RADIUS + 0.3; obst.append((OX + hx - r, OY + hy - r, OX + hx + r, OY + hy + r))
    x0, y0, x1, y1 = OX + 0.8, OY + 0.8, OX + L.W - 0.8, OY + L.H - 0.8
    jobs = []
    for s, size in back_blocks():
        t = text(b, s, pcbnew.B_SilkS, size, 0, 0, mirror=True); jobs.append(t)
    jobs.sort(key=lambda t: -(box(t)[2] - box(t)[0]) * (box(t)[3] - box(t)[1]))

    def search(w, h):
        best = None; yi = 0
        while y0 + yi * 0.5 + h <= y1:
            y = y0 + yi * 0.5; xi = 0
            while x0 + xi * 0.5 + w <= x1:
                x = x0 + xi * 0.5; r = (x, y, x + w, y + h)
                if not any(hit(r, o, 0.2) for o in obst):
                    score = y * 1000 - x           # from the back: read top-right first
                    if best is None or score < best[0]: best = (score, r)
                xi += 1
            yi += 1
        return best

    missing = 0
    for t in jobs:
        for ang in (0, 90):                        # 90 deg: e.g. the pad-free strip between the Heltec rows
            t.SetTextAngleDegrees(ang); bb = box(t)
            best = search(bb[2] - bb[0], bb[3] - bb[1])
            if best: break
        if best is None:
            missing += 1; print(f"no back space for block '{t.GetText().splitlines()[0]}'"); continue
        r = best[1]
        t.SetPosition(pcbnew.VECTOR2I(mm((r[0] + r[2]) / 2 - (bb[0] + bb[2]) / 2), mm((r[1] + r[3]) / 2 - (bb[1] + bb[3]) / 2)))
        b.Add(t); obst.append(r)
        print(f"back block '{t.GetText().splitlines()[0]}': {r[2] - r[0]:.1f} x {r[3] - r[1]:.1f} mm, {ang} deg at ({r[0] - OX:.1f}, {r[1] - OY:.1f})")
    return missing


if __name__ == "__main__":
    brd = pcbnew.LoadBoard(sys.argv[1])
    # the generator's short back texts are replaced by the full back documentation
    for d in list(brd.GetDrawings()):
        if d.GetLayer() == pcbnew.B_SilkS and hasattr(d, "GetText") and d.GetText().startswith(("Universal Weather Node", "franzenhof", "CERN", "Heltec WiFi", "JP1 VANE", "JP2/JP3")):
            brd.Delete(d)
    front_values(brd)
    if back_doc(brd): print('WARNING: back documentation incomplete')
    pcbnew.SaveBoard(sys.argv[1], brd)
