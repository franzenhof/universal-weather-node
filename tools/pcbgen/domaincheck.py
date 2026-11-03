"""Check that no copper crosses between the main and the isolated domains."""
import sys, os, importlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew
L = importlib.import_module(os.environ.get("LAYOUT", "layout_v2"))
b = pcbnew.LoadBoard(sys.argv[1])
OX, OY = 50, 50
def inside(poly, x, y):
    c = False; n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1: c = not c
    return c
def near(poly, x, y, m):
    return any(inside(poly, x + dx, y + dy) for dx in (-m, 0, m) for dy in (-m, 0, m))
pads = {}
for fp in b.GetFootprints():
    for p in fp.Pads(): pads.setdefault(p.GetNetname(), set())
dom_of = {}
for dn, poly in L.DOMAINS.items():
    # all nets that have a pad inside this domain polygon belong to it
    for fp in b.GetFootprints():
        for p in fp.Pads():
            x, y = pcbnew.ToMM(p.GetPosition().x) - OX, pcbnew.ToMM(p.GetPosition().y) - OY
            if p.GetNetname() and inside(poly, x, y): dom_of[p.GetNetname()] = dn
foreign, escaped = [], []
for t in b.GetTracks():
    n = t.GetNetname()
    p0 = t.GetStart() if t.GetClass() == "PCB_TRACK" else t.GetPosition()
    p1 = t.GetEnd() if t.GetClass() == "PCB_TRACK" else p0
    for k in range(21):
        x = (pcbnew.ToMM(p0.x) * (20 - k) + pcbnew.ToMM(p1.x) * k) / 20 - OX
        y = (pcbnew.ToMM(p0.y) * (20 - k) + pcbnew.ToMM(p1.y) * k) / 20 - OY
        if n in dom_of:
            if not near(L.DOMAINS[dom_of[n]], x, y, 1.2): escaped.append((n, round(x, 1), round(y, 1))); break
        elif any(inside(poly, x, y) for poly in L.DOMAINS.values()): foreign.append((n, round(x, 1), round(y, 1))); break
print("isolated nets:", {d: sorted(n for n, dd in dom_of.items() if dd == d) for d in L.DOMAINS})
print("main-domain copper inside isolated areas:", len(foreign), foreign[:5])
print("isolated-domain copper outside its area:", len(escaped), escaped[:5])
