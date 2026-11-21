import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, find, get
from design import PARTS
import json
d = json.load(open(sys.argv[2]))
v = [x for s in d['sheets'] for x in s['violations']]
print("ERC violations:", len(v), sorted({(x['severity'], x['type']) for x in v}))
n = parse(open(sys.argv[1]).read())[0]
got = {}
for net in find(get(n, 'nets'), 'net'):
    name = get(net, 'name')[1]
    nodes = frozenset((get(x, 'ref')[1], get(x, 'pin')[1]) for x in find(net, 'node') if not get(x, 'ref')[1].startswith('#'))
    if nodes: got[name] = nodes
want = {}
for p in PARTS:
    for pin, net in p['pins'].items():
        if net: want.setdefault(net, set()).add((p['ref'], pin))
gotn = {k.lstrip('/'): v for k, v in got.items()}
bad = 0
for k, v in want.items():
    if gotn.get(k) != frozenset(v): bad += 1; print("MISMATCH", k, sorted(v), sorted(gotn.get(k, [])))
extra = [k for k, v in gotn.items() if k not in want and len(v) > 1]
print("nets wanted", len(want), "mismatches", bad, "extra multi-pin nets", extra)

# ---- Heltec double socket: every function on its intended V3 and V4 pin, forbidden pins open ----
# function: (net, V3 pin, V4 pin); V3 = J1 (5V side) / J2 (3V3 side), V4 = J19 / J20 + P4 socket J21
HELTEC = {
    "GND":              ("GND",        [("J1", "1"), ("J2", "1")], [("J19", "1"), ("J20", "1")]),
    "5V":               ("+5V",        [("J1", "2")], []),
    "5V V4 (VBUS)":     ("+5V_V4",     [], [("J19", "2")]),
    "3V3":              ("3V3_HELTEC", [("J2", "2"), ("J2", "3")], [("J20", "2"), ("J20", "3")]),
    "wind direction":   ("WDIR_ADC",   [("J2", "13")], [("J20", "17")]),     # V3 GPIO2  / V4 GPIO6
    "UV":               ("UV_ADC",     [("J2", "14")], [("J20", "14")]),     # GPIO3
    "AUX analog":       ("AIN_ADC",    [("J2", "15")], [("J20", "15")]),     # GPIO4
    "1-Wire":           ("ONEWIRE_IO", [("J2", "16")], [("J19", "6")]),      # V3 GPIO5  / V4 GPIO43
    "wind speed":       ("WSPD_OUT",   [("J2", "17")], [("J19", "5")]),      # V3 GPIO6  / V4 GPIO44
    "rain":             ("RAIN_OUT",   [("J2", "18")], [("J21", "2")]),      # V3 GPIO7  / V4 GPIO16
    "heater":           ("HEAT_PWM",   [("J2", "11")], [("J20", "11")]),     # GPIO38
    "fan":              ("FAN_PWM",    [("J2", "10")], [("J20", "10")]),     # GPIO39
    "tacho":            ("TACH_OUT",   [("J2", "9")], [("J20", "9")]),       # GPIO40
    "SDA":              ("SDA",        [("J2", "8")], [("J20", "8")]),       # GPIO41
    "SCL":              ("SCL",        [("J2", "7")], [("J20", "7")]),       # GPIO42
    "VE.Direct TX":     ("VE_TX_MCU",  [("J1", "11")], [("J20", "6")]),      # V3 GPIO34 / V4 GPIO45
    "VE.Direct RX":     ("VE_RX_MCU",  [("J1", "12")], [("J19", "12")]),     # GPIO33
    "1-Wire switch":    ("GPIO47",     [("J1", "13")], [("J19", "13")]),     # GPIO47
    "AUX pulse":        ("PULSE_OUT",  [("J1", "14")], [("J19", "14")]),     # GPIO48
    "revision ID":      ("HW_ID",      [("J1", "17")], [("J21", "1")]),      # V3 GPIO20 / V4 GPIO15
    "vane enable":      ("VANE_EN",    [("J1", "18")], [("J19", "11")]),     # V3 GPIO19 / V4 GPIO34
}
OPEN = {"J1": "3 4 5 6 7 8 9 10 15 16", "J2": "4 5 6 12",
        "J19": "3 4 7 8 9 10 15 16 17 18",     # Ve, RST, GPIO0, 36, 35, 26, 21, 20 (D+), 19 (D-)
        "J20": "4 5 12 13 16 18"}              # GPIO37, 46, 1, 2, 5, 7
pin_net = {(r, p): k for k, v in gotn.items() for r, p in v if not k.startswith('unconnected-')}   # single-pin no-connect nets = open
hb = 0
for fn, (net, v3, v4) in HELTEC.items():
    for rp in v3 + v4:
        if pin_net.get(rp) != net: hb += 1; print("HELTEC MISMATCH", fn, rp, "is", pin_net.get(rp), "want", net)
for ref, pins in OPEN.items():
    for p in pins.split():
        if (ref, p) in pin_net: hb += 1; print("HELTEC PIN MUST BE OPEN", ref, p, "on", pin_net[(ref, p)])
used = {rp for _, v3, v4 in HELTEC.values() for rp in v3 + v4}
for (r, p), n in pin_net.items():
    if r in ("J1", "J2", "J19", "J20", "J21") and (r, p) not in used: hb += 1; print("HELTEC UNLISTED PIN", r, p, n)
print("Heltec V3/V4 pin map:", len(HELTEC), "functions,", "OK" if hb == 0 else f"{hb} ERRORS")
