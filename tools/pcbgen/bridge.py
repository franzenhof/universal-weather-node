"""Connect isolated GND islands: shortest straight / L-shaped track (F.Cu or B.Cu) from a GND pad or via
of the island to a GND via / pad of the main ground, checked against all foreign copper."""
import sys, math, pcbnew
f = sys.argv[1]; net = sys.argv[2] if len(sys.argv) > 2 else "GND"
b = pcbnew.LoadBoard(f); mm = pcbnew.FromMM
def groups():
    isl = {}
    for z in b.Zones():
        if z.GetIsRuleArea() or z.GetNetname() != net: continue
        lay = z.GetLayer(); ps = z.GetFilledPolysList(lay)
        for i in range(ps.OutlineCount()):
            s = pcbnew.SHAPE_POLY_SET(); s.AddOutline(ps.Outline(i)); isl[(lay, i)] = s
    par = {k: k for k in isl}
    def fd(k):
        while par[k] != k: k = par[k]
        return k
    items = [(t.GetPosition(), None) for t in b.GetTracks() if t.GetClass() == "PCB_VIA" and t.GetNetname() == net]
    items += [(p.GetPosition(), None if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH else p.GetLayer()) for fp in b.GetFootprints() for p in fp.Pads() if p.GetNetname() == net]
    for pos, only in items:
        h = [k for k, s in isl.items() if (only is None or k[0] == only) and s.Contains(pos)]
        for k in h[1:]: par[fd(k)] = fd(h[0])
    for t in b.GetTracks():
        if t.GetClass() == "PCB_TRACK" and t.GetNetname() == net:
            h = [k for k, s in isl.items() if k[0] == t.GetLayer() and (s.Collide(t.GetStart(), mm(0.05)) or s.Collide(t.GetEnd(), mm(0.05)))]
            for k in h[1:]: par[fd(k)] = fd(h[0])
    g = {}
    for k in isl: g.setdefault(fd(k), []).append(k)
    return isl, sorted(g.values(), key=lambda ks: -sum(isl[k].Area() for k in ks))
W, C = mm(0.3), mm(0.25)
def free(a, c, lay):
    seg = pcbnew.SHAPE_SEGMENT(a, c, W)
    for t in b.GetTracks():
        if t.GetNetname() == net: continue
        if (t.GetClass() == "PCB_VIA" or t.GetLayer() == lay) and t.GetEffectiveShape().Collide(seg, C): return False
    for fp in b.GetFootprints():
        for p in fp.Pads():
            if p.GetNetname() == net or not p.IsOnLayer(lay): continue
            if p.GetEffectiveShape(lay).Collide(seg, C): return False
    for z in b.Zones():
        if z.GetIsRuleArea() and z.GetDoNotAllowTracks() and z.IsOnLayer(lay) and z.Outline().Collide(seg): return False
    return True
for rnd in range(4):
    isl, gs = groups()
    if len(gs) < 2: print("ground is one piece"); break
    main = gs[0]
    def inmain(p, lay=None): return any(isl[k].Contains(p) for k in main if lay is None or k[0] == lay)
    tg = [t.GetPosition() for t in b.GetTracks() if t.GetClass() == "PCB_VIA" and t.GetNetname() == net and inmain(t.GetPosition())]
    tg += [p.GetPosition() for fp in b.GetFootprints() for p in fp.Pads() if p.GetNetname() == net and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH and inmain(p.GetPosition())]
    fixed = False
    for grp in gs[1:]:
        starts = []
        for t in b.GetTracks():
            if t.GetClass() == "PCB_VIA" and t.GetNetname() == net and any(isl[k].Contains(t.GetPosition()) for k in grp): starts.append((t.GetPosition(), (pcbnew.F_Cu, pcbnew.B_Cu)))
        for fp in b.GetFootprints():
            for p in fp.Pads():
                if p.GetNetname() == net and any(isl[k].Contains(p.GetPosition()) for k in grp):
                    starts.append((p.GetPosition(), (pcbnew.F_Cu, pcbnew.B_Cu) if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH else (p.GetLayer(),)))
        best = None
        # targets: GND vias / PTH pads of the main ground, plus points of the main pour itself (layer-specific)
        for s, lays in starts:
            for lay in lays:
                for dx in range(-24, 25):
                    for dy in range(-24, 25):
                        pt = pcbnew.VECTOR2I(s.x + mm(dx * 0.4), s.y + mm(dy * 0.4))
                        if not any(isl[k].Contains(pt) for k in main if k[0] == lay): continue
                        ring = [pcbnew.VECTOR2I(int(pt.x + mm(0.35) * math.cos(a)), int(pt.y + mm(0.35) * math.sin(a))) for a in [i * math.pi / 4 for i in range(8)]]
                        if not all(any(isl[k].Contains(r) for k in main if k[0] == lay) for r in ring): continue
                        for path in ([s, pt], [s, pcbnew.VECTOR2I(pt.x, s.y), pt], [s, pcbnew.VECTOR2I(s.x, pt.y), pt]):
                            L = sum(math.dist((path[i].x, path[i].y), (path[i + 1].x, path[i + 1].y)) for i in range(len(path) - 1))
                            if best and L >= best[0]: continue
                            if all(free(path[i], path[i + 1], lay) for i in range(len(path) - 1) if path[i] != path[i + 1]): best = (L, path, lay)
        for s, lays in starts:
            for t in tg:
                if math.dist((s.x, s.y), (t.x, t.y)) > mm(14): continue
                for path in ([s, t], [s, pcbnew.VECTOR2I(t.x, s.y), t], [s, pcbnew.VECTOR2I(s.x, t.y), t]):
                    L = sum(math.dist((path[i].x, path[i].y), (path[i + 1].x, path[i + 1].y)) for i in range(len(path) - 1))
                    for lay in lays:
                        if best and L >= best[0]: continue
                        if all(free(path[i], path[i + 1], lay) for i in range(len(path) - 1)): best = (L, path, lay)
        if best:
            L, path, lay = best
            for i in range(len(path) - 1):
                if path[i] == path[i + 1]: continue
                tr = pcbnew.PCB_TRACK(b); tr.SetStart(path[i]); tr.SetEnd(path[i + 1]); tr.SetWidth(W); tr.SetLayer(lay); tr.SetNet(b.FindNet(net)); b.Add(tr)
            print(f"bridged island ({len(grp)} parts) with {L / 1e6:.1f} mm on {pcbnew.LayerName(lay)}"); fixed = True
        else: print("no bridge for island", [(pcbnew.LayerName(k[0])) for k in grp])
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    if not fixed: break
b.Save(f)
