"""Place the board's self-contained operating reference on back silkscreen."""
import sys

import pcbnew

TITLE = "UNIVERSAL WEATHER NODE v2.0 / franzenhof.com / CERN-OHL-P v2"
BLOCKS = (
    ("PINOUTS", "PINOUTS: actual connector pin numbers,\n"
     "NOT left-to-right order in this view.\n"
     "Pin 1: square pad / connector marking."),
    ("POWER", "POWER / JP8 - ONE SOURCE ONLY\n"
     "J3: 1 VIN 6-29.2V | 2 GND\n"
     "J18 DC jack: center +, <=24V; not with J3\n"
     "JP8 fitted: VIN buck -> 5V rail\n"
     "JP8: REMOVE BEFORE USB\n"
     "USB only: remove shunt; disconnect VIN\n"
     "V4: 5V pin = USB VBUS; ideal diodes U8/U9\n"
     "No auto-mux / no reverse-current isolation.\n"
     "Power OFF + discharge before rewiring.\n"
     "USB does not power VIN / heater loads."),
    ("OUTPUTS", "HEATER / FAN - CHECK LOAD RATINGS\n"
     "J12: 1 +V = protected VIN | 2 HEAT-\n"
     "Heater <=3A; low-side PWM (GPIO38).\n"
     "HEAT- is switched, NOT permanent GND.\n"
     "J13: 1 FAN+ | 2 GND | 3 TACH\n"
     "Fan: high-side PWM (GPIO39).\n"
     "JP9 1-2: 5V, <=0.5A (default)\n"
     "JP9 2-3: protected VIN, <=1A\n"
     "JP12 fit: OC tacho; off: ext. pull-up."),
    ("UV", "UV / J17 / JP10 - GPIO3 ADC1\n"
     "J17: 1 GND | 2 SIGNAL | 3 3V3 | 4 5V\n"
     "Power sensor from ONE supply pin only.\n"
     "JP10 1-2: input 0-3.3V; scale 1.1\n"
     "JP10 2-3: input 0-5V; scale 1.66534\n"
     "Select range BEFORE applying signal.\n"
     "5V range: 5V input -> 3.0024V ADC.\n"
     "Sensor volts != UVI: calibrate sensor."),
    ("AUX", "AUX / J11 - ANALOG + PULSE\n"
     "Pins: 1 3V3 | 2 AIN | 3 PULSE | 4 GND\n"
     "AIN GPIO4 ADC1; R24 series = 10k.\n"
     "R25 absent: scale 1; input <=3.3V\n"
     "(useful ADC range about 0-3.1V).\n"
     "Fit R25 15k for 0-5V; scale 1.66667\n"
     "(downlink x1000 = 1667; 5V -> 3V ADC).\n"
     "PULSE GPIO48: reed / open collector;\n"
     "Never connect battery / 12V to AUX."),
    ("BUSES", "SENSOR BUSES - 3.3V LOGIC ONLY\n"
     "J9 I2C: 1 3V3 | 2 SDA | 3 SCL | 4 GND\n"
     "BME280: SDA GPIO41 / SCL GPIO42.\n"
     "JP1 SDA / JP11 SCL: fit for extra 4k7\n"
     "pull-ups; check total bus pull-up load.\n"
     "J10 1-Wire: 1 GND | 2 3V3 | 3 DQ\n"
     "DQ V3 GPIO5 / V4 GPIO43; 3-wire bus.\n"
     "3V3 sensors: own LDO U7, F3 PTC 0.25A;\n"
     "1-Wire branch F4 PTC 0.10A nominal."),
    ("DAVIS", "DAVIS WIND / RAIN\n"
     "J5 WIND: 1 GND | 2 SPD | 3 DIR | 4 3V3\n"
     "J4 RJ11: 2 3V3 | 3 DIR | 4 GND | 5 SPD\n"
     "J7 RAIN: 1 GND | 2 RAIN\n"
     "J6 RJ11: 3 RAIN | 4 GND; 1/2/5/6 NC\n"
     "RJ11 + screw terminal are parallel.\n"
     "JP2 OPEN: Davis potentiometer vane.\n"
     "JP2 FITTED: extra 4k7 vane pull-up.\n"
     "V3 GPIO: speed 6 / rain 7 / dir 2\n"
     "V4 GPIO: speed 44 / rain 16 / dir 6"),
    ("VIC", "ISOLATED VE.DIRECT / J14 + J15\n"
     "Both: 1 ISO GND | 2 TX | 3 RX | 4 V+\n"
     "TX = node -> Victron RX (V3 34/V4 45)\n"
     "RX = Victron TX -> node (GPIO33)\n"
     "V+ from Victron powers isolated side.\n"
     "Do NOT bridge ISO GND to board GND.\n"
     "No RS232 +/-voltage or RS485 interface."),
    ("EXP", "EXPANSION / TEST POINTS\n"
     "J16 DNP: 1 GND | 2 3V3 | 3 SDA\n"
     "4 SCL | 5 VANE | 6 5V\n"
     "VANE = V3 GPIO19 / V4 GPIO34; vane\n"
     "supply LOW = ON; not a free output.\n"
     "J16 signals: 3.3V, not field protected.\n"
     "TP1/TP5 GND | TP2 protected VIN\n"
     "TP3 5V | TP4 3V3 | TP6 3V3H | TP7 OW\n"
     "ADC probe pads: C17 AUX / C27 UV."),
    ("START", "FIRST POWER-UP / HARDWARE\n"
     "Heltec V3: J1/J2 OR V4: J19/J20/J21;\n"
     "fit ONE socket set; USB-C end = pin 1.\n"
     "Remove module; current-limit supply.\n"
     "Select JP8; verify TP3=5V, no shorts.\n"
     "Power OFF; fit module; check TP4=3.3V.\n"
     "ID ~1.65V (V3 GPIO20/V4 GPIO15) = v2.0\n"
     "No powered sensor signals with MCU OFF."),
    ("LIMITS", "PROTECTION / MECHANICAL LIMITS\n"
     "VIN: Q7 MOSFET reverse protection.\n"
     "USB current shares Heltec power path.\n"
     "D1 MUST be bidirectional SMBJ30CA.\n"
     "F7 6.3A main / F5 4A heater fuse.\n"
     "Fuse rating is NOT allowed load current!\n"
     "Washers <=7mm OD; mind underside."),
)

def documentation_blocks():
    blocks = []
    for name, text in BLOCKS:
        lines = text.splitlines()
        if name == "VIC":
            blocks.extend([(name, "\n".join(lines[:4])),
                           (name + "_CONT", "VE.DIRECT / POWER + ISOLATION\n" + "\n".join(lines[4:]))])
        elif name == "EXP":
            blocks.extend([(name, "\n".join(lines[:5])),
                           (name + "_CONT", "TEST POINTS / CONTINUED\n" + "\n".join(lines[5:]))])
        elif name == "LIMITS":
            blocks.extend([(name, "\n".join(lines[:6])),
                           (name + "_CONT", "LIMITS / CONTINUED\n" + "\n".join(lines[6:]))])
        elif name == "POWER":
            blocks.extend([(name, "\n".join(lines[:5])),
                           (name + "_CONT", "POWER / CONTINUED\n" + "\n".join(lines[5:]))])
        elif name == "START":
            blocks.extend([(name, "\n".join(lines[:5])),
                           (name + "_CONT", "FIRST POWER-UP / CONTINUED\n" + "\n".join(lines[5:]))])
        else:
            blocks.append((name, text))
    return blocks


def apply(board):
    """Replace legacy summary blocks; retain labels beside the actual pins."""
    mm = pcbnew.FromMM
    retained = []
    replaced_texts = {text for _, text in documentation_blocks()} | {text for _, text in BLOCKS} | {
        TITLE, "Universal Weather Node v2.0", "franzenhof.com", "CERN-OHL-P v2",
        "FAN SUPPLY\n1-2: 5V (default)\n2-3: VIN",
        "R25 15k:\nAIN 0-5V x1.667",
        "I2C PULL-UP 4k7\nJP1 SDA / JP11 SCL\nfit for long cables",
        "JP1/JP11 I2C PULL-UP 4k7\nSDA / SCL; fit for long cables",
        "JP8: 1-2 VIN BUCK / 2-3 EXT 5V\nUSB ONLY: REMOVE JP8 SHUNT\n"
        "NEVER USB + EXTERNAL POWER\nCHANGE JUMPERS ONLY UNPOWERED",
        "JP8: 1-2 VIN\n2-3 5V IN\nNO USB + EXT",
        "JP2 VANE PULL-UP\nfit: reed-switch vane\nopen: Davis pot",
        "UV RANGE\n1-2: 3.3V\n2-3: 5V",
    }
    for drawing in list(board.GetDrawings()):
        if drawing.GetLayer() != pcbnew.B_SilkS or not hasattr(drawing, "GetText"):
            continue
        text = drawing.GetText()
        heads = {t.split("\n")[0] + "\n" for _, t in documentation_blocks()} | {
            "DAVIS WIND / RAIN\n", "EXPANSION / TEST POINTS\n", "FIRST POWER-UP / HARDWARE\n"}
        if text in replaced_texts or text.startswith(tuple(heads)) or text.startswith(("POWER / JP8 - ONE SOURCE ONLY\n",
                                                     "PROTECTION / MECHANICAL LIMITS\n",
                                                     "HEATER / FAN - CHECK LOAD RATINGS\n")):
            retained.append(drawing)
            board.Remove(drawing)
    obstacles = []
    def bbox(obj):
        box = obj.GetBoundingBox()
        return tuple(pcbnew.ToMM(v) for v in (box.GetLeft(), box.GetTop(), box.GetRight(), box.GetBottom()))
    for footprint in board.GetFootprints():
        obstacles.extend(bbox(p) for p in footprint.Pads() if p.IsOnLayer(pcbnew.B_Cu))
        obstacles.extend(bbox(g) for g in footprint.GraphicalItems() if g.GetLayer() == pcbnew.B_SilkS)
    obstacles.extend(bbox(d) for d in board.GetDrawings() if d.GetLayer() == pcbnew.B_SilkS)
    obstacles.extend((x - 3.9, y - 3.9, x + 3.9, y + 3.9)
                     for x, y in ((54, 54), (146, 54), (54, 146), (146, 146)))
    def hit(a, b):
        return not (a[2] + .25 <= b[0] or b[2] + .25 <= a[0] or a[3] + .25 <= b[1] or b[3] + .25 <= a[1])
    jobs = [("TITLE", TITLE, .8)] + [
        (name, text, .8) for name, text in sorted(documentation_blocks(), key=lambda block: -len(block[1].splitlines()))]
    prepared = []
    for name, text, size in jobs:
        obj = pcbnew.PCB_TEXT(board)
        obj.SetText(text)
        obj.SetLayer(pcbnew.B_SilkS)
        obj.SetMirrored(True)
        obj.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        obj.SetTextThickness(mm(.13 if size == .8 else .15))
        obj.SetPosition(pcbnew.VECTOR2I(mm(100), mm(100)))
        prepared.append((name, text, obj, bbox(obj)))
    # greedy packing; reading from the back starts at the upper right in board coordinates.
    # Further strategies only if the first one leaves a block without space.
    strategies = (lambda x, y: y * 1000 - x, lambda x, y: -x * 1000 + y, lambda x, y: y * 1000 + x,
                  lambda x, y: x * 1000 + y)
    for strategy in strategies:
        placed, obst = [], list(obstacles)
        for name, text, obj, box in prepared:
            width, height = box[2] - box[0], box[3] - box[1]
            best = None
            for yi in range(102, 268):
                y = yi / 2
                for xi in range(104, 295):
                    x = xi / 2
                    rect = (x, y, x + width, y + height)
                    if rect[2] > 148.8 or rect[3] > 133.5 or any(hit(rect, o) for o in obst):
                        continue
                    score = strategy(x, y)
                    if best is None or score < best[0]:
                        best = score, rect
            if best is None:
                print(f"no back-silkscreen space for {name}: {width:.2f} x {height:.2f} mm, next strategy")
                break
            placed.append((name, text, obj, box, best[1])); obst.append(best[1])
        if len(placed) == len(prepared): break
    else:
        raise RuntimeError("No back-silkscreen layout fits every documentation block")
    for name, text, obj, box, rect in placed:
        width, height = box[2] - box[0], box[3] - box[1]
        offset_x = (box[0] + box[2]) / 2 - 100
        offset_y = (box[1] + box[3]) / 2 - 100
        obj.SetPosition(pcbnew.VECTOR2I(mm((rect[0] + rect[2]) / 2 - offset_x),
                                       mm((rect[1] + rect[3]) / 2 - offset_y)))
        board.Add(obj)
        retained.append(obj)
        print(f"Back documentation {name}: {len(text.splitlines())} lines, {width:.1f} x {height:.1f} mm at {rect}")
    return retained


if __name__ == "__main__":
    board = pcbnew.LoadBoard(sys.argv[1])
    retained = apply(board)
    pcbnew.SaveBoard(sys.argv[1], board)
