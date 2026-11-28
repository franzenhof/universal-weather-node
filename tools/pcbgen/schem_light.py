"""Schematic placement for the Light board (gen_sch2.py with DESIGN=design_light).

Coordinates in 2.54 mm grid units, relative to the block origin. Orientation cheat sheet:
  R/C/F: a=90 horizontal (pin 1 left), a=270 (pin 1 right), a=0 vertical (pin 1 top)
  diodes: a=270 vertical (K top), a=180 horizontal (K right)
  FET: a=0 -> G left, D top, S bottom; P-FET a=180 -> G right, S top, D bottom
"""
TITLE = "Universal Weather Node"
REV = "V4 Light Rev 1.0 (THT)"
COMMENTS = ["Hand-solderable through-hole board for the Heltec WiFi LoRa 32 V4; SMB TVS diodes only SMD",
            "Same GPIOs and signal conditioning as the full board (V4 socket set); powered through the Heltec USB-C",
            "franzenhof.com"]
PAPER = "A2"

RAILS = {"GND": "GND", "+3V3": "+3V3", "3V3_HELTEC": "+3V3"}
DOWN_RAILS = ["GND"]
PWR_FLAGS = ["GND", "+3V3", "3V3_HELTEC"]
LABEL_ONLY_NETS = set()
FLAG_AT = (130, 6)
PASSIVE_PINS = {}
LABEL_PARTS = ["J1", "J2", "J3", "J4", "J5", "J6", "J7", "J8", "J9"]
LABEL_AT = {("C1", "1"): True, ("R24", "2"): True, ("C9", "1"): True}

M = {"m": True}
H, HR, V, VF = {"a": 90}, {"a": 270}, {"a": 0}, {"a": 180}
DK = {"a": 270}                             # diode K top
PF = {"a": 180}                             # P-FET: G right, S top, D bottom

BLOCKS = [
    dict(title="Heltec WiFi LoRa 32 V4 (J1 5 V side, J2 3V3 side, J3 = P4)", x=2, y=2, w=60, h=56, parts=[
        ("J1", 16, 28), ("J2", 40, 28, M), ("J3", 40, 50, M),
        ("R1", 8, 44, V), ("R2", 8, 50, V), ("C1", 13, 50, V)],
        notes=[(3, 6, "V4.2 / V4.3 only. Pin 1 = GND (USB-C end). J1 pin 2 (USB VBUS) stays open:\n"
                      "the module is powered through its USB-C connector."),
               (3, 40, "HW revision ID (GPIO15):\nLight = 1M / 470k -> 0.32 x 3V3\n(full board: 1M / 1M)"),
               (24, 54, "Open on purpose: GPIO2/5/7/46 (RF front end), 19/20 (USB), 26, 0, 1, 21, 35-37")]),
    dict(title="3.3 V sensor rail", x=64, y=2, w=60, h=22, parts=[
        ("F1", 8, 8, H), ("C2", 16, 11, V), ("C3", 20, 11, V), ("D1", 24, 10.5, DK)],
        notes=[(30, 8, "From the Heltec 3.3 V through a 170 mA PTC;\nTVS on the rail, the BAT43 clamps of all\ninputs dump into it.")]),
    dict(title="Wind (Davis 6410)", x=64, y=26, w=100, h=42, parts=[
        ("J4", 5, 12, M), ("J5", 5, 22, M),
        ("D3", 16, 9.5, DK), ("R3", 20, 8, H), ("R4", 24, 4, V), ("D4", 28, 4.5, DK), ("D5", 28, 11.5, DK),
        ("R5", 33, 8, H), ("C4", 37, 9.5, V), ("U1", 45, 8, {"u": 1}), ("R6", 54, 8, H),
        ("D2", 16, 29.5, DK), ("R7", 20, 28, H), ("R8", 24, 21, V), ("JP1", 28, 24, M),
        ("D6", 32, 24.5, DK), ("D7", 32, 31.5, DK), ("R9", 37, 28, H), ("C5", 41, 29.5, V), ("R10", 45, 28, H),
        ("Q1", 68, 30, PF), ("R11", 73, 28.5, V), ("C6", 64, 34, V), ("D8", 60, 34, DK)],
        notes=[(52, 14, "J4 Davis RJ11: 2 yellow 3V3 (switched), 3 green DIR, 4 red GND, 5 black SPD\n"
                        "R8 + JP1: pull-up for reed-switch vanes (jumper on)\n"
                        "Q1: vane supply, VANE_EN (GPIO34) LOW = on"),
               (3, 38, "speed: tau 0.5-0.9 ms, direction: 10k / 1 uF")]),
    dict(title="Rain (Davis 6466M) + Schmitt trigger", x=64, y=70, w=72, h=38, parts=[
        ("J6", 5, 12, M), ("J7", 5, 20, M),
        ("D9", 14, 9.5, DK), ("R12", 21, 8, H), ("R13", 25, 4, V), ("D10", 29, 4.5, DK), ("D11", 29, 11.5, DK),
        ("R14", 34, 8, H), ("C7", 38, 9.5, V), ("U1", 46, 8, {"u": 2}), ("R15", 54, 8, H),
        ("U1", 14, 28, {"u": 3}), ("U1", 25, 28, {"u": 4}), ("U1", 36, 28, {"u": 5}), ("U1", 47, 28, {"u": 6}),
        ("U1", 58, 28, {"u": 7}), ("C8", 64, 28, V)],
        notes=[(3, 35, "J6 Davis RJ11: 3 reed switch, 4 GND (3/4 may be swapped); rain: tau ~10 ms; "
                       "U1 74HC14 inverting, unused inputs to GND")]),
    dict(title="BME280 / I2C", x=2, y=60, w=58, h=38, parts=[
        ("J8", 5, 20, M), ("D19", 11, 30, {"a": 0}),
        ("R16", 16, 8, H), ("D12", 22, 4.5, DK), ("D13", 22, 11.5, DK),
        ("R18", 30, 14, H), ("R19", 30, 20, H), ("JP2", 40, 14), ("JP3", 40, 20),
        ("R17", 16, 28, H), ("D14", 22, 24.5, DK), ("D15", 22, 31.5, DK)],
        notes=[(30, 28, "J8: 3V3 | SDA | SCL | GND\nExtra 4.7k pull-ups: JP2 SDA, JP3 SCL\nfor cables longer than 0.5 m")]),
    dict(title="1-Wire (DS18B20)", x=2, y=100, w=60, h=42, parts=[
        ("J9", 5, 34, M), ("D20", 13, 36, {"a": 270}),
        ("Q2", 12, 8, PF), ("R20", 17, 6.5, V), ("Q3", 22, 13, M), ("R21", 28, 13, HR), ("R22", 31, 14.5, V),
        ("F2", 10, 17, V), ("C9", 14, 22.5, V), ("D16", 20, 22.5, DK), ("R23", 26, 22.5, V),
        ("R24", 20, 29, H), ("D17", 30, 26.5, DK), ("D18", 30, 33.5, DK)],
        notes=[(36, 6, "supply switched by GPIO47\n(HIGH = on), 100 mA PTC\nup to 3 DS18B20")]),
]
