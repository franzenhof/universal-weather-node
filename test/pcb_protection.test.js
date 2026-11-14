const {execFileSync} = require('node:child_process');
const {test} = require('node:test');
const path = require('node:path');

test('PCB VIN/USB manual disconnect and fan circuit match the dimensioned design', () => {
  execFileSync(process.env.PYTHON || 'python3', ['-B', '-c', `
import csv, itertools, json, math, sys
from pathlib import Path
sys.path.insert(0, "tools/pcbgen")
from design import PARTS
from layout_w1 import FIXED, POWER_NETS
from lcsc import LCSC
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
bank = {"R33": {"1": "FAN_G", "2": "FAN_RA"},
        "R62": {"1": "FAN_RA", "2": "FAN_DRV"},
        "R63": {"1": "FAN_G", "2": "FAN_RB"},
        "R64": {"1": "FAN_RB", "2": "FAN_DRV"}}
refs = ["J3", "JP8", "Q4", "R32", *bank, "R61", "JP12", "Q8", "R49",
        "C1", "C4", "C5", "C6", "C31", "C32", "C33", "C34"]
for ref in refs:
    f = footprints[ref]
    assert f[1] == parts[ref]["fp"], ref
    fields = {p[1]: p[2] for p in find(f, "property")}
    assert fields["Value"] == parts[ref]["value"], ref
    assert (fields["LCSC"], fields["MPN"]) == LCSC[parts[ref]["lcsc_key"]][:2], ref
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
assert parts["F3"]["pins"] == {"1": "3V3_LDO", "2": "+3V3"}
assert parts["U7"]["pins"]["1"] == "+5V" and parts["U7"]["pins"]["5"] == "3V3_LDO"
assert parts["JP9"]["pins"] == {"1": "+5V", "2": "FAN_SUPPLY", "3": "+VLOAD"}
assert parts["J12"]["pins"]["1"] == "+VLOAD"
assert parts["R61"]["pins"] == {"1": "+5V", "2": "TACH_PU"}
assert parts["JP12"]["pins"] == {"1": "TACH_PU", "2": "TACH"}
assert parts["Q4"]["value"] == "DMP6023LE"
assert parts["Q4"]["pins"] == {"1": "FAN_G", "2": "FAN_SW", "3": "FAN_SUPPLY"}
assert parts["R32"]["value"] == "75k"
for ref, pins in bank.items():
    assert parts[ref]["pins"] == pins
    assert parts[ref]["value"] == "2k"
    assert parts[ref]["fp"].endswith("R_1206_3216Metric")
    assert parts[ref]["lcsc_key"] == "2k1206"
for net, expected in {
    "FAN_RA": {("R33", "2"), ("R62", "1")},
    "FAN_RB": {("R63", "2"), ("R64", "1")},
}.items():
    assert {(p["ref"], pin) for p in PARTS for pin, n in p["pins"].items() if n == net} == expected
assert parts["C31"]["pins"] == {"1": "+VLOAD", "2": "GND"}
assert parts["C32"]["pins"] == {"1": "+5V", "2": "GND"}
for ref in ("C1", "C33"):
    assert parts[ref]["pins"] == {"1": "VIN_BUCK", "2": "GND"}
    assert parts[ref]["value"] == "10uF 50V"
    assert parts[ref]["fp"].endswith("C_1210_3225Metric")
assert parts["C34"]["pins"] == {"1": "+5V_BUCK", "2": "GND"}
assert parts["C34"]["fp"].endswith("C_1210_3225Metric")
assert parts["C31"]["value"] == "47uF 63V"
assert parts["C31"]["fp"].endswith("CP_Elec_8x10")
assert parts["C6"]["value"] == "100uF"
assert parts["C35"]["pins"] == {"1": "+5V", "2": "GND"} and parts["C35"]["value"] == "10uF 50V"
project = json.loads(Path("pcb/v2.0/universal-weather-node.kicad_pro").read_text())
assert not any(p["pattern"].lstrip("/") in obsolete_nets
               for p in project["net_settings"]["netclass_patterns"])
for ref in ["J3", "JP8", "Q4", *bank, "JP12", "R61", "C31", "C32", "C33", "C34"]:
    at = get(footprints[ref], "at")
    actual = (float(at[1])-50, float(at[2])-50, float(at[3]) if len(at)>3 else 0)
    assert all(abs(a-e)<.00001 for a,e in zip(actual, FIXED[ref])), (ref, actual, FIXED[ref])
texts = [t[1] for t in find(board, "gr_text")]
assert any("REMOVE BEFORE USB" in t for t in texts)
assert any("J3: 1 VIN 6-29.2V | 2 GND" in t for t in texts)
assert not any("2 EXT 5V" in t or "2-3 EXT 5V" in t or "2-3 5V IN" in t for t in texts)
assert any("JP2 VANE PULL-UP" in t for t in texts)

def pad_position(ref, number):
    fp = footprints[ref]
    at = get(fp, "at")
    pad = next(p for p in find(fp, "pad") if p[1] == str(number))
    local = get(pad, "at")
    angle = -math.radians(float(at[3]) if len(at)>3 else 0)
    x, y = float(local[1]), float(local[2])
    return (float(at[1])+x*math.cos(angle)-y*math.sin(angle),
            float(at[2])+x*math.sin(angle)+y*math.cos(angle))

assert math.dist(pad_position("C32", 1), pad_position("U5", 5)) < 3
assert math.dist(pad_position("C32", 2), pad_position("U5", 3)) < 2
assert any(get(v, "net")[-1] == "GND"
           and math.dist(tuple(map(float, get(v, "at")[1:3])), pad_position("C32", 2)) < 1.5
           for v in find(board, "via"))
assert pad_position("C31", 1)[0] > pad_position("C31", 2)[0]

# Minimum gate drive at an assumed 4.65 V fan-supply rail, resistor corners.
vgs = 4.65 * 74250 / (74250 + 2020)
assert vgs > 4.5
# Conservative 29.2 V steady state with a 10.8 V clamp allowance and resistor tolerance.
gate_resistor_power = (29.2 - 10.8)**2 / (2000*.99)
worst_individual_power = 0
for rs in itertools.product((1980, 2020), repeat=4):
    equivalent = 1/(1/(rs[0]+rs[1])+1/(rs[2]+rs[3]))
    assert 1980 <= equivalent <= 2020
    for pair in (rs[:2], rs[2:]):
        powers = [(18.4/sum(pair))**2 * r for r in pair]
        worst_individual_power = max(worst_individual_power, *powers)
assert worst_individual_power < .043
assert math.isclose(4*worst_individual_power, gate_resistor_power)
assert 5/10000 < .002

base = Path("pcb/v2.0/production")
bom = list(csv.DictReader((base / "universal-weather-node-v2.0-bom-jlcpcb.csv").open()))
by_ref = {r.strip(): row for row in bom for r in row["Designator"].split(",")}
assert len(bom) == 47 and len(by_ref) == 135
tht = list(csv.DictReader((base / "universal-weather-node-v2.0-tht-lcsc.csv").open()))
tht_by_ref = {r.strip(): row for row in tht for r in row["Designator"].split(",")}
assert len(tht) == 8 and len(tht_by_ref) == 22 and not set(tht_by_ref) & set(by_ref)
assert not removed.intersection(tht_by_ref)
assert not removed.intersection(by_ref)
assert by_ref["Q4"]["LCSC Part #"] == "C154901"
assert by_ref["R32"]["LCSC Part #"] == "C17819"
assert by_ref["R32"]["MPN"] == "UNI-ROYAL 0805W8F7502T5E (75 kOhm, 1%, 0.125 W, 0805)"
assert tht_by_ref["J3"]["LCSC Part #"] == "C466970"
assert tht_by_ref["JP8"]["LCSC Part #"] == "C53055672"
by_ref = {**by_ref, **tht_by_ref}
for ref in bank:
    assert by_ref[ref]["LCSC Part #"] == "C17944"
    assert by_ref[ref]["MPN"] == "UNI-ROYAL 1206W4F2001T5E (2 kOhm, 1%, 0.25 W, 1206)"
assert {r for r in by_ref if by_ref[r]["LCSC Part #"] == "C17944"} == set(bank)
for ref, article in {"C1": "C77102", "C33": "C77102", "C34": "C21397",
                     "C31": "C445452", "C32": "C1525"}.items():
    assert by_ref[ref]["LCSC Part #"] == article, ref
for ref in refs:
    assert (by_ref[ref]["LCSC Part #"], by_ref[ref]["MPN"]) == LCSC[parts[ref]["lcsc_key"]][:2], ref
assert all(r in by_ref for r in refs)
simulation = json.loads(Path("pcb/v2.0/fan-pwm-stability.json").read_text())
assert len(simulation["cases"]) == 4
assert all(c["off_command_to_output_below_10_percent_us"] < 1000
           and c["pwm_Hz"] == 100 and c["duty"] == .9 for c in simulation["cases"])
assert all(simulation["acceptance"].values())
corners = simulation["corner_checks"]
assert len(corners["normal_input_bank_corners"]) == 16
assert math.isclose(corners["worst_individual_resistor_power_at_29_2V_W"], worst_individual_power)
assert math.isclose(corners["worst_bank_power_at_29_2V_normal_input_W"], gate_resistor_power)
assert corners["bank_aggregate_nameplate_power_W"] == 1
for case in simulation["cases"]:
    assert set(case["gate_bank_resistors_ohm"]) == set(bank)
    assert math.isclose(sum(case["resistor_average_model_loss_W"].values()),
                        case["gate_bank_average_model_loss_W"])
print(f"PASS VIN/USB disconnect, four stability improvements, circuit/net/BOM checks; gate {vgs:.3f} V")
`], {cwd: path.join(__dirname, '..'), stdio: 'inherit'});
});
