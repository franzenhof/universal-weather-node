# universal-weather-node hardware v2.0: KiCad/SMD design data, derived from the
# Rev 1.3 Fritzing netlist (pcblayout/universal-weather-node-pcb.fzz) - same GPIOs,
# same signal conditioning - plus wide-range input, parallel screw terminals,
# AUX inputs, HW revision ID, test points and expansion header.
# Each part: ref, (lib, symbol), value, footprint, {pin: net|None}, placement key, block
R0805 = "Resistor_SMD:R_0805_2012Metric"
R2512 = "Resistor_SMD:R_2512_6332Metric"
R1206 = "Resistor_SMD:R_1206_3216Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
C1210 = "Capacitor_SMD:C_1210_3225Metric"
CP63 = "Capacitor_SMD:CP_Elec_6.3x7.7"
F1206 = "Fuse:Fuse_1206_3216Metric"
F1812 = "Fuse:Fuse_1812_4532Metric"
SMB = "Diode_SMD:D_SMB"
SOT23 = "Package_TO_SOT_SMD:SOT-23"
HDR = "Connector_PinHeader_2.54mm:PinHeader_1x{n:02d}_P2.54mm_Vertical"
SOCK18 = "Connector_PinSocket_2.54mm:PinSocket_1x18_P2.54mm_Vertical"
TB = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_PT-1,5-{n}-3.5-H_1x{n:02d}_P3.50mm_Horizontal"
RJ11 = "Connector_RJ:RJ12_Amphenol_54601-x06_Horizontal"     # = EVERCOM 5301-6P6C pad pattern
RJ45 = "Connector_RJ:RJ45_Amphenol_54602-x08_Horizontal"     # = EVERCOM 5301-8P8C pad pattern

def conn(n): return ("Connector_Generic", f"Conn_01x{n:02d}")

PARTS = []
def P(ref, sym, value, fp, pins, place, block, desc="", lcsc_key=None, dnp=False):
    PARTS.append(dict(ref=ref, sym=sym, value=value, fp=fp, pins=pins, place=place,
                      block=block, desc=desc, lcsc_key=lcsc_key or value, dnp=dnp))

# ---------------- Heltec WiFi LoRa 32 V3 (GPIOs as on Rev 1.3) -------------------
j1 = {str(i): None for i in range(1, 19)}
j1.update({"1": "GND", "2": "+5V", "11": "VE_TX_MCU", "12": "VE_RX_MCU", "13": "GPIO47", "14": "PULSE_OUT", "17": "HW_ID", "18": "GPIO19"})
P("J1", conn(18), "Heltec V3 header (5V side)", SOCK18, j1, None, "MCU",
  "Heltec pins: 1 GND, 2 5V, 11 GPIO34, 12 GPIO33, 13 GPIO47, 14 GPIO48, 17 GPIO20, 18 GPIO19", lcsc_key="SOCK18")
j2 = {str(i): None for i in range(1, 19)}
j2.update({"1": "GND", "2": "3V3_HELTEC", "3": "3V3_HELTEC", "7": "SCL", "8": "SDA", "9": "TACH_OUT", "10": "FAN_PWM", "11": "HEAT_PWM", "13": "WDIR_ADC", "14": "UV_ADC",
           "15": "AIN_ADC", "16": "ONEWIRE_IO", "17": "WSPD_OUT", "18": "RAIN_OUT"})
P("J2", conn(18), "Heltec V3 header (3V3 side)", SOCK18, j2, None, "MCU",
  "Heltec pins: 1 GND, 2/3 3V3, 7 GPIO42, 8 GPIO41, 9 GPIO40, 10 GPIO39, 11 GPIO38, 13 GPIO2, 14 GPIO3, 15 GPIO4, 16 GPIO5, 17 GPIO6, 18 GPIO7",
  lcsc_key="SOCK18")

P("J3", conn(2), "Power", TB.format(n=2), {"1": "VIN_IN", "2": "GND"}, None, "Power",
  "VIN 6-29.2 V LiFePO4 | GND; no external 5 V input", lcsc_key="TB2")
P("F7", ("Device", "Fuse"), "6.3A 125V", "Fuse:Fuse_Littelfuse-NANO2-451_453", {"1": "VIN_IN", "2": "VIN_F"}, None, "Power",
  "Littelfuse 045106.3MRL SMD main fuse; 125 VDC, not the former 5x20 mm cartridge", lcsc_key="FUSE6A3_125")
P("D1", ("Device", "D_TVS"), "SMBJ30CA", SMB, {"1": "VIN_F", "2": "GND"}, None, "Power",
  "bidirectional 30 V standoff TVS, 48.4 V max clamp; no forward crowbar with reversed VIN")
P("Q7", ("Transistor_FET", "Q_PMOS_GDS"), "NCE60P50K", "Package_TO_SOT_SMD:TO-252-3_TabPin2",
  {"1": "PWR_GATE", "2": "VIN_F", "3": "+VLOAD"}, None, "Power",
  "60 V P-channel reverse-polarity switch; drain at input, source at protected rail; 28 mOhm max at VGS=-10 V",
  lcsc_key="NCE60P50K")
P("D31", ("Device", "D_Zener"), "BZT52C12", "Diode_SMD:D_SOD-123",
  {"1": "+VLOAD", "2": "PWR_GATE"}, None, "Power", "12 V gate-source clamp for Q7")
P("R45", ("Device", "R"), "10k", R0805, {"1": "PWR_GATE", "2": "GND"}, None, "Power",
  "pulls Q7 gate low; D31 limits VGS to 12 V")
P("F1", ("Device", "Polyfuse"), "PTC 1.1A 60V", "Fuse:Fuse_2920_7451Metric",
  {"1": "+VLOAD", "2": "VIN_BUCK"}, None, "Power", "buck input branch protection")
P("U1", ("WeatherNode", "LMR36520"), "LMR36520ADDAR", "Package_SO:SOIC-8-1EP_3.9x4.9mm_P1.27mm_EP2.514x3.2mm",
  {"1": "GND", "2": "VIN_BUCK", "3": "VIN_BUCK", "4": None, "5": "BUCK_FB",
   "6": "BUCK_VCC", "7": "BUCK_BST", "8": "BUCK_SW", "9": "GND"}, None, "Power",
  "4.2-65 V in, 5 V rail via JP8 disconnect, 2 A synchronous buck", lcsc_key="LMR36520")
P("C1", ("Device", "C"), "10uF 100V", C1210, {"1": "VIN_BUCK", "2": "GND"}, None, "Power")
P("C2", ("Device", "C"), "220nF 100V", C0805, {"1": "VIN_BUCK", "2": "GND"}, None, "Power")
P("C3", ("Device", "C"), "100nF", C0805, {"1": "BUCK_BST", "2": "BUCK_SW"}, None, "Power", "bootstrap")
P("L1", ("Device", "L"), "10uH", "Inductor_SMD:L_Bourns_SRP5030T", {"1": "BUCK_SW", "2": "+5V_BUCK"}, None, "Power",
  "Bourns SRP5030T-100M: 2.75 A RMS, 3.5 A soft saturation; LMR36520 400 kHz", lcsc_key="L10u")
P("C26", ("Device", "C"), "1uF 50V", C0805, {"1": "BUCK_VCC", "2": "GND"}, None, "Power", "LMR36520 mandatory 1 uF VCC bypass", lcsc_key="1uF")
P("C4", ("Device", "C"), "22uF 25V", C0805, {"1": "+5V_BUCK", "2": "GND"}, None, "Power", lcsc_key="22u")
P("C5", ("Device", "C"), "22uF 25V", C0805, {"1": "+5V_BUCK", "2": "GND"}, None, "Power", lcsc_key="22u")
P("R46", ("Device", "R"), "47k", R0805, {"1": "+5V_BUCK", "2": "BUCK_FB"}, None, "Power", "LMR36520 feedback top")
P("R47", ("Device", "R"), "10k", R0805, {"1": "BUCK_FB", "2": "FB_LOW"}, None, "Power", "LMR36520 feedback bottom")
P("R48", ("Device", "R"), "1.8k", R0805, {"1": "FB_LOW", "2": "GND"}, None, "Power", "series with R47; total feedback bottom 11.8k")
P("JP8", conn(2), "VIN buck enable", HDR.format(n=2),
  {"1": "+5V_BUCK", "2": "+5V"}, None, "Power",
  "fitted: VIN buck powers 5 V rail; REMOVE BEFORE USB; no automatic source isolation",
  lcsc_key="HDR2")
P("C6", ("Device", "C_Polarized"), "100uF", CP63, {"1": "+5V", "2": "GND"}, None, "Power")
P("C7", ("Device", "C"), "100nF", C0805, {"1": "+5V", "2": "GND"}, None, "Power")
# 3.3 V sensor rail from the Heltec LDO, current-limited for the external cables
P("F3", ("Device", "Polyfuse"), "PTC 0.25A", F1206, {"1": "3V3_HELTEC", "2": "+3V3"}, None, "Power")
P("C8", ("Device", "C"), "10uF 50V", C1210, {"1": "+3V3", "2": "GND"}, None, "Power", "bulk 3.3 V sensor rail (Rev 1.3: 100 uF electrolytic)")
P("C9", ("Device", "C"), "100nF", C0805, {"1": "+3V3", "2": "GND"}, None, "Power")
P("D5", ("Device", "D_Zener"), "SMLVT3V3", SMB, {"1": "+3V3", "2": "GND"}, None, "Power", "TVS 3.3V on the sensor rail")

# ---------------- hardware revision ID: divider on GPIO20 (ADC2, read before WiFi) ---
# v1.x: pin open, v2.0: 1M/1M -> 0.50 * 3V3 (1.65 uA, always-on rail)
P("R1", ("Device", "R"), "1M", R0805, {"1": "3V3_HELTEC", "2": "HW_ID"}, None, "MCU", "HW revision ID (top)")
P("R2", ("Device", "R"), "1M", R0805, {"1": "HW_ID", "2": "GND"}, None, "MCU", "HW revision ID (bottom), v2.0: 0.50 * 3V3")
P("C10", ("Device", "C"), "100nF", C0805, {"1": "HW_ID", "2": "GND"}, None, "MCU", "low source impedance for the ADC")

# ---------------- test points and expansion header (not populated) ---------------
TPFP = "TestPoint:TestPoint_THTPad_D2.0mm_Drill1.0mm"
for ref, net, lab in (("TP1", "GND", "GND"), ("TP2", "+VLOAD", "VIN"), ("TP3", "+5V", "5V"), ("TP4", "+3V3", "3V3"),
                      ("TP5", "GND", "GND")):
    P(ref, ("Connector", "TestPoint"), lab, TPFP, {"1": net}, None, "Power", "test point (not populated)", lcsc_key="-")
P("J16", conn(6), "Expansion", HDR.format(n=6), {"1": "GND", "2": "+3V3", "3": "SDA", "4": "SCL", "5": "GPIO19", "6": "+5V"},
  None, "MCU", "I2C + GPIO19 (3.3 V logic, unprotected), not populated", lcsc_key="HDR6", dnp=True)

# ---------------- Davis anemometer: RJ11 + screw terminal in parallel ---------------
P("J4", conn(6), "Davis wind (RJ11)", RJ11, {"1": None, "2": "WDIR_VCC", "3": "WDIR", "4": "GND", "5": "WSPD", "6": None},
  None, "Wind", "2 yellow 3.3 V, 3 green direction, 4 red GND, 5 black speed", lcsc_key="RJ11")
P("J5", conn(4), "Wind", TB.format(n=4), {"1": "GND", "2": "WSPD", "3": "WDIR", "4": "WDIR_VCC"}, None, "Wind",
  "right edge below the RJ11 jack, read top->bottom: 3V3 | DIR | SPD | GND", lcsc_key="TB4")
P("D6", ("Device", "D_Zener"), "SMLVT3V3", SMB, {"1": "WDIR", "2": "GND"}, None, "Wind", "TVS wind direction (at J5, jack J4 next to it)")
P("D7", ("Device", "D_Zener"), "SMLVT3V3", SMB, {"1": "WSPD", "2": "GND"}, None, "Wind", "TVS wind speed (at J5, jack J4 next to it)")
# wind speed: 330R -> pull-up/clamp -> RC 4.7k/100nF -> Schmitt -> 1k -> GPIO6
P("R3", ("Device", "R"), "330", R1206, {"1": "WSPD", "2": "WSPD_A"}, None, "Wind", "surge series resistor", lcsc_key="330R1206")
P("R4", ("Device", "R"), "4.7k", R0805, {"1": "+3V3", "2": "WSPD_A"}, None, "Wind", "reed-switch pull-up")
P("D8", ("Diode", "BAT54S"), "BAT54S", SOT23, {"1": "GND", "2": "+3V3", "3": "WSPD_A"}, None, "Wind", "clamp (was BAT43 pair)")
P("R5", ("Device", "R"), "4.7k", R0805, {"1": "WSPD_A", "2": "WSPD_B"}, None, "Wind", "RC low-pass")
P("C11", ("Device", "C"), "100nF", C0805, {"1": "WSPD_B", "2": "GND"}, None, "Wind", "RC low-pass")
P("R6", ("Device", "R"), "1k", R0805, {"1": "WSPD_SCH", "2": "WSPD_OUT"}, None, "Wind", "series to GPIO6")
# wind direction: 330R -> clamp (+ optional pull-up for non-Davis vanes) -> 10k/1uF -> 1k -> GPIO2
P("R7", ("Device", "R"), "330", R1206, {"1": "WDIR", "2": "WDIR_A"}, None, "Wind", "surge series resistor", lcsc_key="330R1206")
P("D9", ("Diode", "BAT54S"), "BAT54S", SOT23, {"1": "GND", "2": "+3V3", "3": "WDIR_A"}, None, "Wind", "clamp (was BAT43 pair)")
P("R8", ("Device", "R"), "4.7k", R0805, {"1": "WDIR_VCC", "2": "WDIR_PU"}, None, "Wind",
  "pull-up for reed-switch wind vanes (Rev 1.3: R21), active with JP2")
P("JP2", conn(2), "Vane pull-up", HDR.format(n=2), {"1": "WDIR_PU", "2": "WDIR_A"}, None, "Wind",
  "jumper on = 4.7k pull-up for reed-switch vanes, off = Davis potentiometer", lcsc_key="HDR2")
P("R9", ("Device", "R"), "10k", R0805, {"1": "WDIR_A", "2": "WDIR_B"}, None, "Wind", "RC low-pass")
P("C12", ("Device", "C"), "1uF 50V", C0805, {"1": "WDIR_B", "2": "GND"}, None, "Wind", "RC low-pass", lcsc_key="1uF")
P("R10", ("Device", "R"), "1k", R0805, {"1": "WDIR_B", "2": "WDIR_ADC"}, None, "Wind", "series to GPIO2")
# switched supply of the wind-vane potentiometer (J4 pin 2 / J5 3V3): GPIO19 LOW = on.
# Off between samples in low-power mode (the Davis pot draws ~165 uA continuously).
P("Q6", ("Transistor_FET", "AO3401A"), "AO3401A", SOT23, {"1": "GPIO19", "2": "+3V3", "3": "WDIR_VCC"}, None, "Wind",
  "high-side switch of the wind-vane supply, GPIO19 LOW = on")
P("R38", ("Device", "R"), "100k", R0805, {"1": "+3V3", "2": "GPIO19"}, None, "Wind", "keeps the vane supply off without firmware")
P("C23", ("Device", "C"), "100nF", C0805, {"1": "WDIR_VCC", "2": "GND"}, None, "Wind", "decoupling vane supply")
P("D29", ("Device", "D_Zener"), "SMLVT3V3", SMB, {"1": "WDIR_VCC", "2": "GND"}, None, "Wind", "TVS switched vane supply (J5 3V3 / J4.2)")

# ---------------- Davis rain gauge: RJ11 + screw terminal in parallel ---------------
P("J6", conn(6), "Davis rain (RJ11)", RJ11, {"1": None, "2": None, "3": "RAIN", "4": "GND", "5": None, "6": None},
  None, "Rain", "3 reed switch, 4 GND (3/4 may be swapped)", lcsc_key="RJ11")
P("J7", conn(2), "Rain", TB.format(n=2), {"1": "GND", "2": "RAIN"}, None, "Rain",
  "right edge below the RJ11 jack, read top->bottom: RAIN | GND", lcsc_key="TB2")
P("D20", ("Device", "D_Zener"), "SMLVT3V3", SMB, {"1": "RAIN", "2": "GND"}, None, "Rain", "TVS rain (at J7, jack J6 next to it)")
P("R11", ("Device", "R"), "330", R1206, {"1": "RAIN", "2": "RAIN_A"}, None, "Rain", "surge series resistor", lcsc_key="330R1206")
P("R12", ("Device", "R"), "4.7k", R0805, {"1": "+3V3", "2": "RAIN_A"}, None, "Rain", "reed-switch pull-up")
P("D11", ("Diode", "BAT54S"), "BAT54S", SOT23, {"1": "GND", "2": "+3V3", "3": "RAIN_A"}, None, "Rain", "clamp (was BAT43 pair)")
P("R13", ("Device", "R"), "10k", R0805, {"1": "RAIN_A", "2": "RAIN_B"}, None, "Rain", "RC low-pass (tau ~10 ms)")
P("C13", ("Device", "C"), "1uF 50V", C0805, {"1": "RAIN_B", "2": "GND"}, None, "Rain", "RC low-pass", lcsc_key="1uF")
P("R14", ("Device", "R"), "1k", R0805, {"1": "RAIN_SCH", "2": "RAIN_OUT"}, None, "Rain", "series to GPIO7")
P("U2", ("74xGxx", "74LVC2G14"), "SN74LVC2G14DBVR", "Package_TO_SOT_SMD:SOT-23-6",
  {"1": "RAIN_B", "6": "RAIN_SCH", "3": "WSPD_B", "4": "WSPD_SCH", "5": "+3V3", "2": "GND"}, None, "Rain",
  "dual Schmitt trigger (inverting): rain -> GPIO7, wind speed -> GPIO6", lcsc_key="LVC2G14")
PARTS[-1]["units"] = 3
P("C14", ("Device", "C"), "100nF", C0805, {"1": "+3V3", "2": "GND"}, None, "Rain", "decoupling U2")

# ---------------- BME280 (I2C): screw terminal (the Rev 1.3 RJ45 jack was dropped) -----
P("J9", conn(4), "I2C", TB.format(n=4), {"1": "+3V3", "2": "SDA_EXT", "3": "SCL_EXT", "4": "GND"}, None, "I2C",
  "3V3 | SDA | SCL | GND", lcsc_key="TB4")
P("D28", ("Device", "D_TVS_Dual_AAC"), "PESD5V0S2BT", SOT23, {"1": "SDA_EXT", "2": "SCL_EXT", "3": "GND"}, None, "I2C",
  "ESD at the I2C terminal", lcsc_key="PESD")
P("R15", ("Device", "R"), "33 1W", R2512, {"1": "SDA_EXT", "2": "SDA"}, None, "I2C", "series resistor SDA", lcsc_key="33 1W")
P("R16", ("Device", "R"), "33 1W", R2512, {"1": "SCL_EXT", "2": "SCL"}, None, "I2C", "series resistor SCL", lcsc_key="33 1W")
P("D12", ("Diode", "BAT54S"), "BAT54S", SOT23, {"1": "GND", "2": "+3V3", "3": "SDA"}, None, "I2C", "clamp SDA")
P("D13", ("Diode", "BAT54S"), "BAT54S", SOT23, {"1": "GND", "2": "+3V3", "3": "SCL"}, None, "I2C", "clamp SCL")
P("JP1", ("Jumper", "Jumper_2_Open"), "SDA pull-up", "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
  {"1": "SDA_PU", "2": "SDA"}, None, "I2C",
  "extra 4.7k on SDA; separate 2-pin header, 5.08 mm row spacing from JP11", lcsc_key="HDR2")
P("JP11", ("Jumper", "Jumper_2_Open"), "SCL pull-up", "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
  {"1": "SCL_PU", "2": "SCL"}, None, "I2C",
  "extra 4.7k on SCL; separate 2-pin header, one empty 2.54 mm grid row from JP1", lcsc_key="HDR2")
P("R17", ("Device", "R"), "4.7k", R0805, {"1": "+3V3", "2": "SCL_PU"}, None, "I2C", "extra pull-up for long cables")
P("R18", ("Device", "R"), "4.7k", R0805, {"1": "+3V3", "2": "SDA_PU"}, None, "I2C", "extra pull-up for long cables")

# ---------------- 1-Wire (DS18B20) with switched supply (GPIO47 HIGH = on) ----------
P("J10", conn(3), "1-Wire", TB.format(n=3), {"1": "GND", "2": "OW_VCC", "3": "OW_DQ"}, None, "1-Wire",
  "GND | 3V3 | DQ", lcsc_key="TB3")
P("Q1", ("Transistor_FET", "AO3401A"), "AO3401A", SOT23, {"1": "OW_GATE", "2": "+3V3", "3": "OW_SW"}, None, "1-Wire",
  "high-side switch (was LP0701)")
P("R19", ("Device", "R"), "47k", R0805, {"1": "+3V3", "2": "OW_GATE"}, None, "1-Wire", "keeps Q1 off")
P("Q2", ("Transistor_FET", "AO3400A"), "AO3400A", SOT23, {"1": "Q2_GATE", "2": "GND", "3": "OW_GATE"}, None, "1-Wire",
  "driver (was TN2106)")
P("R20", ("Device", "R"), "1k", R0805, {"1": "GPIO47", "2": "Q2_GATE"}, None, "1-Wire", "gate series resistor")
P("R21", ("Device", "R"), "10k", R0805, {"1": "GPIO47", "2": "GND"}, None, "1-Wire", "keeps the bus off at reset")
P("F4", ("Device", "Polyfuse"), "PTC 0.1A", F1206, {"1": "OW_SW", "2": "OW_VCC"}, None, "1-Wire")
P("C15", ("Device", "C"), "10uF 50V", C1210, {"1": "OW_VCC", "2": "GND"}, None, "1-Wire", "bus buffer (Rev 1.3: 4.7 uF)")
P("C16", ("Device", "C"), "100nF", C0805, {"1": "OW_VCC", "2": "GND"}, None, "1-Wire")
P("D14", ("Device", "D_Zener"), "SMLVT3V3", SMB, {"1": "OW_VCC", "2": "GND"}, None, "1-Wire", "TVS 1-Wire supply")
P("R22", ("Device", "R"), "4.7k", R0805, {"1": "OW_VCC", "2": "ONEWIRE_IO"}, None, "1-Wire", "1-Wire pull-up")
P("R23", ("Device", "R"), "33 1W", R2512, {"1": "OW_DQ", "2": "ONEWIRE_IO"}, None, "1-Wire", "series resistor DQ", lcsc_key="33 1W")
P("D15", ("Diode", "BAT54S"), "BAT54S", SOT23, {"1": "GND", "2": "+3V3", "3": "ONEWIRE_IO"}, None, "1-Wire", "clamp DQ")
P("D16", ("Device", "D_TVS_Dual_AAC"), "PESD5V0S2BT", SOT23, {"1": "OW_DQ", "2": None, "3": "GND"}, None, "1-Wire",
  "ESD at the 1-Wire data input", lcsc_key="PESD")

# ---------------- AUX inputs: analog 0-3.3 V (GPIO4) + pulse/digital (GPIO48) -------
P("J11", conn(4), "AUX", TB.format(n=4), {"1": "+3V3", "2": "AIN", "3": "PULSE", "4": "GND"}, None, "AUX",
  "3V3 | AIN | PULSE | GND", lcsc_key="TB4")
P("D17", ("Device", "D_TVS_Dual_AAC"), "PESD5V0S2BT", SOT23, {"1": "AIN", "2": "PULSE", "3": "GND"}, None, "AUX",
  "ESD/surge at both AUX inputs", lcsc_key="PESD")
P("R24", ("Device", "R"), "10k", R0805, {"1": "AIN", "2": "AIN_ADC"}, None, "AUX", "series resistor / divider top")
P("R25", ("Device", "R"), "15k", R0805, {"1": "AIN_ADC", "2": "GND"}, None, "AUX",
  "optional 0-5 V divider: 10k/15k gives 3 V at ADC, scale 1.66667; DNP = direct input", dnp=True)
P("D18", ("Diode", "BAT54S"), "BAT54S", SOT23, {"1": "GND", "2": "+3V3", "3": "AIN_ADC"}, None, "AUX", "clamp AIN")
P("C17", ("Device", "C"), "100nF", C0805, {"1": "AIN_ADC", "2": "GND"}, None, "AUX", "ADC filter")
P("R26", ("Device", "R"), "220", R0805, {"1": "PULSE", "2": "PULSE_A"}, None, "AUX", "series resistor")
P("R27", ("Device", "R"), "4.7k", R0805, {"1": "+3V3", "2": "PULSE_A"}, None, "AUX", "pull-up (reed / open collector)")
P("D19", ("Diode", "BAT54S"), "BAT54S", SOT23, {"1": "GND", "2": "+3V3", "3": "PULSE_A"}, None, "AUX", "clamp PULSE")
P("R28", ("Device", "R"), "4.7k", R0805, {"1": "PULSE_A", "2": "PULSE_B"}, None, "AUX", "RC low-pass (tau 0.47 ms)")
P("C18", ("Device", "C"), "100nF", C0805, {"1": "PULSE_B", "2": "GND"}, None, "AUX", "RC low-pass")
P("U3", ("74xGxx", "74LVC2G14"), "SN74LVC2G14DBVR", "Package_TO_SOT_SMD:SOT-23-6",
  {"1": "PULSE_B", "6": "PULSE_SCH", "3": "TACH_B", "4": "TACH_SCH", "5": "+3V3", "2": "GND"}, None, "AUX",
  "dual Schmitt trigger (inverting): AUX pulse -> GPIO48, fan tacho -> GPIO40", lcsc_key="LVC2G14")
PARTS[-1]["units"] = 3
P("C19", ("Device", "C"), "100nF", C0805, {"1": "+3V3", "2": "GND"}, None, "AUX", "decoupling U3")
P("R29", ("Device", "R"), "1k", R0805, {"1": "PULSE_SCH", "2": "PULSE_OUT"}, None, "AUX", "series to GPIO48")


# ---------------- switched outputs from the protected VIN rail -----------------------
P("J12", conn(2), "Heater", TB.format(n=2), {"1": "+VLOAD", "2": "HEAT"}, None, "Outputs",
  "bottom edge next to POWER: +V | HEAT-", lcsc_key="TB2")
P("F5", ("Device", "Fuse"), "4A 250V", "Fuse:Fuse_Littelfuse-NANO2-451_453", {"1": "HEAT", "2": "HEAT_D"}, None, "Outputs",
  "SCT6125 SMD time-lag fuse: 250 VAC / 125 VDC; heater max 3 A; NANO2 pad geometry matches 6.9 x 3.0 mm land pattern", lcsc_key="FUSE4A250")
P("Q3", ("Transistor_FET", "SiS454DN"), "AON7264E", "Package_SO:Vishay_PowerPAK_1212-8_Single",
  {"1": "GND", "2": "GND", "3": "GND", "4": "HEAT_G", "5": "HEAT_D"}, None, "Outputs",
  "heater low-side switch 60 V, 13 mOhm at 4.5 V gate drive (driven with 5 V by U5), DFN 3.3x3.3; GPIO38 HIGH = on (PWM)")
# 5 V gate driver: the AON7264E is only specified from 4.5 V gate drive; the TTL input takes the 3.3 V GPIO
P("U5", ("74xGxx", "74LVC1G125"), "SN74AHCT1G125DCKR", "Package_TO_SOT_SMD:SOT-353_SC-70-5",
  {"1": "GND", "2": "HEAT_PWM", "3": "GND", "4": "HEAT_DRV", "5": "+5V"}, None, "Outputs", "heater gate driver 3.3 V -> 5 V", lcsc_key="AHCT1G125")
P("R39", ("Device", "R"), "100k", "Resistor_SMD:R_0402_1005Metric", {"1": "HEAT_PWM", "2": "GND"}, None, "Outputs",
  "driver input pull-down: heater off while GPIO38 floats at reset", lcsc_key="100k0402")
P("R30", ("Device", "R"), "220", R0805, {"1": "HEAT_DRV", "2": "HEAT_G"}, None, "Outputs", "gate series resistor")
P("R31", ("Device", "R"), "100k", R0805, {"1": "HEAT_G", "2": "GND"}, None, "Outputs", "keeps the heater off at reset")
P("D22", ("Diode", "SS34"), "B360A", "Diode_SMD:D_SMA", {"1": "+VLOAD", "2": "HEAT"}, None, "Outputs",
  "heater freewheel: 3 A / 60 V; reverse rating exceeds 48.4 V input TVS clamp", lcsc_key="B360A")
P("J13", conn(3), "Fan", TB.format(n=3), {"1": "FAN", "2": "GND", "3": "TACH"}, None, "Outputs",
  "bottom edge: FAN+ | GND | TACH; 0.5 A at 5 V or 1 A at VIN", lcsc_key="TB3")
P("JP9", conn(3), "Fan supply", HDR.format(n=3), {"1": "+5V", "2": "FAN_SUPPLY", "3": "+VLOAD"}, None, "Outputs",
  "shunt 1-2 = 5 V (default, max 0.5 A); 2-3 = VIN (max 1 A)", lcsc_key="HDR3")
P("Q4", ("Transistor_FET", "Q_PMOS_GDS"), "DMP6023LE", "Package_TO_SOT_SMD:SOT-223-3_TabPin2",
  {"1": "FAN_G", "2": "FAN_SW", "3": "FAN_SUPPLY"}, None, "Outputs",
  "60 V P-MOSFET fan switch; 35 mOhm max at VGS=-4.5 V; SOT-223, G/D/S=1/2/3", lcsc_key="DMP6023LE")
P("R32", ("Device", "R"), "100k", R0805, {"1": "FAN_SUPPLY", "2": "FAN_G"}, None, "Outputs", "keeps Q4 off; with R33, VGS is 97.85% of supply below the zener clamp")
P("D26", ("Device", "D_Zener"), "BZT52C12", "Diode_SMD:D_SOD-123", {"1": "FAN_SUPPLY", "2": "FAN_G"}, None, "Outputs", "gate-source clamp 12 V")
P("R33", ("Device", "R"), "2.2k", R1206, {"1": "FAN_G", "2": "FAN_DRV"}, None, "Outputs", "gate drive; about 0.14 W at 29.2 V, 0.25 W 1206", lcsc_key="2k2R1206")
P("Q5", ("Transistor_FET", "AO3400A"), "AO3422", SOT23, {"1": "FAN_PWM", "2": "GND", "3": "FAN_DRV"}, None, "Outputs",
  "fan driver 55 V, GPIO39 HIGH = on (PWM capable)", lcsc_key="AO3422")
P("R37", ("Device", "R"), "100k", R0805, {"1": "FAN_PWM", "2": "GND"}, None, "Outputs", "keeps the fan off at reset")
P("R61", ("Device", "R"), "10k", R0805, {"1": "+5V", "2": "TACH_PU"}, None, "Outputs", "PC fan open-collector tacho pull-up, enabled by JP12")
P("JP12", conn(2), "Tacho pull-up", HDR.format(n=2), {"1": "TACH_PU", "2": "TACH"}, None, "Outputs",
  "fit for open-collector PC fan; remove for externally pulled-up or push-pull tacho", lcsc_key="HDR2")
P("F6", ("Device", "Polyfuse"), "PTC 1.5A 60V", "Fuse:Fuse_2920_7451Metric", {"1": "FAN_SW", "2": "FAN"}, None, "Outputs", "fan output, max 1 A; 60 V withstands the TVS clamp", lcsc_key="PTC1.5A60")
P("D23", ("Diode", "SS34"), "B360A", "Diode_SMD:D_SMA", {"1": "FAN", "2": "GND"}, None, "Outputs",
  "fan freewheel: 3 A / 60 V; reverse rating exceeds 48.4 V input TVS clamp", lcsc_key="B360A")
# Fan tacho is level-shifted before the 3.3 V logic network; 24/29.2 V pull-ups
# can only drive Q8's base through R49 and cannot back-feed the 3.3 V rail.
P("D24", ("Device", "D_Zener"), "SMBJ30A", SMB, {"1": "TACH", "2": "GND"}, None, "Outputs", "30 V standoff tacho TVS")
P("R49", ("Device", "R"), "47k", R0805, {"1": "TACH", "2": "TACH_BASE"}, None, "Outputs", "limits level-shifter base current at 29.2 V")
P("R50", ("Device", "R"), "100k", R0805, {"1": "TACH_BASE", "2": "GND"}, None, "Outputs", "keeps tacho level shifter off")
P("Q8", ("Transistor_BJT", "Q_NPN_BEC"), "MMBT3904", SOT23,
  {"1": "TACH_BASE", "2": "GND", "3": "TACH_A"}, None, "Outputs",
  "40 V NPN tacho level shifter; prevents external 24/29.2 V pull-up back-feed", lcsc_key="MMBT3904")
P("R34", ("Device", "R"), "10k", R0805, {"1": "+3V3", "2": "TACH_A"}, None, "Outputs", "3.3 V pull-up on level-shifted collector")
P("R35", ("Device", "R"), "4.7k", R0805, {"1": "TACH_A", "2": "TACH_B"}, None, "Outputs", "series resistor / RC (tau 0.47 ms)")
P("D25", ("Diode", "BAT54S"), "BAT54S", SOT23, {"1": "GND", "2": "+3V3", "3": "TACH_B"}, None, "Outputs", "clamp tacho")
P("C20", ("Device", "C"), "100nF", C0805, {"1": "TACH_B", "2": "GND"}, None, "Outputs", "RC low-pass tacho")
P("R36", ("Device", "R"), "1k", R0805, {"1": "TACH_SCH", "2": "TACH_OUT"}, None, "Outputs", "series to GPIO40")

# ---------------- Victron VE.Direct (isolated, as on the water node) --------------------
# VE.Direct GND is the charge controller's battery minus -> galvanic isolation (ADuM121N)
P("U4", ("Isolator", "ADuM121N"), "ADuM121N1BRZ", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm",
  {"1": "+3V3", "2": "VE_RX_MCU", "3": "VE_TX_MCU", "4": "GND", "5": "GND_VICTRON", "6": "VE_TX", "7": "VE_RX", "8": "VDD_VICTRON"},
  None, "Victron", "VE.Direct TX -> GPIO33, GPIO34 -> VE.Direct RX; side 2 powered by the Victron", lcsc_key="ADUM121N")
P("C21", ("Device", "C"), "100nF", C0805, {"1": "+3V3", "2": "GND"}, None, "Victron", "decoupling U4 side 1")
P("C22", ("Device", "C"), "100nF", C0805, {"1": "VDD_VICTRON", "2": "GND_VICTRON"}, None, "Victron", "decoupling U4 side 2")
P("J14", conn(4), "VE.Direct", TB.format(n=4), {"1": "GND_VICTRON", "2": "VE_TX", "3": "VE_RX", "4": "VDD_VICTRON"}, None, "Victron",
  "1 GND, 2 TX (to Victron), 3 RX (from Victron), 4 +V", lcsc_key="TB4")
P("J15", conn(4), "VE.Direct (JST-PH)", "Connector_JST:JST_PH_B4B-PH-K_1x04_P2.00mm_Vertical",
  {"1": "GND_VICTRON", "2": "VE_TX", "3": "VE_RX", "4": "VDD_VICTRON"}, None, "Victron",
  "device pin 2 RX = node TX, pin 3 TX = node RX; original Victron cable", lcsc_key="PH4")
P("D27", ("Device", "D_TVS_Dual_AAC"), "PESD5V0S2BT", SOT23, {"1": "VE_RX", "2": "VE_TX", "3": "GND_VICTRON"}, None, "Victron",
  "ESD at the VE.Direct data lines", lcsc_key="PESD")

# ---------------- UV sensor terminal: selectable 3.3 V / 5 V analog range ------------
P("J17", conn(4), "UV sensor", TB.format(n=4),
  {"1": "GND", "2": "UV_SIGNAL", "3": "+3V3", "4": "+5V"}, None, "UV",
  "UV terminal: GND | SIGNAL | 3V3 | 5V; use only one sensor supply", lcsc_key="TB4")
P("D32", ("Device", "D_TVS_Dual_AAC"), "PESD5V0S2BT", SOT23,
  {"1": "UV_SIGNAL", "2": None, "3": "GND"}, None, "UV",
  "5 V bidirectional ESD/TVS at J17 SIGNAL, before R51; second channel unused", lcsc_key="PESD")
P("R51", ("Device", "R"), "10k", R0805, {"1": "UV_SIGNAL", "2": "UV_ADC"}, None, "UV",
  "series/top resistor for the selectable UV ADC divider")
P("R52", ("Device", "R"), "91k", R0805, {"1": "UV_3V3_RANGE", "2": "GND"}, None, "UV",
  "bottom resistor for 0-3.3 V range; scale 0.900990", lcsc_key="91k")
P("R53", ("Device", "R"), "10k", R0805, {"1": "UV_5V_RANGE", "2": "UV_5V_B"}, None, "UV",
  "5 V range bottom-resistor string")
P("R54", ("Device", "R"), "4.7k", R0805, {"1": "UV_5V_B", "2": "UV_5V_C"}, None, "UV",
  "5 V range bottom-resistor string")
P("R55", ("Device", "R"), "330", R1206, {"1": "UV_5V_C", "2": "GND"}, None, "UV",
  "5 V range bottom-resistor string, total 15.03k", lcsc_key="330R1206")
P("JP10", conn(3), "UV range", HDR.format(n=3),
  {"1": "UV_3V3_RANGE", "2": "UV_ADC", "3": "UV_5V_RANGE"}, None, "UV",
  "shunt 1-2 = 0-3.3 V; 2-3 = 0-5 V; fit one position only", lcsc_key="HDR3")
P("D30", ("Diode", "BAT54S"), "BAT54S", SOT23,
  {"1": "GND", "2": "+3V3", "3": "UV_ADC"}, None, "UV", "ADC rail clamps")
P("C27", ("Device", "C"), "100nF", C0805, {"1": "UV_ADC", "2": "GND"}, None, "UV", "ADC input filter")

BLOCKS = ["MCU", "Power", "Wind", "Rain", "I2C", "1-Wire", "AUX", "Outputs", "Victron"]
