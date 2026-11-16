"""Adds J18 (DC barrel jack 5.5x2.1, XKB DC-005-5A-2.0, hand-soldered THT) to the
routed v2.0 board in place: moves F7/D1/R45 to the right of it, re-routes the
local VIN_IN / VIN_F / PWR_GATE / GND copper and refills the pours. The rest of
the routing stays untouched. Run with KiCad's Python:
  python3 tools/pcbgen/add_dcjack.py pcb/v2.0/universal-weather-node.kicad_pcb <J18 symbol uuid>
"""
import sys
import pcbnew

PCB, SYM_UUID = sys.argv[1], sys.argv[2]
FPDIR = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
b = pcbnew.LoadBoard(PCB)
mm, V = pcbnew.FromMM, lambda x, y: pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))
net = lambda n: b.FindNet(n)
FPS = {fp.GetReference(): fp for fp in b.GetFootprints()}   # before any Remove() (SWIG loses the type afterwards)
assert "J18" not in FPS, "J18 already on the board"

# ---- remove the copper around the old F7/D1/R45 positions ----
def near(p, x, y, tol=0.03):
    return abs(pcbnew.ToMM(p.x) - x) < tol and abs(pcbnew.ToMM(p.y) - y) < tol
def seg_is(t, x1, y1, x2, y2):
    s, e = t.GetStart(), t.GetEnd()
    return (near(s, x1, y1) and near(e, x2, y2)) or (near(s, x2, y2) and near(e, x1, y1))
REMOVE_SEGS = {
    "/VIN_F":    [(59.67, 126.8, 59.5, 126.97), (59.5, 126.97, 59.5, 129.6), (59.5, 129.6, 71.09, 129.6)],
    "/PWR_GATE": [(64.14, 125.0, 64.14, 122.25), (63.87, 125.27, 64.14, 125.0)],
    "GND":       [(59.67, 122.5, 59.67, 123.0), (59.67, 123.0, 63.76, 127.09), (63.76, 127.09, 63.87, 127.09),
                  (59.67, 122.5, 59.67, 121.2)],
}
REMOVE_VIAS = [(54.95, 126.95), (54.95, 130.95), (62.95, 126.95), (62.95, 134.95), (62.95, 138.95), (66.95, 134.95), (70.95, 134.95)]
removed = 0
for t in list(b.GetTracks()):
    n = t.GetNetname()
    if t.GetClass() == "PCB_VIA":
        p = t.GetPosition()
        if n == "/VIN_IN" or (n == "GND" and any(near(p, x, y) for x, y in REMOVE_VIAS)):
            b.Delete(t); removed += 1
    elif n == "/VIN_IN" or any(seg_is(t, *s) for s in REMOVE_SEGS.get(n, [])):
        b.Delete(t); removed += 1
print("removed tracks/vias:", removed)

# ---- parts ----
def move(ref, x, y, rot):
    f = FPS[ref]
    f.SetPosition(V(x, y)); f.SetOrientationDegrees(rot)
    return f
move("F7", 67.3, 131.5, 90)     # pin 1 (VIN_IN) bottom, pin 2 (VIN_F) top
move("D1", 71.6, 134.0, -90)    # pin 1 (VIN_F) top, pin 2 (GND) bottom
move("R45", 71.3, 124.9, 0)     # pin 1 (PWR_GATE) left, pin 2 (GND) right

j = pcbnew.FootprintLoad(f"{FPDIR}/Connector_BarrelJack.pretty", "BarrelJack_Horizontal")
j.SetFPID(pcbnew.LIB_ID("Connector_BarrelJack", "BarrelJack_Horizontal"))
j.SetReference("J18"); j.SetValue("DC jack 5.5x2.1")
for k, v in (("LCSC", "C381116"), ("MPN", "XKB DC-005-5A-2.0 (5.5x2.1 mm, 5 A, 24 V, THT)"),
             ("Description", "alternative VIN input parallel to J3 (center positive), hand-soldered THT; 5 A / 24 V "
                             "contact rating - use J3 above 24 V (8s LiFePO4); never both inputs at once. "
                             "XKB DC-005-5A-2.0 pins: 1 center, 2 sleeve, 3 switch")):
    j.SetField(k, v); j.GetField(k).SetVisible(False)
j.SetPath(pcbnew.KIID_PATH("/" + SYM_UUID))
j.SetSheetname("/"); j.SetSheetfile("universal-weather-node.kicad_sch")
j.SetPosition(V(63.3, 129.0)); j.SetOrientationDegrees(0)   # opening to the left board edge (front 0.45 mm beyond it)
j.Value().SetVisible(False)
j.Reference().SetTextSize(pcbnew.VECTOR2I(mm(1.0), mm(1.0))); j.Reference().SetTextThickness(mm(0.15))
for pad in j.Pads():
    pad.SetNet(net("/VIN_IN") if pad.GetNumber() == "1" else net("GND"))
    pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)   # solid ground for up to 5 A
b.Add(j)
j.Reference().SetPosition(V(56.0, 123.0))

# ---- new copper ----
def trk(n, w, layer, *pts):
    for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b); t.SetStart(V(x1, y1)); t.SetEnd(V(x2, y2))
        t.SetWidth(mm(w)); t.SetLayer(layer); t.SetNet(net(n)); b.Add(t)
def via(n, x, y, d=0.8, drill=0.4):
    v = pcbnew.PCB_VIA(b); v.SetPosition(V(x, y)); v.SetWidth(mm(d)); v.SetDrill(mm(drill)); v.SetNet(net(n)); b.Add(v)
F, B = pcbnew.F_Cu, pcbnew.B_Cu
# VIN_IN: J3.1 -> F7.1 (top), J3.1 -> J18.1 (bottom), J18.1 -> F7.1 (top), all 1.5 mm
trk("/VIN_IN", 1.5, F, (60.0, 145.0), (60.0, 140.2), (63.0, 137.2), (65.9, 137.2), (67.3, 135.8), (67.3, 133.96))
trk("/VIN_IN", 1.5, B, (60.0, 145.0), (60.0, 140.2), (63.3, 136.9), (63.3, 129.0))
trk("/VIN_IN", 1.5, F, (63.3, 129.0), (63.3, 131.2), (65.3, 133.2), (67.3, 133.96))
# VIN_F: F7.2 and D1.1 to the existing via cluster at y 129.6 (B.Cu riser to Q7)
trk("/VIN_F", 1.5, F, (67.3, 129.04), (68.0, 129.6), (69.89, 129.6))
trk("/VIN_F", 1.5, F, (71.6, 131.85), (71.09, 129.6))
# PWR_GATE: R45.1 -> via -> B.Cu to the existing gate via at (68.51, 120.13)
via("/PWR_GATE", 69.2, 125.9)
trk("/PWR_GATE", 0.3, F, (70.35, 124.9), (69.2, 125.9))
trk("/PWR_GATE", 0.3, B, (69.2, 125.9), (68.51, 125.21), (68.51, 120.13))
# GND: D1.2 to the existing stitching via
trk("GND", 0.6, F, (71.6, 136.15), (71.6, 138.3), (70.95, 138.95))

# ---- silkscreen note beside the jack ----
t = pcbnew.PCB_TEXT(b); t.SetText("DC IN 5.5x2.1\n+ CENTER, MAX 24 V"); t.SetLayer(pcbnew.F_SilkS)
t.SetTextSize(pcbnew.VECTOR2I(mm(0.8), mm(0.8))); t.SetTextThickness(mm(0.12))
t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT); t.SetPosition(V(50.8, 120.6)); b.Add(t)

filler = pcbnew.ZONE_FILLER(b); filler.Fill(b.Zones())
pcbnew.SaveBoard(PCB, b)
print("saved", PCB)
