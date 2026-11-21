"""Generate a drawn KiCad schematic from design.py + a placement spec (SCHEM module).

The spec places every symbol (block-relative, in 2.54 mm grid units); this script
draws the wiring: nets inside a block become orthogonal wires (Steiner-like tree
with T-junctions), supply nets become power symbols at each pin, nets that leave
the block get a local label. ERC + netlist comparison (checknet.py) verify it.
"""
import sys, uuid, math, json, os, importlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import Sym, dump, flat, pins, find, get
from design import PARTS
from lcsc import LCSC, VALUE_OVERRIDE
S = importlib.import_module(os.environ.get("SCHEM", "schem_w"))

OUT = sys.argv[1]
PROJECT = os.path.basename(OUT).replace(".kicad_sch", "")
G = 2.54
ROOT = str(uuid.uuid4())
U = lambda: str(uuid.uuid4())
r4 = lambda v: round(v, 4)
snap = lambda v: r4(round(v / 1.27) * 1.27)
PART = {p["ref"]: p for p in PARTS}

def eff(size=1.27, hide=False, justify=None, bold=False):
    e = [Sym("effects"), [Sym("font"), [Sym("size"), size, size]] + ([[Sym("bold"), Sym("yes")]] if bold else [])]
    if justify: e.append([Sym("justify")] + [Sym(j) for j in justify.split()])
    if hide: e.append([Sym("hide"), Sym("yes")])
    return e
def wire(a, b):
    return [Sym("wire"), [Sym("pts"), [Sym("xy"), r4(a[0]), r4(a[1])], [Sym("xy"), r4(b[0]), r4(b[1])]],
            [Sym("stroke"), [Sym("width"), 0], [Sym("type"), Sym("default")]], [Sym("uuid"), U()]]

libsyms = {}
items = []
def symgeom(lib, name, angle, mirror, unit):
    sym = flat(lib, name)
    out = []
    for p in pins(sym, unit):
        x, y, a = p["x"], p["y"], p["a"]
        if mirror: x, a = -x, (180 - a) % 360
        r = math.radians(angle)
        rx, ry = x * math.cos(r) - y * math.sin(r), x * math.sin(r) + y * math.cos(r)
        out.append(dict(p, rx=r4(rx), ry=r4(-ry), out=int((a + angle + 180) % 360)))
    # body extent (graphics only) for collision checks
    xs, ys = [], []
    for sub in find(sym, "symbol"):
        su_ = int(sub[1].rsplit("_", 2)[-2])
        if su_ not in (0, unit): continue
        for g in sub[2:]:
            if not isinstance(g, list): continue
            pts = []
            if g[0] == "rectangle": pts = [get(g, "start")[1:3], get(g, "end")[1:3]]
            elif g[0] == "polyline": pts = [e[1:3] for e in get(g, "pts")[1:]]
            elif g[0] == "circle":
                c = get(g, "center"); rr = float(get(g, "radius")[1])
                pts = [(float(c[1]) - rr, float(c[2]) - rr), (float(c[1]) + rr, float(c[2]) + rr)]
            for px, py in pts:
                px, py = float(px), float(py)
                if mirror: px = -px
                r = math.radians(angle)
                xs.append(px * math.cos(r) - py * math.sin(r)); ys.append(-(px * math.sin(r) + py * math.cos(r)))
    body = (min(xs), min(ys), max(xs), max(ys)) if xs else (-1, -1, 1, 1)
    return sym, out, body
OUTV = {0: (1, 0), 180: (-1, 0), 90: (0, -1), 270: (0, 1)}     # outward pin direction (schematic, y down)

# ------------------------------------------------------------------ placement
placed = []                      # dicts: ref, unit, x, y, angle, mirror, pins(abs), body(abs), block
blocks = []
OFF = getattr(S, "OFFSET", (5, 5))
for blk in S.BLOCKS:
    blk["x"] += OFF[0]; blk["y"] += OFF[1]
    bx, by = blk["x"] * G, blk["y"] * G
    blocks.append(dict(blk, ax=bx, ay=by))
    for e in blk["parts"]:
        ref, x, y = e[0], e[1], e[2]
        o = e[3] if len(e) > 3 else {}
        p = PART[ref]
        angle, mirror, unit = o.get("a", 0), o.get("m", False), o.get("u", 1)
        sym, ps, body = symgeom(p["sym"][0], p["sym"][1], angle, mirror, unit)
        sx, sy = snap(bx + x * G), snap(by + y * G)
        libsyms.setdefault(f"{p['sym'][0]}:{p['sym'][1]}", sym)
        placed.append(dict(ref=ref, unit=unit, x=sx, y=sy, angle=angle, mirror=mirror, opt=o, block=blk["title"],
                           pins=[dict(pp, X=r4(sx + pp["rx"]), Y=r4(sy + pp["ry"])) for pp in ps],
                           body=(sx + body[0], sy + body[1], sx + body[2], sy + body[3])))
# every part / unit must be placed exactly once
need = {(p["ref"], u) for p in PARTS for u in range(1, p.get("units", 1) + 1)}
have = [(q["ref"], q["unit"]) for q in placed]
assert set(have) == need and len(have) == len(need), (sorted(need - set(have)), sorted(set(have) - need))

# ------------------------------------------------------------------ symbols
for q in placed:
    p = PART[q["ref"]]; key = p["lcsc_key"]; lc = LCSC.get(key, ("", "", False))
    value = VALUE_OVERRIDE.get(key, p["value"])
    su = U()
    if q["unit"] == 1: p["uuid"] = su
    b = q["body"]
    xs_ = {round(pp["rx"], 2) for pp in q["pins"]}; ys_ = {round(pp["ry"], 2) for pp in q["pins"]}
    vertical = (len(q["pins"]) == 2 and len(xs_) == 1) or (len(q["pins"]) != 2 and (b[3] - b[1]) > (b[2] - b[0]) * 1.2)
    o = q["opt"]
    if o.get("lab") == "left" or (vertical and o.get("lab") != "right" and False): pass
    if vertical:   # texts right of the body (or left with lab="left")
        if o.get("lab") == "left":
            rx, jr = b[0] - 0.8, "right"
        else:
            rx, jr = b[2] + 0.8, "left"
        rpos, vpos, just = (rx, q["y"] - 1.0), (rx, q["y"] + 1.6), jr
    else:          # texts above / below the body
        if o.get("lab") == "below":
            rpos, vpos, just = (q["x"], b[3] + 1.9), (q["x"], b[3] + 4.2), None
        else:
            rpos, vpos, just = (q["x"], b[1] - 2.8), (q["x"], b[1] - 0.6), None
    hide_val = p["sym"][0] == "Connector_Generic" and q["ref"] in ("J1", "J2")
    props = [("Reference", q["ref"], rpos, False), ("Value", value, vpos, hide_val or (q["unit"] == 3 and p.get("units") == 3)),
             ("Footprint", p["fp"], (q["x"], q["y"]), True), ("Datasheet", "~", (q["x"], q["y"]), True),
             ("Description", p["desc"], (q["x"], q["y"]), True), ("LCSC", lc[0], (q["x"], q["y"]), True), ("MPN", lc[1], (q["x"], q["y"]), True)]
    s = [Sym("symbol"), [Sym("lib_id"), f"{p['sym'][0]}:{p['sym'][1]}"], [Sym("at"), q["x"], q["y"], q["angle"]]]
    if q["mirror"]: s.append([Sym("mirror"), Sym("y")])
    s += [[Sym("unit"), q["unit"]], [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("no") if key == "-" else Sym("yes")],
          [Sym("on_board"), Sym("yes")], [Sym("dnp"), Sym("yes") if p.get("dnp") else Sym("no")], [Sym("uuid"), su]]
    for name, val, (px, py), hide in props:
        fa = q["angle"] if q["angle"] in (90, 270) else 0          # field angles are relative to the symbol
        s.append([Sym("property"), name, val, [Sym("at"), r4(px), r4(py), fa], eff(hide=hide, justify=just + " bottom" if just and name in ("Reference", "Value") else None)])
    for pp in q["pins"]: s.append([Sym("pin"), pp["num"], [Sym("uuid"), U()]])
    s.append([Sym("instances"), [Sym("project"), PROJECT, [Sym("path"), "/" + ROOT, [Sym("reference"), q["ref"]], [Sym("unit"), q["unit"]]]]])
    items.append(s)

# ------------------------------------------------------------------ nets
RAILS = S.RAILS                  # net -> power symbol (lib name); GND-like ones point down
pwr_n = [0]
def power_symbol(net, x, y, down, ang=0):
    """power symbol whose connection point is (x, y); ang rotates it (0 = natural: GND down / supply up)"""
    lib = RAILS[net]
    sym = flat("power", lib); key = f"power:{lib}"
    libsyms.setdefault(key, sym)
    pwr_n[0] += 1; ref = f"#PWR0{pwr_n[0]:03d}"
    d = 1 if down else -1
    vx, vy = {0: (0, d), 180: (0, -d), 90: (d, 0), 270: (-d, 0)}[ang]
    if not down: vx, vy = {0: (0, -1), 180: (0, 1), 90: (-1, 0), 270: (1, 0)}[ang]
    tx, ty = x + vx * 4.4, y + vy * 4.4
    items.append([Sym("symbol"), [Sym("lib_id"), key], [Sym("at"), r4(x), r4(y), ang], [Sym("unit"), 1],
                  [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("yes")], [Sym("on_board"), Sym("yes")], [Sym("dnp"), Sym("no")],
                  [Sym("uuid"), U()],
                  [Sym("property"), "Reference", ref, [Sym("at"), r4(x), r4(y), 0], eff(hide=True)],
                  [Sym("property"), "Value", net, [Sym("at"), r4(tx), r4(ty), ang if ang in (90, 270) else 0], eff()],
                  [Sym("property"), "Footprint", "", [Sym("at"), r4(x), r4(y), 0], eff(hide=True)],
                  [Sym("property"), "Datasheet", "", [Sym("at"), r4(x), r4(y), 0], eff(hide=True)],
                  [Sym("pin"), "1", [Sym("uuid"), U()]],
                  [Sym("instances"), [Sym("project"), PROJECT, [Sym("path"), "/" + ROOT, [Sym("reference"), ref], [Sym("unit"), 1]]]]])
DOWN = set(S.DOWN_RAILS)
sym_pts = []

segs = []                         # all wire segments (for junctions)
def add_wire(a, b, net=None):
    if abs(a[0] - b[0]) < 1e-6 and abs(a[1] - b[1]) < 1e-6: return
    segs.append((a, b)); items.append(wire(a, b)); wires_net.append((a, b, net))
labels = []
def add_label(net, x, y, out):
    ang = {0: 0, 180: 180, 90: 90, 270: 270}[out]
    just = {0: "left bottom", 180: "right bottom", 90: "left bottom", 270: "right bottom"}[out]
    items.append([Sym("label"), net, [Sym("at"), r4(x), r4(y), ang], eff(justify=just), [Sym("uuid"), U()]])

by_block_net = {}
for q in placed:
    p = PART[q["ref"]]
    for pp in q["pins"]:
        net = p["pins"].get(pp["num"], "__missing__")
        assert net != "__missing__", (q["ref"], pp["num"])
        if net is None:
            items.append([Sym("no_connect"), [Sym("at"), pp["X"], pp["Y"]], [Sym("uuid"), U()]]); continue
        by_block_net.setdefault((q["block"], net), []).append((q, pp))
blocks_of_net = {}
for (blk, net) in by_block_net: blocks_of_net.setdefault(net, set()).add(blk)
bodies = {blk["title"]: [q["body"] for q in placed if q["block"] == blk["title"]] for blk in S.BLOCKS}

pin_net = {}
for q in placed:
    for pp in q["pins"]:
        pin_net[(round(pp["X"], 3), round(pp["Y"], 3))] = PART[q["ref"]]["pins"].get(pp["num"])
wires_net = []                     # (a, b, net) of all drawn wires
def on_seg(p, a, b, interior=False):
    if abs(a[1] - b[1]) < 1e-6 and abs(p[1] - a[1]) < 1e-6 and min(a[0], b[0]) - 1e-6 <= p[0] <= max(a[0], b[0]) + 1e-6:
        return not interior or (min(a[0], b[0]) + 1e-6 < p[0] < max(a[0], b[0]) - 1e-6)
    if abs(a[0] - b[0]) < 1e-6 and abs(p[0] - a[0]) < 1e-6 and min(a[1], b[1]) - 1e-6 <= p[1] <= max(a[1], b[1]) + 1e-6:
        return not interior or (min(a[1], b[1]) + 1e-6 < p[1] < max(a[1], b[1]) - 1e-6)
    return False
def foreign(a, b, net):
    """penalty for touching other nets: their pins on the segment, collinear overlap, crossing"""
    pen = 0
    for pt, n in pin_net.items():
        if n != net and on_seg(pt, a, b): pen += 100000
    for (c, d, n) in wires_net:
        if n == net: continue
        if on_seg(c, a, b) or on_seg(d, a, b) or on_seg(a, c, d) or on_seg(b, c, d): pen += 100000   # touch / overlap
        elif (abs(a[1] - b[1]) < 1e-6) != (abs(c[1] - d[1]) < 1e-6):                                  # perpendicular crossing
            h, v = ((a, b), (c, d)) if abs(a[1] - b[1]) < 1e-6 else ((c, d), (a, b))
            if min(h[0][0], h[1][0]) < v[0][0] < max(h[0][0], h[1][0]) and min(v[0][1], v[1][1]) < h[0][1] < max(v[0][1], v[1][1]): pen += 3
    return pen
def crosses(a, b, blk, own=()):
    """count symbol bodies hit by segment a-b (bodies shrunk a bit)"""
    n = 0
    x0, x1 = sorted((a[0], b[0])); y0, y1 = sorted((a[1], b[1]))
    for bb in bodies[blk]:
        if bb in own: continue
        if x1 > bb[0] + 0.3 and x0 < bb[2] - 0.3 and y1 > bb[1] + 0.3 and y0 < bb[3] - 0.3: n += 1
    return n

FORCE_LABEL = set(getattr(S, "LABEL_NETS", ()))
LABEL_AT = getattr(S, "LABEL_AT", {})          # (ref, pin) -> True: put the label at this pin
for (blk, net), lst in sorted(by_block_net.items(), key=lambda kv: (kv[0][0], kv[0][1])):
    if net in RAILS:
        down = net in DOWN
        for q, pp in lst:
            ox, oy = OUTV[pp["out"]]
            x, y = pp["X"], pp["Y"]
            L = 2.54
            while any(abs(x + ox * L - px) < 4.0 and abs(y + oy * L - py) < 4.0 for px, py in sym_pts) and L < 12: L += 2.54
            e = (x + ox * L, y + oy * L); add_wire((x, y), e, net); sym_pts.append(e)
            if oy != 0:   # vertical pin: natural orientation if it points the symbol's way, else upside down
                natural = (oy == 1) == down
                power_symbol(net, e[0], e[1], down, 0 if natural else 180)
            else:         # horizontal pin: symbol turned to point outward
                if down: ang = 90 if ox == 1 else 270
                else: ang = 270 if ox == 1 else 90
                power_symbol(net, e[0], e[1], down, ang)
        continue
    LP = set(getattr(S, "LABEL_PARTS", ()))
    LON = set(getattr(S, "LABEL_ONLY_NETS", ()))
    lab_pts = [(pp["X"], pp["Y"], q, pp) for q, pp in lst if q["ref"] in LP or net in LON]
    for x, y, q, pp in lab_pts:                                       # header pins: stub + label only
        ox, oy = OUTV[pp["out"]]; e = (x + ox * 2.54, y + oy * 2.54); add_wire((x, y), e, net); add_label(net, e[0], e[1], pp["out"])
    pts = [(pp["X"], pp["Y"], q, pp) for q, pp in lst if q["ref"] not in LP and net not in LON]
    if not pts: continue
    # wiring tree: start with the left-most pin, attach the nearest pin each step
    tree_pts = [pts[0]]; rest = pts[1:]; tsegs = []
    while rest:
        best = None
        for cand in rest:
            cx, cy = cand[0], cand[1]
            options = []
            for (a, b) in tsegs:                                       # T onto an existing segment
                if abs(a[1] - b[1]) < 1e-6 and min(a[0], b[0]) <= cx <= max(a[0], b[0]):
                    options.append(((cx, cy), (cx, a[1]), None))
                if abs(a[0] - b[0]) < 1e-6 and min(a[1], b[1]) <= cy <= max(a[1], b[1]):
                    options.append(((cx, cy), (a[0], cy), None))
            for t in tree_pts:                                         # L to a tree point
                tx, ty = t[0], t[1]
                options.append(((cx, cy), (tx, cy), (tx, ty)))         # horizontal first
                options.append(((cx, cy), (cx, ty), (tx, ty)))         # vertical first
            for o in options:
                path = [o[0], o[1]] + ([o[2]] if o[2] else [])
                L = sum(abs(path[i][0] - path[i + 1][0]) + abs(path[i][1] - path[i + 1][1]) for i in range(len(path) - 1))
                own = (cand[2]["body"],)
                pen = sum(crosses(path[i], path[i + 1], blk, own) for i in range(len(path) - 1)) * 1000
                pen += sum(foreign(path[i], path[i + 1], net) for i in range(len(path) - 1))
                if best is None or L + pen < best[0]: best = (L + pen, cand, path)
        _, cand, path = best
        if best[0] >= 100000: print("WARNING: wire touches another net:", blk, net, path)
        for i in range(len(path) - 1):
            add_wire(path[i], path[i + 1], net); tsegs.append((path[i], path[i + 1]))
        tree_pts.append(cand); rest.remove(cand)
    if not (len(blocks_of_net[net]) > 1 or net in FORCE_LABEL or len(pts) == 1 or lab_pts):
        # internal node: name it with a small label on its longest horizontal wire (keeps the PCB net names)
        hs = [sg for sg in tsegs if abs(sg[0][1] - sg[1][1]) < 1e-6] or tsegs
        a, b = max(hs, key=lambda sg: abs(sg[0][0] - sg[1][0]) + abs(sg[0][1] - sg[1][1]))
        mx, my = snap((a[0] + b[0]) / 2), snap((a[1] + b[1]) / 2)
        if abs(a[1] - b[1]) < 1e-6: my = a[1]
        else: mx = a[0]
        items.append([Sym("label"), net, [Sym("at"), r4(mx), r4(my), 0], eff(1.0, justify="left bottom"), [Sym("uuid"), U()]])
        continue
    if len(blocks_of_net[net]) > 1 or net in FORCE_LABEL or len(pts) == 1 or lab_pts:
        sel = [t for t in pts if LABEL_AT.get((t[2]["ref"], t[3]["num"]))] or pts
        # label at the pin whose stub is free (no foreign pin / wire)
        cand = []
        for x, y, q, pp in sel:
            ox, oy = OUTV[pp["out"]]; e = (x + ox * 2.54, y + oy * 2.54)
            touches = any(on_seg(e, a, b) for a, b, n in wires_net if True) or any(on_seg(pt, (x, y), e) and pt != (round(x, 3), round(y, 3)) for pt in pin_net)
            cand.append((touches, (x, y, q, pp)))
        cand.sort(key=lambda c: c[0])
        if cand[0][0] and tsegs:          # no free stub: label sits on the longest horizontal wire
            hs = [sg for sg in tsegs if abs(sg[0][1] - sg[1][1]) < 1e-6] or tsegs
            a, b = max(hs, key=lambda sg: abs(sg[0][0] - sg[1][0]) + abs(sg[0][1] - sg[1][1]))
            mx, my = snap((a[0] + b[0]) / 2), snap((a[1] + b[1]) / 2)
            if abs(a[1] - b[1]) < 1e-6: my = a[1]; ang_ = 0
            else: mx = a[0]; ang_ = 90
            items.append([Sym("label"), net, [Sym("at"), r4(mx), r4(my), ang_], eff(justify="left bottom"), [Sym("uuid"), U()]])
            continue
        x, y, q, pp = cand[0][1]
        ox, oy = OUTV[pp["out"]]
        e = (x + ox * 2.54, y + oy * 2.54); add_wire((x, y), e, net); add_label(net, e[0], e[1], pp["out"])

# junctions: a wire end on the interior of another wire, or >= 3 wire ends in one point
def on_interior(p, s):
    a, b = s
    if abs(a[1] - b[1]) < 1e-6 and abs(p[1] - a[1]) < 1e-6 and min(a[0], b[0]) + 1e-6 < p[0] < max(a[0], b[0]) - 1e-6: return True
    if abs(a[0] - b[0]) < 1e-6 and abs(p[0] - a[0]) < 1e-6 and min(a[1], b[1]) + 1e-6 < p[1] < max(a[1], b[1]) - 1e-6: return True
    return False
ends = {}
for a, b in segs:
    for p in (a, b): ends[(round(p[0], 3), round(p[1], 3))] = ends.get((round(p[0], 3), round(p[1], 3)), 0) + 1
pinpts = {(round(pp["X"], 3), round(pp["Y"], 3)) for q in placed for pp in q["pins"]}
jn = set()
for p, n in ends.items():
    if n + (1 if p in pinpts else 0) >= 3 or any(on_interior(p, s) for s in segs): jn.add(p)
for p in jn: items.append([Sym("junction"), [Sym("at"), p[0], p[1]], [Sym("diameter"), 0], [Sym("color"), 0, 0, 0, 0], [Sym("uuid"), U()]])

# block frames, titles and notes
for blk in blocks:
    x, y, w, h = blk["ax"], blk["ay"], blk["w"] * G, blk["h"] * G
    items.append([Sym("rectangle"), [Sym("start"), r4(x), r4(y)], [Sym("end"), r4(x + w), r4(y + h)],
                  [Sym("stroke"), [Sym("width"), 0], [Sym("type"), Sym("dash")]], [Sym("fill"), [Sym("type"), Sym("none")]], [Sym("uuid"), U()]])
    items.append([Sym("text"), blk["title"], [Sym("at"), r4(x + 2), r4(y + 5), 0], eff(2.54, justify="left bottom", bold=True), [Sym("uuid"), U()]])
    for tx, ty, txt in blk.get("notes", []):
        items.append([Sym("text"), txt, [Sym("at"), r4(x + tx * G), r4(y + ty * G), 0], eff(1.27, justify="left top"), [Sym("uuid"), U()]])

# PWR_FLAGs for supply nets that are only fed through passive parts
flags = list(S.PWR_FLAGS)
pf = flat("power", "PWR_FLAG"); libsyms["power:PWR_FLAG"] = pf
fx, fy = (S.FLAG_AT[0] + OFF[0]) * G, (S.FLAG_AT[1] + OFF[1]) * G
for i, net in enumerate(flags):
    x = r4(fx + i * 15.24); y = r4(fy)
    s = [Sym("symbol"), [Sym("lib_id"), "power:PWR_FLAG"], [Sym("at"), x, y, 0], [Sym("unit"), 1],
         [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("yes")], [Sym("on_board"), Sym("yes")], [Sym("dnp"), Sym("no")], [Sym("uuid"), U()],
         [Sym("property"), "Reference", f"#FLG0{i + 1:02d}", [Sym("at"), x, r4(y - 6), 0], eff(hide=True)],
         [Sym("property"), "Value", "PWR_FLAG", [Sym("at"), x, r4(y - 4), 0], eff()],
         [Sym("property"), "Footprint", "", [Sym("at"), x, y, 0], eff(hide=True)],
         [Sym("property"), "Datasheet", "~", [Sym("at"), x, y, 0], eff(hide=True)],
         [Sym("pin"), "1", [Sym("uuid"), U()]],
         [Sym("instances"), [Sym("project"), PROJECT, [Sym("path"), "/" + ROOT, [Sym("reference"), f"#FLG0{i + 1:02d}"], [Sym("unit"), 1]]]]]
    items.append(s)
    if net in RAILS:
        if net in DOWN:
            e = (x, r4(y + 2.54)); add_wire((x, y), e, net); power_symbol(net, e[0], e[1], True, 0)
        else:
            e = (x, r4(y + 2.54)); add_wire((x, y), e, net); power_symbol(net, e[0], e[1], False, 180)
    else:
        e = (x, r4(y + 2.54)); add_wire((x, y), e, net); add_label(net, e[0], e[1], 270)

doc = [Sym("kicad_sch"), [Sym("version"), 20250114], [Sym("generator"), "eeschema"], [Sym("generator_version"), "9.0"],
       [Sym("uuid"), ROOT], [Sym("paper"), S.PAPER],
       [Sym("title_block"), [Sym("title"), S.TITLE], [Sym("rev"), S.REV]] + [[Sym("comment"), i + 1, c] for i, c in enumerate(S.COMMENTS)],
       [Sym("lib_symbols")] + list(libsyms.values())]
doc += items
doc += [[Sym("sheet_instances"), [Sym("path"), "/", [Sym("page"), "1"]]], [Sym("embedded_fonts"), Sym("no")]]
def passive_pins(text):
    """schem spec PASSIVE_PINS: {lib_id: {pin names}} -> those pins of the embedded library
    symbol become passive (e.g. ideal diodes wired as OR-ing elements, open-drain ST to GND)"""
    import re
    for lib_id, names in getattr(S, "PASSIVE_PINS", {}).items():
        i = text.find(f'(symbol "{lib_id}"')
        if i < 0: continue
        j = text.find('\n\t\t(symbol "', i + 10)
        j = len(text) if j < 0 else j
        blk = re.sub(r'\(pin \w+ (\w+)(?=((?:(?!\(pin ).)*?)\(name "([^"]*)")',
                     lambda m: f"(pin passive {m.group(1)}" if m.group(3) in names else m.group(0), text[i:j], flags=re.S)
        text = text[:i] + blk + text[j:]
    return text
open(OUT, "w").write(passive_pins(dump(doc) + "\n"))
json.dump({"root": ROOT, "parts": {p["ref"]: p["uuid"] for p in PARTS}, "flags": flags},
          open(OUT.replace(".kicad_sch", ".uuids.json"), "w"), indent=1)
print("written", OUT, "parts", len(PARTS), "symbols", len(placed), "wires", len(segs), "junctions", len(jn))
