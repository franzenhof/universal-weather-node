"""Second pass: references that still touch silk / pads get the nearest free spot (grid search ±5 mm)."""
import sys, json, pcbnew
f = sys.argv[1]; refs = sys.argv[2].split(",")
notes = sys.argv[3].split(",") if len(sys.argv) > 3 and sys.argv[3] else []   # free texts "TEXT@REF"
b = pcbnew.LoadBoard(f); mm = pcbnew.FromMM
def bb(o):
    x = o.GetBoundingBox(); return (x.GetLeft(), x.GetTop(), x.GetRight(), x.GetBottom())
def hit(a, c, m=int(mm(0.15))): return not (a[2] + m <= c[0] or c[2] + m <= a[0] or a[3] + m <= c[1] or c[3] + m <= a[1])
obs = {}
for fp in b.GetFootprints():
    for q in fp.Pads(): obs.setdefault(fp.GetReference(), []).append(bb(q))
    for g in fp.GraphicalItems():
        if g.GetLayer() == pcbnew.F_SilkS: obs.setdefault(fp.GetReference(), []).append(bb(g))
    if fp.Reference().IsVisible(): obs.setdefault("ref:" + fp.GetReference(), []).append(bb(fp.Reference()))
for d in b.GetDrawings():
    if d.GetLayer() == pcbnew.F_SilkS: obs.setdefault("draw", []).append(bb(d))
edge = [s for s in b.GetDrawings() if s.GetLayer() == pcbnew.Edge_Cuts][0].GetBoundingBox()
# label strips above the bottom terminals stay reserved for the terminal titles
obs.setdefault("strip", []).append((edge.GetLeft(), edge.GetBottom() - mm(12.5), edge.GetRight(), edge.GetBottom()))
jobs = [(r_, None) for r_ in refs if r_] + [(n.split("@")[1], n.split("@")[0]) for n in notes]
for ref, txt in jobs:
    fp = b.FindFootprintByReference(ref)
    if txt is None: r = fp.Reference()
    else:
        r = [d for d in b.GetDrawings() if hasattr(d, "GetText") and d.GetText() == txt and d.GetLayer() == pcbnew.F_SilkS][0]
        obs["draw"] = [o for o in obs["draw"] if o != bb(r)]
    p0 = fp.GetPosition() if txt else r.GetPosition()
    others = [o for k, v in obs.items() if k != "ref:" + ref for o in v]
    best = None
    for ang in ((0, 90) if txt is None else (0,)):
        r.SetTextAngleDegrees(ang)
        for dx in [i * 0.25 for i in range(-20, 21)]:
            for dy in [i * 0.25 for i in range(-20, 21)]:
                r.SetPosition(pcbnew.VECTOR2I(p0.x + mm(dx), p0.y + mm(dy))); t = bb(r)
                if t[0] < edge.GetLeft() + mm(0.6) or t[2] > edge.GetRight() - mm(0.6) or t[1] < edge.GetTop() + mm(0.6) or t[3] > edge.GetBottom() - mm(0.6): continue
                if any(hit(t, o) for o in others): continue
                c = fp.GetPosition(); d = ((t[0] + t[2]) / 2 - c.x) ** 2 + ((t[1] + t[3]) / 2 - c.y) ** 2
                if best is None or d < best[0]: best = (d, ang, r.GetPosition())
    if best:
        r.SetTextAngleDegrees(best[1]); r.SetPosition(best[2])
        if txt is None: obs.setdefault("ref:" + ref, []).clear(); obs["ref:" + ref].append(bb(r))
        else: obs["draw"].append(bb(r))
        print(ref, "moved", round(best[0] ** 0.5 / 1e6, 1), "mm from centre")
    else:
        r.SetPosition(p0); print(ref, "no free spot")
b.Save(f)
