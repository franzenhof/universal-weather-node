# Minimal Wireless Build (no custom PCB)

A wireless-only station without the custom PCB: a Heltec V4 (or V3), an optional BME280 on four jumper wires and your Bresser, TFA IT+ or Ecowitt / Froggit sensors.

[← back to README](../README.md)

For a wireless station (Bresser 5/6/7-in-1, TFA / LaCrosse IT+ or Ecowitt / Froggit sensors) the custom PCB is optional: the Heltec module receives the sensors itself, and a **BME280** module adds the air pressure. *Not yet built and tested as described here — the firmware paths are the same as on the boards.*

| Part | Notes |
|------|-------|
| Heltec WiFi LoRa 32 **V4** (recommended; not V4-R8) or **V3 / V3.1 / V3.2** | with an **868 MHz** antenna (EU); never power it without the antenna. The V4 works in this build without PCB or with the custom PCB v2.0 (its own V4 socket set — not with Rev 1.2/1.3, where GPIO2/5/7 would hit the V4's RF front end); don't plug a GNSS module into a V4 when the BME280 is on GPIO41/42 |
| BME280 module (I²C, 3.3 V) — *recommended* | air pressure (+ optionally temperature / humidity); address **0x76** (SDO to GND; many modules are 0x76, some 0x77 — check the jumper/solder bridge). Must be a BME280, not a BMP280 (no humidity) |
| USB-C power supply 5 V, ≥ 1 A | the Heltec's own USB-C socket |
| Bresser station, TFA IT+ or Ecowitt / Froggit sensor | see [Wireless Sensors](wireless-sensors.md) |

<!-- help:skip -->
<p align="center">
  <img src="../pictures/minimal-wireless-build.png" alt="Minimal wireless build: Heltec V3 powered over USB-C, BME280 on four jumper wires (3V3, GND, 41 SDA, 42 SCL), Bresser, TFA IT+ or Ecowitt / Froggit sensors over 868 MHz" width="100%">
</p>
<!-- /help:skip -->

**Wiring** (four wires, Heltec pin labels; keep them short, < 30 cm — there is no protection circuitry without the PCB):

| BME280 | Heltec V4 / V3 (same pins) |
|--------|-----------|
| VIN / VCC | **3V3** |
| GND | **GND** |
| SDA | **41** |
| SCL | **42** |

**Settings:** nothing to change for this build — the defaults are made for it: board wind and rain sensors **off**, *Wireless sensors* **on** with *Wireless sensor type* **Auto (search)**, LoRaWAN **off**, Wi-Fi and the web dashboard **on**. Enter your Wi-Fi in the portal and the node finds the station by itself. Optional, in the web settings:

1. Mark your station ★ *Mine* (status page button or *Settings → Sensors*), and hide (✕) neighbours' stations in range — see [Mine, Received, Hidden](wireless-sensors.md#mine-received-hidden).
2. *Source: temperature / humidity* = **Wireless 1** if the BME280 sits close to the warm module (not needed with the [enclosure](#minimal-wireless-build-no-custom-pcb) below, whose BME280 chamber is thermally separated). The BME280 provides the air pressure (QNH) by default (*Source: pressure* = Auto), which is not affected by its location; without a BME280 an Ecowitt WS69 pressure module or WN32P can provide it.
3. MQTT / weather networks / LoRaWAN as needed.

Everything else — dashboard, Home Assistant, LoRaWAN, weather networks, derived values — works as on the boards. Not available without the PCB: Davis / wired sensors, DS18B20, UV, AUX, heater/fan, Victron, the wide-range and protected power input.

**Display:** the Heltec's built-in OLED shows the status screen for 20 s after a press of the PRG button; *Settings → Display* can keep it on (with night-off hours) and rotate through big weather screens — temperature, wind, rain, pressure, status ([Display](settings.md#display)).

**Enclosure:** a 3D-printable wall-mount case for this build is in [`3dprint/`](../3dprint/README.md#minimal-wireless-build-enclosure-bare-heltec-v3--bme280-no-pcb) (`case_minimal.py`, STL/STEP in `3dprint/minimal/`). The Heltec sits in the upper chamber — no screws through the board, display window, PRG/RST pushers, USB-C opening and an SMA bulkhead for an external 868 MHz antenna. The BME280 has its own louvred chamber below, separated by a double wall with a ventilated air gap so the ESP32's heat does not reach it; the only link is a 12.5 × 4.5 mm cable slot (wide enough for the Dupont plugs), sealed with hot glue after wiring. Print in PETG/ASA without supports; for a sheltered place (under a roof or eave), not waterproof. *Not yet printed — measure your BME280 module and check the fit first.*

<!-- help:skip -->
<p align="center"><img src="../pictures/case-minimal.png" alt="3D-printed enclosure for the minimal wireless build: Heltec V3 chamber on top, ventilated air gap, louvred BME280 chamber below" width="40%"></p>
<!-- /help:skip -->
