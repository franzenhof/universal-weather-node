# Weather Node V4 Light Rev 1.0 (Heltec V4, through-hole)

A hand-solderable, low-cost board for the **Heltec WiFi LoRa 32 V4** (V4.2 / V4.3, not V4-R8).
All parts are through-hole except the TVS diodes: six SMB (large pads, easy to solder by hand) and
two optional PESD5V0S2BT in SOT-23.
It carries the core functions of the **V4 Full** with the **same GPIOs and the same signal
conditioning**:

| Function | Connector | V4 GPIO |
|----------|-----------|---------|
| Davis wind speed / direction | J4 RJ11 + J5 screw terminal (GND, SPD, DIR, 3V3) | 44 / 6 (ADC1) |
| Wind-vane supply (switched, LOW = on) | J4 pin 2 / J5 3V3 | 34 |
| Davis rain | J6 RJ11 + J7 screw terminal (GND, RAIN) | 16 (P4) |
| BME280 (I²C) | J8 (3V3, SDA, SCL, GND) | 41 / 42 |
| DS18B20 (1-Wire), switched supply (HIGH = on) | J9 (GND, 3V3, DQ) | 43 / 47 |
| Hardware revision ID | – | 15 (P4) |

**Power:** the board has no power input of its own. Power and flash the module through its
**USB-C** connector (V4 header pin 2 = USB VBUS stays open, so there is no back-feeding).
The sensors run from the module's 3.3 V through a 170 mA PTC (F1); the 1-Wire supply has its
own 100 mA PTC (F2).

**Not on the V4 Light** (V4 Full only): VIN 6–29 V supply, heater and fan outputs, fan tacho,
AUX inputs, UV input, VE.Direct.

**Board:** 100 × 80 mm, 2 layers, four M3 holes on a 66 × 72 mm pattern (centres 30/96 mm from the
left edge, 4/76 mm from the top). The V4 stands at the left edge, USB-C flush with the top edge.
71 parts (2 of them optional), see the [BOM](production/universal-weather-node-v4-light-bom.csv).

## Protection and conditioning

- **SMB TVS SMLVT3V3** at wind speed, wind direction, rain (between each jack and its terminal),
  the switched vane supply, the 1-Wire supply and the 3.3 V sensor rail.
- **Resistors on a 10.16 mm (400 mil) pitch** like Rev 1.3: room for Vishay PR01 (1 W, 6.5 × 2.5 mm)
  as well as ordinary 0207 / 0204 metal-film parts.
- **330 Ω** series resistors (Davis lines), **100 Ω** (I²C, 1-Wire DQ), **BAT43** clamps to 3.3 V
  and GND on every input.
- **PESD5V0S2BT** (low-capacitance dual ESD TVS, SOT-23) directly at J8 (SDA, SCL) and J9 (DQ) —
  **optional**: leave them out if SOT-23 is too fiddly, the 100 Ω + BAT43 into the TVS-protected
  3.3 V rail remain. An SMB TVS is not used there because its capacitance would load the buses.
- **Mounting holes plated and on GND:** screwed to a grounded metal plate or enclosure, ESD
  discharges take the short way to ground instead of crossing the board.
- RC filters as on the V4 Full (wind speed 4.7k/100 nF, direction 10k/1 µF, rain 10k/1 µF) and a
  **74HC14** (DIP-14) inverting Schmitt trigger for wind speed and rain — the same polarity as the
  V4 Full's 74LVC2G14, so the firmware counts identically.
- JP1: 4.7k pull-up for reed-switch vanes (open for the Davis potentiometer). JP2/JP3: extra 4.7k
  I²C pull-ups for cables longer than 0.5 m.

## Silkscreen

Verbose on purpose: every part carries its value on the front (inside the body outline where it is
free of pads, otherwise beside it — only C3 is printed in the back BOM alone), FET type
(P-FET / N-FET) next to Q1–Q3, terminal pin names, jumper functions. The back carries the complete
BOM grouped by value (resistors, capacitors / fuses, diodes / FET / IC, connectors), the connector
pinouts by pin number, jumpers and GPIOs, and polarity / assembly-order notes
(`tools/pcbgen/light_silk.py`).

## Revision ID and firmware

R1/R2 = 1 MΩ / 470 kΩ put **0.32 × 3.3 V ≈ 1.06 V** on GPIO15 (V4 Full: 1 MΩ / 1 MΩ ≈ 1.65 V,
accepted window 1.30–2.00 V). The firmware (4.x) accepts **0.85–1.25 V as V4 Light** (`pcbRev` 21;
V4 Full stays 20), logs `HW revision ID (GPIO15): … mV -> PCB V4 Light` and offers only the
functions above: heater, fan, tacho, AUX, second wind sensor, UV and VE.Direct are hidden in the
web settings and refused by downlink/MQTT, and their GPIOs are never driven.

## Assembly

1. SMD first: the SMB TVS diodes (band = cathode = pad 1 on the signal side) and, if fitted,
   D19/D20 (PESD5V0S2BT, SOT-23); then the low parts
   (resistors, BAT43 — band = cathode, ceramic capacitors), DIP socket, TO-92, PTCs, C2 (+ = square pad),
   headers, terminals and RJ11 jacks, the Heltec sockets last. The TVS diodes sit 3–5 mm clear of the RJ11 jacks and screw terminals,
   so they can still be soldered if the jacks went in first.
2. **Q1/Q2 (LP0701N3-G) and Q3 (TN2106N3-G) are S-G-D in TO-92 (N3)** (datasheets checked; the SOT-23 versions K1/LG differ) — check the flat side against the
   silkscreen outline.
3. **J3 (P4 socket, GPIO15/16) sits 0.45 mm off the 2.54 mm grid** (position from Heltec's V4 3D model):
   check it with a real V4 before soldering the sockets. Solder J1/J2/J3 with the module plugged in.

## Status

Generated with `tools/pcbgen` (`design_light.py`, `schem_light.py`, `layout_light.py`, then
`light_silk.py` for the verbose silkscreen): ERC 0, DRC 0 errors / 0 unconnected, schematic parity OK;
remaining warnings are silkscreen overlaps (value texts touching outlines) and clipped silk at pads.
**Not yet built or tested.**

```bash
mkdir -p proj && cp pcb/v4-light/universal-weather-node-v4-light.kicad_pro proj/
DESIGN=design_light SCHEM=schem_light LAYOUT=layout_light N=universal-weather-node-v4-light \
  OUT=$PWD/proj/out ATTEMPTS=8 bash tools/pcbgen/build2.sh $PWD/proj
LAYOUT=layout_light <KiCad python> tools/pcbgen/light_silk.py proj/universal-weather-node-v4-light.kicad_pcb
bash tools/pcbgen/export_light.sh pcb/v4-light     # after copying the result to pcb/v4-light
```
