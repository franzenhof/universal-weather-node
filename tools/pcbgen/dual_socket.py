"""Heltec V3 / V4 double socket: in-place edit of the routed v2.0 board (like add_dcjack.py).

Adds the V4 socket set (J19 5V side, J20 3V3 side, both 2.54 mm towards J1; J21 = 1x2
socket for the V4's P4 pins GPIO15/16 at the Heltec STEP position), the V4 5 V diodes
the V4 5 V ideal diodes U8/U9 (+C39) and the VE.Direct TX pull-down R65; moves the parts in the way (C36, C37, R1,
JP8, JP12, J14 and its labels), moves the VE.Direct isolation moat 1.65 mm down,
applies the new netlist (GPIO19 -> VANE_EN, +5V_V4), rips up only the copper that now
collides and routes the open connections with route_grid.py (Freerouting would re-fan the
finished board). The rest of the reviewed routing stays untouched.

Run with KiCad's Python (dual_socket.sh does all steps):
  python3 dual_socket.py prepare  <board> <netlist> <uuids.json>
  python3 dual_socket.py geometry <board> <drc.json> <geometry.json>
  python3 route_grid.py <geometry.json> <routes.json>          # numpy / scipy / pillow
  python3 dual_socket.py apply    <board> <routes.json>
  python3 dual_socket.py silk     <board>
"""
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew
from sexp import parse, find, get
from design import PARTS
from lcsc import LCSC, VALUE_OVERRIDE

FPDIR = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
mm, MM = pcbnew.FromMM, pcbnew.ToMM
V = lambda x, y: pcbnew.VECTOR2I(mm(x), mm(y))
PART = {p["ref"]: p for p in PARTS}

# ---- geometry (board coordinates, origin of the 100 x 100 mm board at 50/50) ----
PITCH = 2.54
ROW_J1, ROW_J2 = 82.36, 59.5            # V3 rows (unchanged)
SHIFT = PITCH                           # V4 rows towards J1: the other direction puts V4 pin 1
                                        # 2.97 mm from the H1 screw and over F3/C9/D5/J16
ROW_J19, ROW_J20 = ROW_J1 + SHIFT, ROW_J2 + SHIFT
X_PIN1 = 54.2
# P4 (GPIO15/16) from the Heltec V4 STEP model: holes at module (47.429, +5.842 / +3.302) mm,
# header pin 1 at module (2.16, +-11.43) -> board x = 47.429 - 2.16 + 54.2, y = ROW_J20 + 11.43 - Y
P4_X = round(47.429 - 2.16 + X_PIN1, 3)
P4_Y = round(ROW_J20 + 11.43 - 5.842, 3)         # pin 1 = GPIO15 (outer), pin 2 = GPIO16 below
NEW = {   # ref: (x, y, rotation)
    "J19": (X_PIN1, ROW_J19, 90), "J20": (X_PIN1, ROW_J20, 90), "J21": (P4_X, P4_Y, 0),
    "U8": (59.6, 78.6, 0), "U9": (59.6, 75.8, 180), "C39": (62.9, 78.6, 90), "R65": (75.0, 66.6, 0),
}
MOVE = {  # parts in the way of the V4 rows
    "C37": (56.0, 66.9, -90), "C36": (59.4, 66.45, -90), "R1": (95.6, 77.2, 90),
    "JP8": (73.5, 88.6, 0), "JP12": (78.5, 88.9, 90), "J14": (55.0, 89.3, -90),
}
TEXT_SHIFT = {  # silkscreen labels that follow a moved part: (text, x, y) -> dy
    ("GND", 59.9, 88.8): 0.5, ("TX", 59.9, 92.3): 0.5, ("RX", 59.9, 95.8): 0.5, ("V+", 59.9, 99.3): 0.5,
    ("J14 VE.DIRECT", 62.4, 94.05): 0.5, ("JP8: VIN BUCK\nREMOVE BEFORE USB", 82.0, 91.6): 0.6,
}
MOAT_DY = 1.65                          # VE.Direct isolation moat / GND_VICTRON pour top edge down
VIC_TOP = (84.3, 85.95)                 # old top edges: GND notch 84.35, moats 84.4 / 85.9, GND_VICTRON 85.85
CLEAR = 0.25                            # rip-up clearance (power class 0.25 mm)
F = "F"
# fixed routes (net, width, layer, points); 0.3 mm where they pass between two socket pins
# (0.84 mm gap, 0.27 mm clearance each side), as the reviewed 3V3_LDO run between J2 3/4
PRE = [
    # U8 (board 5 V -> V4 VBUS) / U9 (V4 VBUS -> board 5 V), LM66100 SC-70-6, CE = own VOUT
    ("+5V", 0.6, F, [(56.74, 82.36), (56.74, 80.3), (57.2, 79.84), (57.2, 77.95)]),       # J1.2 down to U8/U9
    ("+5V", 0.3, F, [(57.2, 77.95), (58.763, 77.95)]),                                     # -> U8.1 VIN
    ("+5V", 0.5, F, [(57.2, 77.95), (57.2, 76.45)]),
    ("+5V", 0.3, F, [(57.2, 76.45), (58.763, 76.45)]),                                     # -> U9.6 VOUT
    ("+5V", 0.25, F, [(57.2, 76.45), (57.2, 74.45), (60.438, 74.45), (60.438, 75.15)]),    # -> U9.3 CE
    ("/+5V_V4", 0.3, F, [(56.74, 84.9), (58.01, 84.9), (58.01, 80.6)]),                   # J19.2 between J1.2/3
    ("/+5V_V4", 0.4, F, [(58.01, 80.6), (61.5, 80.6), (61.5, 76.45)]),
    ("/+5V_V4", 0.25, F, [(58.763, 79.25), (58.763, 80.6)]),                              # U8.3 CE
    ("/+5V_V4", 0.3, F, [(61.5, 77.95), (60.438, 77.95)]),                                # U8.6 VOUT
    ("/+5V_V4", 0.3, F, [(61.5, 76.45), (60.438, 76.45)]),                                # U9.1 VIN
    ("/+5V_V4", 0.4, F, [(61.5, 79.55), (62.9, 79.55)]),                                  # C39
    # V4 GPIO44 / GPIO43 (J19.5 / J19.6) sit above the VE.Direct moat: leave upwards between J1 pins
    ("/WSPD_OUT", 0.2, F, [(64.36, 84.9), (65.63, 83.63), (65.63, 77.0)]),
    ("/ONEWIRE_IO", 0.2, F, [(66.9, 84.9), (68.17, 83.63), (68.17, 77.0)]),
    # +5V (board rail <-> J1.2 / U8 / U9 / U7 cluster) and 3V3_HELTEC (module 3.3 V <-> U4 / TP6 / R1)
    # cross both V3 and V4 5V-side rows between unconnected pins (J1/J19 8|9 and 7|8): 0.3 mm necks
    ("+5V", 0.3, F, [(73.25, 80.6), (73.25, 86.4)]),
    ("+5V", 0.6, F, [(73.25, 86.4), (75.3, 86.4), (75.3, 90.6), (74.76, 91.14), (73.5, 91.14)]),
    ("3V3_HELTEC", 0.3, "B", [(70.71, 80.6), (70.71, 86.8), (70.5, 87.01), (70.5, 93.0)]),
    ("/3V3_LDO", 0.3, F, [(59.4, 64.975), (60.55, 64.975), (60.55, 57.5), (59.6, 56.55), (59.2, 56.05)]),  # C36 -> F3 between J20/J2 3/4
    ("3V3_HELTEC", 0.6, F, [(56.0, 65.425), (56.0, 63.4), (56.74, 62.66), (56.74, 62.04)]),  # C37 -> J20.2
]
# U8's two GND pins sit in a pocket enclosed by its 5 V routes: ground via between them
# (0.28 mm to the neighbouring signal pads), the B.Cu pour carries the return
PRE_VIAS = [("GND", 59.6, 78.6, 0.6, 0.3)]
POWER_NEW = ["+5V_V4"]


def board_net(b, name):
    n = b.FindNet(name)
    if n is None:
        n = pcbnew.NETINFO_ITEM(b, name); b.Add(n)
    return n


def load(libfp):
    lib, name = libfp.split(":")
    fp = pcbnew.FootprintLoad(f"{FPDIR}/{lib}.pretty", name)
    fp.SetFPID(pcbnew.LIB_ID(lib, name)); return fp


def items_at(b, pt, net, tol=mm(0.02)):
    """tracks/vias touching point pt (same net)"""
    out = []
    for t in b.GetTracks():
        if t.GetNetCode() != net: continue
        if t.GetClass() == "PCB_VIA":
            if (t.GetPosition() - pt).EuclideanNorm() <= tol: out.append(t)
        elif (t.GetStart() - pt).EuclideanNorm() <= tol or (t.GetEnd() - pt).EuclideanNorm() <= tol:
            out.append(t)
    return out


def pad_at(b, pt, net, layer):
    for fp in b.GetFootprints():
        for p in fp.Pads():
            if p.GetNetCode() == net and p.IsOnLayer(layer) and p.HitTest(pt): return True
    return False


def chain_delete(b, seed, dead):
    """delete seed and the rest of its chain up to the next pad, via or junction (>= 3 branches)"""
    uid = lambda o: o.m_Uuid.AsString()
    stack = [seed]
    while stack:
        t = stack.pop()
        if uid(t) in dead: continue
        if t is not seed and (t.GetClass() == "PCB_VIA" or t.IsLocked()): continue      # vias stay: the rest of the chain
                                                                        # remains a nearby target for the router
        dead[uid(t)] = t
        ends = [t.GetPosition()] if t.GetClass() == "PCB_VIA" else [t.GetStart(), t.GetEnd()]
        for e in ends:
            if t.GetClass() != "PCB_VIA" and pad_at(b, e, t.GetNetCode(), t.GetLayer()): continue
            nb = [o for o in items_at(b, e, t.GetNetCode()) if uid(o) != uid(t) and uid(o) not in dead]
            vias = [o for o in nb if o.GetClass() == "PCB_VIA"]
            trk = [o for o in nb if o.GetClass() != "PCB_VIA"]
            if t.GetClass() == "PCB_VIA":
                if t is seed or len(trk) <= 1: stack += trk     # via between two layers: follow on
            elif vias:
                if vias[0].GetNetname() != "GND" and len(trk) <= 1: stack += vias + trk
            elif len(trk) == 1:
                stack += trk


def prepare(pcb, netlist, uuids):
    b = pcbnew.LoadBoard(pcb)
    FPS = {fp.GetReference(): fp for fp in b.GetFootprints()}
    assert "J19" not in FPS, "double socket already on the board"
    padnet = {}
    for n in find(get(parse(open(netlist).read())[0], "nets"), "net"):
        for x in find(n, "node"): padnet[(get(x, "ref")[1], get(x, "pin")[1])] = get(n, "name")[1]
    uu = json.load(open(uuids))["parts"]

    # net classes: the new V4 5 V rail is a power net
    ns = b.GetDesignSettings().m_NetSettings
    for pat in POWER_NEW:
        ns.SetNetclassPatternAssignment("/" + pat, "Power"); ns.SetNetclassPatternAssignment(pat, "Power")
    # rename GPIO19 -> VANE_EN (net, incl. its tracks)
    old = b.FindNet("/GPIO19")
    vane = board_net(b, "/VANE_EN")
    for t in b.GetTracks():
        if old and t.GetNetCode() == old.GetNetCode(): t.SetNet(vane)

    # old pad positions of moved parts -> copper there gets ripped up
    oldpads = []
    for ref in MOVE:
        for p in FPS[ref].Pads(): oldpads.append((p.GetPosition(), p.GetNetCode(), p))
    for ref, (x, y, r) in MOVE.items():
        f = FPS[ref]; f.SetPosition(V(x, y)); f.SetOrientationDegrees(r)
    for d in b.GetDrawings():
        if hasattr(d, "GetText"):
            p = d.GetPosition()
            for (txt, x, y), dy in TEXT_SHIFT.items():
                if d.GetText() == txt and abs(MM(p.x) - x) < 0.05 and abs(MM(p.y) - y) < 0.05:
                    d.SetPosition(V(x, y + dy))

    # new footprints
    for ref, (x, y, r) in NEW.items():
        p = PART[ref]; lc = LCSC.get(p["lcsc_key"], ("", "", False))
        fp = load(p["fp"])
        fp.SetReference(ref); fp.SetValue(VALUE_OVERRIDE.get(p["lcsc_key"], p["value"]))
        for k, v in (("LCSC", lc[0]), ("MPN", lc[1]), ("Description", p["desc"])):
            fp.SetField(k, v); fp.GetField(k).SetVisible(False)
        fp.SetSheetname("/"); fp.SetSheetfile("universal-weather-node.kicad_sch")
        sz = 1.0 if p["fp"].startswith("Connector_") else 0.8
        fp.Reference().SetTextSize(pcbnew.VECTOR2I(mm(sz), mm(sz))); fp.Reference().SetTextThickness(mm(sz * 0.15))
        fp.Value().SetVisible(False)
        fp.SetPosition(V(x, y)); fp.SetOrientationDegrees(r)
        b.Add(fp); FPS[ref] = fp
    for ref in ("J19", "J20"):                 # reference hidden like J1/J2 (silk "V3"/"V4" labels instead)
        FPS[ref].Reference().SetVisible(False)
    FPS["J21"].Reference().SetVisible(False)
    FPS["R65"].Reference().SetPosition(V(75.0, 65.3))
    FPS["U8"].Reference().SetPosition(V(59.6, 81.4)); FPS["U9"].Reference().SetPosition(V(63.0, 75.8)); FPS["C39"].Reference().SetPosition(V(64.6, 78.6))

    # netlist -> every pad; values / descriptions / paths from design.py
    for ref, fp in FPS.items():
        if ref not in PART: continue
        p = PART[ref]; lc = LCSC.get(p["lcsc_key"], ("", "", False))
        fp.SetValue(VALUE_OVERRIDE.get(p["lcsc_key"], p["value"]))
        for k, v in (("LCSC", lc[0]), ("MPN", lc[1]), ("Description", p["desc"])):
            if fp.HasField(k): fp.SetField(k, v)
        fp.SetPath(pcbnew.KIID_PATH("/" + uu[ref]))
        for pad in fp.Pads():
            if pad.GetNumber() == "": continue
            nn = padnet[(ref, pad.GetNumber())]
            want = p["pins"].get(pad.GetNumber())
            assert want is None or nn in ("/" + want, want), (ref, pad.GetNumber(), nn, want)
            pad.SetNet(board_net(b, nn))
    if old and not any(t.GetNetCode() == old.GetNetCode() for t in b.GetTracks()):
        b.Remove(old)

    # VE.Direct isolation: moat and GND_VICTRON pour top edge MOAT_DY lower (room for the J19 row)
    for z in b.Zones():
        if not (z.GetNetname() in ("GND", "GND_VICTRON") or z.GetZoneName() in ("MOAT_0", "MOAT_1")): continue
        ol = z.Outline()
        for i in range(ol.TotalVertices()):
            v = ol.CVertex(i)
            if MM(v.x) <= 69.6 and VIC_TOP[0] <= MM(v.y) <= VIC_TOP[1]:
                ol.SetVertex(i, pcbnew.VECTOR2I(v.x, v.y + mm(MOAT_DY)))
    moats = [z for z in b.Zones() if z.GetZoneName() in ("MOAT_0", "MOAT_1")]

    # ---- rip-up: copper at old pad positions, copper colliding with new / moved pads, copper in the moat
    dead = {}
    tracks = list(b.GetTracks())
    for pos, net, _ in oldpads:
        for t in items_at(b, pos, net, tol=mm(1.3)):
            if t.GetClass() != "PCB_VIA": chain_delete(b, t, dead)
    newpads = [p for ref in list(NEW) + list(MOVE) for p in FPS[ref].Pads()]
    for t in tracks:
        if t.m_Uuid.AsString() in dead: continue
        layers = [pcbnew.F_Cu, pcbnew.B_Cu] if t.GetClass() == "PCB_VIA" else [t.GetLayer()]
        hit = False
        for p in newpads:
            for L in layers:
                if not p.IsOnLayer(L): continue
                same = p.GetNetCode() == t.GetNetCode() and p.GetNetCode() > 0
                if p.GetEffectiveShape(L).Collide(t.GetEffectiveShape(L), 0 if same else mm(CLEAR)):
                    # same-net copper may touch its own pad, but not through a THT hole
                    if not same or (t.GetClass() == "PCB_VIA" and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH):
                        hit = True
                if same and t.GetClass() == "PCB_VIA" and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH and \
                        (t.GetPosition() - p.GetPosition()).EuclideanNorm() < mm(1.6):
                    hit = True
        for z in moats:
            for L in layers:
                if z.Outline().Collide(t.GetEffectiveShape(L), 0): hit = True
        if hit: chain_delete(b, t, dead)
    for t in dead.values(): b.Remove(t)
    # fixed routes; foreign copper in their way goes as well
    pre = []
    for net, w, lay, pts in PRE:
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(b); t.SetStart(V(x1, y1)); t.SetEnd(V(x2, y2)); t.SetWidth(mm(w))
            t.SetLayer(pcbnew.F_Cu if lay == "F" else pcbnew.B_Cu); t.SetNet(b.FindNet(net)); t.SetLocked(True)
            b.Add(t); pre.append(t)
    for net, x, y, d, drill in PRE_VIAS:
        v = pcbnew.PCB_VIA(b); v.SetPosition(V(x, y)); v.SetWidth(mm(d)); v.SetDrill(mm(drill))
        v.SetNet(b.FindNet(net)); v.SetLocked(True); b.Add(v); pre.append(v)
    dead2 = {}
    for t in list(b.GetTracks()):
        if any(t.m_Uuid.AsString() == p.m_Uuid.AsString() for p in pre): continue
        for p in pre:
            if p.GetNetCode() != t.GetNetCode() and any(t.IsOnLayer(L) and
               p.GetEffectiveShape(L).Collide(t.GetEffectiveShape(L), mm(CLEAR))
               for L in ([pcbnew.F_Cu, pcbnew.B_Cu] if p.GetClass() == "PCB_VIA" else [p.GetLayer()])):
                chain_delete(b, t, dead2); break
    for t in dead2.values(): b.Remove(t)
    print("ripped up tracks/vias:", len(dead) + len(dead2))

    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(pcb, b)
    print("prepared", pcb)


POWER_W = {"+5V", "/+5V_V4", "GND", "+5V_BUCK"}           # 0.4 mm routes
MID_W = {"3V3_HELTEC", "/3V3_LDO", "+3V3"}                 # 0.3 mm


def polys(item, layer):
    ps = pcbnew.SHAPE_POLY_SET()
    item.TransformShapeToPolygon(ps, layer, 0, mm(0.005), pcbnew.ERROR_INSIDE)
    out = []
    for i in range(ps.OutlineCount()):
        ol = ps.Outline(i)
        out.append([[round(MM(ol.CPoint(k).x), 4), round(MM(ol.CPoint(k).y), 4)] for k in range(ol.PointCount())])
    return out


def geometry(pcb, drc_json, out):
    """copper / keep-outs / holes and the open connections (DRC) for route_grid.py"""
    b = pcbnew.LoadBoard(pcb)
    bb = b.GetBoardEdgesBoundingBox()
    g = {"box": [MM(bb.GetLeft()), MM(bb.GetTop()), MM(bb.GetRight()), MM(bb.GetBottom())],
         "copper": [], "keepout": [], "holes": [], "conns": []}
    LYR = ((pcbnew.F_Cu, "F"), (pcbnew.B_Cu, "B"))
    byid = {}
    for fp in b.GetFootprints():
        for p in fp.Pads():
            byid[p.m_Uuid.AsString()] = p
            for L, n in LYR:
                if p.IsOnLayer(L):
                    for pl in polys(p, L): g["copper"].append({"layer": n, "net": p.GetNetCode() or -1, "poly": pl})
            if p.GetDrillSize().x > 0:
                g["holes"].append([MM(p.GetPosition().x), MM(p.GetPosition().y), MM(p.GetDrillSize().x) / 2])
    for t in b.GetTracks():
        byid[t.m_Uuid.AsString()] = t
        for L, n in LYR:
            if t.IsOnLayer(L):
                for pl in polys(t, L): g["copper"].append({"layer": n, "net": t.GetNetCode(), "poly": pl})
        if t.GetClass() == "PCB_VIA":
            g["holes"].append([MM(t.GetPosition().x), MM(t.GetPosition().y), MM(t.GetDrillValue()) / 2])
    for z in b.Zones():
        if not z.GetIsRuleArea(): continue
        ol = z.Outline().Outline(0)
        g["keepout"].append({"layers": [n for L, n in LYR if z.IsOnLayer(L)],
                             "tracks": z.GetDoNotAllowTracks(), "vias": z.GetDoNotAllowVias(),
                             "poly": [[MM(ol.CPoint(k).x), MM(ol.CPoint(k).y)] for k in range(ol.PointCount())]})
    for u in json.load(open(drc_json)).get("unconnected_items", []):
        it = [byid.get(i["uuid"]) for i in u["items"]]
        if None in it or len(it) != 2:
            print("skip", u["description"], [i["description"] for i in u["items"]]); continue
        ends = []
        for o in it:
            lay = [n for L, n in LYR if o.IsOnLayer(L)]
            e = {"layers": lay, "poly": polys(o, pcbnew.F_Cu if "F" in lay else pcbnew.B_Cu)[0]}
            if o.GetClass() == "PAD": e["center"] = [MM(o.GetPosition().x), MM(o.GetPosition().y)]
            ends.append(e)
        net = it[0].GetNetname()
        g["conns"].append({"net": it[0].GetNetCode(), "width": 0.4 if net in POWER_W else 0.3 if net in MID_W else 0.2,
                           "label": net + ": " + " / ".join(i["description"][:60] for i in u["items"]), "a": ends[0], "b": ends[1]})
    json.dump(g, open(out, "w"))
    print("geometry:", len(g["copper"]), "copper shapes,", len(g["conns"]), "open connections")


def apply_routes(pcb, routes):
    b = pcbnew.LoadBoard(pcb)
    r = json.load(open(routes))
    nets = {n.GetNetCode(): n for n in b.GetNetsByNetcode().values()}
    for x1, y1, x2, y2, w, l, net in r["tracks"]:
        if abs(x1 - x2) < 0.01 and abs(y1 - y2) < 0.01: continue
        t = pcbnew.PCB_TRACK(b); t.SetStart(V(x1, y1)); t.SetEnd(V(x2, y2)); t.SetWidth(mm(w))
        t.SetLayer(pcbnew.F_Cu if l == "F" else pcbnew.B_Cu); t.SetNet(nets[net]); b.Add(t)
    for x, y, net in r["vias"]:
        v = pcbnew.PCB_VIA(b); v.SetPosition(V(x, y)); v.SetWidth(mm(0.8)); v.SetDrill(mm(0.4)); v.SetNet(nets[net]); b.Add(v)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(pcb, b)
    print("applied", len(r["tracks"]), "tracks,", len(r["vias"]), "vias; failed:", r["failed"])


def prune(pcb, drc_json):
    """remove what KiCad's DRC reports as dangling (vias; track stubs, shortened to their
    last T-junction instead of deleted when another item still ends on them)"""
    b = pcbnew.LoadBoard(pcb)
    flagged = {i["uuid"]: v["type"] for v in json.load(open(drc_json))["violations"]
               if v["type"] in ("track_dangling", "via_dangling") for i in v["items"]}
    tracks = list(b.GetTracks())
    dead, short = [], 0
    for t in tracks:
        kind = flagged.get(t.m_Uuid.AsString())
        if not kind or t.IsLocked(): continue      # fixed routes and the original locked tracks stay
        if t.GetClass() == "PCB_VIA":
            # a via joining two tracks on one layer is "dangling" for KiCad but still a junction
            touching = [o for o in tracks if o.GetNetCode() == t.GetNetCode() and o.GetClass() != "PCB_VIA"
                        and (t.HitTest(o.GetStart(), 1000) or t.HitTest(o.GetEnd(), 1000))]
            if len(touching) <= 1: dead.append(t)
            continue
        same = [o for o in tracks if o.GetNetCode() == t.GetNetCode() and o.m_Uuid.AsString() != t.m_Uuid.AsString()
                and (o.GetClass() == "PCB_VIA" or o.GetLayer() == t.GetLayer())]
        ends = []
        for e in (t.GetStart(), t.GetEnd()):
            ends.append(pad_at(b, e, t.GetNetCode(), t.GetLayer()) or any(o.HitTest(e, 1000) for o in same))
        pts = [q for o in same for q in ([o.GetPosition()] if o.GetClass() == "PCB_VIA" else [o.GetStart(), o.GetEnd()])
               if t.HitTest(q, 1000)]
        s0, s1 = t.GetStart(), t.GetEnd()
        L2 = float((s1.x - s0.x) ** 2 + (s1.y - s0.y) ** 2) or 1.0
        par = lambda q: ((q.x - s0.x) * (s1.x - s0.x) + (q.y - s0.y) * (s1.y - s0.y)) / L2
        on = lambda q: pcbnew.VECTOR2I(int(s0.x + (s1.x - s0.x) * par(q)), int(s0.y + (s1.y - s0.y) * par(q)))  # keep the line
        k0 = s0 if ends[0] else (on(min(pts, key=par)) if pts else None)
        k1 = s1 if ends[1] else (on(max(pts, key=par)) if pts else None)
        if k0 is None or k1 is None or (k0 - k1).EuclideanNorm() < 1000:
            dead.append(t)
        else:
            t.SetStart(k0); t.SetEnd(k1); short += 1
    for t in dead: b.Delete(t)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(pcb, b)
    print("dangling: removed", len(dead), "shortened", short)
    return len(dead) + short


def ripviol(pcb, drc_json):
    """delete the routed tracks / signal vias involved in clearance violations (re-routed next)"""
    b = pcbnew.LoadBoard(pcb)
    ids = set()
    for v in json.load(open(drc_json))["violations"]:
        if v["type"] != "clearance": continue
        for i in v["items"]:
            if i["description"].startswith(("Track", "Leiterbahn")) or \
               (i["description"].startswith("Via") and "[GND]" not in i["description"]):
                ids.add(i["uuid"])
    dead = {}
    for t in list(b.GetTracks()):
        if t.m_Uuid.AsString() in ids and t.m_Uuid.AsString() not in dead:
            if t.IsLocked(): continue
            chain_delete(b, t, dead)
    for t in dead.values(): b.Delete(t)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(pcb, b)
    print("clearance rip-up:", len(dead))
    return len(dead)


def text(b, s, x, y, size=0.8, angle=0, layer=pcbnew.F_SilkS, just=None):
    t = pcbnew.PCB_TEXT(b); t.SetText(s); t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size))); t.SetTextThickness(mm(max(0.12, size * 0.15)))
    if just == "left": t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
    if just == "right": t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_RIGHT)
    t.SetTextAngleDegrees(angle); t.SetPosition(V(x, y)); b.Add(t); return t


def silkscreen(b):
    RENAME = {"Heltec WiFi LoRa 32 V3.2": "Heltec WiFi LoRa 32 V3 or V4",
              "(pin-compatible V3 / V3.1)": "POPULATE ONE SOCKET SET ONLY",
              "IO19": "VANE"}
    MOVE_T = {"Heltec WiFi LoRa 32 V3 or V4": (82.0, 75.0, 1.1), "POPULATE ONE SOCKET SET ONLY": (82.0, 77.0, 0.9),
              "ANT": (95.6, 70.9, None), "V3: J1+J2  |  V4: J19+J20+J21 (P4)": (82.0, 78.8, None)}
    for d in list(b.GetDrawings()):
        if not (hasattr(d, "GetText") and d.GetLayer() == pcbnew.F_SilkS): continue
        if d.GetText() in RENAME: d.SetText(RENAME[d.GetText()])
        p = d.GetPosition()
        # the brand block under the module (duplicate of the one at the top edge) makes room for the notes
        if d.GetText() in ("Universal Weather Node v2.0", "franzenhof.com") and MM(p.y) > 70: b.Delete(d); continue
        if d.GetText() in MOVE_T:
            x, y, sz = MOVE_T[d.GetText()]
            d.SetPosition(V(x, y))
            if sz: d.SetTextSize(pcbnew.VECTOR2I(mm(sz), mm(sz))); d.SetTextThickness(mm(max(0.15, sz * 0.15)))
    FPS = {f.GetReference(): f for f in b.GetFootprints()}
    for ref in ("J19", "J20"):          # outlines would overlap the V3 rows 2.54 mm away: keep them on F.Fab
        for g in FPS[ref].GraphicalItems():
            if g.GetLayer() == pcbnew.F_SilkS: g.SetLayer(pcbnew.F_Fab)
    FPS["U7"].Reference().SetPosition(V(65.6, 65.6)); FPS["U8"].Reference().SetPosition(V(55.4, 78.6))
    FPS["C35"].Reference().SetPosition(V(61.9, 72.4))
    if any(hasattr(d, "GetText") and d.GetText() == "V4 P4" for d in b.GetDrawings()):
        return                                  # labels already added (idempotent)
    # socket set labels beyond pin 18 and pin-1 marks before pin 1
    for row, lab in ((ROW_J2, "V3"), (ROW_J20, "V4"), (ROW_J1, "V3"), (ROW_J19, "V4")):
        text(b, lab, 99.0, row, 0.8, just="left")
        text(b, "1", 52.5, row, 0.8)
    text(b, "V4 P4", 101.7, P4_Y + PITCH / 2, 0.8, angle=90)
    text(b, "V3: J1+J2  |  V4: J19+J20+J21 (P4)", 82.0, 78.8, 0.8)


if __name__ == "__main__":
    if sys.argv[1] == "prepare": prepare(*sys.argv[2:5])
    elif sys.argv[1] == "geometry": geometry(*sys.argv[2:5])
    elif sys.argv[1] == "apply": apply_routes(*sys.argv[2:4])
    elif sys.argv[1] == "ripviol": sys.exit(0 if ripviol(*sys.argv[2:4]) else 3)
    elif sys.argv[1] == "prune": sys.exit(0 if prune(*sys.argv[2:4]) else 3)
    elif sys.argv[1] == "silk":
        bb = pcbnew.LoadBoard(sys.argv[2]); silkscreen(bb); pcbnew.ZONE_FILLER(bb).Fill(bb.Zones()); pcbnew.SaveBoard(sys.argv[2], bb)
    else: raise SystemExit(__doc__)
