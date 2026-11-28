"""Build the optimized water-node board (KiCad bundled python).

Layout concept (board-local mm, origin top-left, y down):
  * Heltec V3 top-left, USB-C (pin-1 end) flush with the left edge,
    nothing but the two sockets underneath the module.
  * All field connectors as 3.5 mm screw terminals along the bottom edge,
    wire entry towards the edge; isolated VE.Direct / DALY at the right.
  * Each channel's protection/conditioning sits directly above its terminal.
  * Isolators straddle the domain boundaries, keep-out strip under them.
"""
import sys, os, json, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew
import importlib as _il, os as _os
_D = _il.import_module(_os.environ.get("DESIGN", "design")); PARTS = _D.PARTS
from lcsc import LCSC, VALUE_OVERRIDE
from sexp import parse, find, get

out_pcb, uuids_file, netlist_file = sys.argv[1:4]
import random
SEED = int(os.environ.get("SEED", "0"))         # 0 = deterministic placement; >0 = small random variation per attempt
rnd = random.Random(SEED)
FPDIR = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
OX, OY = 50.0, 50.0
import importlib
L = importlib.import_module(os.environ.get("LAYOUT", "layout_v2"))
W, H = L.W, L.H
ROW_TOP, ROW_BOT, MOD, X0 = L.ROW_TOP, L.ROW_BOT, L.MOD, L.X0
HOLES, FIXED, ZONES, MOATS, TEXTS = L.HOLES, L.FIXED, L.ZONES, L.MOATS, L.TEXTS
mm = pcbnew.FromMM

uu = json.load(open(uuids_file))
PADNET = {}
for n in find(get(parse(open(netlist_file).read())[0], "nets"), "net"):
    for x in find(n, "node"):
        PADNET[(get(x, "ref")[1], get(x, "pin")[1])] = get(n, "name")[1]

board = pcbnew.NewBoard(out_pcb)
ds = board.GetDesignSettings()
ds.SetCopperLayerCount(2)
ds.m_TrackMinWidth = mm(0.15)
ds.m_CopperEdgeClearance = mm(getattr(L, "EDGE", 0.3))
ns = ds.m_NetSettings
d = ns.GetDefaultNetclass()
d.SetTrackWidth(mm(getattr(L, "DEF_W", 0.25))); d.SetClearance(mm(getattr(L, "CLEAR", 0.2)))
d.SetViaDiameter(mm(getattr(L, "VIA", (0.6, 0.3))[0])); d.SetViaDrill(mm(getattr(L, "VIA", (0.6, 0.3))[1]))
pw = pcbnew.NETCLASS("Power")
pw.SetTrackWidth(mm(getattr(L, "POWER_W", 0.6))); pw.SetClearance(mm(getattr(L, "PCLEAR", 0.25))); pw.SetViaDiameter(mm(getattr(L, "PVIA", (0.8, 0.4))[0])); pw.SetViaDrill(mm(getattr(L, "PVIA", (0.8, 0.4))[1]))
ns.SetNetclass("Power", pw)
for pat in getattr(L, "POWER_NETS", ["GND", "GND_VICTRON", "GND_DALY", "+5V", "5V_IN", "+3V3", "3V3_HELTEC", "+3V3_SW", "+12V",
            "+12V_IN", "LOAD", "24V_IN", "+24V_LOOP", "+5V_FLOW", "FLOW_VCC*", "REG_IN"]):
    ns.SetNetclassPatternAssignment("/" + pat, "Power"); ns.SetNetclassPatternAssignment(pat, "Power")
# medium-current rails (sensor supplies): own class with a narrower track
if getattr(L, "MEDIUM_NETS", None):
    md = pcbnew.NETCLASS("Medium")
    md.SetTrackWidth(mm(L.MEDIUM_W)); md.SetClearance(mm(getattr(L, "CLEAR", 0.2)))
    md.SetViaDiameter(mm(getattr(L, "VIA", (0.8, 0.4))[0])); md.SetViaDrill(mm(getattr(L, "VIA", (0.8, 0.4))[1]))
    ns.SetNetclass("Medium", md)
    for pat in L.MEDIUM_NETS: ns.SetNetclassPatternAssignment("/" + pat, "Medium"); ns.SetNetclassPatternAssignment(pat, "Medium")
# high-current paths (e.g. a 3 A heater output): wider track, power vias
if getattr(L, "HEAVY_NETS", None):
    hv = pcbnew.NETCLASS("Heavy")
    hv.SetTrackWidth(mm(L.HEAVY_W)); hv.SetClearance(mm(getattr(L, "PCLEAR", 0.25)))
    hv.SetViaDiameter(mm(getattr(L, "PVIA", (1.0, 0.5))[0])); hv.SetViaDrill(mm(getattr(L, "PVIA", (1.0, 0.5))[1]))
    ns.SetNetclass("Heavy", hv)
    for pat in L.HEAVY_NETS: ns.SetNetclassPatternAssignment("/" + pat, "Heavy"); ns.SetNetclassPatternAssignment(pat, "Heavy")

nets = {}
def net(name):
    if name not in nets:
        n = pcbnew.NETINFO_ITEM(board, name); board.Add(n); nets[name] = n
    return nets[name]
def V(x, y): return pcbnew.VECTOR2I(mm(OX + x), mm(OY + y))

def rect(layer, x0, y0, x1, y1, w=0.12):
    r = pcbnew.PCB_SHAPE(board); r.SetShape(pcbnew.SHAPE_T_RECT)
    r.SetStart(V(x0, y0)); r.SetEnd(V(x1, y1)); r.SetLayer(layer); r.SetWidth(mm(w)); board.Add(r)
rect(pcbnew.Edge_Cuts, 0, 0, W, H, 0.1)
rect(pcbnew.F_SilkS, MOD[0] + 0.3, MOD[1], MOD[2], MOD[3], 0.15)      # Heltec outline
for txt, x, y, sz in TEXTS:
    t = pcbnew.PCB_TEXT(board); t.SetText(txt); t.SetPosition(V(x, y)); t.SetLayer(pcbnew.F_SilkS)
    t.SetTextSize(pcbnew.VECTOR2I(mm(sz), mm(sz))); t.SetTextThickness(mm(sz * 0.15)); board.Add(t)

def load(libfp):
    lib, name = libfp.split(":")
    fp = pcbnew.FootprintLoad(f"{FPDIR}/{lib}.pretty", name)
    fp.SetFPID(pcbnew.LIB_ID(lib, name)); return fp

fps = {}
for p in PARTS:
    lc = LCSC.get(p["lcsc_key"], ("", "", False))
    fp = load(p["fp"])
    fp.SetReference(p["ref"]); fp.SetValue(VALUE_OVERRIDE.get(p["lcsc_key"], p["value"]))
    for k, v in (("LCSC", lc[0]), ("MPN", lc[1]), ("Description", p["desc"])):
        fp.SetField(k, v); fp.GetField(k).SetVisible(False)
    fp.SetPath(pcbnew.KIID_PATH("/" + uu["parts"][p["ref"]]))
    fp.SetSheetname("/"); fp.SetSheetfile(os.path.basename(out_pcb).replace(".kicad_pcb", ".kicad_sch"))
    is_conn = p["fp"].startswith(("Connector_", "TerminalBlock_"))
    sz = 1.0 if is_conn else 0.8
    fp.Reference().SetTextSize(pcbnew.VECTOR2I(mm(sz), mm(sz))); fp.Reference().SetTextThickness(mm(sz * 0.15))
    fp.Value().SetVisible(False)
    for pad in fp.Pads():
        if pad.GetNumber() == "": continue
        nn = PADNET[(p["ref"], pad.GetNumber())]
        want = p["pins"].get(pad.GetNumber())
        assert want is None or nn in ("/" + want, want), (p["ref"], pad.GetNumber(), nn, want)
        pad.SetNet(net(nn))
    if p.get("dnp"):
        fp.SetDNP(True); fp.SetExcludedFromPosFiles(True)
    if p["lcsc_key"] == "-":   # solder jumpers: PCB only
        fp.SetExcludedFromBOM(True); fp.SetExcludedFromPosFiles(True)
    board.Add(fp); fps[p["ref"]] = fp

def bbox(fp, g=0.0):
    c = fp.GetCourtyard(pcbnew.F_CrtYd)
    b = c.BBox() if c.OutlineCount() else fp.GetBoundingBox(False)
    return (pcbnew.ToMM(b.GetLeft()) - OX - g, pcbnew.ToMM(b.GetTop()) - OY - g,
            pcbnew.ToMM(b.GetRight()) - OX + g, pcbnew.ToMM(b.GetBottom()) - OY + g)
def hit(a, b): return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])

occupied = []
for i, (hx, hy) in enumerate(HOLES, 1):
    # HOLE_FP / HOLE_NET: plated mounting holes tied to a net (e.g. GND for an ESD path to a grounded chassis)
    fp = load(getattr(L, "HOLE_FP", "MountingHole:MountingHole_3.2mm_M3")); fp.SetReference(f"H{i}"); fp.SetValue("M3")
    if getattr(L, "HOLE_NET", None):
        for q in fp.Pads(): q.SetNet(net(L.HOLE_NET))
    fp.SetPosition(V(hx, hy)); fp.SetBoardOnly(True); fp.SetExcludedFromBOM(True); fp.SetExcludedFromPosFiles(True)
    fp.Reference().SetVisible(False); fp.Value().SetVisible(False)
    board.Add(fp)
    radius = getattr(L, "MOUNTING_COPPER_RADIUS", 0)
    occupied.append((hx - radius, hy - radius, hx + radius, hy + radius) if radius else bbox(fp))
for ref, (x, y, r) in FIXED.items():
    fp = fps[ref]; fp.SetOrientationDegrees(r); fp.SetPosition(V(x, y)); occupied.append(bbox(fp))
occupied.append((MOD[0], MOD[1], MOD[2], MOD[3]))   # nothing under the Heltec module
for rr in getattr(L, "RESERVED", []): occupied.append(rr)   # e.g. corridors of pre-routed tracks
# fixed silkscreen notes with a reserved (part-free) area: (text, x, y, size, angle)
for txt, x, y, sz, ang in getattr(L, "FIXED_NOTES", []):
    t = pcbnew.PCB_TEXT(board); t.SetText(txt); t.SetLayer(pcbnew.F_SilkS); t.SetTextAngleDegrees(ang)
    t.SetTextSize(pcbnew.VECTOR2I(mm(sz), mm(sz))); t.SetTextThickness(mm(sz * 0.16))
    t.SetPosition(V(x, y)); board.Add(t)
    tb = t.GetBoundingBox()
    occupied.append((pcbnew.ToMM(tb.GetLeft()) - OX - 0.3, pcbnew.ToMM(tb.GetTop()) - OY - 0.3,
                     pcbnew.ToMM(tb.GetRight()) - OX + 0.3, pcbnew.ToMM(tb.GetBottom()) - OY + 0.3))
# top-side label strip at every field terminal: pin functions right at the pins, title line
# (reference + function, completed by brand.py) above it; kept free of parts
for ref, labs in getattr(L, "TOP_PIN_LABELS", {}).items():
    fp = fps[ref]; bb = bbox(fp)
    pads = sorted([q for q in fp.Pads() if q.GetNumber()], key=lambda q: q.GetPosition().x)
    bottom = (pcbnew.ToMM(pads[0].GetPosition().y) - OY) > H / 2
    edge = bb[1] if bottom else bb[3]                    # body edge facing the board
    sgn = -1 if bottom else 1
    label_dy = getattr(L, "TOP_PIN_LABEL_DY", {}).get(ref, 0.0)
    sz = 1.0 if max(len(s) for s in labs) <= 3 else 0.8
    for q, lab in zip(pads, labs):
        t = pcbnew.PCB_TEXT(board); t.SetText(lab); t.SetLayer(pcbnew.F_SilkS)
        t.SetTextSize(pcbnew.VECTOR2I(mm(sz), mm(sz))); t.SetTextThickness(mm(sz * 0.16))
        t.SetPosition(pcbnew.VECTOR2I(q.GetPosition().x, mm(OY + edge + sgn * 1.0 + label_dy))); board.Add(t)
    r = fp.Reference(); r.SetTextAngleDegrees(0)
    r.SetPosition(pcbnew.VECTOR2I(r.GetPosition().x, mm(OY + edge + sgn * 2.6)))
    xs = [pcbnew.ToMM(q.GetPosition().x) - OX for q in pads]
    ys = (edge, edge + sgn * 3.6, edge + sgn * 1.0 + label_dy)
    y0, y1 = min(ys), max(ys)
    occupied.append((min(xs) - 1.75, y0, max(xs) + 1.75, y1))
# parts pinned by one pad: (ref, pad, x, y, other_pad, direction pad->other_pad)
for ref, pnum, tx, ty, other, dirn in getattr(L, "FIXED_PADS", []):
    fp = fps[ref]; done = False
    for r in (0, 90, 180, 270):
        fp.SetOrientationDegrees(r); fp.SetPosition(pcbnew.VECTOR2I(0, 0))
        pp = {q.GetNumber(): q.GetPosition() for q in fp.Pads() if q.GetNumber()}
        vx, vy = pp[other].x - pp[pnum].x, pp[other].y - pp[pnum].y
        ok = {"up": vy < 0 and abs(vy) > abs(vx), "down": vy > 0 and abs(vy) > abs(vx),
              "left": vx < 0 and abs(vx) > abs(vy), "right": vx > 0 and abs(vx) > abs(vy)}[dirn]
        if ok:
            fp.SetPosition(pcbnew.VECTOR2I(mm(OX + tx) - pp[pnum].x, mm(OY + ty) - pp[pnum].y))
            bb = bbox(fp)
            clash = [i for i, o in enumerate(occupied) if hit(bb, o)]
            if clash: print("FIXED_PADS overlap:", ref, bb, clash)
            occupied.append(bb); done = True; break
    assert done, ref
for ref in ("J1", "J2", "J19", "J20"):
    if ref in fps: fps[ref].Reference().SetVisible(False)   # module outline is labelled instead

# ---------------- connectivity-aware placement inside each zone ---------------
# Each part goes to the free spot (0.5 mm grid, 0/90/180/270 deg) that minimises the
# distance of its pads to already placed pads of the same nets (GND weighted low).
GAP = getattr(L, "GAP", 0.35)
problems = []
placed_pads = {}
def note_pads(fp):
    for p in fp.Pads():
        if p.GetNetname():
            placed_pads.setdefault(p.GetNetname(), []).append((pcbnew.ToMM(p.GetPosition().x) - OX, pcbnew.ToMM(p.GetPosition().y) - OY))
for ref in FIXED: note_pads(fps[ref])
for e in getattr(L, "FIXED_PADS", []): note_pads(fps[e[0]])
def wnet(n):
    return 0.15 if n.lstrip("/").startswith("GND") else 1.0
anchors_of = {}
for ref, own, aref, apad in getattr(L, "ANCHORS", []): anchors_of.setdefault(ref, []).append((own, aref, apad))
def fps_or_fixed(ref): return fps[ref]
def place_in(fp, rects, step=0.5):
    best = None
    for r in (0, 90, 180, 270):
        fp.SetOrientationDegrees(r); fp.SetPosition(V(0, 0))
        bb = bbox(fp, GAP / 2)
        w, h = bb[2] - bb[0], bb[3] - bb[1]
        offs = [(p.GetNetname(), pcbnew.ToMM(p.GetPosition().x) - OX, pcbnew.ToMM(p.GetPosition().y) - OY) for p in fp.Pads() if p.GetNetname()]
        offs_n = [(p.GetNetname(), pcbnew.ToMM(p.GetPosition().x) - OX, pcbnew.ToMM(p.GetPosition().y) - OY, p.GetNumber()) for p in fp.Pads() if p.GetNumber()]
        for (x0, x1, y0, y1) in rects:
            y = y0
            while y + h <= y1 + 1e-6:
                x = x0
                while x + w <= x1 + 1e-6:
                    cand = (x, y, x + w, y + h)
                    if not any(hit(cand, o) for o in occupied):
                        dx, dy = x - bb[0], y - bb[1]
                        cost = 0.0; linked = False
                        for n, px, py in offs:
                            if n in placed_pads:
                                linked = True
                                cost += wnet(n) * min(((px + dx - qx) ** 2 + (py + dy - qy) ** 2) ** 0.5 for qx, qy in placed_pads[n])
                        for own, aref, apad in anchors_of.get(fp.GetReference(), []):
                            ap = [q for q in fps_or_fixed(aref).Pads() if q.GetNumber() == apad][0].GetPosition()
                            ox_, oy_ = [(px, py) for n_, px, py, num in offs_n if num == own][0]
                            cost += 25.0 * (((ox_ + dx) - (pcbnew.ToMM(ap.x) - OX)) ** 2 + ((oy_ + dy) - (pcbnew.ToMM(ap.y) - OY)) ** 2) ** 0.5
                            linked = True
                        if not linked: cost = 1000 + (y - y0) * 10 + (x - x0)   # unconnected so far: pack top-left
                        if SEED: cost += rnd.uniform(0, 1.5)
                        if best is None or cost < best[0] - 1e-9: best = (cost, r, dx, dy, cand)
                    x += step
                y += step
    if best is None: return False
    cost, r, dx, dy, cand = best
    fp.SetOrientationDegrees(r); fp.SetPosition(V(dx, dy)); occupied.append(cand); note_pads(fp)
    return True
pinned = {e[0] for e in getattr(L, "FIXED_PADS", [])}
assigned = set(FIXED) | pinned | {ref for zone in ZONES for ref in zone[5]}
unassigned = sorted(set(fps) - assigned)
if unassigned:
    raise SystemExit(f"parts without a placement zone or fixed position: {unassigned}")
for m in MOATS: occupied.append(m)
for name, x0, x1, y0, y1, refs in ZONES:
    for ref in refs:
        if ref in pinned or ref in FIXED: continue
        if not place_in(fps[ref], [(x0, x1, y0, y1)]): problems.append((name, ref))
    if name in getattr(L, "ISO_ZONES", ()):            # the isolated domain is closed for all other parts
        occupied.append((x0 - 0.6, y0 - 0.3, x1 + 0.3, y1 + 0.3))

# refinement: anchored parts that ended up > 4 mm from their (first) anchor pad get
# re-placed inside a small window around that pad (only into free space)
def pad_xy(ref, num):
    q = [p for p in fps[ref].Pads() if p.GetNumber() == num][0].GetPosition()
    return pcbnew.ToMM(q.x) - OX, pcbnew.ToMM(q.y) - OY
def anchor_dist(ref):
    own, aref, apad = anchors_of[ref][0]
    (x1, y1), (x2, y2) = pad_xy(ref, own), pad_xy(aref, apad)
    return ((x1 - x2) ** 2 + (y1 - y2) ** 2) ** 0.5
for ref in getattr(L, "REFINE", []):
    if ref not in anchors_of or anchor_dist(ref) <= 4.0: continue
    fp = fps[ref]; before = anchor_dist(ref)
    old_bb = bbox(fp, GAP / 2)
    idx = [i for i, o in enumerate(occupied) if all(abs(a_ - b_) < 0.3 for a_, b_ in zip(o, old_bb))]
    saved_pos, saved_rot = fp.GetPosition(), fp.GetOrientationDegrees()
    removed = occupied.pop(idx[0]) if idx else None
    own, aref, apad = anchors_of[ref][0]; ax, ay = pad_xy(aref, apad)
    zr = [z for z in ZONES if ref in z[5]][0]
    win = (max(zr[1], ax - 6), min(zr[2], ax + 6), max(zr[3], ay - 6), min(zr[4], ay + 6))
    n0 = len(occupied)
    ok = place_in(fp, [win], step=0.25)
    if ok and anchor_dist(ref) < before:
        print(f"refined {ref}: {before:.1f} -> {anchor_dist(ref):.1f} mm")
    else:
        # restore the original spot (and only remove what this attempt added)
        if len(occupied) > n0: occupied.pop()
        fp.SetOrientationDegrees(saved_rot); fp.SetPosition(saved_pos)
        occupied.append(removed if removed is not None else old_bb)

# rotation pass: with all parts placed, turn each part in place (same centre) so that its
# pads face their partners - short, direct connections that need no detour around / under
# the part itself. Cost per pad: distance to the nearest foreign pad of the same net plus a
# penalty when that partner lies "behind" the pad (on the far side of the part body).
def pads_of_others(ref):
    d_ = {}
    for r_, f_ in fps.items():
        if r_ == ref or pcbnew.ToMM(f_.GetPosition().x) < OX - 1: continue    # skip still unplaced parts
        for q in f_.Pads():
            if q.GetNetname(): d_.setdefault(q.GetNetname(), []).append((pcbnew.ToMM(q.GetPosition().x) - OX, pcbnew.ToMM(q.GetPosition().y) - OY))
    return d_
def orient_cost(fp, others):
    c = fp.GetPosition(); cx, cy = pcbnew.ToMM(c.x) - OX, pcbnew.ToMM(c.y) - OY
    cost = 0.0
    for q in fp.Pads():
        n = q.GetNetname()
        if not n or n not in others: continue
        px, py = pcbnew.ToMM(q.GetPosition().x) - OX, pcbnew.ToMM(q.GetPosition().y) - OY
        tx, ty = min(others[n], key=lambda t: (t[0] - px) ** 2 + (t[1] - py) ** 2)
        d = ((tx - px) ** 2 + (ty - py) ** 2) ** 0.5
        ox, oy = px - cx, py - cy; ol = (ox * ox + oy * oy) ** 0.5 or 1.0
        behind = max(0.0, -((ox / ol) * (tx - px) + (oy / ol) * (ty - py)))
        cost += wnet(n) * (d + 2.0 * behind)
    return cost
fixed_refs = set(FIXED) | pinned
movable = [r_ for z in ZONES for r_ in z[5] if r_ not in fixed_refs and r_ in fps]
for it in range(2):
    turned = 0
    for ref in movable:
        fp = fps[ref]; others = pads_of_others(ref)
        old_bb = bbox(fp, GAP / 2)
        idx = [i for i, o in enumerate(occupied) if all(abs(a_ - b_) < 0.3 for a_, b_ in zip(o, old_bb))]
        if not idx: continue
        own = occupied.pop(idx[0])
        c0 = fp.GetPosition(); r0 = fp.GetOrientationDegrees()
        best = (orient_cost(fp, others), r0, own)
        for r in (0, 90, 180, 270):
            if abs((r - r0) % 360) < 1: continue
            fp.SetOrientationDegrees(r); fp.SetPosition(c0)
            bb = bbox(fp, GAP / 2)
            if any(hit(bb, o) for o in occupied) or bb[0] < 0.5 or bb[1] < 0.5 or bb[2] > W - 0.5 or bb[3] > H - 0.5: continue
            cst = orient_cost(fp, others)
            if cst < best[0] - 0.3: best = (cst, r, bb)
        fp.SetOrientationDegrees(best[1]); fp.SetPosition(c0); occupied.append(best[2])
        if abs((best[1] - r0) % 360) > 1: turned += 1
    print(f"rotation pass {it + 1}: {turned} parts turned")

# leftovers: spill areas (main GND domain only) - never for isolated-domain parts
still = []
ISO_ZONES = getattr(L, "ISO_ZONES", ("VIC", "DALY"))
for name, ref in problems:
    if name not in ISO_ZONES and place_in(fps[ref], getattr(L, "SPILL", [])): print("spilled", ref, "from", name)
    else: still.append((name, ref))
problems = still
if problems:
    raise SystemExit(f"unable to place all footprints: {problems}")

# keep-out under the isolators / optocoupler body (no tracks, vias, pours)
for ref in L.ISOLATORS:
    fp = fps[ref]
    cx, cy = pcbnew.ToMM(fp.GetPosition().x) - OX, pcbnew.ToMM(fp.GetPosition().y) - OY
    vertical_barrier = int(fp.GetOrientationDegrees()) % 180 == 0
    w, h = ((1.8, 5.2) if ref != "U5" else (3.0, 5.0)) if vertical_barrier else (5.2, 1.8)
    z = pcbnew.ZONE(board); z.SetIsRuleArea(True)
    z.SetDoNotAllowTracks(True); z.SetDoNotAllowVias(True); z.SetDoNotAllowZoneFills(True)
    z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
    z.SetLayerSet(pcbnew.LSET.AllCuMask()); z.SetZoneName(f"ISO_{ref}")
    ol = z.Outline(); ol.NewOutline()
    for dx, dy in ((-w/2, -h/2), (w/2, -h/2), (w/2, h/2), (-w/2, h/2)): ol.Append(mm(OX + cx + dx), mm(OY + cy + dy))
    board.Add(z)

# no foreign tracks between the pad rows of small ICs / transistors (F.Cu): the router has to
# go around the part instead of squeezing a track under its body
for ref, fp in fps.items():
    if not fp.GetFPIDAsString().split(":")[-1].startswith(("SOT-23", "TSOT-23", "SOIC-8", "SOT-89")): continue
    pp = [(pcbnew.ToMM(q.GetPosition().x) - OX, pcbnew.ToMM(q.GetPosition().y) - OY, q) for q in fp.Pads() if q.GetNumber()]
    xs = sorted({round(x, 2) for x, y, q in pp}); ys = sorted({round(y, 2) for x, y, q in pp})
    vertical_rows = len(ys) <= 2 and (len(xs) > 2 or (ys[-1] - ys[0]) > (xs[-1] - xs[0]))   # pads in two rows above / below
    boxes = [(pcbnew.ToMM(q.GetBoundingBox().GetLeft()) - OX, pcbnew.ToMM(q.GetBoundingBox().GetTop()) - OY,
              pcbnew.ToMM(q.GetBoundingBox().GetRight()) - OX, pcbnew.ToMM(q.GetBoundingBox().GetBottom()) - OY) for x, y, q in pp]
    if vertical_rows:
        mid = (ys[0] + ys[-1]) / 2
        y0 = max(b[3] for b in boxes if (b[1] + b[3]) / 2 < mid) + 0.2; y1 = min(b[1] for b in boxes if (b[1] + b[3]) / 2 > mid) - 0.2
        x0 = min(b[0] for b in boxes); x1 = max(b[2] for b in boxes)
    else:
        mid = (xs[0] + xs[-1]) / 2
        x0 = max(b[2] for b in boxes if (b[0] + b[2]) / 2 < mid) + 0.2; x1 = min(b[0] for b in boxes if (b[0] + b[2]) / 2 > mid) - 0.2
        y0 = min(b[1] for b in boxes); y1 = max(b[3] for b in boxes)
    if x1 - x0 < 0.3 or y1 - y0 < 0.3: continue
    z = pcbnew.ZONE(board); z.SetIsRuleArea(True)
    # U1's exposed GND pad needs a short fanout to its thermal via.
    z.SetDoNotAllowTracks(ref != "U1"); z.SetDoNotAllowVias(ref != "U1"); z.SetDoNotAllowZoneFills(False)
    z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
    z.SetLayer(pcbnew.F_Cu); z.SetZoneName(f"UNDER_{ref}")
    ol = z.Outline(); ol.NewOutline()
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)): ol.Append(mm(OX + x), mm(OY + y))
    board.Add(z)

# moat between main GND domain and the isolated domains: no tracks/vias/pours,
# so the autorouter cannot cross it (isolators/optocoupler straddle it)
for i, (x0, y0, x1, y1) in enumerate(MOATS):
    z = pcbnew.ZONE(board); z.SetIsRuleArea(True)
    z.SetDoNotAllowTracks(True); z.SetDoNotAllowVias(True); z.SetDoNotAllowZoneFills(True)
    z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
    z.SetLayerSet(pcbnew.LSET.AllCuMask()); z.SetZoneName(f"MOAT_{i}")
    ol = z.Outline(); ol.NewOutline()
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)): ol.Append(mm(OX + x), mm(OY + y))
    board.Add(z)
# Keep copper clear of M3 hardware with a 7 mm outer diameter plus radial margin.
for hx, hy in HOLES:
    radius = getattr(L, "MOUNTING_COPPER_RADIUS", 0)
    if not radius or getattr(L, "HOLE_NET", None): continue   # holes on a net: the pad is the washer area and connects
    z = pcbnew.ZONE(board); z.SetIsRuleArea(True)
    z.SetZoneName("M3 7mm hardware + 0.25mm copper margin")
    z.SetLayerSet(pcbnew.LSET.AllCuMask())
    z.SetDoNotAllowTracks(True); z.SetDoNotAllowVias(True); z.SetDoNotAllowZoneFills(True)
    z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
    ol = z.Outline(); ol.NewOutline()
    import math
    for i in range(64):
        angle = 2 * math.pi * i / 64
        outer = radius / math.cos(math.pi / 64)
        ol.Append(mm(OX + hx + outer * math.cos(angle)), mm(OY + hy + outer * math.sin(angle)))
    board.Add(z)

# ground vias right at the converter GND pins (placed before routing)
import math
def pre_gnd_via(ref, padnum):
    fp = fps[ref]; pad = [p for p in fp.Pads() if p.GetNumber() == padnum][0]
    c0 = pad.GetPosition(); cc = fp.GetPosition(); net = pad.GetNet()
    base = math.atan2(c0.y - cc.y, c0.x - cc.x)
    for rr in (1.3, 1.6, 1.9, 2.2):
        for da in (0, 0.5, -0.5, 1.0, -1.0, 1.5, -1.5):
            a = base + da
            vc = pcbnew.VECTOR2I(int(c0.x + mm(rr) * math.cos(a)), int(c0.y + mm(rr) * math.sin(a)))
            ok = True
            for f2 in board.GetFootprints():
                for p in f2.Pads():
                    if p.GetNetname() == pad.GetNetname(): continue
                    for lay in (pcbnew.F_Cu, pcbnew.B_Cu):
                        if p.IsOnLayer(lay) and (p.GetEffectiveShape(lay).Collide(pcbnew.SHAPE_CIRCLE(vc, mm(0.4)), mm(0.3))
                                                 or p.GetEffectiveShape(lay).Collide(pcbnew.SHAPE_SEGMENT(c0, vc, mm(0.4)), mm(0.3)) and lay == pcbnew.F_Cu):
                            ok = False
            if ok:
                t = pcbnew.PCB_TRACK(board); t.SetStart(c0); t.SetEnd(vc); t.SetWidth(mm(0.4)); t.SetLayer(pcbnew.F_Cu); t.SetNet(net); board.Add(t)
                v = pcbnew.PCB_VIA(board); v.SetPosition(vc); v.SetWidth(mm(0.8)); v.SetDrill(mm(0.4)); v.SetNet(net); board.Add(v)
                return True
    print("no pre-via for", ref, padnum); return False
for ref, pn in getattr(L, "GND_VIAS", []): pre_gnd_via(ref, pn)
for ref, (pn, offsets) in getattr(L, "THERMAL_VIAS", {}).items():
    fp = fps[ref]; pad = next(p for p in fp.Pads() if p.GetNumber() == pn)
    angle = math.radians(fp.GetOrientationDegrees())
    for dx, dy in offsets:
        p = pad.GetPosition()
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I(p.x + mm(dx * math.cos(angle) + dy * math.sin(angle)),
                                     p.y + mm(dy * math.cos(angle) - dx * math.sin(angle))))
        v.SetWidth(mm(0.6)); v.SetDrill(mm(0.3)); v.SetViaType(pcbnew.VIATYPE_THROUGH)
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(pad.GetNet()); board.Add(v)

# pre-routed chains (e.g. connector pin -> TVS -> second connector pin): short, direct,
# locked tracks on F.Cu so the surge path is fixed before Freerouting fills in the rest
def _pad(ref, num): return [p for p in fps[ref].Pads() if p.GetNumber() == num][0]
def _seg_free(a, c, netname, w):
    seg = pcbnew.SHAPE_SEGMENT(a, c, w)
    clearance = getattr(L, "PCLEAR", 0.25) if netname.lstrip("/") in getattr(L, "POWER_NETS", ()) else getattr(L, "CLEAR", 0.2)
    for f2 in board.GetFootprints():
        for p in f2.Pads():
            if p.GetNetname() != netname and p.IsOnLayer(pcbnew.F_Cu) and p.GetEffectiveShape(pcbnew.F_Cu).Collide(seg, mm(clearance)):
                return False
    for t in board.GetTracks():
        if t.GetNetname() != netname and t.IsOnLayer(pcbnew.F_Cu) and t.GetEffectiveShape(pcbnew.F_Cu).Collide(seg, mm(0.26)):
            return False
    return True
def pre_route(chain, w, strict=False):
    # chain items: (ref, pad) or ("@", x, y) waypoints in board mm; a segment touching a
    # waypoint is drawn straight, pad -> pad also tries one-corner / 45 deg paths
    pads_ = [_pad(*it) for it in chain if it[0] != "@"]
    net = pads_[0].GetNet(); netname = pads_[0].GetNetname()
    def pos(it): return _pad(*it).GetPosition() if it[0] != "@" else V(it[1], it[2])
    for i1, i2 in zip(chain, chain[1:]):
        if i1[0] == "@" or i2[0] == "@":
            a, c = pos(i1), pos(i2)
            if _seg_free(a, c, netname, w):
                t = pcbnew.PCB_TRACK(board); t.SetStart(a); t.SetEnd(c); t.SetWidth(w)
                t.SetLayer(pcbnew.F_Cu); t.SetNet(net); t.SetLocked(True); board.Add(t)
            else:
                if strict: raise RuntimeError(f"Critical pre-route blocked: {i1} -> {i2}")
                print("pre-route segment blocked:", i1, "->", i2)
            continue
        (r1, p1), (r2, p2) = i1, i2
        pa, pc = _pad(r1, p1), _pad(r2, p2); a, c = pa.GetPosition(), pc.GetPosition()
        dx, dy = c.x - a.x, c.y - a.y
        sx, sy = (1 if dx >= 0 else -1), (1 if dy >= 0 else -1); d = min(abs(dx), abs(dy))
        VI = pcbnew.VECTOR2I
        paths = [[a, c],
                 [a, VI(c.x - sx * d, a.y), c] if abs(dx) >= abs(dy) else [a, VI(a.x, c.y - sy * d), c],   # straight, then 45 deg
                 [a, VI(a.x + sx * d, a.y + sy * d), c],                                                  # 45 deg, then straight
                 [a, VI(c.x, a.y), c], [a, VI(a.x, c.y), c]]
        for pts in paths:
            if all(_seg_free(u, v, pa.GetNetname(), w) for u, v in zip(pts, pts[1:])):
                for u, v in zip(pts, pts[1:]):
                    if u == v: continue
                    t = pcbnew.PCB_TRACK(board); t.SetStart(u); t.SetEnd(v); t.SetWidth(w)
                    t.SetLayer(pcbnew.F_Cu); t.SetNet(net); t.SetLocked(True); board.Add(t)
                break
        else:
            if strict: raise RuntimeError(f"Critical pre-route blocked: {r1}.{p1} -> {r2}.{p2}")
            print("no pre-route for", r1, p1, "->", r2, p2)

for width, chain in getattr(L, "BUCK_PRE_ROUTE", []): pre_route(chain, mm(width), strict=True)
for width, chain in getattr(L, "LOCAL_PRE_ROUTE", []): pre_route(chain, mm(width), strict=True)
for chain in getattr(L, "PRE_ROUTE", []): pre_route(chain, mm(getattr(L, "PRE_ROUTE_W", 0.5)))

# silkscreen pin labels for unpopulated headers (test points, expansion)
for ref, labs in getattr(L, "PIN_LABELS", {}).items():
    fp = fps[ref]
    pads = sorted([q for q in fp.Pads() if q.GetNumber()], key=lambda q: int(q.GetNumber()))
    vertical = len(pads) == 1 or abs(pads[-1].GetPosition().x - pads[0].GetPosition().x) < abs(pads[-1].GetPosition().y - pads[0].GetPosition().y)
    for q, lab in zip(pads, labs):
        t = pcbnew.PCB_TEXT(board); t.SetText(lab); t.SetLayer(pcbnew.F_SilkS)
        t.SetTextSize(pcbnew.VECTOR2I(mm(0.8), mm(0.8))); t.SetTextThickness(mm(0.12))
        pos = q.GetPosition()
        if vertical:
            t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT); t.SetPosition(pcbnew.VECTOR2I(pos.x + mm(1.6), pos.y))
        else:
            t.SetTextAngleDegrees(90); t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT); t.SetPosition(pcbnew.VECTOR2I(pos.x, pos.y - mm(1.6)))
        board.Add(t)

# row labels beside 2-row headers (e.g. SCL / SDA jumper pairs), on the side away from the other row
for ref, rows in getattr(L, "ROW_LABELS", {}).items():
    fp = fps[ref]; pp = {q.GetNumber(): q.GetPosition() for q in fp.Pads() if q.GetNumber()}
    c = fp.GetPosition()
    for a_, b_, lab in rows:
        mx, my = (pp[a_].x + pp[b_].x) // 2, (pp[a_].y + pp[b_].y) // 2
        horiz = abs(pp[a_].x - pp[b_].x) > abs(pp[a_].y - pp[b_].y)
        t = pcbnew.PCB_TEXT(board); t.SetText(lab); t.SetLayer(pcbnew.F_SilkS)
        t.SetTextSize(pcbnew.VECTOR2I(mm(0.9), mm(0.9))); t.SetTextThickness(mm(0.14))
        if horiz: t.SetPosition(pcbnew.VECTOR2I(mx, my + (mm(2.2) if my > c.y else -mm(2.2))))
        else:
            t.SetTextAngleDegrees(90); t.SetPosition(pcbnew.VECTOR2I(mx + (mm(2.2) if mx > c.x else -mm(2.2)), my))
        board.Add(t)

# free silkscreen notes next to a footprint
for ref, txt, dx, dy, sz in getattr(L, "NOTES", []):
    fp = fps[ref]; t = pcbnew.PCB_TEXT(board); t.SetText(txt); t.SetLayer(pcbnew.F_SilkS)
    t.SetTextSize(pcbnew.VECTOR2I(mm(sz), mm(sz))); t.SetTextThickness(mm(sz * 0.15))
    t.SetPosition(pcbnew.VECTOR2I(fp.GetPosition().x + mm(dx), fp.GetPosition().y + mm(dy))); board.Add(t)

# bottom silkscreen: pin functions of every field terminal + title block (mirrored)
def btxt(s_, x, y, size, angle):
    t = pcbnew.PCB_TEXT(board); t.SetText(s_); t.SetLayer(pcbnew.B_SilkS); t.SetMirrored(True)
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size))); t.SetTextThickness(mm(size * 0.15))
    t.SetTextAngleDegrees(angle); t.SetPosition(pcbnew.VECTOR2I(x, y)); board.Add(t)
for ref, labs in getattr(L, "BACK_LABELS", {}).items():
    fp = fps[ref]
    pads = sorted([q for q in fp.Pads() if q.GetNumber()], key=lambda q: int(q.GetNumber()))
    top = pcbnew.ToMM(pads[0].GetPosition().y) - OY < H / 2       # terminal at the top edge
    sgn = 1 if top else -1
    for q, lab in zip(pads, labs):
        pos = q.GetPosition(); btxt(lab, pos.x, pos.y + sgn * mm(5.6), 1.0, 90)
    xs = [q.GetPosition().x for q in pads]
    btxt(L.BACK_TITLES[ref], (min(xs) + max(xs)) // 2, pads[0].GetPosition().y + sgn * mm(10.2), 1.0, 0)
for ref, labs in getattr(L, "BACK_LABELS_V", {}).items():   # terminals at the left / right edge
    fp = fps[ref]
    pads = sorted([q for q in fp.Pads() if q.GetNumber()], key=lambda q: int(q.GetNumber()))
    left = pcbnew.ToMM(pads[0].GetPosition().x) - OX < W / 2
    sgn = 1 if left else -1
    for q, lab in zip(pads, labs):
        pos = q.GetPosition(); btxt(lab, pos.x + sgn * mm(5.6), pos.y, 1.0, 0)
    ys = [q.GetPosition().y for q in pads]
    if ref in getattr(L, "BACK_TITLES", {}):
        btxt(L.BACK_TITLES[ref], pads[0].GetPosition().x + sgn * mm(10.2), (min(ys) + max(ys)) // 2, 1.0, 90)
for txt, x, y, sz in getattr(L, "BACK_TEXTS", ()):
    btxt(txt, mm(OX + x), mm(OY + y), sz, 0)

# ring along the board edge: no tracks/vias (Freerouting does not know the edge clearance)
E = getattr(L, "EDGE", 0.3) + 0.15
for i, (x0, y0, x1, y1) in enumerate(((0, 0, W, E), (0, H - E, W, H), (0, 0, E, H), (W - E, 0, W, H))):
    z = pcbnew.ZONE(board); z.SetIsRuleArea(True)
    z.SetDoNotAllowTracks(True); z.SetDoNotAllowVias(True); z.SetDoNotAllowZoneFills(False)
    z.SetDoNotAllowPads(False); z.SetDoNotAllowFootprints(False)
    z.SetLayerSet(pcbnew.LSET.AllCuMask()); z.SetZoneName(f"EDGE_{i}")
    ol = z.Outline(); ol.NewOutline()
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)): ol.Append(mm(OX + x), mm(OY + y))
    board.Add(z)
pcbnew.SaveBoard(out_pcb, board)
print("saved", out_pcb, f"{W}x{H} mm", "unplaced:", problems)
