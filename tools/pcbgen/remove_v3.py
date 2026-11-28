"""V4 Full Rev 1.0: remove the Heltec V3 socket set (J1/J2) from the routed v2.0 board, in place.

Deletes J1/J2, applies the new netlist (design.py without J1/J2) and the new schematic
UUIDs to every footprint, and updates the silkscreen (V3 labels gone, J19/J20 outlines
back on F.Silkscreen, board name "V4 Full"). Connections that ran through a V3 pad are
re-routed afterwards with dual_socket.py geometry / route_grid.py / apply; dangling stubs
are pruned (remove_v3.sh runs all steps).

  python3 remove_v3.py prepare <board> <netlist> <uuids.json>
"""
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew
from sexp import parse, find, get
from design import PARTS
from lcsc import LCSC, VALUE_OVERRIDE

mm, MM = pcbnew.FromMM, pcbnew.ToMM
PART = {p["ref"]: p for p in PARTS}
ROW_J1, ROW_J2 = 82.36, 59.5          # former V3 rows (board coordinates incl. the 50/50 origin offset)
RENAME = {"Heltec WiFi LoRa 32 V3 or V4": "Heltec WiFi LoRa 32 V4",
          "POPULATE ONE SOCKET SET ONLY": "V4.2 / V4.3 (not V4-R8)",
          "Universal Weather Node v2.0": "Universal Weather Node V4 Full",
          "UNIVERSAL WEATHER NODE v2.0 / franzenhof.com / CERN-OHL-P v2":
              "UNIVERSAL WEATHER NODE V4 FULL REV 1.0 / franzenhof.com / CERN-OHL-P v2"}
DELETE = {"V3: J1+J2  |  V4: J19+J20+J21 (P4)"}


def board_net(b, name):
    n = b.FindNet(name)
    if n is None:
        n = pcbnew.NETINFO_ITEM(b, name); b.Add(n)
    return n


def prepare(pcb, netlist, uuids):
    padnet = {}                                    # parse before touching the board (SWIG objects)
    for n in find(get(parse(open(netlist).read())[0], "nets"), "net"):
        for x in find(n, "node"): padnet[(get(x, "ref")[1], get(x, "pin")[1])] = get(n, "name")[1]
    b = pcbnew.LoadBoard(pcb)
    FPS = {fp.GetReference(): fp for fp in b.GetFootprints()}
    assert "J1" in FPS and "J19" in FPS, "V3 socket set already removed?"
    for ref in ("J1", "J2"):
        b.Delete(FPS.pop(ref))                     # Delete: the board frees it (Remove + GC crashes)
    uu = json.load(open(uuids))["parts"]
    for ref, fp in FPS.items():
        if ref not in PART: continue
        p = PART[ref]; lc = LCSC.get(p["lcsc_key"], ("", "", False))
        fp.SetValue(VALUE_OVERRIDE.get(p["lcsc_key"], p["value"]))
        want_f = {"LCSC": lc[0], "MPN": lc[1], "Description": p["desc"]}
        for fl in fp.GetFields():
            if fl.GetName() in want_f: fl.SetText(want_f[fl.GetName()])
        fp.SetPath(pcbnew.KIID_PATH("/" + uu[ref]))
        for pad in fp.Pads():
            if pad.GetNumber() == "": continue
            nn = padnet[(ref, pad.GetNumber())]
            want = p["pins"].get(pad.GetNumber())
            assert want is None or nn in ("/" + want, want), (ref, pad.GetNumber(), nn, want)
            pad.SetNet(board_net(b, nn))
    # silkscreen: socket outlines of the V4 rows back on the silkscreen (no V3 rows next to them)
    for ref in ("J19", "J20"):
        for g in FPS[ref].GraphicalItems():
            # library silkscreen lines are 0.12 mm wide, the fab outline 0.10 mm
            if g.GetLayer() == pcbnew.F_Fab and not hasattr(g, "GetText") and abs(MM(g.GetWidth()) - 0.12) < 0.005:
                g.SetLayer(pcbnew.F_SilkS)
    n_del = n_ren = 0
    for d in list(b.GetDrawings()):
        if not hasattr(d, "GetText"): continue
        t, p = d.GetText(), d.GetPosition()
        if t in DELETE or (d.GetLayer() == pcbnew.F_SilkS and (
                (t == "V3" and abs(MM(p.y) - ROW_J1) < 0.2 or t == "V3" and abs(MM(p.y) - ROW_J2) < 0.2) or
                (t == "1" and abs(MM(p.x) - 52.5) < 0.2 and (abs(MM(p.y) - ROW_J1) < 0.2 or abs(MM(p.y) - ROW_J2) < 0.2)))):
            b.Delete(d); n_del += 1; continue
        if t in RENAME: d.SetText(RENAME[t]); n_ren += 1
    print(f"removed J1/J2, silkscreen: {n_del} texts deleted, {n_ren} renamed")
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(pcb, b)


if __name__ == "__main__":
    if sys.argv[1] == "prepare": prepare(*sys.argv[2:5])
    else: raise SystemExit(__doc__)
