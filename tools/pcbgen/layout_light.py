"""Weather Node V4 Light layout: 100 x 80 mm, 4 x M3 (66 x 72 mm pattern), Heltec V4 only, all THT.

Board-local mm, origin top-left, y down. The Heltec V4 stands vertically at the left edge,
USB-C flush with the top edge (powered and flashed there). Along the right edge the Davis
RJ11 jacks, each with its screw terminal right below it (RAIN, WIND); along the bottom edge
the 1-WIRE and I2C terminals. One SMB TVS per Davis line between the jack and its terminal.
Rules: 0.3 mm signal / 0.4 mm sensor supply / 0.6 mm power tracks, 0.8/0.4 mm vias, 0.5 mm edge clearance.
"""
W, H = 100.0, 80.0

# Heltec V4 vertical: J2 (3V3 side) right, J1 (5 V side) left, pin 1 (GND) at the USB-C end (top)
XR, Y1 = 24.43, 4.4                       # J2 pin 1
XL = XR - 22.86                           # J1 pin 1
X0, ROW_TOP, ROW_BOT = Y1, XL, XR         # (generator compatibility; the rows are vertical here)
MOD = (XL - 1.27, Y1 - 4.2, XR + 1.27, Y1 + 47.5)   # V4 51.7 x 25.4 mm, USB-C end at the board edge
HOLES = [(30.0, 4.0), (W - 4.0, 4.0), (30.0, H - 4.0), (W - 4.0, H - 4.0)]
HOLE_FP, HOLE_NET = "MountingHole:MountingHole_3.2mm_M3_Pad_Via", "GND"   # plated, on GND: ESD path to a grounded chassis
MOUNTING_COPPER_RADIUS = 3.75
PITCH = 3.5
TPIN = H - 5.0
J9_X = 36.5                                  # 1-WIRE: GND | 3V3 | DQ
J8_X = J9_X + 2 * PITCH + 7.0                # I2C: 3V3 | SDA | SCL | GND
TERMS = [("J9", J9_X), ("J8", J8_X)]

# RJ jacks at the right edge, rotated 90 deg (as on the full board)
RJ_X = W - 16.9
# extra gaps jack -> terminal: the TVS between them stays 3-5 mm clear of both bodies, so it can
# still be soldered after the jacks and terminals (beginner-proof assembly order)
GAP_RAIN, GAP_WIND = 2.0, 4.0
RJ_RAIN_TOP, RJ_WIND_TOP = 8.5, 34.5
TERM_RX = W - 5.0
J7_Y1 = RJ_RAIN_TOP + 13.3 + 1.75 + PITCH + 1.2 + GAP_RAIN       # RAIN: pin 2 RAIN on top, pin 1 GND below
J5_Y1 = RJ_WIND_TOP + 13.3 + 1.75 + 3 * PITCH + 1.2 + GAP_WIND   # WIND: 3V3 | DIR | SPD | GND top->bottom
FIXED = {
    "J1": (XL, Y1, 0), "J2": (XR, Y1, 0),
    "J3": (round(XR - 5.588, 3), round(Y1 + 45.269, 3), 270),   # P4: GPIO15 pin 1, GPIO16 pin 2 (STEP position)
    "J6": (RJ_X, RJ_RAIN_TOP + 6.65 + 3.175, 90),
    "J4": (RJ_X, RJ_WIND_TOP + 6.65 + 3.175, 90),
    "J7": (TERM_RX, J7_Y1, 90),
    "J5": (TERM_RX, J5_Y1, 90),
}
for ref, x in TERMS: FIXED[ref] = (x, TPIN, 0)

C_TOP, C_BOT = 8.0, TPIN - 3.9
RJ_L = RJ_X - 1.6
MX = MOD[2] + 0.8                              # right of the module
ZONES = [   # order = placement priority; ANCHORS pull critical pads together
    ("HWID", 0.6, MOD[2], MOD[3] + 0.5, H - 0.6, ["R1", "R2", "C1"]),
    ("PWR", MX, RJ_L - 0.3, C_TOP, 24.0, ["F1", "C2", "C3", "D1"]),
    ("WX", MX + 8.0, RJ_L - 0.3, C_TOP, C_BOT, [
        "R12", "R13", "D10", "D11", "R14", "C7",                    # rain conditioning
        "R3", "R4", "D4", "D5", "R5", "C4",                         # wind speed
        "R7", "D6", "D7", "R8", "JP1", "R9", "C5", "R10",           # wind direction
        "Q1", "R11", "C6", "D8",                                    # switched vane supply
        "U1", "C8", "R6", "R15"]),                                  # Schmitt trigger near the GPIO pins
    ("I2C", J8_X - 6.0, RJ_L - 0.3, 34.0, C_BOT, [
        "D19", "R16", "R17", "D12", "D13", "D14", "D15", "JP2", "JP3", "R18", "R19"]),
    ("OW", 0.6, J8_X, MOD[3] + 0.5, C_BOT, [
        "D20", "F2", "C9", "D16", "Q2", "R20", "Q3", "R21", "R22", "R23", "R24", "D17", "D18"]),
]
ANCHORS = [
    ("R1", "2", "J3", "1"), ("R2", "1", "J3", "1"), ("C1", "1", "J3", "1"),
    ("F1", "1", "J2", "2"), ("C2", "1", "F1", "2"), ("C3", "1", "F1", "2"), ("D1", "1", "F1", "2"),
    # wind (protection at the RJ11 pins, the terminal J5 is in parallel)
    ("D8", "1", "J5", "4"), ("Q1", "3", "D8", "1"), ("C6", "1", "Q1", "3"), ("R11", "2", "Q1", "2"),
    ("R3", "1", "J4", "5"), ("R7", "1", "J4", "3"),
    ("R4", "2", "R3", "2"), ("D4", "2", "R3", "2"), ("D5", "1", "R3", "2"), ("R5", "1", "R3", "2"), ("C4", "1", "R5", "2"),
    ("D6", "2", "R7", "2"), ("D7", "1", "R7", "2"), ("JP1", "2", "R7", "2"), ("R8", "2", "JP1", "1"),
    ("R9", "1", "R7", "2"), ("C5", "1", "R9", "2"), ("R10", "1", "R9", "2"),
    # rain
    ("R12", "1", "D9", "1"), ("R13", "2", "R12", "2"), ("D10", "2", "R12", "2"), ("D11", "1", "R12", "2"),
    ("R14", "1", "R12", "2"), ("C7", "1", "R14", "2"),
    ("U1", "1", "C4", "1"), ("U1", "3", "C7", "1"), ("C8", "1", "U1", "14"),
    ("R6", "1", "U1", "2"), ("R15", "1", "U1", "4"),
    # I2C
    ("D19", "1", "J8", "2"), ("D19", "2", "J8", "3"), ("D20", "1", "J9", "3"),
    ("R16", "1", "J8", "2"), ("R17", "1", "J8", "3"), ("D12", "2", "R16", "2"), ("D13", "1", "R16", "2"),
    ("D14", "2", "R17", "2"), ("D15", "1", "R17", "2"),
    ("JP2", "2", "R16", "2"), ("JP3", "2", "R17", "2"), ("R18", "2", "JP2", "1"), ("R19", "2", "JP3", "1"),
    # 1-Wire
    ("F2", "2", "J9", "2"), ("C9", "1", "J9", "2"), ("D16", "1", "J9", "2"), ("Q2", "3", "F2", "1"),
    ("R20", "2", "Q2", "2"), ("Q3", "3", "Q2", "2"), ("R21", "2", "Q3", "2"), ("R22", "1", "R21", "1"),
    ("R24", "1", "J9", "3"), ("D17", "2", "R24", "2"), ("D18", "1", "R24", "2"), ("R23", "2", "R24", "2"),
]
SPILL = [(MX, RJ_L - 0.3, C_TOP, C_BOT), (0.6, RJ_L - 0.3, MOD[3] + 0.5, C_BOT)]

MOATS = []
DOMAINS = {}
GND_OUTLINE = [(0, 0), (W, 0), (W, H), (0, H)]
ISOLATORS = ()
ISO_ZONES = ()
CX = (MOD[0] + MOD[2]) / 2
TEXTS = (("Heltec WiFi LoRa 32", CX, 20.0, 1.2), ("V4 (V4.2 / V4.3)", CX, 22.5, 1.2),
         ("USB-C", CX, 2.2, 1.2), ("ANT", CX, MOD[3] - 2.0, 1.2), ("franzenhof.com", CX, 26.0, 1.0))
BRAND = (("Universal Weather Node V4 Light", 1.4), ("Rev 1.0 | franzenhof.com", 1.4))
BACK_LABELS_V = {"J7": ["GND", "RAIN"], "J5": ["GND", "SPD", "DIR", "3V3"]}
BACK_LABELS = {"J9": ["GND", "3V3", "DQ"], "J8": ["3V3", "SDA", "SCL", "GND"]}
BACK_TITLES = {"J9": "1-WIRE", "J8": "I2C", "J7": "RAIN", "J5": "WIND"}
BACK_DOCUMENTATION = False
BACK_TEXTS = (("Universal Weather Node V4 Light", 60.0, 20.0, 2.0), ("franzenhof.com", 60.0, 24.5, 1.6),
              ("CERN-OHL-P v2", 60.0, 28.0, 1.2),
              ("Heltec WiFi LoRa 32 V4 only\nPower and flash via the Heltec USB-C", 60.0, 33.0, 1.0))

POWER_NETS = ["GND", "3V3_HELTEC"]
MEDIUM_NETS, MEDIUM_W = ["+3V3", "OW_SW", "OW_VCC", "WDIR_VCC"], 0.4
HEAVY_NETS, HEAVY_W = [], 1.0
POWER_W = 0.6
DEF_W, CLEAR = 0.3, 0.25
VIA, PVIA = (0.8, 0.4), (1.0, 0.5)
PCLEAR = 0.25
EDGE = 0.5
GAP = 0.6
GND_VIAS = [("D1", "2"), ("D2", "2"), ("D3", "2"), ("D8", "2"), ("D9", "2"), ("D16", "2"), ("D19", "3"), ("D20", "3")]
BRAND_BLOCK = False          # brand texts: FIXED_NOTES (top edge) + TEXTS (module outline)
THERMAL_VIAS = {}

# Davis protection: one TVS per line between the RJ11 pin and its screw terminal
TVS_X = TERM_RX - 10.4
FIXED_PADS = [
    ("D9", "1", TVS_X, J7_Y1 - PITCH, "2", "down"),               # J7 RAIN / J6.3
    ("D3", "1", TVS_X, J5_Y1 - PITCH, "2", "down"),               # J5 SPD / J4.5
    ("D2", "1", TVS_X, J5_Y1 - 2 * PITCH, "2", "up"),             # J5 DIR / J4.3
]
JPIN_X = RJ_X
WSPD_Y, WDIR_Y = J5_Y1 - PITCH, J5_Y1 - 2 * PITCH
J4_P5_Y, J4_P3_Y = RJ_WIND_TOP + 6.65 + 3.175 - 5.08, RJ_WIND_TOP + 6.65 + 3.175 - 2.54
J6_P3_Y = RJ_RAIN_TOP + 6.65 + 3.175 - 2.54
CX_SPD, CX_DIR, CX_RAIN = JPIN_X - 2.5, JPIN_X - 1.5, JPIN_X - 1.5
PRE_ROUTE = [[("J5", "2"), ("D3", "1"), ("@", CX_SPD, WSPD_Y), ("@", CX_SPD, J4_P5_Y), ("J4", "5")],
             [("J5", "3"), ("D2", "1"), ("@", CX_DIR, WDIR_Y), ("@", CX_DIR, J4_P3_Y), ("J4", "3")],
             [("J7", "2"), ("D9", "1"), ("@", CX_RAIN, J7_Y1 - PITCH), ("@", CX_RAIN, J6_P3_Y), ("J6", "3")]]
RESERVED = [(CX_SPD - 0.6, J4_P5_Y - 0.6, CX_DIR + 0.6, WSPD_Y + 0.6),
            (CX_RAIN - 0.6, J6_P3_Y - 0.6, CX_RAIN + 0.6, J7_Y1 - PITCH + 0.6)]
PRE_ROUTE_W = 0.5
ROW_LABELS = {"JP2": [("1", "2", "SDA")], "JP3": [("1", "2", "SCL")]}
PIN_LABELS = {"JP1": ["R8", "DIR"]}
REFINE = ["C1", "C3", "C4", "C5", "C7", "C8", "C9", "R3", "R7", "R12", "R16", "R17"]
SOLID_GND = ("J1", "J2", "J4", "J6", "J7", "J5", "J8", "J9")

NOTES = [("JP2", "JP2/JP3 I2C PULL-UP 4k7\nfit for long cables", 0, 0, 0.8),
         ("JP1", "JP1 VANE PULL-UP\nfit: reed-switch vane\nopen: Davis pot", 0, 0, 0.8)]
TERM_FUNC = {"J9": "1-WIRE", "J8": "I2C BME280"}
TOP_PIN_LABELS = {"J9": ["GND", "3V3", "DQ"], "J8": ["3V3", "SDA", "SCL", "GND"]}
BRAND_X = (MOD[2] + RJ_L) / 2
FIXED_NOTES = [("Universal Weather Node V4 Light", BRAND_X, 2.2, 1.2, 0), ("Rev 1.0 | franzenhof.com", BRAND_X + 4.0, 4.6, 1.0, 0),
               ("J6 DAVIS RAIN", RJ_L - 0.9, RJ_RAIN_TOP + 6.65, 1.0, 90),
               ("J4 DAVIS WIND", RJ_L - 0.9, RJ_WIND_TOP + 6.65, 1.0, 90),
               ("J7 RAIN", W - 12.0, J7_Y1 - PITCH / 2, 1.0, 90),
               ("RAIN", W - 9.9, J7_Y1 - PITCH, 0.8, 90), ("GND", W - 9.9, J7_Y1, 0.8, 90),
               ("J5 WIND", W - 12.0, J5_Y1 - 1.5 * PITCH, 1.0, 90),
               ("3V3", W - 9.9, J5_Y1 - 3 * PITCH, 0.8, 90), ("DIR", W - 9.9, J5_Y1 - 2 * PITCH, 0.8, 90),
               ("SPD", W - 9.9, J5_Y1 - PITCH, 0.8, 90), ("GND", W - 9.9, J5_Y1, 0.8, 90)]
HIDE_REFS = ("J4", "J6", "J7", "J5")
