"""Schematic placement for the weather node v2.0 (gen_sch2.py).

Coordinates in 2.54 mm grid units, relative to the block origin. Orientation cheat sheet:
  R/C/L/F: a=90 horizontal (pin 1 left), a=270 (pin 1 right), a=0 vertical (pin 1 top)
  diodes:  a=270 vertical (K top), a=180 horizontal (K right)
  BAT54S:  a=180 -> COM up, K (pin 2) left, A (pin 1) right
  FET:     a=0 -> G left, D top, S bottom; P-FET a=180 -> G right, S top, D bottom
"""
TITLE = "Universal Weather Node"
REV = "2.0 (SMD)"
COMMENTS = ["KiCad/SMD redesign of the Fritzing Rev 1.3 board; parts with LCSC numbers (JLCPCB)",
            "franzenhof.com"]
PAPER = "A1"

RAILS = {"GND": "GND", "GND_VICTRON": "GND", "+3V3": "+3V3", "3V3_HELTEC": "+3V3", "+5V": "+5V", "+5V_BUCK": "+5V",
         "+VLOAD": "VCC", "VDD_VICTRON": "VCC"}
DOWN_RAILS = ["GND", "GND_VICTRON"]
PWR_FLAGS = ["GND", "GND_VICTRON", "+3V3", "3V3_HELTEC", "+5V", "+5V_BUCK", "+VLOAD", "VDD_VICTRON",
             "VIN_BUCK"]
LABEL_ONLY_NETS = {"BUCK_FB", "BUCK_VCC", "FB_LOW", "FAN_SUPPLY", "FAN_G", "FAN_SW",
                   "UV_SIGNAL", "UV_ADC", "UV_3V3_RANGE", "UV_5V_RANGE", "UV_5V_B", "UV_5V_C",
                   "TACH", "TACH_PU"}
FLAG_AT = (66, 56)
# connectors: every signal pin gets its own label (no long wires through the sheet)
LABEL_PARTS = ["J1", "J2", "J3", "J4", "J5", "J6", "J7", "J9", "J10", "J11", "J12", "J13", "J14", "J15", "J16", "J17", "J18"]
LABEL_AT = {("C10", "1"): True, ("D12", "3"): True, ("D13", "3"): True, ("D15", "3"): True, ("D16", "1"): True,
            ("C15", "1"): True, ("R21", "1"): True, ("D24", "1"): True, ("D22", "2"): True, ("F6", "2"): True,
            ("R37", "1"): True, ("F7", "2"): True}

M = {"m": True}
H, HR, V, VF = {"a": 90}, {"a": 270}, {"a": 0}, {"a": 180}
DK, DH = {"a": 270}, {"a": 180}            # diode K top / K right
BS = {"a": 180}                             # BAT54S COM up
PF = {"a": 180}                             # P-FET: G right, S top, D bottom

BLOCKS = [
    dict(title="Heltec WiFi LoRa 32 V3.2", x=2, y=2, w=58, h=62, parts=[
        ("J1", 16, 30), ("J2", 40, 30, M),
        ("R1", 8, 48, V), ("R2", 8, 54, V), ("C10", 13, 54, V), ("C37", 52, 30, V),
        ("J16", 30, 52, M),
        ("TP1", 44, 47), ("TP2", 47, 47, VF), ("TP3", 50, 47, VF), ("TP4", 53, 47, VF), ("TP5", 56, 47),
        ("TP6", 50, 57, VF)],
        notes=[(3, 7, "J1 = header on the 5 V side, J2 = header on the 3V3 side (pin 1 = GND, USB-C end)\n"
                      "V3 / V3.1 / V3.2 are pin-compatible"),
               (3, 42, "HW revision ID (GPIO20):\nv2.0 = 1M / 1M -> 0.5 x 3V3"),
               (22, 58, "J16 expansion header, TP1-TP7 test pads (not populated)")]),
    dict(title="Power supply: VIN 6-29.2 V or Heltec USB-C; manual JP8 disconnect", x=62, y=2, w=106, h=46, parts=[
        ("F1", 30, 9, H), ("C1", 32, 13, V), ("C33", 35, 13, V), ("C2", 38, 13, V),
        ("U1", 40, 11), ("C3", 45, 10, H), ("L1", 50, 10, H), ("C26", 44, 17, V),
        ("C4", 55, 12, V), ("C5", 60, 12, V), ("C34", 65, 12, V),
        ("C6", 36, 22, V), ("C7", 39, 22, V),
        ("JP8", 30, 19),
        ("C35", 3, 36, V), ("U7", 8, 34), ("C36", 13, 36, V),
        ("F3", 18, 34, H), ("C8", 23, 36, V), ("C9", 26, 36, V), ("D5", 29, 35.5, DK)],
        notes=[(66, 8, "LMR36520: 4.2-65 V in, 5 V / 2 A nominal buck"),
               (44, 28, "J3 has VIN and GND only; no external 5 V input.\nUSB supplies logic/sensors; heater requires VIN."),
               (44, 18, "JP8 fitted = VIN via buck\nREMOVE BEFORE USB\nopen = USB only; never parallel sources"),
               (76, 39, "USB mode: remove JP8 and disconnect VIN before connecting USB-C.\nHeltec VBUS connects to the module 5 V pin; no automatic isolation."),
               (33, 32, "3.3 V sensor rail: own LDO U7 from 5 V,\nPTC 250 mA + TVS for the external cables;\nthe Heltec 3.3 V rail powers only the module")]),
    dict(title="VIN reverse / transient protection", x=235, y=2, w=64, h=32, parts=[
        ("J3", 5, 11, M), ("J18", 5, 16, M), ("F7", 14, 9, H), ("D1", 19, 10.5, DK),
        ("Q7", 26, 11), ("D31", 33, 13, DK), ("R45", 37, 15, H), ("C31", 46, 14, V)],
        notes=[(3, 25, "VIN 6-29.2 V battery max. Q7 drain=input, source=protected rail.\nD31 is the 12 V VGS clamp; bidirectional D1: 48.4 V max clamp, no reverse crowbar.\nC31 is 47 uF/63 V on protected +VLOAD; source-dependent inrush.")]),
    dict(title="LMR36520 feedback divider", x=235, y=40, w=55, h=24, parts=[
        ("R46", 9, 10, H), ("R47", 16, 12, H), ("R48", 28, 12, H)],
        notes=[(2, 20, "1.0 V FB reference: 47k / (10k + 1.8k) = 4.983 V nominal")]),
    dict(title="PC fan tacho pull-up", x=235, y=112, w=55, h=23, parts=[
        ("R61", 12, 8, V), ("JP12", 25, 10)],
        notes=[(2, 18, "JP12 fitted: 10k to +5V for open collector.\nRemove for externally pulled-up / push-pull tacho.")]),
    dict(title="Fan gate resistor bank", x=235, y=68, w=55, h=26, parts=[
        ("R33", 15, 9, H), ("R62", 29, 9, H),
        ("R63", 15, 16, H), ("R64", 29, 16, H)],
        notes=[(3, 21, "(2k + 2k) || (2k + 2k) = 2k effective\n4 x C17944 / 1206 / 0.25 W, no new article\n1 W aggregate nameplate only; thermal/pulse tests required")]),
    dict(title="Victron VE.Direct (isolated)", x=170, y=2, w=62, h=36, parts=[
        ("U4", 30, 16), ("C21", 20, 8, V), ("C22", 40, 10, V), ("D27", 42, 23),
        ("J14", 56, 16), ("J15", 56, 27)],
        notes=[(3, 26, "GPIO33 = RX (from Victron TX)\nGPIO34 = TX (to Victron RX)\nside 2 powered by the Victron (VE.Direct +V)"),
               (44, 31, "J15 pin 2 RX / pin 3 TX:\nnode TX -> pin 2, pin 3 -> node RX")]),
    dict(title="Outputs: rain-gauge heater (low side), fan (high side) + tacho", x=170, y=40, w=62, h=52, parts=[
        ("J12", 58, 6),
        ("F5", 46, 10, HR), ("D22", 50, 8.5, DK), ("Q3", 42, 13), ("R30", 35, 13, H), ("U5", 27, 13), ("C32", 33, 7, V), ("R39", 21, 14.5, V), ("R31", 38, 14.5, V),
        ("Q4", 36, 24), ("JP9", 52, 18), ("F6", 35, 31, V), ("D23", 48, 34, DK),
        ("R32", 40, 22.5, V), ("D26", 44, 22.5, DK), ("Q5", 52, 26), ("R37", 47, 27.5, V),
        ("J13", 58, 28),
        ("D24", 8, 39.5, DK), ("R49", 13, 43, H), ("R50", 17, 46, V), ("Q8", 22, 43),
        ("R34", 11, 37, V), ("R35", 15, 39, H), ("D25", 20, 40, BS), ("C20", 24, 40, V),
        ("U3", 32, 42, {"u": 2}), ("R36", 41, 42, H)],
        notes=[(3, 8, "load supply from common protected\nVIN rail through Q7"),
               (3, 14, "GPIO38 HIGH = heater on\nGPIO39 HIGH = fan on\n(both PWM capable)"),
               (3, 49, "J12: +V | HEAT- max 3 A   J13: FAN+ | GND | TACH\nFan max 0.5 A at 5 V (JP9 default), 1 A at VIN")]),
    dict(title="Wind (Davis 6410)", x=2, y=66, w=78, h=40, parts=[
        ("J4", 5, 12, M), ("J5", 5, 22, M),
        ("D7", 16, 9.5, DK), ("R3", 20, 8, H), ("R4", 24, 6.5, V), ("D8", 27, 10, BS), ("R5", 33, 8, H),
        ("C11", 37, 9.5, V), ("U2", 45, 8, {"u": 2}), ("R6", 54, 8, H),
        ("D6", 16, 29.5, DK), ("R7", 20, 28, H), ("R8", 24, 21, V), ("JP2", 28, 24, M), ("D9", 27, 30, BS), ("R9", 33, 28, H),
        ("C12", 37, 29.5, V), ("R10", 41, 28, H),
        ("Q6", 62, 30, PF), ("R38", 67, 28.5, V), ("C23", 58, 34, V), ("D29", 54, 34, DK)],
        notes=[(48, 24, "J4 Davis RJ11: 2 yellow 3V3 (switched), 3 green DIR,\n4 red GND, 5 black SPD\nR8 + JP2: pull-up for reed-switch\nvanes (jumper on); Q6: vane supply, GPIO19 LOW = on"),
               (3, 35, "speed: tau 0.5-0.9 ms (min. contact time ~1.5 ms), direction: 10k / 1 uF")]),
    dict(title="Rain (Davis 6466M)", x=82, y=66, w=60, h=40, parts=[
        ("J6", 5, 12, M), ("J7", 5, 20, M),
        ("D20", 14, 9.5, DK), ("R11", 21, 8, H), ("R12", 25, 6.5, V), ("D11", 28, 10, BS),
        ("R13", 34, 8, H), ("C13", 38, 9.5, V), ("U2", 46, 8, {"u": 1}), ("R14", 54, 8, H),
        ("U2", 20, 30, {"u": 3}), ("C14", 24, 30, V)],
        notes=[(30, 20, "J6 Davis RJ11: 3 reed switch, 4 GND\n(3/4 may be swapped)\nD20 TVS between jack and terminal\nU2 74LVC2G14: inverting"),
               (3, 36, "rain: tau ~10-15 ms")]),
    dict(title="BME280 / I2C", x=2, y=108, w=56, h=40, parts=[
        ("J9", 5, 20, M), ("D28", 11, 30, {"a": 0}),
        ("R15", 16, 8, H), ("D12", 21, 10, BS), ("R18", 26, 14, H), ("R17", 26, 17, H), ("JP1", 34, 14), ("JP11", 34, 19),
        ("R16", 16, 22, H), ("D13", 21, 24, BS)],
        notes=[(36, 8, "J9: 3V3 | SDA | SCL | GND\nCat5: SDA and SCL each\ntwisted with a GND wire"),
               (36, 24, "Extra 4.7k pull-ups: JP1 SDA, JP11 SCL\nSeparate 2-pin headers, rows 5.08 mm apart\nfor cables longer than 0.5 m")]),
    dict(title="1-Wire (DS18B20)", x=60, y=108, w=48, h=40, parts=[
        ("J10", 5, 32, M),
        ("Q1", 10, 8, {"a": 180}), ("R19", 15, 6.5, V), ("Q2", 19, 12, M), ("R20", 24, 12, HR), ("R21", 27, 13.5, V),
        ("TP7", 14.5, 16, VF),
        ("C38", 5, 8, V), ("F4", 9, 14, V), ("C15", 12, 19.5, V), ("C16", 16, 19.5, V), ("D14", 20, 19.5, DK), ("R22", 25, 19.5, V),
        ("R23", 20, 24, H), ("D16", 14, 27.5, {"a": 270}), ("D15", 29, 26, BS)],
        notes=[(31, 6, "supply switched by GPIO47\n(HIGH = on), 100 mA PTC\nup to 3 DS18B20")]),
    dict(title="AUX inputs", x=110, y=108, w=58, h=40, parts=[
        ("J11", 5, 34, M), ("D17", 12, 15, {"a": 90}),
        ("R26", 17, 8, H), ("R27", 21, 6.5, V), ("D19", 24, 10, BS), ("R28", 30, 8, H), ("C18", 34, 9.5, V),
        ("U3", 42, 8, {"u": 1}), ("R29", 51, 8, H),
        ("R24", 17, 24, H), ("R25", 21, 25.5, V), ("D18", 25, 26, BS), ("C17", 30, 25.5, V),
        ("U3", 44, 24, {"u": 3}), ("C19", 48, 24, V)],
        notes=[(14, 32, "AIN -> GPIO4 (ADC1); R25 = 15k for 0-5 V sensors (DNP; scale 1.66667)\n"
                        "PULSE: reed / open collector -> GPIO48 (inverted)")]),
    dict(title="UV sensor input", x=170, y=96, w=76, h=50, parts=[
        ("J17", 5, 20, M), ("F8", 12, 11, V), ("D32", 12, 29), ("R51", 18, 20, H), ("JP10", 29, 22, M),
        ("D30", 41, 18, BS), ("C27", 46, 25, V), ("R52", 55, 11, H),
        ("R53", 55, 18, H), ("R54", 55, 23, H), ("R55", 55, 28, H)],
        notes=[(3, 37, "J17 pins 1-4: GND | SIGNAL | 3V3 | 5V; use one sensor supply only"),
               (34, 37, "JP10 1-2: 0-3.3 V, ratio 0.909091 (3.3 V -> 3.0000 V)\n"
                        "JP10 2-3: 0-5 V, ratio 0.600479 (5 V -> 3.0024 V)\n"
                        "Input scale factors: 1.1 / 1.66534; GPIO3 ADC1")]),
]
