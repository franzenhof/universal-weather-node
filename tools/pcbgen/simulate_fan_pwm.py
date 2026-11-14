#!/usr/bin/env python3
"""Reproduce the Q4 100 Hz / 90% PWM turn-off check with KiCad's ngspice."""

import ctypes
import itertools
import json
import math
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).with_name("out")
MODEL = OUT / "DMP6023LE.spice.txt"
RESULT = ROOT / "pcb" / "v2.0" / "fan-pwm-stability.json"
NGSPICE = Path("/Applications/KiCad/KiCad.app/Contents/PlugIns/sim")
ENGINE = NGSPICE / "libngspice.0.dylib"
CODEMODELS = NGSPICE / "ngspice"
BANK_REFS = ("R33", "R62", "R63", "R64")


def run_netlist(path: Path) -> None:
    messages = []
    send_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p)
    exit_type = ctypes.CFUNCTYPE(
        ctypes.c_int, ctypes.c_int, ctypes.c_bool, ctypes.c_bool, ctypes.c_int, ctypes.c_void_p
    )

    @send_type
    def output(text, _ident, _user):
        messages.append(text.decode(errors="replace"))
        return 0

    @exit_type
    def controlled_exit(status, _immediate, _quit_requested, _ident, _user):
        messages.append(f"controlled exit {status}")
        return 0

    engine = ctypes.CDLL(str(ENGINE))
    engine.ngSpice_Init.argtypes = [
        send_type,
        ctypes.c_void_p,
        exit_type,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    engine.ngSpice_Command.argtypes = [ctypes.c_char_p]
    assert engine.ngSpice_Init(output, None, controlled_exit, None, None, None, None) == 0
    assert engine.ngSpice_Command(b"set ngbehavior=ps") == 0
    for model in ("analog", "digital", "spice2poly", "xtradev", "xtraevt", "table", "tlines"):
        command = f"codemodel {CODEMODELS / (model + '.cm')}".encode()
        assert engine.ngSpice_Command(command) == 0
    assert engine.ngSpice_Command(f"source {path}".encode()) == 0
    errors = [
        message
        for message in messages
        if "error" in message.lower()
        or "aborted" in message.lower()
        or "failed" in message.lower()
        or "controlled exit" in message.lower()
    ]
    if errors:
        raise RuntimeError("\n".join(errors))


def average(xs, ys, start, end):
    pairs = [(x, y) for x, y in zip(xs, ys) if start <= x <= end]
    return sum(
        (b[0] - a[0]) * (a[1] + b[1]) / 2 for a, b in zip(pairs, pairs[1:])
    ) / (pairs[-1][0] - pairs[0][0])


def bank_corners(voltage):
    results = []
    for factors in itertools.product((.99, 1.01), repeat=4):
        resistors = [2_000 * f for f in factors]
        currents = [voltage / sum(resistors[:2]), voltage / sum(resistors[2:])]
        powers = [r * currents[i // 2] ** 2 for i, r in enumerate(resistors)]
        results.append({
            "resistors_ohm": dict(zip(BANK_REFS, resistors)),
            "effective_ohm": 1 / (1 / sum(resistors[:2]) + 1 / sum(resistors[2:])),
            "power_W": dict(zip(BANK_REFS, powers)),
            "total_power_W": sum(powers),
        })
    return results


def simulate_once(supply, load, r32=75_000, bank_resistor=2_000):
    name = f"fan-pwm-{str(supply).replace('.', '_')}"
    netlist = OUT / f"{name}.cir"
    datafile = OUT / f"{name}.dat"
    netlist.write_text(
        f"""Q4 PWM turn-off check; Diodes DMP6023LE official SPICE model.
.include {MODEL}
Vrail supply 0 {supply}
Vsense supply src 0
Vctrl ctrl 0 PULSE(0 3.3 1m 100n 100n 9m 10m)
Sdriver drv 0 ctrl 0 DRIVER
.model DRIVER SW(RON=1 ROFF=1e12 VT=1.5 VH=.1)
R32 src gate {r32}
R33 gate ra {bank_resistor}
R62 ra drv {bank_resistor}
R63 gate rb {bank_resistor}
R64 rb drv {bank_resistor}
D26 gate src DZ
.model DZ D(BV=12 IBV=5m RS=5 CJO=100p)
Vgate gate qgate 0
XQ4 out qgate src DMP6023LE
Rload out 0 {load}
Rleak drv 0 1e12
.control
tran 2u 22m 0 2u
set wr_singlescale
set wr_vecnames
wrdata {datafile.name} v(src) v(gate) v(out) v(drv) i(Vgate) v(ra) v(rb)
.endc
.end
"""
    )
    old_dir = Path.cwd()
    try:
        os.chdir(OUT)
        run_netlist(netlist)
        rows = datafile.read_text().splitlines()
        header = rows[0].split()
        values = [[float(v) for v in row.split()] for row in rows[1:] if row.strip()]
        assert values and all(math.isfinite(value) for row in values for value in row)
        signals = {column: [row[i] for row in values] for i, column in enumerate(header)}
        times = signals["time"]
        off_command = 0.0100001
        start = next(i for i, time in enumerate(times) if time >= off_command)
        off10 = next(
            (i for i in range(start, len(times)) if signals["v(out)"][i] < 0.1 * supply),
            None,
        )
        off90 = next(
            (i for i in range(start, len(times)) if signals["v(out)"][i] < 0.9 * supply),
            None,
        )
        assert off10 is not None and times[off10] < 0.011
        on_index = min(range(len(times)), key=lambda i: abs(times[i] - 0.005))
        q4_power = [
            (source - output) * output / load + (gate - source) * gate_current
            for source, gate, output, gate_current in zip(
                signals["v(src)"], signals["v(gate)"],
                signals["v(out)"], signals["i(Vgate)"]
            )
        ]
        resistor_power = {
            ref: [(a-b)**2 / bank_resistor for a, b in zip(signals[left], signals[right])]
            for ref, left, right in (
                ("R33", "v(gate)", "v(ra)"), ("R62", "v(ra)", "v(drv)"),
                ("R63", "v(gate)", "v(rb)"), ("R64", "v(rb)", "v(drv)"),
            )
        }
        bank_power = [sum(powers) for powers in zip(*resistor_power.values())]
        zener_power = []
        for source, gate, drive, gate_current in zip(
            signals["v(src)"], signals["v(gate)"], signals["v(drv)"], signals["i(Vgate)"]
        ):
            bank_current = (gate - drive) / bank_resistor
            r32_current = (source - gate) / r32
            zener_current = bank_current + gate_current - r32_current
            zener_power.append((source - gate) * zener_current)
        return {
            "fan_supply_V": supply,
            "resistive_load_ohm": load,
            "R32_ohm": r32,
            "gate_bank_resistors_ohm": dict.fromkeys(BANK_REFS, bank_resistor),
            "gate_bank_effective_ohm": bank_resistor,
            "pwm_Hz": 100,
            "duty": 0.9,
            "on_gate_drive_V": signals["v(src)"][on_index] - signals["v(gate)"][on_index],
            "off_command_to_output_below_10_percent_us": (times[off10] - off_command) * 1e6,
            "output_90_to_10_percent_us": (times[off10] - times[off90]) * 1e6,
            "Q4_average_model_loss_W": average(times, q4_power, 0.010, 0.020),
            "gate_bank_average_model_loss_W": average(times, bank_power, 0.010, 0.020),
            "resistor_average_model_loss_W": {
                ref: average(times, power, 0.010, 0.020) for ref, power in resistor_power.items()
            },
            "D26_average_model_loss_W": average(times, zener_power, 0.010, 0.020),
        }
    finally:
        os.chdir(old_dir)
        netlist.unlink(missing_ok=True)
        datafile.unlink(missing_ok=True)


def simulate(supply, load, r32=75_000, bank_resistor=2_000):
    completed = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve()),
            "--case",
            str(supply),
            str(load),
            str(r32),
            str(bank_resistor),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode:
        raise RuntimeError(completed.stderr or completed.stdout)
    return json.loads(completed.stdout)


def main():
    if not MODEL.is_file():
        raise FileNotFoundError(
            f"Download the official model to {MODEL}: "
            "https://www.diodes.com/spice/download/2345/DMP6023LE.spice.txt"
        )
    OUT.mkdir(exist_ok=True)
    minimum_gate_drive = 4.65 * (75_000 * 0.99) / (75_000 * 0.99 + 2_000 * 1.01)
    normal_corners = bank_corners(29.2 - 10.8)
    transient_corners = bank_corners(48.4 - 10.8)
    max_normal_bank_power = max(c["total_power_W"] for c in normal_corners)
    max_normal_resistor_power = max(p for c in normal_corners for p in c["power_W"].values())
    max_normal_zener_power = 10.8 * (29.2 - 10.8) / (2_000 * 0.99)
    results = {
        "scope": "Nominal subcircuit SPICE only; not a whole-board or real-fan qualification.",
        "model": "Diodes Incorporated DMP6023LE model, v1.1 (2021-12-22)",
        "model_url": "https://www.diodes.com/spice/download/2345/DMP6023LE.spice.txt",
        "other_approximations": [
            "Q5 AO3422 represented by an ideal voltage-controlled switch.",
            "D26 represented by a nominal 12 V zener model, not its vendor model or a temperature corner.",
            "Fan represented by a resistor (0.5 A at 5 V; 1 A at 29.2 V), not a BLDC motor.",
            "No cable, source impedance, PCB parasitics, thermal coupling, or fan inductance.",
        ],
        "cases": [
            {
                "condition": "assumed minimum 4.65 V fan supply, worst gate-drive resistor corner",
                **simulate(4.65, 9.3, 75_000 * 0.99, 2_000 * 1.01),
            },
            {"condition": "5 V nominal fan supply", **simulate(5.0, 10.0)},
            {"condition": "29.2 V nominal fan supply", **simulate(29.2, 29.2)},
            {
                "condition": "29.2 V, slow turn-off resistor corner",
                **simulate(29.2, 29.2, 75_000 * 1.01, 2_000 * 1.01),
            },
        ],
        "corner_checks": {
            "R32_75k_tolerance_percent": 1,
            "bank_resistor_2k_tolerance_percent": 1,
            "bank_topology": "(R33 + R62) || (R63 + R64)",
            "individual_resistor_tolerance_corner_count": len(normal_corners),
            "normal_input_bank_corners": normal_corners,
            "minimum_gate_drive_at_assumed_4_65V_supply_V": minimum_gate_drive,
            "minimum_gate_drive_required_V": 4.5,
            "worst_bank_power_at_29_2V_normal_input_W": max_normal_bank_power,
            "worst_individual_resistor_power_at_29_2V_W": max_normal_resistor_power,
            "per_resistor_nameplate_power_W": 0.25,
            "bank_aggregate_nameplate_power_W": 1.0,
            "individual_resistor_power_margin_at_29_2V_W": 0.25 - max_normal_resistor_power,
            "power_rating_caveat": "Nameplate sum only; PCB thermal coupling, ambient derating and pulse capability remain unqualified.",
            "worst_D26_power_at_29_2V_normal_input_W": max_normal_zener_power,
            "D26_assumed_rating_W": 0.5,
            "D26_power_at_48_4V_TVS_max_clamp_W": 10.8 * (48.4 - 10.8) / (2_000 * 0.99),
            "bank_power_at_48_4V_TVS_max_clamp_W": max(c["total_power_W"] for c in transient_corners),
            "individual_resistor_power_at_48_4V_TVS_max_clamp_W": max(
                p for c in transient_corners for p in c["power_W"].values()
            ),
            "48_4V_state": "transient clamp only; resistor pulse qualification is not established by this model",
        },
        "acceptance": {
            "turn_off_window_us": 1000,
            "all_cases_below_10_percent_before_next_PWM_on": True,
            "minimum_gate_drive_pass": minimum_gate_drive >= 4.5,
            "normal_input_each_resistor_power_below_nameplate": max_normal_resistor_power < 0.25,
            "normal_input_D26_power_below_assumed_rating": max_normal_zener_power < 0.5,
        },
    }
    assert results["acceptance"]["minimum_gate_drive_pass"]
    assert results["acceptance"]["normal_input_each_resistor_power_below_nameplate"]
    assert results["acceptance"]["normal_input_D26_power_below_assumed_rating"]
    assert all(
        case["off_command_to_output_below_10_percent_us"] < 1000 for case in results["cases"]
    )
    RESULT.write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    if len(sys.argv) == 6 and sys.argv[1] == "--case":
        print(
            json.dumps(
                simulate_once(
                    float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
                )
            )
        )
    else:
        main()
