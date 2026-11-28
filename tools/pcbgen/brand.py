"""Place the brand/version block on a free spot of the top silkscreen (outside the Heltec module)."""
import sys, os, importlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew
L = importlib.import_module(os.environ.get("LAYOUT", "layout_v5"))
b = pcbnew.LoadBoard(sys.argv[1]); mm = pcbnew.FromMM
OX, OY = 50.0, 50.0
# test points: the function label is enough, hide the reference designator
for fp in b.GetFootprints():
    if fp.GetReference().startswith("TP"): fp.Reference().SetVisible(False)
for r_ in getattr(L, "HIDE_REFS", ()):
    if b.FindFootprintByReference(r_): b.FindFootprintByReference(r_).Reference().SetVisible(False)
# terminal functions: shift the reference to the left end of the terminal body and write the function after it
for ref, func in getattr(L, "TERM_FUNC", {}).items():
    fp = b.FindFootprintByReference(ref)
    if fp is None or any(hasattr(d, "GetText") and d.GetText() == func and d.GetLayer() == pcbnew.F_SilkS
                         and abs(d.GetPosition().y - fp.Reference().GetPosition().y) < mm(0.3) for d in b.GetDrawings()): continue
    r = fp.Reference(); pads = [q.GetPosition().x for q in fp.Pads() if q.GetNumber()]
    sz = pcbnew.ToMM(r.GetTextSize().y); rp = r.GetPosition()
    total = (len(ref) + 1 + len(func)) * sz * 0.85
    x0 = (min(pads) + max(pads)) / 2 - mm(total / 2)
    r.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT); r.SetPosition(pcbnew.VECTOR2I(int(x0), rp.y))
    t = pcbnew.PCB_TEXT(b); t.SetText(func); t.SetLayer(pcbnew.F_SilkS); t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
    t.SetTextSize(r.GetTextSize()); t.SetTextThickness(r.GetTextThickness())
    t.SetPosition(pcbnew.VECTOR2I(int(x0 + mm((len(ref) + 1) * sz * 0.85)), rp.y)); b.Add(t)

# reference designators: put each one on the first side of its part (above, below, left,
# right) where it touches no pad, no other text and no other part body; terminal titles,
# Heltec sockets and test points keep their place
def _bb(o):
    bb = o.GetBoundingBox(); return (bb.GetLeft(), bb.GetTop(), bb.GetRight(), bb.GetBottom())
def _hit(a, c, m=int(mm(0.1))): return not (a[2] + m <= c[0] or c[2] + m <= a[0] or a[3] + m <= c[1] or c[3] + m <= a[1])
KEEP = set(getattr(L, "TERM_FUNC", {})) | set(getattr(L, "TOP_PIN_LABELS", {})) | {"J1", "J2"}
_pads = [_bb(q) for f_ in b.GetFootprints() for q in f_.Pads()]
_crt = {}
for f_ in b.GetFootprints():
    c = f_.GetCourtyard(pcbnew.F_CrtYd)
    if c.OutlineCount():
        bb = c.BBox(); _crt[f_.GetReference()] = (bb.GetLeft(), bb.GetTop(), bb.GetRight(), bb.GetBottom())
_texts = [_bb(d) for d in b.GetDrawings() if d.GetLayer() == pcbnew.F_SilkS and hasattr(d, "GetText")]
_texts += [_bb(f_.Reference()) for f_ in b.GetFootprints() if f_.GetReference() in KEEP and f_.Reference().IsVisible()]
_edge = (mm(OX + 0.6), mm(OY + 0.6), mm(OX + L.W - 0.6), mm(OY + L.H - 0.6))
moved = 0
for f_ in sorted(b.GetFootprints(), key=lambda f: f.GetReference()):
    ref = f_.GetReference(); r = f_.Reference()
    if ref in KEEP or ref.startswith(("TP", "H")) or not r.IsVisible() or ref not in _crt: continue
    cb = _crt[ref]; cx, cy = (cb[0] + cb[2]) // 2, (cb[1] + cb[3]) // 2
    sz = r.GetTextSize().y; g = mm(0.25)
    cands = [(0, cx, cb[1] - g - sz // 2), (0, cx, cb[3] + g + sz // 2),
             (90, cb[0] - g - sz // 2, cy), (90, cb[2] + g + sz // 2, cy)]
    best = None
    for ang, x, y in cands:
        r.SetTextAngleDegrees(ang); r.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_CENTER); r.SetPosition(pcbnew.VECTOR2I(int(x), int(y)))
        tb = _bb(r)
        if tb[0] < _edge[0] or tb[1] < _edge[1] or tb[2] > _edge[2] or tb[3] > _edge[3]: continue
        bad = sum(_hit(tb, p) for p in _pads) * 3 + sum(_hit(tb, t) for t in _texts) * 3 \
            + sum(_hit(tb, c) for k, c in _crt.items() if k != ref)
        if best is None or bad < best[0]: best = (bad, ang, x, y)
        if bad == 0: break
    if best:
        _, ang, x, y = best
        r.SetTextAngleDegrees(ang); r.SetPosition(pcbnew.VECTOR2I(int(x), int(y))); _texts.append(_bb(r)); moved += 1
print("reference texts placed:", moved)
# notes are (re)placed below on the nearest free spot -> drop earlier copies
for d in list(b.GetDrawings()):
    if hasattr(d, "GetText") and d.GetLayer() in (pcbnew.F_SilkS, pcbnew.B_SilkS) and d.GetText().startswith("JP8 ") \
       and d.GetText() not in {n[1] for n in getattr(L, "NOTES", [])} | {n[0] for n in getattr(L, "FIXED_NOTES", [])}: b.Remove(d)
NOTE_TXT = {n[1] for n in getattr(L, "NOTES", [])}
note_obj = {}
for d in b.GetDrawings():
    if d.GetLayer() in (pcbnew.F_SilkS, pcbnew.B_SilkS) and hasattr(d, "GetText") and d.GetText() in NOTE_TXT: note_obj[d.GetText()] = d
obst = []
for fp in b.GetFootprints():
    for lay in (pcbnew.F_CrtYd,):
        c = fp.GetCourtyard(lay)
        if c.OutlineCount():
            bb = c.BBox(); obst.append((pcbnew.ToMM(bb.GetLeft()) - OX, pcbnew.ToMM(bb.GetTop()) - OY, pcbnew.ToMM(bb.GetRight()) - OX, pcbnew.ToMM(bb.GetBottom()) - OY))
    for t in (fp.Reference(),):
        if t.IsVisible():
            bb = t.GetBoundingBox(); obst.append((pcbnew.ToMM(bb.GetLeft()) - OX, pcbnew.ToMM(bb.GetTop()) - OY, pcbnew.ToMM(bb.GetRight()) - OX, pcbnew.ToMM(bb.GetBottom()) - OY))
for d in b.GetDrawings():
    if d.GetLayer() == pcbnew.F_SilkS and not (hasattr(d, "GetText") and d.GetText() in NOTE_TXT):
        bb = d.GetBoundingBox(); obst.append((pcbnew.ToMM(bb.GetLeft()) - OX, pcbnew.ToMM(bb.GetTop()) - OY, pcbnew.ToMM(bb.GetRight()) - OX, pcbnew.ToMM(bb.GetBottom()) - OY))
obst.append(L.MOD)
def hit(a, c): return not (a[2] <= c[0] or c[2] <= a[0] or a[3] <= c[1] or c[3] <= a[1])
def find(w, h):
    best = None
    y = 1.0
    while y + h < L.H - 1.0:
        x = 1.0
        while x + w < L.W - 1.0:
            r = (x, y, x + w, y + h)
            if not any(hit(r, o) for o in obst):
                score = abs(x + w / 2 - L.W / 2) + abs(y + h / 2 - L.H / 2) * 0.5
                if best is None or score < best[0]: best = (score, r)
            x += 0.5
        y += 0.5
    return best
def put(txt, sz, cx, cy):
    t = pcbnew.PCB_TEXT(b); t.SetText(txt); t.SetLayer(pcbnew.F_SilkS)
    t.SetTextSize(pcbnew.VECTOR2I(mm(sz), mm(sz))); t.SetTextThickness(mm(sz * 0.17))
    t.SetPosition(pcbnew.VECTOR2I(mm(OX + cx), mm(OY + cy))); b.Add(t)
existing = {d.GetText() for d in b.GetDrawings() if d.GetLayer() == pcbnew.F_SilkS and hasattr(d, "GetText")}
done = all(t in existing and t != "franzenhof.com" for t, _ in L.BRAND[:1]) and sum(1 for d in b.GetDrawings() if hasattr(d, "GetText") and d.GetText() == "franzenhof.com" and d.GetLayer() == pcbnew.F_SilkS) >= 2
if not getattr(L, "BRAND_BLOCK", True): done = True      # the layout places the brand texts itself
if done: print("brand block already present")
for sz in ((1.6, 1.4, 1.2) if not done else ()):
    lines = [(t, sz) for t, _ in L.BRAND]
    w = max(len(t) * sz * 0.85 for t, _ in lines) + 1.0; h = len(lines) * sz * 1.6 + 0.6
    best = find(w, h)
    if best:
        x0, y0, x1, y1 = best[1]; yy = y0 + 0.3
        for t, _ in lines:
            put(t, sz, (x0 + x1) / 2, yy + sz * 0.8); yy += sz * 1.6
        obst.append(best[1])      # notes below must not land on the brand block
        print(f"brand block {sz} mm at x {x0:.1f}-{x1:.1f}, y {y0:.1f}-{y1:.1f}"); done = True; break
if not done:      # place the lines individually
    for t, _ in L.BRAND:
        for sz in (1.6, 1.4, 1.2):
            w = len(t) * sz * 0.85 + 1.0; h = sz * 1.6 + 0.6
            best = find(w, h)
            if best:
                x0, y0, x1, y1 = best[1]; put(t, sz, (x0 + x1) / 2, (y0 + y1) / 2)
                obst.append(best[1]); print(f"'{t}' {sz} mm at x {x0:.1f}-{x1:.1f}, y {y0:.1f}-{y1:.1f}"); break
        else:
            print("no spot for", t)
# notes: nearest free spot to their footprint
for ref, txt, dx, dy, sz in getattr(L, "NOTES", []):
    fp = b.FindFootprintByReference(ref); fx, fy = pcbnew.ToMM(fp.GetPosition().x) - OX, pcbnew.ToMM(fp.GetPosition().y) - OY
    lns = txt.split("\n")
    w = max(len(l) for l in lns) * sz * 0.85 + 0.6; h = len(lns) * sz * 1.6 + 0.4; cand = None
    y = max(1.0, fy - 18)
    while y + h < min(L.H - 1.0, fy + 18):
        x = max(1.0, fx - 22)
        while x + w < min(L.W - 1.0, fx + 22):
            r = (x, y, x + w, y + h)
            if not any(hit(r, o) for o in obst):
                dd = ((x + w / 2 - fx) ** 2 + (y + h / 2 - fy) ** 2) ** 0.5
                if cand is None or dd < cand[0]: cand = (dd, r)
            x += 0.25
        y += 0.25
    if cand:
        x0, y0, x1, y1 = cand[1]
        if txt in note_obj: note_obj[txt].SetPosition(pcbnew.VECTOR2I(mm(OX + (x0 + x1) / 2), mm(OY + (y0 + y1) / 2)))
        else: put(txt, sz, (x0 + x1) / 2, (y0 + y1) / 2)
        obst.append(cand[1])
        print(f"note '{txt}' at {cand[0]:.1f} mm from {ref}")
    else:
        # top side is full around the part: move the note to the bottom silkscreen right under it
        o = note_obj.get(txt)
        if o is None:
            o = pcbnew.PCB_TEXT(b); o.SetText(txt); b.Add(o)
        o.SetLayer(pcbnew.B_SilkS); o.SetMirrored(True)
        o.SetTextSize(pcbnew.VECTOR2I(mm(sz), mm(sz))); o.SetTextThickness(mm(sz * 0.15))
        o.SetPosition(pcbnew.VECTOR2I(fp.GetPosition().x, fp.GetPosition().y))
        print(f"note '{txt}' -> bottom silkscreen under {ref}")
# test-point labels: first free side (right, below, above, left) that touches no pad and no other text
def tbox(t):
    bb = t.GetBoundingBox(); return (bb.GetLeft(), bb.GetTop(), bb.GetRight(), bb.GetBottom())
def ihit(a, c, m=int(mm(0.15))): return not (a[2] + m <= c[0] or c[2] + m <= a[0] or a[3] + m <= c[1] or c[3] + m <= a[1])
pads_bb = [(q.GetBoundingBox().GetLeft(), q.GetBoundingBox().GetTop(), q.GetBoundingBox().GetRight(), q.GetBoundingBox().GetBottom(), q)
           for f_ in b.GetFootprints() for q in f_.Pads()]
texts = [d for d in b.GetDrawings() if d.GetLayer() == pcbnew.F_SilkS and hasattr(d, "GetText")]
tp_labels = {}
for f_ in b.GetFootprints():
    if not f_.GetReference().startswith("TP"): continue
    pp = f_.Pads()[0].GetPosition()
    for d in texts:
        if abs(d.GetPosition().x - pp.x) < mm(4) and abs(d.GetPosition().y - pp.y) < mm(4) and d.GetText() in {l for v in getattr(L, "PIN_LABELS", {}).values() for l in v}:
            tp_labels[f_.GetReference()] = (d, pp); break
for ref, (d, pp) in sorted(tp_labels.items()):
    others = [tbox(t) for t in texts if t is not d] + [tbox(f_.Reference()) for f_ in b.GetFootprints() if f_.Reference().IsVisible()] \
             + [(c.BBox().GetLeft(), c.BBox().GetTop(), c.BBox().GetRight(), c.BBox().GetBottom()) for f_ in b.GetFootprints() if f_.GetReference() != ref for c in [f_.GetCourtyard(pcbnew.F_CrtYd)] if c.OutlineCount()]
    for dx, dy, just in ((1.5, 0, pcbnew.GR_TEXT_H_ALIGN_LEFT), (0, 2.0, pcbnew.GR_TEXT_H_ALIGN_CENTER),
                         (0, -2.0, pcbnew.GR_TEXT_H_ALIGN_CENTER), (-1.5, 0, pcbnew.GR_TEXT_H_ALIGN_RIGHT)):
        d.SetTextAngleDegrees(0); d.SetHorizJustify(just); d.SetPosition(pcbnew.VECTOR2I(pp.x + mm(dx), pp.y + mm(dy)))
        tb = tbox(d)
        if not any(ihit(tb, pb[:4]) for pb in pads_bb) and not any(ihit(tb, o) for o in others): break
if getattr(L, "BACK_DOCUMENTATION", False):
    from backdoc import apply
    back_documentation = apply(b)
pcbnew.SaveBoard(sys.argv[1], b)
