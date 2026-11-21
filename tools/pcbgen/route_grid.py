"""Small two-layer grid router for local board edits (dual_socket.py).

Freerouting re-fans and re-routes a hand-finished board, so the few connections that a
local edit opens are routed here instead: A* on a 0.1 mm grid per copper layer with
vias, obstacles = foreign copper inflated by half the track width + clearance, rule-area
keep-outs, board edge and drill holes; the grid path is then straightened (line of sight).
Input / output are JSON files written / read by dual_socket.py (KiCad Python has no numpy).

  python3 route_grid.py <geometry.json> <routes.json>
Needs numpy, scipy and pillow (not KiCad's bundled Python).
"""
import heapq, json, math, os, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import binary_erosion, distance_transform_edt

RES = 0.025                    # grid (mm)
CLEAR = 0.25                   # copper clearance (net classes Default / Power)
MARGIN = float(os.environ.get("ROUTE_MARGIN", 0.03))   # raster error allowance (DRC checks the result)
EDGE = 0.5                     # copper to board edge
VIA_D, VIA_DRILL, HOLE_CLEAR = 0.8, 0.4, 0.3
VIA_COST = 100                 # in grid steps (2.5 mm)


def main(geo_file, out_file):
    g = json.load(open(geo_file))
    x0, y0, x1, y1 = g["box"]
    W, H = int(round((x1 - x0) / RES)) + 1, int(round((y1 - y0) / RES)) + 1
    P = lambda x, y: ((x - x0) / RES, (y - y0) / RES)
    LAY = {"F": 0, "B": 1}
    lab = [Image.new("I", (W, H), 0) for _ in LAY]
    keep_t = [Image.new("L", (W, H), 0) for _ in LAY]
    keep_v = Image.new("L", (W, H), 0)
    holes = Image.new("L", (W, H), 0)
    for c in g["copper"]:     # filled + 1 px outline: the raster never under-covers copper
        pts = [P(*p) for p in c["poly"]]; v = c["net"] if c["net"] > 0 else -1
        dr = ImageDraw.Draw(lab[LAY[c["layer"]]]); dr.polygon(pts, fill=v); dr.line(pts + pts[:1], fill=v, width=2)
    for k in g["keepout"]:
        for l in k["layers"]:
            if k["tracks"]: ImageDraw.Draw(keep_t[LAY[l]]).polygon([P(*p) for p in k["poly"]], fill=1)
        if k["vias"]: ImageDraw.Draw(keep_v).polygon([P(*p) for p in k["poly"]], fill=1)
    for x, y, r in g["holes"]:
        cx, cy = P(x, y); rr = r / RES
        ImageDraw.Draw(holes).ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=1)
    L = [np.array(im, dtype=np.int64) for im in lab]
    KT = [np.array(im, dtype=bool) for im in keep_t]
    KV = np.array(keep_v, dtype=bool)
    HO = np.array(holes, dtype=bool)
    yy, xx = np.mgrid[0:H, 0:W]
    edge_d = np.minimum.reduce([xx, yy, W - 1 - xx, H - 1 - yy]) * RES
    d_keep = [distance_transform_edt(~k) * RES for k in KT]
    d_keepv = distance_transform_edt(~KV) * RES
    d_hole = distance_transform_edt(~HO) * RES

    L0, HO_d0 = [l.copy() for l in L], d_hole.copy()
    state = {"L": L, "d_hole": d_hole}

    def cells(item):
        im = Image.new("L", (W, H), 0); dr = ImageDraw.Draw(im)
        dr.polygon([P(*p) for p in item["poly"]], fill=1)
        m = np.array(im, dtype=bool)
        e = binary_erosion(m, iterations=2)     # end points well inside the item's copper
        if e.any(): m = e
        return [(LAY[l], m) for l in item["layers"]]

    def route(conn):
        L, d_hole = state["L"], state["d_hole"]
        net, w = conn["net"], conn["width"]
        r_t = w / 2 + CLEAR + MARGIN
        r_v = VIA_D / 2 + CLEAR + MARGIN
        blocked, dist = [], []
        for li in range(2):
            other = (L[li] != 0) & (L[li] != net)
            d = distance_transform_edt(~other) * RES
            dist.append(d)
            blocked.append((d <= r_t) | (d_keep[li] <= w / 2 + MARGIN) | (edge_d <= EDGE + w / 2))
        via_ok = (dist[0] > r_v) & (dist[1] > r_v) & (d_keepv > VIA_D / 2 + MARGIN) & \
                 (d_hole > VIA_DRILL / 2 + HOLE_CLEAR) & (edge_d > EDGE + VIA_D / 2) & ~blocked[0] & ~blocked[1]
        src, dst = cells(conn["a"]), cells(conn["b"])
        bx = [p[0] for it in (conn["a"], conn["b"]) for p in it["poly"]]
        by = [p[1] for it in (conn["a"], conn["b"]) for p in it["poly"]]
        win = (max(0, int((min(bx) - x0 - 14) / RES)), max(0, int((min(by) - y0 - 14) / RES)),
               min(W - 1, int((max(bx) - x0 + 14) / RES)), min(H - 1, int((max(by) - y0 + 14) / RES)))
        path = astar(src, dst, blocked, via_ok, win)
        if not path: return None
        runs, cur = [], [path[0]]
        for p in path[1:]:
            if p[0] != cur[-1][0]: runs.append(cur); cur = [p]
            else: cur.append(p)
        runs.append(cur)
        tracks, vias = [], []
        for i, run in enumerate(runs):
            li = run[0][0]
            pts = [(c[1], c[2]) for c in run]
            # pads: start / end at the pad centre (tracks: at the cell found inside the item)
            if i == 0 and conn["a"].get("center") and "FB"[li] in conn["a"]["layers"]:
                pts.insert(0, P(*conn["a"]["center"]))
            if i == len(runs) - 1 and conn["b"].get("center") and "FB"[li] in conn["b"]["layers"]:
                pts.append(P(*conn["b"]["center"]))
            simp = pull(pts, blocked[li], L[li], net)
            for (ax, ay), (bx_, by_) in zip(simp, simp[1:]):
                tracks.append([x0 + ax * RES, y0 + ay * RES, x0 + bx_ * RES, y0 + by_ * RES, w, "FB"[li], net])
            if i < len(runs) - 1:
                vias.append([x0 + run[-1][1] * RES, y0 + run[-1][2] * RES, net])
        return tracks, vias

    def commit(res):
        L, d_hole = state["L"], state["d_hole"]
        tracks, vias = res
        for t in tracks: draw_seg(L["FB".index(t[5])], t[:4], t[4], t[6], x0, y0)
        for vx, vy, net in vias:
            for li2 in range(2): draw_disc(L[li2], vx, vy, VIA_D / 2, net, x0, y0)
            draw_disc_bool(d_hole, vx, vy, VIA_DRILL / 2, x0, y0)

    def rebuild(results):
        state["L"] = [l.copy() for l in L0]; state["d_hole"] = HO_d0.copy()
        for res in results.values(): commit(res)

    conns = g["conns"]
    span = lambda c: math.hypot(*(np.ptp([p[k] for it in (c["a"], c["b"]) for p in it["poly"]]) for k in (0, 1)))
    order = sorted(range(len(conns)), key=lambda i: -span(conns[i]))      # long connections first
    results, failed, ripped = {}, [], {}
    queue = list(order)
    while queue:
        i = queue.pop(0)
        res = route(conns[i])
        if res:
            results[i] = res; commit(res); print("routed", conns[i]["label"]); continue
        if ripped.get(i, 0) >= 2:
            failed.append(i); print("FAILED", conns[i]["label"]); continue
        # rip-up and retry: remove this run's routes of other nets near the connection
        ripped[i] = ripped.get(i, 0) + 1
        c = conns[i]
        bx = [p[0] for it in (c["a"], c["b"]) for p in it["poly"]]
        by = [p[1] for it in (c["a"], c["b"]) for p in it["poly"]]
        box = (min(bx) - 3, min(by) - 3, max(bx) + 3, max(by) + 3)
        inbox = lambda x, y: box[0] <= x <= box[2] and box[1] <= y <= box[3]
        victims = [j for j, (tr, vi) in results.items() if conns[j]["net"] != c["net"] and
                   (any(inbox(t[0], t[1]) or inbox(t[2], t[3]) for t in tr) or any(inbox(v[0], v[1]) for v in vi))]
        if not victims:
            failed.append(i); print("FAILED", c["label"]); continue
        saved = {j: results.pop(j) for j in victims}
        rebuild(results)
        res = route(c)
        if not res:
            results.update(saved); rebuild(results); failed.append(i); print("FAILED", c["label"]); continue
        results[i] = res; commit(res); print("routed after rip-up", c["label"], "re-queue", len(victims))
        queue = victims + queue
    out = {"tracks": [t for r in results.values() for t in r[0]], "vias": [v for r in results.values() for v in r[1]],
           "failed": [conns[i]["label"] for i in failed]}
    json.dump(out, open(out_file, "w"))


def draw_seg(arr, seg, w, net, x0, y0):
    im = Image.fromarray(np.zeros(arr.shape, dtype=np.uint8)); dr = ImageDraw.Draw(im)
    a = ((seg[0] - x0) / RES, (seg[1] - y0) / RES); b = ((seg[2] - x0) / RES, (seg[3] - y0) / RES)
    r = w / 2 / RES
    r += 1                                   # 1 px larger than drawn copper (conservative)
    dr.line([a, b], fill=1, width=max(1, int(round(w / RES))) + 2)
    for c in (a, b): dr.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=1)
    arr[np.array(im, dtype=bool)] = net


def draw_disc(arr, x, y, r, net, x0, y0):
    im = Image.fromarray(np.zeros(arr.shape, dtype=np.uint8)); dr = ImageDraw.Draw(im)
    cx, cy, rr = (x - x0) / RES, (y - y0) / RES, r / RES + 1
    dr.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=1)
    arr[np.array(im, dtype=bool)] = net


def draw_disc_bool(dmap, x, y, r, x0, y0):
    H, W = dmap.shape
    yy, xx = np.ogrid[0:H, 0:W]
    d = np.sqrt((xx * RES + x0 - x) ** 2 + (yy * RES + y0 - y) ** 2) - r
    np.minimum(dmap, np.maximum(d, 0), out=dmap)


def astar(src, dst, blocked, via_ok, win):
    wx0, wy0, wx1, wy1 = win
    target = set()
    for li, m in dst:
        ys, xs = np.nonzero(m[wy0:wy1 + 1, wx0:wx1 + 1])
        for x, y in zip(xs + wx0, ys + wy0): target.add((li, x, y))
    if not target: return None
    tx = np.mean([t[1] for t in target]); ty = np.mean([t[2] for t in target])
    openh, came, gs = [], {}, {}
    for li, m in src:
        ys, xs = np.nonzero(m[wy0:wy1 + 1, wx0:wx1 + 1])
        for x, y in zip(xs + wx0, ys + wy0):
            s = (li, int(x), int(y)); gs[s] = 0; came[s] = None
            heapq.heappush(openh, (math.hypot(x - tx, y - ty), 0, s))
    # same-net copper of the endpoints is passable even if a foreign keep-out margin overlaps it
    passable = {s for s in gs} | target
    D = [(1, 0, 1), (-1, 0, 1), (0, 1, 1), (0, -1, 1), (1, 1, 1.4142), (1, -1, 1.4142), (-1, 1, 1.4142), (-1, -1, 1.4142)]
    n = 0
    while openh:
        f, gcur, s = heapq.heappop(openh)
        if gcur > gs.get(s, 1e18): continue
        if s in target:
            path = []
            while s is not None: path.append(s); s = came[s]
            return path[::-1]
        n += 1
        if n > 30_000_000: return None
        li, x, y = s
        nbrs = []
        for dx, dy, c in D:
            nx, ny = x + dx, y + dy
            if wx0 <= nx <= wx1 and wy0 <= ny <= wy1:
                t = (li, nx, ny)
                if not blocked[li][ny, nx] or t in passable: nbrs.append((t, c))
        if via_ok[y, x]: nbrs.append(((1 - li, x, y), VIA_COST))
        for t, c in nbrs:
            ng = gcur + c
            if ng < gs.get(t, 1e18):
                gs[t] = ng; came[t] = s
                h = math.hypot(t[1] - tx, t[2] - ty)
                heapq.heappush(openh, (ng + h, ng, t))
    return None


def pull(pts, blocked, lab, net):
    """straighten the grid path into 0/45/90 degree segments (line of sight on the blocked map;
    blocked cells may only be crossed inside same-net copper)"""
    def free(a, b):        # conservative: every cell touching the sampled line must be free
        n = int(max(abs(b[0] - a[0]), abs(b[1] - a[1])) * 4) + 1
        for i in range(n + 1):
            x = a[0] + (b[0] - a[0]) * i / n; y = a[1] + (b[1] - a[1]) * i / n
            for xi in {math.floor(x), math.ceil(x)}:
                for yi in {math.floor(y), math.ceil(y)}:
                    if blocked[yi, xi] and lab[yi, xi] != net: return False
        return True
    sgn = lambda v: (v > 0) - (v < 0)
    def octo(a, b):
        dx, dy = b[0] - a[0], b[1] - a[1]
        if abs(dx) < 1e-6 or abs(dy) < 1e-6 or abs(abs(dx) - abs(dy)) < 1e-6:
            return [b] if free(a, b) else None
        m = min(abs(dx), abs(dy))
        for bend in ((a[0] + sgn(dx) * m, a[1] + sgn(dy) * m), (b[0] - sgn(dx) * m, b[1] - sgn(dy) * m)):
            if free(a, bend) and free(bend, b): return [bend, b]
        return None
    out = [pts[0]]; i = 0
    while i < len(pts) - 1:
        for j in range(len(pts) - 1, i, -1):
            step = octo(pts[i], pts[j])
            if step: break
        else:
            j, step = i + 1, [pts[i + 1]]          # neighbouring cells / pad centre: always connected
        out += step; i = j
    # merge collinear runs
    res = [out[0]]
    for k in range(1, len(out) - 1):
        a, b, c = res[-1], out[k], out[k + 1]
        if abs((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])) > 1e-6: res.append(b)
    res.append(out[-1])
    return res


if __name__ == "__main__":
    main(*sys.argv[1:3])
