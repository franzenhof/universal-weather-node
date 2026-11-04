const {execFileSync} = require('node:child_process');
const {test} = require('node:test');
const path = require('node:path');

test('PCB VIN/USB manual disconnect and fan circuit match the dimensioned design', () => {
  execFileSync(process.env.PYTHON || 'python3', ['-B', '-c', `
import csv, json, sys
from pathlib import Path
sys.path.insert(0, "tools/pcbgen")
from design import PARTS
from layout_w1 import FIXED, POWER_NETS
from sexp import parse, find, get

parts = {p["ref"]: p for p in PARTS}
board = parse(Path("pcb/v2.0/universal-weather-node.kicad_pcb").read_text())[0]
footprints = {next(p[2] for p in find(f, "property") if p[1] == "Reference"): f
              for f in find(board, "footprint")}
removed = {"F2", "D4", "Q9", "R56", "U6", "R57", "R58", "R59", "R60", "C28", "C29", "C30"}
assert not removed.intersection(parts)
assert not removed.intersection(footprints)
obsolete_nets = {"5V_IN", "5V_IN_F", "5V_RP", "5V_PROT", "5V_GATE",
                 "5V_OVLO", "5V_OVLO_LOW", "5V_DVDT", "5V_ILIM"}
assert not obsolete_nets.intersection(POWER_NETS)
assert not any(net in obsolete_nets for p in PARTS for net in p["pins"].values())
for item in [*find(board, "segment"), *find(board, "via"),
             *(p for f in footprints.values() for p in find(f, "pad"))]:
    net = get(item, "net")
    assert not net or net[-1].lstrip("/") not in obsolete_nets
refs = ["J3", "JP8", "Q4", "R32", "R33", "R61", "JP12", "Q8", "R49"]
for ref in refs:
    f = footprints[ref]
    assert f[1] == parts[ref]["fp"], ref
    fields = {p[1]: p[2] for p in find(f, "property")}
    assert fields["Value"] == parts[ref]["value"], ref
    for pad in find(f, "pad"):
        wanted = parts[ref]["pins"].get(pad[1])
        if wanted is not None:
            net = get(pad, "net")[-1]
            assert net in (wanted, "/" + wanted), (ref, pad[1], net, wanted)

assert parts["J3"]["pins"] == {"1": "VIN_IN", "2": "GND"}
assert parts["JP8"]["pins"] == {"1": "+5V_BUCK", "2": "+5V"}
for ref in ("J3", "JP8"):
    assert {p[1] for p in find(footprints[ref], "pad")} == {"1", "2"}
assert parts["J1"]["pins"]["2"] == "+5V"
assert parts["J2"]["pins"]["2"] == "3V3_HELTEC"
assert parts["F3"]["pins"] == {"1": "3V3_HELTEC", "2": "+3V3"}
assert parts["JP9"]["pins"] == {"1": "+5V", "2": "FAN_SUPPLY", "3": "+VLOAD"}
assert parts["J12"]["pins"]["1"] == "+VLOAD"
assert parts["R61"]["pins"] == {"1": "+5V", "2": "TACH_PU"}
assert parts["JP12"]["pins"] == {"1": "TACH_PU", "2": "TACH"}
assert parts["Q4"]["value"] == "DMP6023LE"
assert parts["Q4"]["pins"] == {"1": "FAN_G", "2": "FAN_SW", "3": "FAN_SUPPLY"}
assert parts["R32"]["value"] == "100k"
assert parts["R33"]["value"] == "2.2k"
assert parts["R33"]["fp"].endswith("R_1206_3216Metric")
project = json.loads(Path("pcb/v2.0/universal-weather-node.kicad_pro").read_text())
assert not any(p["pattern"].lstrip("/") in obsolete_nets
               for p in project["net_settings"]["netclass_patterns"])
for ref in ["J3", "JP8", "Q4", "R33", "JP12", "R61"]:
    at = get(footprints[ref], "at")
    actual = (float(at[1])-50, float(at[2])-50, float(at[3]) if len(at)>3 else 0)
    assert all(abs(a-e)<.00001 for a,e in zip(actual, FIXED[ref])), (ref, actual, FIXED[ref])
texts = [t[1] for t in find(board, "gr_text")]
assert any("REMOVE BEFORE USB" in t for t in texts)
assert any("J3: 1 VIN 6-29.2V | 2 GND" in t for t in texts)
assert not any("2 EXT 5V" in t or "2-3 EXT 5V" in t or "2-3 5V IN" in t for t in texts)

# Minimum gate drive at a measured 4.65 V fan-supply rail, resistor corners.
vgs = 4.65 * 99000 / (99000 + 2222)
assert vgs > 4.5
# Conservative 29.2 V steady state with 11.4 V zener and resistor tolerance.
gate_resistor_power = (29.2 - 11.4)**2 / (2200*.99)
assert gate_resistor_power < .16 < .25
assert 5/10000 < .002

base = Path("pcb/v2.0/production")
bom = list(csv.DictReader((base / "universal-weather-node-v2.0-bom-jlcpcb.csv").open()))
by_ref = {r.strip(): row for row in bom for r in row["Designator"].split(",")}
assert len(bom) == 67 and len(by_ref) == 144
assert not removed.intersection(by_ref)
assert by_ref["Q4"]["LCSC Part #"] == "C154901"
assert by_ref["J3"]["LCSC Part #"] == "C466970"
assert by_ref["JP8"]["LCSC Part #"] == "C53055672"
assert by_ref["R33"]["LCSC Part #"] == "C17948"
assert by_ref["R33"]["MPN"] == "UNI-ROYAL 1206W4F2201T5E (2.2k, 1%, 0.25 W)"
assert all(r in by_ref for r in refs)
print(f"PASS VIN/USB disconnect, removed 5V branch, circuit/net/BOM checks; gate {vgs:.3f} V")
`], {cwd: path.join(__dirname, '..'), stdio: 'inherit'});
});
