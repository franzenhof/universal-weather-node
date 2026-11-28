"""Hand-assembly BOM of the Light board (grouped), from design_light.py.

usage: python3 -I bom_light.py <out.csv>
"""
import csv, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from design_light import PARTS

# value -> (part / order hint, note); generic parts: any make with these ratings
ORDER = {
    "Heltec V4 header (5V side)": ("1x18 female header 2.54 mm, 8.5 mm high (e.g. Kinghelm KH-2.54FH-1X18P-H8.5)", "J1 + J2"),
    "Heltec V4 P4 (GPIO15/16)": ("1x2 female header 2.54 mm, 8.5 mm high", "check the position with the real V4"),
    "Davis wind (RJ11)": ("RJ11/RJ12 6P6C jack, horizontal THT, unshielded (EVERCOM 5301-6P6C / Amphenol 54601-906)", ""),
    "Davis rain (RJ11)": ("RJ11/RJ12 6P6C jack, horizontal THT, unshielded (EVERCOM 5301-6P6C / Amphenol 54601-906)", ""),
    "Wind": ("terminal block 3.5 mm, 4-pole (e.g. DB125-3.5-4P / Phoenix PT 1,5/4-3,5-H)", ""),
    "Rain": ("terminal block 3.5 mm, 2-pole (e.g. DB125-3.5-2P / Phoenix PT 1,5/2-3,5-H)", ""),
    "I2C": ("terminal block 3.5 mm, 4-pole", ""),
    "1-Wire": ("terminal block 3.5 mm, 3-pole (e.g. DB125-3.5-3P / Phoenix PT 1,5/3-3,5-H)", ""),
    "Vane pull-up": ("pin header 1x2, 2.54 mm + jumper", "fit only for reed-switch vanes"),
    "SDA pull-up": ("pin header 1x2, 2.54 mm + jumper", "fit for long I2C cables"),
    "SCL pull-up": ("pin header 1x2, 2.54 mm + jumper", "fit for long I2C cables"),
    "SMLVT3V3": ("ST SMLVT3V3 TVS 3.3 V, SMB (DO-214AA)", "SMD; cathode band = pad 1 (signal side)"),
    "PESD5V0S2BT": ("Nexperia PESD5V0S2BT dual ESD TVS, SOT-23", "optional (may be left out); low capacitance, I2C / 1-Wire DQ"),
    "BAT43": ("BAT43 Schottky, DO-35", "band = cathode"),
    "LP0701N3-G": ("Microchip LP0701N3-G P-MOSFET, TO-92", "pinout S-G-D: check the flat side against the silkscreen"),
    "TN2106N3-G": ("Microchip TN2106N3-G N-MOSFET, TO-92", "pinout S-G-D"),
    "74HC14": ("SN74HC14N (DIP-14) + DIP-14 socket", "notch = pin 1"),
    "PTC 0.17A": ("Littelfuse RXEF017 (radial PTC, 5.1 mm)", ""),
    "PTC 0.1A": ("Littelfuse RXEF010 (radial PTC, 5.1 mm)", ""),
    "47uF 16V": ("electrolytic 47 uF >= 16 V, radial 5 mm, 2 mm pitch", "polarised: + = square pad"),
    "100nF": ("ceramic 100 nF 50 V X7R, 2.5 mm pitch", ""),
    "1uF": ("ceramic 1 uF 50 V X7R, 2.5 mm pitch", ""),
}
RES_HINT = "metal film 0.6 W 1 %, 0207 (10.16 mm pitch)"
SURGE = {"330": "PR01000103300JA100", "100": "PR01000101000JA100"}   # Vishay PR01, 1 W

groups = {}
for p in PARTS:
    key = p["value"] if not p["ref"].startswith("J") or p["value"] in ORDER else p["value"]
    if p["ref"] in ("J1", "J2"): key = "Heltec V4 header (5V side)"
    groups.setdefault(key, []).append(p)

def refkey(r):
    return (r.rstrip("0123456789"), int(r[len(r.rstrip("0123456789")):] or 0))

rows = []
for key, ps in groups.items():
    refs = sorted((p["ref"] for p in ps), key=refkey)
    fp = ps[0]["fp"].split(":")[1]
    if ps[0]["sym"] == ("Device", "R") and key in SURGE:
        part, note = f"resistor {key} Ohm, Vishay PR01 1 W (e.g. {SURGE[key]}) or {RES_HINT}", "surge series resistor: pulse-rated PR01 preferred (as Rev 1.3)"
    elif ps[0]["sym"] == ("Device", "R"):
        part, note = f"resistor {key} Ohm, {RES_HINT}", ""
    else:
        part, note = ORDER.get(key, (key, ""))
    rows.append((refkey(refs[0]), [" ".join(refs), len(refs), key, part, fp, note]))
rows.sort()
with open(sys.argv[1], "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["References", "Qty", "Value", "Part / order hint", "Footprint", "Note"])
    for _, r in rows: w.writerow(r)
    w.writerow(["", 1, "Heltec WiFi LoRa 32 V4", "Heltec WiFi LoRa 32 V4 (V4.2 / V4.3, not V4-R8)", "", "on J1/J2/J3"])
print(f"{sys.argv[1]}: {len(rows)} lines, {len(PARTS)} parts")
