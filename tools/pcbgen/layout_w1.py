"""Weather node v2.0 layout: 100 x 100 mm, 4 x M3 (92 x 92 mm corner pattern).

Board-local mm, origin top-left, y down. Heltec module at the left edge (USB-C
flush with the edge) below the top-left hole; 18 screw-terminal poles along the
primary bottom edge: POWER | HEATER | FAN | AUX | 1-WIRE | I2C; the four-pole UV
terminal is on the lower-right edge. Along the right edge the Davis RJ11 jacks,
each with its screw terminal right below it (RAIN, WIND), and the UV terminal
have wire entries facing right; one TVS per line between each jack and terminal.
The isolated VE.Direct port is at the left edge.
Robust rules: 0.3 mm signal / 0.6 mm power / 1.5 mm heater-path tracks, 0.8/0.4 mm vias, 0.5 mm edge clearance.
"""
W, H = 100.0, 100.0

X0 = 4.2                      # Heltec pin 1 (USB end); module edge 1.5 mm inside -> USB-C does not protrude
ROW_TOP = 9.5                 # Heltec J3 (our J2, 3V3 side)
ROW_BOT = ROW_TOP + 22.86     # Heltec J2 (our J1, 5V side)
MOD = (X0 - 2.7, ROW_TOP - 1.32, X0 - 2.7 + 50.2, ROW_BOT + 1.32)
TPIN = H - 5.0
HOLES = [(4.0, 4.0), (W - 4.0, 4.0), (4.0, H - 4.0), (W - 4.0, H - 4.0)]
PITCH = 3.5
GAPT = 6.0                                    # gap between neighbouring terminals (room for the titles)
J3_X = 10.0                                   # POWER: VIN | GND
J12_X = J3_X + 2 * PITCH + GAPT               # retain reviewed heater position; extra gap after 2-pin power
J13_X = J12_X + PITCH + GAPT                  # FAN: FAN+ | GND | TACH
J11_X = J13_X + 2 * PITCH + GAPT              # AUX: 3V3 | AIN | PULSE | GND
J10_X = J11_X + 3 * PITCH + GAPT              # 1-WIRE: GND | 3V3 | DQ
J9_X = J10_X + 2 * PITCH + GAPT               # I2C: 3V3 | SDA | SCL | GND
TERMS = [("J3", J3_X), ("J12", J12_X), ("J13", J13_X), ("J11", J11_X), ("J10", J10_X), ("J9", J9_X)]

# RJ jacks at the right edge, rotated 90 deg: local +y (towards the plug opening) -> board +x.
# Front face 16.9 mm in front of the pin-1 row -> pin-1 row at x = W - 16.9 (front flush with the edge)
RJ_X = W - 16.9
RJ_RAIN_TOP, RJ_WIND_TOP = 8.5, 31.5          # body top edges (RJ11 13.3 mm wide)
# screw terminals right below their jack, rotated 90 deg: wire entry from the right edge,
# pin 1 at the bottom, pins going up (pin y given for pin 1)
TERM_RX = W - 5.0
J7_Y1 = RJ_RAIN_TOP + 13.3 + 1.75 + PITCH + 1.2      # RAIN: pin 2 RAIN on top, pin 1 GND below
J5_Y1 = RJ_WIND_TOP + 13.3 + 1.75 + 3 * PITCH + 1.2  # WIND: 3V3 | DIR | SPD | GND top->bottom
J17_Y1 = 81.0                                        # UV pin 1: 19 mm above bottom edge
FIXED = {
    "J2": (X0, ROW_TOP, 90), "J1": (X0, ROW_BOT, 90),
    "J6": (RJ_X, RJ_RAIN_TOP + 6.65 + 3.175, 90),
    "J4": (RJ_X, RJ_WIND_TOP + 6.65 + 3.175, 90),
    "J7": (TERM_RX, J7_Y1, 90),
    "J5": (TERM_RX, J5_Y1, 90),
    "J17": (W - 5.5, J17_Y1, 90),
}
for ref, x in TERMS: FIXED[ref] = (x, TPIN, 0)
# Preserve the reviewed buck cluster, including its local bypass and feedback parts.
FIXED.update({
    "F1": (46.52, 44.48, 0), "U1": (57.5, 45.0, 0), "L1": (67.5, 44.65, 0),
    "C1": (54.1, 40.2, 0), "C2": (52.65, 44.0, 90), "C33": (59.0, 35.5, 0),
    "C3": (62.9, 45.5, 90), "C26": (62.0, 49.0, 0),
    "C4": (73.0, 42.9, 0), "C5": (73.0, 45.3, 0),
    "C34": (68.0, 39.0, 0),
    "C31": (7.0, 64.5, 180),
    "C32": (29.1, 47.8, 90),
    "R46": (63.0, 52.0, 90), "R47": (56.5, 50.2, 180),
    "R48": (52.8, 50.2, 180),
})
MOUNTING_COPPER_RADIUS = 3.75
# Two 2-pin headers, with one empty 2.54 mm grid row between the jumper rows.
FIXED["JP1"] = (63.09, 73.20, 90)
FIXED["JP11"] = (63.09, 78.28, 90)
FIXED["JP8"] = (23.5, 38.6, 0)                  # 1 mm lower: clear of the V4 row J19
# Heltec V4 socket set (dual_socket.py): rows 2.54 mm towards J1, P4 socket at the STEP position
FIXED.update({"J19": (X0, ROW_BOT + 2.54, 90), "J20": (X0, ROW_TOP + 2.54, 90),
              "J21": (round(47.429 - 2.16 + X0, 3), round(ROW_TOP + 2.54 + 11.43 - 5.842, 3), 0),
              "U8": (9.6, 28.6, 0), "U9": (9.6, 25.8, 180), "C39": (12.9, 28.6, 90), "R65": (25.0, 16.6, 0),
              "C37": (6.0, 16.9, -90), "C36": (9.4, 16.45, -90), "R1": (45.6, 27.2, 90)})
FIXED["J18"] = (13.3, 79.0, 0)        # DC jack (THT, optional), opening at the left edge
FIXED["F7"] = (17.3, 81.5, 90)        # moved right of J18 for the jack
FIXED["D1"] = (21.6, 84.0, -90)
FIXED["R45"] = (21.3, 74.9, 0)
FIXED["D32"] = (88.75, 82.0, 180)
# Retain the checked fan footprint angles.
FIXED.update({
    "U5": (30.92, 47.95, 270), "R32": (31.5, 51.45, 180),
    "D26": (33.97, 46.35, 90),
    "Q4": (38.32, 70.21, 90), "Q5": (44.52, 67.43, 270),
    "R33": (22.8, 61.7, 0), "R62": (27.6, 61.7, 0),
    "R63": (22.8, 64.8, 0), "R64": (27.6, 64.8, 0),
    "JP12": (28.5, 38.9, 90), "R61": (25.5, 59.0, 0),
})
# isolated VE.Direct domain at the left edge below the module (terminal J14 + JST-PH J15)
VIC_X1, VIC_Y0, VIC_Y1 = 18.0, 37.55, 58.0     # domain: x 0..VIC_X1, y VIC_Y0..VIC_Y1 (top 1.65 mm lower for the V4 row J19)
MOAT_W = 1.5
J14_Y = VIC_Y0 + 1.75                           # pin 1 (GND) at the top, wire entry from the left edge
FIXED["J14"] = (5.0, J14_Y, 270)
FIXED["J15"] = (3.0, VIC_Y1 - 3.2, 0)
FIXED["U4"] = (VIC_X1 + MOAT_W / 2, 47.5, 180)   # side 2 (pins 5-8) left in the domain, side 1 right

C_TOP, C_BOT = MOD[3] + 0.5, TPIN - 3.9
RJ_L = RJ_X - 1.6                              # left end of the jack bodies
ZONES = [   # order = placement priority; ANCHORS below pull critical pads together
    ("VIC", 0.6, VIC_X1 - 0.3, VIC_Y0 + 0.3, VIC_Y1 - 0.3, ["D27", "C22"]),
    ("VIC1", VIC_X1 + MOAT_W + 0.3, 36.0, 49.0, 58.0, ["C21"]),    # node-side isolator decoupling
    ("UV", 57.0, 95.0, 61.0, 70.0, ["D32", "R51", "R52", "R53", "R54", "R55", "JP10", "D30", "C27"]),
    ("C", 0.6, MOD[2], C_TOP, C_BOT, [
        "D22", "Q3", "R30", "R31",                                  # heater: freewheel, low-side switch
        "F7", "D1", "Q7", "D31", "R45", "F1",
        "U1", "C1", "C2", "C3", "C26", "L1", "C4", "C5", "C33", "C34", "R46", "R47", "R48", "C31",
        "JP8", "C7", "TP2", "TP3", "TP1",                           # buck disconnect, test points
        "R2", "C10"]),                                              # HW revision ID at J1.17 (V3 GPIO20; V4 GPIO15 at J21)
    ("CLOW", 70.0, 88.0, 47.0, 64.0, ["C6"]),                      # bulk 5 V output capacitor
    ("I2C2", MOD[2] + 1.3, RJ_L - 0.3, 37.0, C_BOT, [
        "R15", "R16", "D12", "D13", "JP1", "JP11", "R17", "R18", "C8"]),   # I2C conditioning near J9
    ("OW", 20.0, J10_X - 0.5, 50.0, C_BOT, [
        "F4", "C16", "C15", "Q1", "R19", "Q2", "R20", "R21",
        "R23", "D15", "R22"]),                                      # 1-Wire conditioning near J10
    ("IO", J13_X - 3.0, RJ_L - 0.3, C_TOP - 6.0, C_BOT, [
        "U5", "C32", "R39", "D26", "R32", "Q4", "R33", "R62", "R63", "R64", "Q5", "R37", "D23", # heater gate driver and fan
        "JP9", "F6", "R49", "R50", "Q8", "R34", "R35", "D25", "C20", "R36", "R61", "JP12",
        "R24", "R25", "D18", "C17",                                 # AUX analog
        "R26", "R27", "D19", "R28", "C18", "U3", "C19", "R29",      # AUX pulse
        ]),
    ("TOPSTRIP", 8.0, MOD[2], 0.6, MOD[1] - 0.2, ["F3", "C9", "D5", "TP4", "TP5", "J16"]),
    ("RIGHT", RJ_L + 0.3, W - 9.0, RJ_WIND_TOP + 13.3 + 0.3, C_BOT, ["R3", "R7"]),                  # wind surge resistors at J4/J5
    ("WX", MOD[2] + 1.3, RJ_L - 0.3, 8.6, C_BOT, [
        "R11", "R12", "D11", "R13", "C13",                          # rain conditioning
        "R4", "D8", "R5", "C11", "D9", "R8", "JP2", "R9", "C12", "R10",    # wind conditioning
        "Q6", "R38", "C23", "D29",                                  # switched vane supply (VANE_EN)
        "U2", "C14", "R6", "R14",                                   # Schmitt trigger at the Heltec GPIO6/7 pins
        ]),
]
# (ref, own pad, anchor ref, anchor pad): the placer keeps these pads close (weight 25)
ANCHORS = [
    ("D32", "1", "J17", "2"),
    ("F7", "1", "J3", "1"), ("D1", "1", "F7", "2"), ("Q7", "2", "F7", "2"),
    ("D31", "1", "Q7", "3"), ("R45", "1", "Q7", "1"),
    ("C31", "1", "Q7", "3"),
    ("F1", "2", "U1", "2"), ("C1", "1", "U1", "2"), ("C2", "1", "U1", "2"), ("C33", "1", "U1", "2"),
    ("C3", "1", "U1", "7"), ("C3", "2", "U1", "8"), ("C26", "1", "U1", "6"),
    ("C32", "1", "U5", "5"), ("C32", "2", "U5", "3"),
    ("L1", "1", "U1", "8"), ("C4", "1", "L1", "2"), ("C5", "1", "L1", "2"),
    ("C34", "1", "L1", "2"),
    ("R46", "1", "L1", "2"), ("R47", "1", "U1", "5"), ("R48", "1", "R47", "2"),
    ("R61", "2", "JP12", "1"), ("JP12", "2", "R49", "1"),
    ("C7", "1", "J1", "2"),
    ("TP2", "1", "C1", "1"), ("TP3", "1", "C7", "1"), ("TP1", "1", "C2", "2"),
    ("R1", "2", "J1", "17"), ("R2", "1", "J1", "17"), ("C10", "1", "J1", "17"),
    ("F3", "1", "J2", "2"), ("C9", "1", "F3", "2"), ("D5", "1", "F3", "2"), ("TP4", "1", "C9", "1"), ("TP5", "1", "C9", "2"),
    # AUX
    ("R24", "1", "J11", "2"), ("R26", "1", "J11", "3"), ("R27", "2", "R26", "2"), ("D19", "3", "R26", "2"),
    ("R28", "1", "R26", "2"), ("C18", "1", "R28", "2"), ("U3", "1", "C18", "1"), ("C19", "1", "U3", "5"),
    ("R29", "1", "U3", "6"), ("D18", "3", "R24", "2"), ("C17", "1", "R24", "2"), ("R25", "1", "R24", "2"),
    # 1-Wire
    ("F4", "2", "J10", "2"), ("C16", "1", "J10", "2"), ("C15", "1", "F4", "2"), ("Q1", "3", "F4", "1"),
    ("R19", "2", "Q1", "1"), ("Q2", "3", "Q1", "1"), ("R20", "2", "Q2", "1"), ("R21", "1", "R20", "1"),
    ("R23", "1", "J10", "3"), ("D15", "3", "R23", "2"), ("R22", "2", "R23", "2"),
    # wind (protection at the RJ11 pins, the terminal J5 is in parallel)
    ("D29", "1", "J5", "4"), ("Q6", "3", "D29", "1"), ("C23", "1", "Q6", "3"), ("R38", "2", "Q6", "1"),
    ("D6", "1", "J4", "3"), ("D7", "1", "J4", "5"), ("R3", "1", "J4", "5"), ("R7", "1", "J4", "3"),
    ("R4", "2", "R3", "2"), ("D8", "3", "R3", "2"), ("R5", "1", "R3", "2"), ("C11", "1", "R5", "2"),
    ("D9", "3", "R7", "2"), ("JP2", "2", "R7", "2"), ("R8", "2", "JP2", "1"), ("R9", "1", "R7", "2"), ("C12", "1", "R9", "2"), ("R10", "1", "R9", "2"),
    # VE.Direct
    ("D27", "1", "J14", "3"), ("D27", "2", "J14", "2"),
    ("C22", "1", "U4", "8"), ("C21", "1", "U4", "1"),
    # outputs
    ("D22", "2", "J12", "2"), ("Q3", "5", "F5", "2"), ("R30", "2", "Q3", "4"), ("R31", "1", "Q3", "4"),
    ("JP9", "2", "Q4", "3"), ("Q4", "2", "F6", "1"), ("F6", "2", "J13", "1"),
    ("R32", "2", "Q4", "1"), ("D26", "2", "Q4", "1"),
    ("D23", "1", "F6", "2"),
    ("R33", "1", "Q4", "1"), ("R33", "2", "R62", "1"),
    ("R63", "1", "R33", "1"), ("R63", "2", "R64", "1"),
    ("R64", "2", "R62", "2"), ("Q5", "3", "R62", "2"), ("R37", "1", "Q5", "1"),
    ("D24", "1", "J13", "3"), ("R49", "1", "J13", "3"), ("R49", "2", "Q8", "1"),
    ("R50", "1", "Q8", "1"), ("Q8", "3", "R34", "2"), ("R35", "1", "R34", "2"),
    ("D25", "3", "R35", "2"), ("C20", "1", "R35", "2"), ("R36", "1", "U3", "4"),
    # rain
    ("R11", "1", "D20", "1"), ("R12", "2", "R11", "2"), ("D11", "3", "R11", "2"),
    ("R13", "1", "R11", "2"), ("C13", "1", "R13", "2"),
    ("U2", "4", "J2", "17"), ("R6", "1", "U2", "4"), ("R14", "1", "U2", "6"), ("C14", "1", "U2", "5"),
    # I2C
    ("R15", "1", "J9", "2"), ("R16", "1", "J9", "3"), ("D12", "3", "R15", "2"), ("D13", "3", "R16", "2"),
    ("JP11", "2", "R16", "2"), ("R17", "2", "JP11", "1"), ("R18", "2", "JP1", "1"),
    ("C8", "1", "J9", "1"),
    # ground side of decoupling / converter capacitors to the IC ground pin
    ("C1", "2", "U1", "1"), ("C2", "2", "U1", "1"), ("C33", "2", "U1", "1"),
    ("C4", "2", "U1", "1"), ("C5", "2", "U1", "1"), ("C34", "2", "U1", "1"),
    ("C26", "2", "U1", "1"),
]
SPILL = [(MOD[2] + 1.3, RJ_L - 0.3, 0.6, C_BOT), (0.6, 64.0, C_TOP, C_BOT), (RJ_L + 0.3, W - 0.6, RJ_WIND_TOP + 14.0, C_BOT)]

MOATS = [(VIC_X1, VIC_Y0 - MOAT_W, VIC_X1 + MOAT_W, VIC_Y1 + MOAT_W),     # right of the domain
         (0, VIC_Y0 - MOAT_W, VIC_X1, VIC_Y0), (0, VIC_Y1, VIC_X1, VIC_Y1 + MOAT_W)]
DOMAINS = {"/GND_VICTRON": [(0, VIC_Y0), (VIC_X1, VIC_Y0), (VIC_X1, VIC_Y1), (0, VIC_Y1)]}
GND_OUTLINE = [(0, 0), (W, 0), (W, H), (0, H), (0, VIC_Y1 + MOAT_W), (VIC_X1 + MOAT_W, VIC_Y1 + MOAT_W),
               (VIC_X1 + MOAT_W, VIC_Y0 - MOAT_W), (0, VIC_Y0 - MOAT_W)]
ISOLATORS = ("U4",)
ISO_ZONES = ("VIC",)
M = MOD[0] + 25.1
# module title shifted right / USB-C left: U7, C35-C37 (sensor LDO, Heltec bulk) sit under the module near J2
TEXTS = (("Heltec WiFi LoRa 32 V3 or V4", M + 3.5, ROW_TOP + 7.5, 1.5), ("POPULATE ONE SOCKET SET ONLY", M + 3.5, ROW_TOP + 10.0, 1.0), ("USB-C", MOD[0] + 2.9, ROW_TOP + 11.4, 1.2),
         ("ANT", MOD[0] + 46.5, ROW_TOP + 11.4, 1.2), ("Universal Weather Node v2.0", M, ROW_TOP + 15.0, 1.0), ("franzenhof.com", M, ROW_TOP + 17.5, 1.0))
BRAND = (("Universal Weather Node v2.0", 1.6), ("franzenhof.com", 1.6))
# bottom-side terminal labels (left->right as seen from the top) and back title block
# (by pin number; BACK_LABELS_V for the terminals at the left/right edge)
BACK_LABELS_V = {"J14": ["GND", "TX", "RX", "V+"], "J7": ["GND", "RAIN"], "J5": ["GND", "SPD", "DIR", "3V3"],
                 "J17": ["GND", "SIG", "3V3", "5V"]}
BACK_LABELS = {"J12": ["+V", "HEAT-"], "J13": ["FAN+", "GND", "TACH"], "J3": ["VIN", "GND"], "J11": ["3V3", "AIN", "PULSE", "GND"], "J10": ["GND", "3V3", "DQ"],
               "J9": ["3V3", "SDA", "SCL", "GND"]}
BACK_TITLES = {"J12": "HEATER", "J13": "FAN", "J3": "POWER", "J11": "AUX", "J10": "1-WIRE", "J9": "I2C", "J7": "RAIN", "J5": "WIND", "J14": "VE.DIRECT", "J17": "UV"}
BACK_DOCUMENTATION = True
BACK_TEXTS = (("Universal Weather Node v2.0", 26.0, 17.0, 2.0), ("franzenhof.com", 26.0, 21.5, 1.6), ("CERN-OHL-P v2", 26.0, 25.0, 1.2),
              ("JP8 FITTED: VIN BUCK\nREMOVE BEFORE USB\nUSB ONLY: DISCONNECT VIN\nCHANGE JUMPERS ONLY UNPOWERED", 31.0, 40.0, 0.9))

POWER_NETS = ["GND", "FAN", "FAN_SW", "FAN_SUPPLY", "VIN_IN", "VIN_F", "+VLOAD", "VIN_BUCK", "+5V_BUCK", "BUCK_SW", "BUCK_BST", "BUCK_VCC", "+5V", "+5V_V4", "3V3_HELTEC", "3V3_LDO"]
MEDIUM_NETS, MEDIUM_W = ["+3V3", "OW_SW", "OW_VCC"], 0.4      # sensor supplies (<= 250 mA, PTC-limited)
HEAVY_NETS, HEAVY_W = ["VIN_IN", "VIN_F", "+VLOAD", "HEAT", "HEAT_D"], 1.5   # conservative width; temperature rise is not validated
POWER_W = 0.6
DEF_W, CLEAR = 0.3, 0.25
VIA, PVIA = (0.8, 0.4), (1.0, 0.5)
PCLEAR = 0.25
EDGE = 0.5
GAP = 0.55
GND_VIAS = [
    # surge current of every TVS / ESD diode / crowbar goes straight into the ground plane
    ("D1", "2"), ("D5", "2"), ("D6", "2"), ("D7", "2"), ("D20", "2"), ("D14", "2"), ("D28", "3"),
    ("D16", "3"), ("D17", "3"), ("D27", "3"), ("D32", "3"), ("C22", "2"), ("C21", "2"), ("D24", "2"), ("D23", "2"), ("Q3", "3"), ("Q5", "2"), ("Q8", "2"), ("C20", "2"),
    # every decoupling / filter capacitor gets its own ground via (placed before routing)
    ("C1", "2"), ("C4", "2"), ("C5", "2"), ("C33", "2"), ("C34", "2"), ("C31", "2"), ("C26", "2"),
    ("C7", "2"), ("C9", "2"), ("C10", "2"), ("C11", "2"), ("C12", "2"), ("C13", "2"), ("C14", "2"),
    ("C16", "2"), ("C17", "2"), ("C18", "2"), ("C19", "2"),
    ("R2", "2"), ("U2", "2"), ("U3", "2")]
THERMAL_VIAS = {"U1": ("9", ((-0.6, -0.6), (0.6, -0.6), (-0.6, 0.6), (0.6, 0.6)))}

# protection right behind the terminal pins (pad at the pin, other pad pointing away)
TVS_X = TERM_RX - 10.5         # right-edge TVS column, left of the terminal titles
RA = TPIN - 9.6                # row A pad y (above the terminal label strip)
FIXED_PADS = [
    ("F7", "1", J3_X - 0.5, RA, "2", "up"),                       # J3.1 VIN -> master fuse
    ("D17", "1", J11_X + PITCH - 0.5, RA + 0.9, "3", "up"),       # J11.2/3 AIN / PULSE ESD
    ("D14", "1", J10_X + PITCH - 1.2, RA + 1.0, "2", "up"),       # clear of separate SCL pull-up header
    ("D16", "1", J10_X + 2 * PITCH - 0.8, RA + 0.9, "3", "up"),   # J10.3 1-Wire DQ ESD
    ("D28", "1", J9_X + PITCH - 0.5, RA + 0.9, "3", "up"),        # J9.2/3 SDA / SCL ESD
    ("F5", "1", J12_X + PITCH + 0.4, RA - 1.0, "2", "up"),        # J12 HEAT- -> 4 A fuse
    ("F6", "2", J13_X - 0.3, RA, "1", "up"),                      # J13 FAN+ <- PTC
    ("D24", "1", J13_X + 2 * PITCH + 0.5, RA, "2", "up"),         # J13 TACH TVS
    # right edge: one TVS per line between the RJ11 pin and its screw terminal
    ("D20", "1", TVS_X, J7_Y1 - PITCH, "2", "down"),              # J7 RAIN / J6.3
    ("D7", "1", TVS_X, J5_Y1 - PITCH, "2", "down"),               # J5 SPD / J4.5
    ("D6", "1", TVS_X, J5_Y1 - 2 * PITCH, "2", "up"),             # J5 DIR / J4.3
]
# surge paths fixed before autorouting: terminal pin -> TVS -> jack pin, TVS -> series resistor
# (TVS -> jack: around the left side of the jack's pin column, corridors kept free of parts)
JPIN_X = RJ_X                   # x of the jack's odd pin column (pins 1/3/5)
WSPD_Y, WDIR_Y = J5_Y1 - PITCH, J5_Y1 - 2 * PITCH
J4_P5_Y, J4_P3_Y = RJ_WIND_TOP + 6.65 + 3.175 - 5.08, RJ_WIND_TOP + 6.65 + 3.175 - 2.54
J6_P3_Y = RJ_RAIN_TOP + 6.65 + 3.175 - 2.54
CX_SPD, CX_DIR, CX_RAIN = JPIN_X - 2.5, JPIN_X - 1.5, JPIN_X - 1.5
PRE_ROUTE = [[("J5", "2"), ("D7", "1"), ("@", CX_SPD, WSPD_Y), ("@", CX_SPD, J4_P5_Y), ("J4", "5")],   # wind speed
             [("J5", "3"), ("D6", "1"), ("@", CX_DIR, WDIR_Y), ("@", CX_DIR, J4_P3_Y), ("J4", "3")],   # wind direction
             [("J7", "2"), ("D20", "1"), ("@", CX_RAIN, J7_Y1 - PITCH), ("@", CX_RAIN, J6_P3_Y), ("J6", "3")],  # rain
             [("J9", "2"), ("D28", "1")], [("J9", "3"), ("D28", "2")],
             [("J17", "2"), ("D32", "1")]]
LOCAL_PRE_ROUTE = [
    (0.3, [("C32", "1"), ("@", 29.1, 49.7), ("@", 31.57, 49.7), ("U5", "5")]),
    (0.3, [("U5", "3"), ("C32", "2")]),
    (0.3, [("R33", "2"), ("R62", "1")]),
    (0.3, [("R63", "2"), ("R64", "1")]),
]
RESERVED = [(CX_SPD - 0.6, J4_P5_Y - 0.6, CX_DIR + 0.6, WSPD_Y + 0.6),
            (CX_RAIN - 0.6, J6_P3_Y - 0.6, CX_RAIN + 0.6, J7_Y1 - PITCH + 0.6)]
PRE_ROUTE_W = 0.5
BUCK_PRE_ROUTE = [
    (0.6, [("U1", "8"), ("@", 60.4, 43.2), ("@", 62.2, 43.2),
           ("@", 62.4, 43.4), ("@", 62.6, 43.4), ("@", 62.8, 43.6),
           ("@", 63.0, 43.6), ("@", 63.2, 43.8), ("@", 63.6, 43.8),
           ("@", 63.8, 44.0), ("@", 64.0, 44.0), ("@", 64.2, 44.2),
           ("@", 64.4, 44.2), ("@", 64.8, 44.6), ("L1", "1")]),
    (0.3, [("L1", "1"), ("@", 65.0, 44.6), ("@", 63.2, 44.6), ("C3", "2")]),
    (0.3, [("U1", "7"), ("@", 60.4, 44.4), ("@", 60.8, 44.8),
           ("@", 61.0, 44.8), ("@", 62.6, 46.4), ("C3", "1")]),
    (0.3, [("U1", "6"), ("@", 60.4, 45.8), ("@", 60.6, 46.0),
           ("@", 60.8, 46.0), ("@", 61.0, 46.2), ("@", 61.2, 46.2),
           ("@", 61.4, 46.4), ("@", 61.4, 48.4), ("@", 61.2, 48.6), ("C26", "1")]),
    (0.6, [("U1", "2"), ("C2", "1")]),
    (0.6, [("U1", "3"), ("U1", "2")]),
    (0.6, [("U1", "1"), ("C2", "2")]),
    (0.6, [("C1", "1"), ("@", 52.6, 40.4), ("@", 52.6, 41.2),
           ("@", 51.2, 42.6), ("@", 51.2, 43.4), ("@", 51.4, 43.6),
           ("@", 51.4, 43.8), ("@", 52.4, 44.8), ("C2", "1")]),
    (0.6, [("L1", "2"), ("C4", "1"), ("C5", "1"), ("C34", "1")]),
]
# silkscreen labels next to the pins of unpopulated headers
ROW_LABELS = {"JP1": [("1", "2", "SDA")], "JP11": [("1", "2", "SCL")],
              "JP10": [("1", "2", "3V3"), ("2", "3", "5V")]}
PIN_LABELS = {"TP1": ["GND"], "TP2": ["VIN"], "TP3": ["5V"], "TP4": ["3V3"], "TP5": ["GND"], "J16": ["GND", "3V3", "SDA", "SCL", "VANE", "5V"], "JP2": ["R8", "DIR"], "JP9": ["5V", "COM", "VIN"],
              }
REFINE = ["C1", "C2", "C3", "L1", "C4", "C5", "C7", "C9", "C11", "C12", "C13", "C14", "C16", "C17", "C18", "C19", "R3", "R7", "R11", "R15", "R16"]
SOLID_GND = ("J1", "J2", "J15", "J4", "J6", "J7", "J5", "J16", "TP1", "TP5")

# extra silkscreen notes placed on the nearest free spot (top side, else bottom side)
NOTES = [
         ("J16", "J16 EXPANSION", 0, 0, 0.8), ("JP1", "JP1/JP11 I2C PULL-UP 4k7\nSDA / SCL; fit for long cables", 0, 0, 0.8),
         ("JP8", "JP8: VIN BUCK\nREMOVE BEFORE USB", 0, 0, 0.8),
         ("JP2", "JP2 VANE PULL-UP\nfit: reed-switch vane\nopen: Davis pot", 0, 0, 0.8), ("R25", "R25 15k:\nAIN 0-5V x1.667", 0, 0, 0.8),
         ("JP9", "FAN SUPPLY\n1-2: 5V (default)\n2-3: VIN", 0, 0, 0.8),
         ("JP10", "UV RANGE\n1-2: 3.3V\n2-3: 5V", 0, 0, 0.8)]
# function written right next to the reference designator of each field terminal
TERM_FUNC = {"J12": "HEATER", "J13": "FAN", "J3": "POWER IN", "J11": "AUX IN", "J10": "1-WIRE", "J9": "I2C BME280"}
# pin functions printed on the top side right above each terminal (left -> right)
TOP_PIN_LABELS = {"J12": ["+V", "HEAT-"], "J13": ["FAN+", "GND", "TACH"], "J3": ["VIN", "GND"], "J11": ["3V3", "AIN", "PULSE", "GND"], "J10": ["GND", "3V3", "DQ"],
                  "J9": ["3V3", "SDA", "SCL", "GND"]}
# notes with a reserved area (written before placement): (text, x, y, size, angle)
BRAND_X = (MOD[2] + RJ_L) / 2
FIXED_NOTES = [("Universal Weather Node v2.0", BRAND_X, 3.0, 1.3, 0), ("franzenhof.com", BRAND_X, 5.6, 1.3, 0),
               ("J14 VE.DIRECT", 12.4, J14_Y + 5.25, 1.0, 90),
               ("GND", 9.9, J14_Y, 0.8, 90), ("TX", 9.9, J14_Y + 3.5, 0.8, 90), ("RX", 9.9, J14_Y + 7.0, 0.8, 90), ("V+", 9.9, J14_Y + 10.5, 0.8, 90),
               ("J6 DAVIS RAIN", RJ_L - 0.9, RJ_RAIN_TOP + 6.65, 1.0, 90),
               ("J4 DAVIS WIND", RJ_L - 0.9, RJ_WIND_TOP + 6.65, 1.0, 90),
               # right-edge screw terminals: title + pin names inside the board (as J14)
               ("J7 RAIN", W - 12.4, J7_Y1 - PITCH / 2, 1.0, 90),
               ("RAIN", W - 9.9, J7_Y1 - PITCH, 0.8, 90), ("GND", W - 9.9, J7_Y1, 0.8, 90),
               ("J5 WIND", W - 12.4, J5_Y1 - 1.5 * PITCH, 1.0, 90),
               ("3V3", W - 9.9, J5_Y1 - 3 * PITCH, 0.8, 90), ("DIR", W - 9.9, J5_Y1 - 2 * PITCH, 0.8, 90),
               ("SPD", W - 9.9, J5_Y1 - PITCH, 0.8, 90), ("GND", W - 9.9, J5_Y1, 0.8, 90),
               ("J17 UV SENSOR", W - 12.4, J17_Y1 - 1.5 * PITCH, 1.0, 90),
               ("5V", W - 9.9, J17_Y1 - 3 * PITCH, 0.8, 90), ("3V3", W - 9.9, J17_Y1 - 2 * PITCH, 0.8, 90),
               ("SIG", W - 9.9, J17_Y1 - PITCH, 0.8, 90), ("GND", W - 9.9, J17_Y1, 0.8, 90)]
HIDE_REFS = ("J4", "J6", "J7", "J5", "J14", "J17")       # named in their FIXED_NOTES label
