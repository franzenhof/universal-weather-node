# universal-weather-node "Light" board: hand-solderable through-hole board for the
# Heltec WiFi LoRa 32 V4 (V4.2 / V4.3) only. Same GPIOs and the same signal
# conditioning as the full board (v2.0, V4 socket set) for the subset it carries:
# Davis wind + rain (RJ11 + screw terminal each), BME280 (I2C), DS18B20 (1-Wire,
# switched supply), switched wind-vane supply, hardware revision ID.
# No power input of its own: the module is powered through its USB-C connector
# (header pin 2 = USB VBUS stays open); the sensors run from the module's 3.3 V
# through a PTC. All parts THT except the TVS diodes (SMB; optional PESD5V0S2BT in SOT-23).
# Each part: ref, (lib, symbol), value, footprint, {pin: net|None}, placement key, block
RES = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal"   # 400 mil as Rev 1.3: fits Vishay PR01 (1 W)
CER = "Capacitor_THT:C_Disc_D3.8mm_W2.6mm_P2.50mm"            # multilayer ceramic, 2.5 mm lead pitch
ELKO = "Capacitor_THT:CP_Radial_D5.0mm_P2.00mm"
PTC = "Capacitor_THT:C_Disc_D7.5mm_W2.5mm_P5.00mm"            # radial PTC (Littelfuse RXEF), 5.1 mm pitch
DO35 = "Diode_THT:D_DO-35_SOD27_P7.62mm_Horizontal"
SMB = "Diode_SMD:D_SMB"
SOT23 = "Package_TO_SOT_SMD:SOT-23"
TO92 = "Package_TO_SOT_THT:TO-92_Inline_Wide"                   # 2.54 mm pin pitch, easy to solder
HDR2 = "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical"
SOCK18 = "Connector_PinSocket_2.54mm:PinSocket_1x18_P2.54mm_Vertical"
TB = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_PT-1,5-{n}-3.5-H_1x{n:02d}_P3.50mm_Horizontal"
RJ11 = "Connector_RJ:RJ12_Amphenol_54601-x06_Horizontal"     # = EVERCOM 5301-6P6C pad pattern

def conn(n): return ("Connector_Generic", f"Conn_01x{n:02d}")
R_, C_, CP_ = ("Device", "R"), ("Device", "C"), ("Device", "C_Polarized")
TVS_, BAT_ = ("Device", "D_Zener"), ("Diode", "BAT43")

PARTS = []
def P(ref, sym, value, fp, pins, block, desc="", units=1, dnp=False):
    PARTS.append(dict(ref=ref, sym=sym, value=value, fp=fp, pins=pins, place=None, block=block,
                      desc=desc, lcsc_key="THT " + value, dnp=dnp, units=units))
def clamp(ka, kb, net, block, what):
    """BAT43 pair: net -> +3V3 (ka) and GND -> net (kb). Symbol pins: 1 = K, 2 = A."""
    P(ka, BAT_, "BAT43", DO35, {"1": "+3V3", "2": net}, block, f"clamp {what} to 3.3 V")
    P(kb, BAT_, "BAT43", DO35, {"1": net, "2": "GND"}, block, f"clamp {what} to GND")

# ---------------- Heltec WiFi LoRa 32 V4: J1 = 5 V side, J2 = 3V3 side, J3 = P4 -------
# GPIOs as on the full board's V4 socket set (J19/J20/J21). Pin 2 of J1 (USB VBUS) stays
# open: the module is powered through its own USB-C connector.
j1 = {str(i): None for i in range(1, 19)}
j1.update({"1": "GND", "5": "WSPD_OUT", "6": "ONEWIRE_IO", "11": "VANE_EN", "13": "GPIO47"})
P("J1", conn(18), "Heltec V4 header (5V side)", SOCK18, j1, "MCU",
  "Heltec V4 pins: 1 GND, 2 VBUS (open), 5 GPIO44, 6 GPIO43, 11 GPIO34, 13 GPIO47; 17/18 = USB D+/D- stay open")
j2 = {str(i): None for i in range(1, 19)}
j2.update({"1": "GND", "2": "3V3_HELTEC", "3": "3V3_HELTEC", "7": "SCL", "8": "SDA", "17": "WDIR_ADC"})
P("J2", conn(18), "Heltec V4 header (3V3 side)", SOCK18, j2, "MCU",
  "Heltec V4 pins: 1 GND, 2/3 3V3, 7 GPIO42, 8 GPIO41, 17 GPIO6; GPIO46/45/2/5/7 stay open")
P("J3", conn(2), "Heltec V4 P4 (GPIO15/16)", "Connector_PinSocket_2.54mm:PinSocket_1x02_P2.54mm_Vertical",
  {"1": "HW_ID", "2": "RAIN_OUT"}, "MCU", "P4 pin 1 GPIO15 = HW revision ID, pin 2 GPIO16 = rain; 0.45 mm off the main grid")
HELTEC = {
    "GND":            ("GND",        [], [("J1", "1"), ("J2", "1")]),
    "3V3":            ("3V3_HELTEC", [], [("J2", "2"), ("J2", "3")]),
    "wind direction": ("WDIR_ADC",   [], [("J2", "17")]),   # GPIO6
    "1-Wire":         ("ONEWIRE_IO", [], [("J1", "6")]),    # GPIO43
    "wind speed":     ("WSPD_OUT",   [], [("J1", "5")]),    # GPIO44
    "rain":           ("RAIN_OUT",   [], [("J3", "2")]),    # GPIO16
    "SDA":            ("SDA",        [], [("J2", "8")]),    # GPIO41
    "SCL":            ("SCL",        [], [("J2", "7")]),    # GPIO42
    "1-Wire switch":  ("GPIO47",     [], [("J1", "13")]),   # GPIO47
    "revision ID":    ("HW_ID",      [], [("J3", "1")]),    # GPIO15
    "vane enable":    ("VANE_EN",    [], [("J1", "11")]),   # GPIO34
}
HELTEC_OPEN = {"J1": "2 3 4 7 8 9 10 12 14 15 16 17 18", "J2": "4 5 6 9 10 11 12 13 14 15 16 18"}
HELTEC_REFS = ("J1", "J2", "J3")

# ---------------- hardware revision ID (GPIO15, ADC2, read before Wi-Fi) -------------
# full board v2.0: 1M / 1M = 0.50 x 3V3 (1.65 V); Light: 1M / 470k = 0.32 x 3V3 (1.06 V)
P("R1", R_, "1M", RES, {"1": "3V3_HELTEC", "2": "HW_ID"}, "MCU", "HW revision ID (top)")
P("R2", R_, "470k", RES, {"1": "HW_ID", "2": "GND"}, "MCU", "HW revision ID (bottom), Light: 0.32 x 3V3 = 1.06 V")
P("C1", C_, "100nF", CER, {"1": "HW_ID", "2": "GND"}, "MCU", "low source impedance for the ADC")

# ---------------- 3.3 V sensor rail from the Heltec 3.3 V through a PTC ---------------
P("F1", ("Device", "Polyfuse"), "PTC 0.17A", PTC, {"1": "3V3_HELTEC", "2": "+3V3"}, "Power",
  "Littelfuse RXEF017 (0.17 A hold, radial): cable faults stay off the module's 3.3 V")
P("C2", CP_, "47uF 16V", ELKO, {"1": "+3V3", "2": "GND"}, "Power", "bulk sensor rail, radial 5 mm")
P("C3", C_, "100nF", CER, {"1": "+3V3", "2": "GND"}, "Power")
P("D1", TVS_, "SMLVT3V3", SMB, {"1": "+3V3", "2": "GND"}, "Power", "TVS 3.3 V sensor rail (absorbs what the BAT43 clamps dump)")

# ---------------- Davis anemometer: RJ11 + screw terminal in parallel ----------------
P("J4", conn(6), "Davis wind (RJ11)", RJ11, {"1": None, "2": "WDIR_VCC", "3": "WDIR", "4": "GND", "5": "WSPD", "6": None},
  "Wind", "Davis 6410: 2 yellow 3V3 (switched), 3 green DIR, 4 red GND, 5 black SPD")
P("J5", conn(4), "Wind", TB.format(n=4), {"1": "GND", "2": "WSPD", "3": "WDIR", "4": "WDIR_VCC"}, "Wind",
  "screw terminal parallel to J4: GND | SPD | DIR | 3V3")
P("D2", TVS_, "SMLVT3V3", SMB, {"1": "WDIR", "2": "GND"}, "Wind", "TVS wind direction")
P("D3", TVS_, "SMLVT3V3", SMB, {"1": "WSPD", "2": "GND"}, "Wind", "TVS wind speed")
# wind speed: 330R -> pull-up/clamp -> RC 4.7k/100nF -> Schmitt -> 1k -> GPIO44
P("R3", R_, "330", RES, {"1": "WSPD", "2": "WSPD_A"}, "Wind", "surge series resistor")
P("R4", R_, "4.7k", RES, {"1": "+3V3", "2": "WSPD_A"}, "Wind", "reed-switch pull-up")
clamp("D4", "D5", "WSPD_A", "Wind", "wind speed")
P("R5", R_, "4.7k", RES, {"1": "WSPD_A", "2": "WSPD_B"}, "Wind", "RC low-pass")
P("C4", C_, "100nF", CER, {"1": "WSPD_B", "2": "GND"}, "Wind", "RC low-pass")
P("R6", R_, "1k", RES, {"1": "WSPD_SCH", "2": "WSPD_OUT"}, "Wind", "series to GPIO44")
# wind direction: 330R -> clamp (+ optional pull-up for reed vanes) -> 10k/1uF -> 1k -> GPIO6
P("R7", R_, "330", RES, {"1": "WDIR", "2": "WDIR_A"}, "Wind", "surge series resistor")
clamp("D6", "D7", "WDIR_A", "Wind", "wind direction")
P("R8", R_, "4.7k", RES, {"1": "WDIR_VCC", "2": "WDIR_PU"}, "Wind", "pull-up for reed-switch vanes, activated with JP1")
P("JP1", conn(2), "Vane pull-up", HDR2, {"1": "WDIR_PU", "2": "WDIR_A"}, "Wind",
  "fit for reed-switch vanes; open for the Davis potentiometer")
P("R9", R_, "10k", RES, {"1": "WDIR_A", "2": "WDIR_B"}, "Wind", "RC low-pass")
P("C5", C_, "1uF", CER, {"1": "WDIR_B", "2": "GND"}, "Wind", "RC low-pass (1 uF 50 V X7R, 2.5 mm)")
P("R10", R_, "1k", RES, {"1": "WDIR_B", "2": "WDIR_ADC"}, "Wind", "series to GPIO6")
# switched vane supply (J4 pin 2 / J5 3V3): VANE_EN (GPIO34) LOW = on
P("Q1", ("Transistor_FET", "Q_PMOS_SGD"), "LP0701N3-G", TO92, {"1": "+3V3", "2": "VANE_EN", "3": "WDIR_VCC"}, "Wind",
  "P-MOSFET, logic level (VGS(th) -0.5..-1 V), TO-92 S-G-D: vane supply, GPIO34 LOW = on")
P("R11", R_, "100k", RES, {"1": "+3V3", "2": "VANE_EN"}, "Wind", "keeps the vane supply off without firmware")
P("C6", C_, "100nF", CER, {"1": "WDIR_VCC", "2": "GND"}, "Wind", "decoupling vane supply")
P("D8", TVS_, "SMLVT3V3", SMB, {"1": "WDIR_VCC", "2": "GND"}, "Wind", "TVS switched vane supply")

# ---------------- Davis rain gauge: RJ11 + screw terminal in parallel ----------------
P("J6", conn(6), "Davis rain (RJ11)", RJ11, {"1": None, "2": None, "3": "RAIN", "4": "GND", "5": None, "6": None},
  "Rain", "Davis 6466M: 3 reed switch, 4 GND (3/4 may be swapped)")
P("J7", conn(2), "Rain", TB.format(n=2), {"1": "GND", "2": "RAIN"}, "Rain", "screw terminal parallel to J6: GND | RAIN")
P("D9", TVS_, "SMLVT3V3", SMB, {"1": "RAIN", "2": "GND"}, "Rain", "TVS rain")
P("R12", R_, "330", RES, {"1": "RAIN", "2": "RAIN_A"}, "Rain", "surge series resistor")
P("R13", R_, "4.7k", RES, {"1": "+3V3", "2": "RAIN_A"}, "Rain", "reed-switch pull-up")
clamp("D10", "D11", "RAIN_A", "Rain", "rain")
P("R14", R_, "10k", RES, {"1": "RAIN_A", "2": "RAIN_B"}, "Rain", "RC low-pass (tau ~10 ms)")
P("C7", C_, "1uF", CER, {"1": "RAIN_B", "2": "GND"}, "Rain", "RC low-pass (1 uF 50 V X7R, 2.5 mm)")
P("R15", R_, "1k", RES, {"1": "RAIN_SCH", "2": "RAIN_OUT"}, "Rain", "series to GPIO16")
# 74HC14 (DIP-14, 2-6 V): two of six inverting Schmitt triggers, unused inputs to GND
u1 = {"1": "WSPD_B", "2": "WSPD_SCH", "3": "RAIN_B", "4": "RAIN_SCH", "5": "GND", "6": None, "9": "GND", "8": None,
      "11": "GND", "10": None, "13": "GND", "12": None, "14": "+3V3", "7": "GND"}
P("U1", ("74xx", "74HC14"), "74HC14", "Package_DIP:DIP-14_W7.62mm", u1, "Rain",
  "inverting Schmitt trigger (as the full board's 74LVC2G14): A wind speed, B rain; DIP socket recommended", units=7)
P("C8", C_, "100nF", CER, {"1": "+3V3", "2": "GND"}, "Rain", "decoupling U1")

# ---------------- BME280 (I2C): screw terminal ----------------------------------------
# Data lines: low-capacitance PESD5V0S2BT at the terminal (optional), then 100R series +
# BAT43 clamps into the TVS-protected 3.3 V rail (an SMB TVS would load the bus).
P("J8", conn(4), "I2C", TB.format(n=4), {"1": "+3V3", "2": "SDA_EXT", "3": "SCL_EXT", "4": "GND"}, "I2C",
  "3V3 | SDA | SCL | GND (Cat5: SDA and SCL each twisted with GND)")
P("D19", ("Device", "D_TVS_Dual_AAC"), "PESD5V0S2BT", SOT23, {"1": "SDA_EXT", "2": "SCL_EXT", "3": "GND"}, "I2C",
  "low-capacitance ESD TVS at J8 (SOT-23, as on the V4 Full); optional - may be left out if too fiddly to solder")
P("R16", R_, "100", RES, {"1": "SDA_EXT", "2": "SDA"}, "I2C", "series resistor SDA")
P("R17", R_, "100", RES, {"1": "SCL_EXT", "2": "SCL"}, "I2C", "series resistor SCL")
clamp("D12", "D13", "SDA", "I2C", "SDA")
clamp("D14", "D15", "SCL", "I2C", "SCL")
P("JP2", conn(2), "SDA pull-up", HDR2, {"1": "SDA_PU", "2": "SDA"}, "I2C", "fit for cables longer than 0.5 m")
P("JP3", conn(2), "SCL pull-up", HDR2, {"1": "SCL_PU", "2": "SCL"}, "I2C", "fit for cables longer than 0.5 m")
P("R18", R_, "4.7k", RES, {"1": "+3V3", "2": "SDA_PU"}, "I2C", "extra pull-up for long cables")
P("R19", R_, "4.7k", RES, {"1": "+3V3", "2": "SCL_PU"}, "I2C", "extra pull-up for long cables")

# ---------------- 1-Wire (DS18B20) with switched supply (GPIO47 HIGH = on) ------------
P("J9", conn(3), "1-Wire", TB.format(n=3), {"1": "GND", "2": "OW_VCC", "3": "OW_DQ"}, "1-Wire",
  "GND | 3V3 | DQ, up to 3 DS18B20")
P("D20", ("Device", "D_TVS_Dual_AAC"), "PESD5V0S2BT", SOT23, {"1": "OW_DQ", "2": None, "3": "GND"}, "1-Wire",
  "low-capacitance ESD TVS at J9 DQ (SOT-23); optional - may be left out if too fiddly to solder")
P("Q2", ("Transistor_FET", "Q_PMOS_SGD"), "LP0701N3-G", TO92, {"1": "+3V3", "2": "OW_GATE", "3": "OW_SW"}, "1-Wire",
  "P-MOSFET high-side switch, TO-92 S-G-D")
P("R20", R_, "47k", RES, {"1": "+3V3", "2": "OW_GATE"}, "1-Wire", "keeps Q2 off")
P("Q3", ("Transistor_FET", "Q_NMOS_SGD"), "TN2106N3-G", TO92, {"1": "GND", "2": "Q3_GATE", "3": "OW_GATE"}, "1-Wire",
  "N-MOSFET, logic level (VGS(th) 0.6-2 V), TO-92 S-G-D: GPIO47 HIGH = bus powered")
P("R21", R_, "1k", RES, {"1": "GPIO47", "2": "Q3_GATE"}, "1-Wire", "gate series resistor")
P("R22", R_, "10k", RES, {"1": "GPIO47", "2": "GND"}, "1-Wire", "keeps the bus off at reset")
P("F2", ("Device", "Polyfuse"), "PTC 0.1A", PTC, {"1": "OW_SW", "2": "OW_VCC"}, "1-Wire", "Littelfuse RXEF010 (0.1 A hold, radial)")
P("C9", C_, "1uF", CER, {"1": "OW_VCC", "2": "GND"}, "1-Wire", "1-Wire supply buffer")
P("D16", TVS_, "SMLVT3V3", SMB, {"1": "OW_VCC", "2": "GND"}, "1-Wire", "TVS 1-Wire supply")
P("R23", R_, "4.7k", RES, {"1": "OW_VCC", "2": "ONEWIRE_IO"}, "1-Wire", "1-Wire pull-up")
P("R24", R_, "100", RES, {"1": "OW_DQ", "2": "ONEWIRE_IO"}, "1-Wire", "series resistor DQ")
clamp("D17", "D18", "ONEWIRE_IO", "1-Wire", "1-Wire DQ")
