"""Import the Freerouting session, add GND pours on both layers, fill, save."""
import sys, pcbnew
pcb, ses = sys.argv[1:3]
b = pcbnew.LoadBoard(pcb)
if ses not in ("-", "fix"):          # "fix": only run the ground clean-up passes on a routed board
    assert pcbnew.ImportSpecctraSES(b, ses)
mm = pcbnew.FromMM
for t in b.GetTracks():   # Freerouting necks down at pads; keep >= 0.15 mm
    if t.GetClass() == "PCB_TRACK" and t.GetWidth() < mm(0.15): t.SetWidth(mm(0.15))
bb = [s for s in b.GetDrawings() if s.GetLayer() == pcbnew.Edge_Cuts][0].GetBoundingBox()
OX, OY = pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop())
W, H = pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())
import os, importlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
LY = importlib.import_module(os.environ.get("LAYOUT", "layout_v2"))
DOMAINS = LY.DOMAINS
MOAT = 1.0
def NN(name):
    """net name as used on this board: '/X' (local label) or 'X' (global power net)"""
    if b.FindNet(name): return name
    alt = name[1:] if name.startswith("/") else "/" + name
    return alt if b.FindNet(alt) else name
GNDS = {NN(n) for n in ("/GND", "/GND_VICTRON", "/GND_DALY")}
def zone(net, layer, pts, holes=(), prio=0):
    net = NN(net)
    z = pcbnew.ZONE(b); z.SetLayer(layer); z.SetNet(b.FindNet(net)); z.SetZoneName(net.strip("/"))
    z.SetLocalClearance(mm(0.3)); z.SetMinThickness(mm(0.25))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THT_THERMAL)   # SMD solid, THT thermal relief
    z.SetThermalReliefGap(mm(0.3)); z.SetThermalReliefSpokeWidth(mm(0.5))
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS); z.SetAssignedPriority(prio)
    ol = z.Outline(); ol.NewOutline()
    for x, y in pts: ol.Append(mm(OX + x), mm(OY + y))
    for hp in holes:
        hi = ol.NewHole()
        for x, y in hp: ol.Append(mm(OX + x), mm(OY + y), 0, hi)
    b.Add(z)
GND_OUTLINE = LY.GND_OUTLINE
# PLANE_B=1: before routing only B.Cu carries the ground pour, so the router has to give every
# F.Cu ground pad its own connection (via) instead of relying on an F.Cu pour that may end up
# cut into islands; the F.Cu pour is added after the session import
PLANE_B = os.environ.get("PLANE_B") == "1"
zl = {z.GetLayer() for z in b.Zones() if not z.GetIsRuleArea()}
NOPLANE = os.environ.get("NOPLANE") == "1"          # ground routed as a normal net, pours only afterwards
want = () if (NOPLANE and ses == "-") else (pcbnew.B_Cu,) if (PLANE_B and ses == "-") else (pcbnew.F_Cu, pcbnew.B_Cu)
for layer in [l for l in want if l not in zl]:
    zone("/GND", layer, GND_OUTLINE)
    for net, pts in DOMAINS.items():
        zone(net, layer, pts, prio=1)
# pads whose thermal spokes would only reach an isolated F.Cu island -> solid connection
for ref in getattr(LY, "SOLID_GND", ("J1", "J2", "J11", "J12", "J13", "J16", "TP1", "TP7")):   # sockets / headers: solid ground pins
    f_ = b.FindFootprintByReference(ref)
    if f_:
        for pad in f_.Pads():
            if pad.GetNetname() in GNDS: pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())

# stitching vias: wherever both copper layers carry the same ground pour with room
# for a via, drop one on a 4 mm grid -> joins F.Cu islands to the B.Cu plane
if ses != "-":
    import math
    STEP, R = 4.0, 0.75            # grid, required free radius (via 0.8 mm + margin)
    def holes():
        h = [t.GetPosition() for t in b.GetTracks() if t.GetClass() == "PCB_VIA"]
        for fp in b.GetFootprints():
            for p in fp.Pads():
                if p.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH): h.append(p.GetPosition())
        return h
    def far(x, y, hl, dmin=1.2):
        return all((x - q.x) ** 2 + (y - q.y) ** 2 >= mm(dmin) ** 2 for q in hl)
    HL = holes()
    gz = {}
    for z in b.Zones():
        if not z.GetIsRuleArea() and z.GetNetname() in GNDS:
            gz.setdefault(z.GetNetname(), {})[z.GetLayer()] = z
    added = 0
    for net, layers in gz.items():
        if pcbnew.F_Cu not in layers or pcbnew.B_Cu not in layers: continue
        fpoly = layers[pcbnew.F_Cu].GetFilledPolysList(pcbnew.F_Cu)
        bpoly = layers[pcbnew.B_Cu].GetFilledPolysList(pcbnew.B_Cu)
        y = OY + 1.0
        while y < OY + H - 1.0:
            x = OX + 1.0
            while x < OX + W - 1.0:
                pts = [pcbnew.VECTOR2I(mm(x + R * math.cos(a)), mm(y + R * math.sin(a))) for a in [k * math.pi / 4 for k in range(8)]]
                pts.append(pcbnew.VECTOR2I(mm(x), mm(y)))
                if far(mm(x), mm(y), HL) and all(fpoly.Contains(p) and bpoly.Contains(p) for p in pts):
                    v = pcbnew.PCB_VIA(b); v.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
                    v.SetWidth(mm(0.8)); v.SetDrill(mm(0.4)); v.SetNet(b.FindNet(net)); b.Add(v); added += 1; HL.append(v.GetPosition())
                x += STEP
            y += STEP
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    # targeted pass: every filled island without a via / THT pad of its net gets one
    def anchors(net):
        pts = [t.GetPosition() for t in b.GetTracks() if t.GetClass() == "PCB_VIA" and t.GetNetname() == net]
        for fp in b.GetFootprints():
            for p in fp.Pads():
                if p.GetNetname() == net and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH: pts.append(p.GetPosition())
        return pts
    fixed = 0
    for rnd in range(3):
        for net, layers in gz.items():
            if pcbnew.F_Cu not in layers or pcbnew.B_Cu not in layers: continue
            for lay, other in ((pcbnew.F_Cu, pcbnew.B_Cu), (pcbnew.B_Cu, pcbnew.F_Cu)):
                polys = layers[lay].GetFilledPolysList(lay); opoly = layers[other].GetFilledPolysList(other)
                anc = anchors(net)
                for i in range(polys.OutlineCount()):
                    isl = pcbnew.SHAPE_POLY_SET(); isl.AddOutline(polys.Outline(i)); HL = holes()
                    if any(isl.Contains(a) for a in anc): continue
                    bb = isl.BBox(); done = False
                    yy = bb.GetTop()
                    while yy < bb.GetBottom() and not done:
                        xx = bb.GetLeft()
                        while xx < bb.GetRight():
                            RI = 0.5                # small via 0.6/0.3 for narrow islands
                            c = [pcbnew.VECTOR2I(int(xx + mm(RI) * math.cos(a)), int(yy + mm(RI) * math.sin(a))) for a in [k * math.pi / 4 for k in range(8)]]
                            c.append(pcbnew.VECTOR2I(int(xx), int(yy)))
                            if far(xx, yy, HL, 0.9) and all(polys.Contains(p) and opoly.Contains(p) for p in c):
                                v = pcbnew.PCB_VIA(b); v.SetPosition(pcbnew.VECTOR2I(int(xx), int(yy)))
                                v.SetWidth(mm(0.6)); v.SetDrill(mm(0.3)); v.SetNet(b.FindNet(net)); b.Add(v)
                                fixed += 1; done = True; break
                            xx += mm(0.25)
                        yy += mm(0.25)
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    # last resort: SMD ground pads on islands that cannot hold a via get a short
    # stub track to a nearby via (collision-checked against all foreign copper)
    CLR = mm(0.3)
    def free(shape, layers, net):
        for t in b.GetTracks():
            if t.GetNetname() == net: continue
            if t.GetClass() == "PCB_VIA" or t.GetLayer() in layers:
                if t.GetEffectiveShape().Collide(shape, CLR): return False
        for fp in b.GetFootprints():
            for p in fp.Pads():
                if p.GetNetname() == net and p.GetNetname(): continue
                for lay in layers:
                    if p.IsOnLayer(lay) and p.GetEffectiveShape(lay).Collide(shape, CLR): return False
        return True
    stubs = 0
    for rnd in range(3):
        for net, layers in gz.items():
            for lay in (pcbnew.F_Cu, pcbnew.B_Cu):
                if lay not in layers: continue
                polys = layers[lay].GetFilledPolysList(lay); anc = anchors(net)
                trk = [t for t in b.GetTracks() if t.GetClass() == "PCB_TRACK" and t.GetNetname() == net]
                for i in range(polys.OutlineCount()):
                    isl = pcbnew.SHAPE_POLY_SET(); isl.AddOutline(polys.Outline(i))
                    if any(isl.Contains(a) for a in anc): continue
                    pads = [p for fp in b.GetFootprints() for p in fp.Pads()
                            if p.GetNetname() == net and p.IsOnLayer(lay) and isl.Collide(p.GetPosition(), mm(0.2))]
                    done = False
                    for p in pads:
                        c0 = p.GetPosition()
                        for rr in [0.9 + 0.25 * k for k in range(17)]:   # up to 4.9 mm
                            for k in range(24):
                                a = 2 * math.pi * k / 24
                                vc = pcbnew.VECTOR2I(int(c0.x + mm(rr) * math.cos(a)), int(c0.y + mm(rr) * math.sin(a)))
                                if not (OX + 0.8 < pcbnew.ToMM(vc.x) < OX + W - 0.8 and OY + 0.8 < pcbnew.ToMM(vc.y) < OY + H - 0.8): continue
                                via_s = pcbnew.SHAPE_CIRCLE(vc, mm(0.3))
                                seg_s = pcbnew.SHAPE_SEGMENT(c0, vc, mm(0.3))
                                if free(via_s, [pcbnew.F_Cu, pcbnew.B_Cu], net) and free(seg_s, [lay], net):
                                    t = pcbnew.PCB_TRACK(b); t.SetStart(c0); t.SetEnd(vc); t.SetWidth(mm(0.3)); t.SetLayer(lay); t.SetNet(b.FindNet(net)); b.Add(t)
                                    v = pcbnew.PCB_VIA(b); v.SetPosition(vc); v.SetWidth(mm(0.6)); v.SetDrill(mm(0.3)); v.SetNet(b.FindNet(net)); b.Add(v)
                                    stubs += 1; done = True; break
                            if done: break
                        if done: break
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    # bridge pass: build a graph of fill islands (F/B) joined by vias / PTH pads; if a
    # net's pour falls apart into several groups, drop a via where an F island of one
    # group overlaps a B island of another group
    bridges = 0
    for rnd in range(4):
        changed = False
        for net, layers in gz.items():
            if pcbnew.F_Cu not in layers or pcbnew.B_Cu not in layers: continue
            isl = {}
            for lay in (pcbnew.F_Cu, pcbnew.B_Cu):
                ps = layers[lay].GetFilledPolysList(lay)
                for i in range(ps.OutlineCount()):
                    s_ = pcbnew.SHAPE_POLY_SET(); s_.AddOutline(ps.Outline(i)); isl[(lay, i)] = s_
            par = {k: k for k in isl}
            def fnd(k):
                while par[k] != k: par[k] = par[par[k]]; k = par[k]
                return k
            for a in holes():
                hit_ = [k for k, s_ in isl.items() if s_.Contains(a)]
                for k in hit_[1:]: par[fnd(k)] = fnd(hit_[0])
            groups = {}
            for k in isl: groups.setdefault(fnd(k), []).append(k)
            if len(groups) <= 1: continue
            # biggest group = main plane; connect every other group to it
            main = max(groups.values(), key=lambda g: sum(isl[k].Area() for k in g))
            for g in groups.values():
                if g is main: continue
                done = False
                for ka in g:
                    for kb in main:
                        if ka[0] == kb[0]: continue
                        ov = pcbnew.SHAPE_POLY_SET(isl[ka]); ov.BooleanIntersection(isl[kb])
                        if ov.OutlineCount() == 0: continue
                        bb = ov.BBox(); yy = bb.GetTop()
                        HL = holes()
                        while yy < bb.GetBottom() and not done:
                            xx = bb.GetLeft()
                            while xx < bb.GetRight():
                                c = [pcbnew.VECTOR2I(int(xx + mm(0.5) * math.cos(a)), int(yy + mm(0.5) * math.sin(a))) for a in [k * math.pi / 4 for k in range(8)]]
                                c.append(pcbnew.VECTOR2I(int(xx), int(yy)))
                                if far(xx, yy, HL, 0.9) and all(ov.Contains(p) for p in c):
                                    v = pcbnew.PCB_VIA(b); v.SetPosition(pcbnew.VECTOR2I(int(xx), int(yy)))
                                    v.SetWidth(mm(0.6)); v.SetDrill(mm(0.3)); v.SetNet(b.FindNet(net)); b.Add(v)
                                    bridges += 1; done = changed = True; break
                                xx += mm(0.25)
                            yy += mm(0.25)
                        if done: break
                    if done: break
        if not changed: break
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    # last resort: a pour group that is isolated on both layers gets a straight track
    # from one of its pads/vias to the nearest pad/via of the main group
    tbr = 0
    for net, layers in gz.items():
        for rnd in range(3):
            isl = {}
            for lay in layers:
                ps = layers[lay].GetFilledPolysList(lay)
                for i in range(ps.OutlineCount()):
                    s_ = pcbnew.SHAPE_POLY_SET(); s_.AddOutline(ps.Outline(i)); isl[(lay, i)] = s_
            par = {k: k for k in isl}
            def fnd(k):
                while par[k] != k: par[k] = par[par[k]]; k = par[k]
                return k
            pts = []
            for t in b.GetTracks():
                if t.GetNetname() == net and t.GetClass() == "PCB_VIA": pts.append((t.GetPosition(), None))
            for fp_ in b.GetFootprints():
                for p_ in fp_.Pads():
                    if p_.GetNetname() == net:
                        pts.append((p_.GetPosition(), None if p_.GetAttribute() == pcbnew.PAD_ATTRIB_PTH else p_.GetLayer()))
            memb = []
            for pos, only in pts:
                hit_ = [k for k, s_ in isl.items() if (only is None or k[0] == only) and s_.Contains(pos)]
                for k in hit_[1:]: par[fnd(k)] = fnd(hit_[0])
                memb.append((pos, fnd(hit_[0]) if hit_ else None))
            groups = {}
            for k in isl: groups.setdefault(fnd(k), []).append(k)
            if len(groups) <= 1: break
            main = fnd(max(groups.values(), key=lambda g: sum(isl[k].Area() for k in g))[0])
            src = [pos for pos, g in memb if g is not None and fnd(g) != main]
            dst = [pos for pos, g in memb if g is not None and fnd(g) == main]
            pairs = sorted(((math.hypot(a.x - c.x, a.y - c.y), a, c) for a in src for c in dst), key=lambda t: t[0])[:60]
            done = False
            for dist, a, c in pairs:
                for lay in (pcbnew.B_Cu, pcbnew.F_Cu):
                    # straight, then the two L-shapes (corner at (a.x, c.y) / (c.x, a.y))
                    for path in ([a, c], [a, pcbnew.VECTOR2I(a.x, c.y), c], [a, pcbnew.VECTOR2I(c.x, a.y), c]):
                        segs = [pcbnew.SHAPE_SEGMENT(path[i], path[i + 1], mm(0.4)) for i in range(len(path) - 1)]
                        if all(free(sg, [lay], net) for sg in segs):
                            for i in range(len(path) - 1):
                                t = pcbnew.PCB_TRACK(b); t.SetStart(path[i]); t.SetEnd(path[i + 1]); t.SetWidth(mm(0.4))
                                t.SetLayer(lay); t.SetNet(b.FindNet(net)); b.Add(t)
                            tbr += 1; done = True; break
                    if done: break
                if done: break
            pcbnew.ZONE_FILLER(b).Fill(b.Zones())
            if not done: break
    print("stitching vias", added, "+ island vias", fixed, "+ pad stubs", stubs, "+ bridges", bridges, "+ bridge tracks", tbr)
pcbnew.SaveBoard(pcb, b)
tracks = [t for t in b.GetTracks()]
print("tracks", sum(1 for t in tracks if t.GetClass() == "PCB_TRACK"), "vias", sum(1 for t in tracks if t.GetClass() == "PCB_VIA"))
