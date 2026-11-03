const {execFileSync} = require('node:child_process');
const {test} = require('node:test');
const path = require('node:path');

test('JLCPCB candidate offsets, identity guards and unchanged positions', () => {
  execFileSync(process.env.PYTHON || 'python3', ['-B', '-c', `
import csv, importlib.util
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
with (base / "universal-weather-node-v2.0-bom-jlcpcb.csv").open() as f:
    bom = list(csv.DictReader(f))
candidate, review = module.build(raw, bom)
assert len(candidate) == len(review) == len(raw) == 125
by_ref = {r["Designator"]: r for r in candidate}
assert by_ref["U1"]["Rotation"] == "270.000000"
assert by_ref["U4"]["Rotation"] == "90.000000"
assert by_ref["U2"]["Rotation"] == "0.000000"
assert by_ref["U3"]["Rotation"] == "90.000000"
assert by_ref["U5"]["Rotation"] == "180.000000"
for before, after in zip(raw, candidate):
    for field in ("Designator", "Mid X", "Mid Y", "Layer"):
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
changed = [dict(r) for r in bom]
next(r for r in changed if r["Designator"] == "U1")["LCSC Part #"] = "replacement"
must_fail(raw, changed)
print("PASS 125 placements, IC/SC-70/DFN and article-matched SOT-23 offsets, unchanged XY, Elko unchanged, invalid-input guards")
`], {cwd: path.join(__dirname, '..'), stdio: 'inherit'});
});
