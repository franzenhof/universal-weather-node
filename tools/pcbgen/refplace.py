"""Check / fix silkscreen reference texts that are hidden or clipped.

A reference text is "bad" when its box overlaps another footprint's courtyard (that
includes the tall socket rows of the unpopulated alternative Heltec set and every
header / connector body that hides the text in the 3D render), any pad, another
silkscreen text, footprint silk outlines of other parts, a mounting-hole keep-out or the
board-edge margin. Bad references are moved to the nearest free spot on rings around
their own footprint (orientation kept). Board texts are only reported.

  python3 refplace.py <board.kicad_pcb> [--fix]          (KiCad Python)
"""
import math
import sys

import pcbnew

MM, mm = pcbnew.ToMM, pcbnew.FromMM
GAP = 0.15                     # minimum free distance around a text box
EDGE = 0.6                     # board edge margin
HOLES = ((54, 54), (146, 54), (54, 146), (146, 146))
HOLE_R = 3.9


def box(item):
    b = item.GetBoundingBox()
    return [MM(b.GetLeft()), MM(b.GetTop()), MM(b.GetRight()), MM(b.GetBottom())]


TALL_PREFIX = ("J", "JP", "H")
TALL_REFS = {"C6", "C31", "L1"}


def tall(ref):
    return ref in TALL_REFS or (ref[:1] == "J") or ref.startswith("H")


def isref(fp, t):
    return fp is not None and t.m_Uuid.AsString() == fp.Reference().m_Uuid.AsString()


def hit(a, b, g=GAP):
    return not (a[2] + g <= b[0] or b[2] + g <= a[0] or a[3] + g <= b[1] or b[3] + g <= a[1])


def analyse(board):
    edge = board.GetBoardEdgesBoundingBox()
    E = [MM(edge.GetLeft()) + EDGE, MM(edge.GetTop()) + EDGE, MM(edge.GetRight()) - EDGE, MM(edge.GetBottom()) - EDGE]
    sides = {pcbnew.F_SilkS: (pcbnew.F_CrtYd, pcbnew.F_Cu), pcbnew.B_SilkS: (pcbnew.B_CrtYd, pcbnew.B_Cu)}
    obstacles = {s: [] for s in sides}          # (owner ref, kind, shape, clearance mm)
    texts = {s: [] for s in sides}              # (owner footprint or None, text item)
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        for silk, (crt, cu) in sides.items():
            # body = extent of the footprint's fabrication outline (what the 3D render shows)
            fab = pcbnew.F_Fab if silk == pcbnew.F_SilkS else pcbnew.B_Fab
            fb = [box(g) for g in fp.GraphicalItems() if g.GetLayer() == fab and not hasattr(g, "GetText")]
            if fb and fp.IsFlipped() == (silk == pcbnew.B_SilkS):
                x0, y0 = min(b[0] for b in fb), min(b[1] for b in fb)
                x1, y1 = max(b[2] for b in fb), max(b[3] for b in fb)
                obstacles[silk].append((ref, "body", pcbnew.SHAPE_RECT(pcbnew.VECTOR2I(mm(x0), mm(y0)), mm(x1 - x0), mm(y1 - y0)), 0.3 if tall(ref) else 0.05))   # headers / sockets render taller than F.Fab
            for p in fp.Pads():
                if p.IsOnLayer(cu): obstacles[silk].append((ref, "pad", p.GetEffectiveShape(cu), 0.1))
            for g in fp.GraphicalItems():
                if g.GetLayer() != silk: continue
                if hasattr(g, "GetText"):
                    if g.IsVisible(): texts[silk].append((fp, g))
                else:
                    obstacles[silk].append((ref, "silk", g.GetEffectiveShape(), 0.05))
        r = fp.Reference()
        if r.IsVisible() and r.GetLayer() in sides: texts[r.GetLayer()].append((fp, r))
    for d in board.GetDrawings():
        if d.GetLayer() in sides and hasattr(d, "GetText"): texts[d.GetLayer()].append((None, d))
    for v in board.GetTracks():          # via rings show through / interrupt the print
        if v.GetClass() == "PCB_VIA":
            for silk, (crt, cu) in sides.items():
                obstacles[silk].append((None, "via", v.GetEffectiveShape(cu), 0.05))
    return E, obstacles, texts


def collide(a, b, clr):
    if isinstance(b, pcbnew.SHAPE_POLY_SET):
        return b.Collide(a, mm(clr)) if hasattr(b, "Collide") else False
    return b.Collide(a, mm(clr))


FORCE = {"C23", "U8", "JP2", "JP12", "C36"}
PREFERRED = {"JP12": [(84.2, 88.9), (84.6, 88.9), (79.8, 86.9), (79.8, 91.3)]}     # reported as hard to read in the render (via rings etc.)


def problems(E, obstacles, texts, silk, fp, t, vias=False):
    tb = box(t)
    own = fp.GetReference() if fp else None
    out = []
    if tb[0] < E[0] or tb[1] < E[1] or tb[2] > E[2] or tb[3] > E[3]: out.append("board edge")
    for hx, hy in HOLES:
        if math.hypot(min(max(hx, tb[0]), tb[2]) - hx, min(max(hy, tb[1]), tb[3]) - hy) < HOLE_R - 0.4:
            out.append("mounting hole")
    ts = t.GetEffectiveTextShape()
    for ref, kind, shp, clr in obstacles[silk]:
        if kind == "via" and not vias: continue
        if ref == own and kind == "silk" and not isref(fp, t): continue
        if collide(ts, shp, clr): out.append(f"{kind} {ref}")
    for ofp, o in texts[silk]:
        if o is t: continue
        if ts.Collide(o.GetEffectiveTextShape(), mm(0.1)): out.append(f"text {o.GetText()!r:.20}")
    return out


def main(path, fix):
    board = pcbnew.LoadBoard(path)
    E, obstacles, texts = analyse(board)
    moved, left = [], []
    for silk in (pcbnew.F_SilkS, pcbnew.B_SilkS):
        for fp, t in list(texts[silk]):
            is_ref = isref(fp, t)
            pr = problems(E, obstacles, texts, silk, fp, t, vias=is_ref and fp.GetReference() in FORCE)
            if not pr: continue
            name = t.GetText() if fp is None else fp.GetReference()
            if not (fix and fp is not None and isref(fp, t)):
                left.append((name, pcbnew.LayerName(silk), sorted(set(pr))[:4])); continue
            start = t.GetPosition(); c = fp.GetPosition()
            fb = fp.GetBoundingBox(False)
            r0 = max(MM(fb.GetWidth()), MM(fb.GetHeight())) / 2
            best = None
            for x, y in PREFERRED.get(name, ()):         # readable spots chosen by hand, tried first
                t.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
                if not problems(E, obstacles, texts, silk, fp, t, vias=True):
                    best = (0, t.GetPosition()); break
            for k in range(0, 40 if best is None else 0):
                r = max(0.6, r0 * 0.4) + k * 0.25
                for a in range(0, 360, 15):
                    p = pcbnew.VECTOR2I(c.x + mm(r * math.cos(math.radians(a))), c.y + mm(r * math.sin(math.radians(a))))
                    t.SetPosition(p)
                    if not problems(E, obstacles, texts, silk, fp, t, vias=True):
                        dist = (p - start).EuclideanNorm()
                        if best is None or dist < best[0]: best = (dist, p)
                if best: break
            if best:
                t.SetPosition(best[1]); moved.append((name, sorted(set(pr))[:3], (round(MM(best[1].x), 2), round(MM(best[1].y), 2))))
            else:
                t.SetPosition(start); left.append((name, pcbnew.LayerName(silk), sorted(set(pr))[:4]))
    if fix:
        # JP2's pin labels ("R8" / "DIR") sit right of the header, under C6's body: mirror them to its left
        jp2 = board.FindFootprintByReference("JP2")
        if jp2:
            px = MM(min(p.GetPosition().x for p in jp2.Pads()))
            for fp_, t in texts[pcbnew.F_SilkS]:
                if fp_ is None and t.GetText() in ("R8", "DIR") and 0 < MM(t.GetPosition().x) - px < 3:
                    old = (t.GetPosition(), t.GetHorizJustify())
                    t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_RIGHT)
                    t.SetPosition(pcbnew.VECTOR2I(mm(px - 1.8), t.GetPosition().y))
                    if problems(E, obstacles, texts, pcbnew.F_SilkS, None, t, vias=False):
                        t.SetPosition(old[0]); t.SetHorizJustify(old[1])
                    else:
                        moved.append((t.GetText(), ["body C6"], (round(px - 1.8, 2), round(MM(t.GetPosition().y), 2))))
    for m in moved: print("moved", *m)
    for l in left: print("still", *l)
    if fix:
        pcbnew.SaveBoard(path, board)
    return len(left)


if __name__ == "__main__":
    sys.exit(1 if main(sys.argv[1], "--fix" in sys.argv) and "--strict" in sys.argv else 0)
