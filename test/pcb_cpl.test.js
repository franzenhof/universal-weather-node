const {execFileSync} = require('node:child_process');
const {test} = require('node:test');
const path = require('node:path');

test('JLCPCB candidate offsets, identity guards and unchanged positions', () => {
  execFileSync(process.env.PYTHON || 'python3', ['-B', '-c', `
import csv, importlib.util, math
from pathlib import Path
spec = importlib.util.spec_from_file_location("cpl", "tools/pcbgen/jlcpcb_cpl.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
base = Path("pcb/v2.0/production")
with (base / "universal-weather-node-v2.0-cpl-jlcpcb.csv").open() as f:
    final = list(csv.DictReader(f))
parser_spec = importlib.util.spec_from_file_location("sexp", "tools/pcbgen/sexp.py")
parser = importlib.util.module_from_spec(parser_spec)
parser_spec.loader.exec_module(parser)
board = parser.parse(Path("pcb/v2.0/universal-weather-node.kicad_pcb").read_text())[0]
angles = {}
for footprint in parser.find(board, "footprint"):
    ref = next(p[2] for p in parser.find(footprint, "property") if p[1] == "Reference")
    at = parser.get(footprint, "at")
    angles[ref] = float(at[3]) if len(at) > 3 else 0
raw = [dict(row, Rotation=f"{angles[row['Designator']]:.6f}") for row in final]
# undo the Q7 centroid shift: raw KiCad Y = CPL Y + 1.4 mm
raw = [dict(r, **{"Mid Y": f"{float(r['Mid Y'][:-2]) + module.Q7_LEAD_SHIFT:.6f}mm"})
       if r["Designator"] == "Q7" else r for r in raw]
with (base / "universal-weather-node-v2.0-bom-jlcpcb.csv").open() as f:
    bom = list(csv.DictReader(f))
candidate, review = module.build(raw, bom)
assert len(candidate) == len(review) == len(raw) == 139   # +U8, U9, C39, R65 (Heltec V4 socket set)
by_ref = {r["Designator"]: r for r in candidate}
assert by_ref["U1"]["Rotation"] == "270.000000"
assert by_ref["U4"]["Rotation"] == "90.000000"
assert by_ref["U2"]["Rotation"] == "270.000000"   # UMW C5375976: +180 after the 2026-11-21 preview
assert by_ref["U3"]["Rotation"] == "0.000000"
assert by_ref["U8"]["Rotation"] == "270.000000"   # LM66100 SC-70-6: +270 after the 2026-11-21 preview
assert by_ref["U9"]["Rotation"] == "90.000000"
assert by_ref["U5"]["Rotation"] == "180.000000"
for before, after in zip(raw, candidate):
    for field in ("Designator", "Mid X", "Mid Y", "Layer"):
        if (before["Designator"], field) != ("Q7", "Mid Y"):
            assert before[field] == after[field]
    item = next(r for r in review if r["Designator"] == before["Designator"])
    assert (float(before["Rotation"]) + item["Candidate Offset"]) % 360 == float(after["Rotation"])
assert next(r for r in review if r["Designator"] == "Q8")["LCSC Part #"] == "C20526"
assert by_ref["Q8"]["Rotation"] == "270.000000"
assert by_ref["Q1"]["Rotation"] == "180.000000"
assert by_ref["Q6"]["Rotation"] == "0.000000"
assert by_ref["D27"]["Rotation"] == "270.000000"
assert by_ref["Q3"]["Rotation"] == "180.000000"
assert next(r for r in review if r["Designator"] == "Q3")["Status"] == "PREVIEW_MATCHED_DFN"
with (base / "universal-weather-node-v2.0-cpl-jlcpcb.csv").open() as f:
    assert list(csv.DictReader(f)) == candidate
assert all(r["Status"] != "MANUAL_CHECK" for r in review)
assert next(r for r in review if r["Designator"] == "C31")["Status"] == "PREVIEW_MATCHED_POLARITY"
assert by_ref["C31"]["Rotation"] == "0.000000"
assert next(r for r in review if r["Designator"] == "C31")["Candidate Offset"] == 180
assert by_ref["Q4"]["Rotation"] == "270.000000"
q7_raw = next(r for r in raw if r["Designator"] == "Q7")
assert math.isclose(float(q7_raw["Mid Y"][:-2]) - float(by_ref["Q7"]["Mid Y"][:-2]), 1.4)
assert by_ref["Q7"]["Rotation"] == "90.000000"
assert next(r for r in review if r["Designator"] == "Q7")["Status"] == "PREVIEW_MATCHED_TO252"
assert "U6" not in by_ref and "Q9" not in by_ref and "D4" not in by_ref
assert next(r for r in review if r["Designator"] == "Q4")["Candidate Offset"] == 180
assert next(r for r in review if r["Designator"] == "Q4")["Status"] == "PREVIEW_MATCHED_SOT223"
assert next(r for r in review if r["Designator"] == "C6")["Candidate Offset"] == 0
def must_fail(positions, parts):
    try:
        module.build(positions, parts)
    except ValueError:
        return
    raise AssertionError("Invalid input accepted")
must_fail(raw + [raw[0]], bom)
must_fail(candidate, bom)
must_fail([dict(r, Layer="Bottom") for r in raw], bom)
must_fail([r for r in raw if r["Designator"] != "U1"], bom)
must_fail([r for r in raw if r["Designator"] != "Q4"], bom)
must_fail([dict(r, Rotation="0.000000") if r["Designator"] == "Q4" else r for r in raw], bom)
must_fail([dict(r, Rotation="270.000000") if r["Designator"] == "Q4" else r for r in raw], bom)
changed = [dict(r) for r in bom]
next(r for r in changed if r["Designator"] == "U1")["LCSC Part #"] = "replacement"
must_fail(raw, changed)
for ref in ("Q4", "C31"):
    changed = [dict(r) for r in bom]
    next(r for r in changed if r["Designator"] == ref)["LCSC Part #"] = "replacement"
    must_fail(raw, changed)
must_fail([dict(r, Rotation="0.000000") if r["Designator"] == "C31" else r for r in raw], bom)
must_fail([dict(r, Rotation="270.000000") if r["Designator"] == "Q7" else r for r in raw], bom)
print("PASS 139 placements, Q4 and C31 +180 preview matched, Q7 centroid -1.4 mm, other XY unchanged, identity/angle guards")
`], {cwd: path.join(__dirname, '..'), stdio: 'inherit'});
});
