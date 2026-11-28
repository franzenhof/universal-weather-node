# Example Setups

Three proven ways to build a station around a **Heltec WiFi LoRa 32 V4** — wired Davis sensors, Ecowitt wireless sensors, or a Bresser 7-in-1 — with rough prices, how often the sensors send and what that means for gusts and rain.

[← back to README](../README.md)

Prices are **rough guides** (EU shops and Amazon.de, checked November 2026, incl. VAT, without shipping); they vary a lot between shops and over time. Always buy the **868 MHz** versions of wireless sensors. Mast, cable, mounting material and power supply are not included.

## Overview

| | A: Davis wired | B: Ecowitt | C: Bresser 7-in-1 |
|---|---|---|---|
| Custom PCB | **yes** (V4 Full or V4 Light) | no ([Minimal Wireless Build](minimal-build.md)) | no ([Minimal Wireless Build](minimal-build.md)) |
| Wind | cup anemometer, every pulse counted | ultrasonic, packet every 4.75 s | packet about every 12 s |
| Gust | highest **3-s mean** (WMO), computed on the node | the sensor's own gust per packet | the sensor's own gust per packet |
| Rain resolution | 0.2 mm per tip, counted at once | 0.1 mm, packet about every 49 s | counter, packet about every 12 s |
| Pressure | BME280 | BME280 (or a WN32P indoors) | BME280 |
| Lightning | optional WH57 (radio) | WH57 | optional WH57 (radio) |
| Approx. cost (sensors + node, without options) | **€500–740** (shield €25–190) | **€275–320** | **€110–160** |
| Strength | accuracy, durability, no batteries outside | good value, many add-on sensors | cheapest, one unit, quick to install |

All three can be combined: the node receives every supported wireless sensor next to its wired ones, and *Settings → Sensors → Value sources* decides which sensor feeds which value (see [Wireless Sensors](wireless-sensors.md#value-sources)).

## A: Davis wired sensors on the custom PCB

The most accurate setup and the one the project started with: Davis Vantage Pro2 spare sensors, wired to the node's PCB — no batteries outside, no radio gaps.

| Part | Notes | Approx. price |
|------|-------|---------------|
| Heltec WiFi LoRa 32 V4 (S3R2, 868 MHz) + antenna | | €25–40 |
| Custom PCB **V4 Light** (hand-soldered, wind / rain / 1-Wire / BME280) or **V4 Full** (SMD, own 6–29 V supply, heater, fan, AUX, VE.Direct) | see [PCB](pcb.md) | boards + parts, see [PCB](pcb.md) |
| **Davis 6410** anemometer (cups + vane, 12 m cable) | wind speed and direction | €270–300 |
| **Davis 6466M** AeroCone rain collector (0.2 mm per tip, pole mount; **6464M** for a flat surface) | rain; replaces the older 7852M (same reed switch and 12 m cable, works the same). The US versions (6466, 7852) tip 0.01 in — set it in *Settings → Sensors* | €180–200 |
| **BME280** module | pressure, temperature, humidity — in a radiation shield, see below | €5–10 |
| Radiation shield for the BME280 | a well-ventilated multi-plate shield — see *Radiation shields* below | €25–190 |
| *Optional:* **Ecowitt WH57** lightning sensor (radio) | strikes, distance — received directly, no gateway | €65–70 |
| *Optional:* **DS18B20** probes (1-Wire) | e.g. ground or water temperature, long cables are fine | €3–5 each |

**What you see**
- **Wind:** the PCB counts every anemometer pulse in hardware (Davis: 1 revolution/s = 3.62 km/h). Mean and direction per cycle; the **gust is the highest 3-second mean** as at official stations (WMO-No. 8) — short spikes are averaged out, real gusts are not missed. Details: [Measured Values](values.md).
- **Rain:** every tip is counted the moment it happens (0.2 mm); rain rate and *rain today* follow without delay.
- **Temperature / humidity:** a BME280 must sit in a **radiation shield**, never in the node's box — the electronics (Wi-Fi, ESP32-S3) warm it noticeably. I²C does not like long cables: keep the BME280 within about 1 m of the node (mount the node box below the shield), or measure temperature / humidity with a wireless **Ecowitt WN32** in a shield (~€15–20 + shield) and keep the BME280 in the node's box for the **pressure** only (pressure does not mind the warmth; set *Value sources → Temperature / humidity* to the WN32).
- **Solar radiation:** a wired Davis 6450 (~€210–270) can be connected to the V4 Full's AUX analog input, but the firmware shows it as a **voltage**, not yet in W/m². For light / UV values today use a wireless sensor (Ecowitt WS80 / WH65, Bresser 6/7-in-1) next to the Davis sensors.

## B: Ecowitt sensors (WS80 + WH40H + WH57 + WN32)

A wireless station from Ecowitt / Froggit parts — no gateway or console needed, the node receives the sensors itself. No PCB: the [Minimal Wireless Build](minimal-build.md) (Heltec + BME280 on four wires).

| Part | Notes | Approx. price |
|------|-------|---------------|
| Heltec WiFi LoRa 32 V4 (S3R2, 868 MHz) + antenna, USB-C power supply | | €30–50 |
| **BME280** module | pressure (in the node's box is fine) | €5–10 |
| **Ecowitt WS80** ultrasonic array | wind, temperature, humidity, light, UV; solar-powered (decoded from published frames, not yet tested with a real WS80) | €115–125 |
| **Ecowitt WH40H** rain gauge (self-emptying; the *H* version has the higher funnel rim, fewer drops splash out) | rain, 0.1 mm | €60–65 |
| **Ecowitt WH57** lightning sensor | strikes today / last hour, distance, time of the last strike | €65–70 |
| *Optional:* **Ecowitt WN32** (or the cheaper WH31, ~€17) in a good radiation shield | a second, shaded temperature / humidity sensor, e.g. at 2 m height | sensor €17–25 + shield, see below |

**How often they send** (the node learns each sensor's rhythm and listens exactly when its next packet is due — see [Wireless Sensors](wireless-sensors.md)):

| Sensor | Interval | What it means |
|--------|----------|---------------|
| WS80 | **4.75 s** | very lively wind display; mean, gust and direction per packet |
| WH40H | about 49 s | rain arrives with up to ~1 min delay; the sensor sends its running total, so a missed packet loses no rain |
| WH57 | about 79 s | strike counter — the node counts *strikes today* / *last hour* from its increments |
| WN32 | about 1 min | temperature / humidity |

**What you see**
- **Wind:** the WS80 measures ultrasonically (no moving parts, no start-up speed). The node uses the **sensor's own gust per packet** and keeps the highest one per interval — not the 3-s WMO gust of setup A, but with a packet every 4.75 s the gusts are close to what the sensor sees.
- **Temperature:** the WS80 sits in the sun at mast height; for shaded air temperature a WN32 / WH31 in a good shield (see *Radiation shields*) is the better source (*Value sources → Temperature / humidity*).
- **Rain:** the WH40 reports 0.1 mm, but its tip is really 0.1016 mm (it under-reads by about 1.6 %) — the *WH40 preset* in the per-sensor calibration corrects that ([Wireless Sensors](wireless-sensors.md#names-ecowitt-ids-rain-and-lightning-per-sensor)).
- **Pressure:** none of these sensors measures it — the BME280 does (or a WN32P indoor sensor).

## C: Bresser 7-in-1

The cheapest complete station: one outdoor unit with wind, rain, temperature, humidity, UV and light, received directly by the node. No PCB: the [Minimal Wireless Build](minimal-build.md).

| Part | Notes | Approx. price |
|------|-------|---------------|
| Heltec WiFi LoRa 32 V4 (S3R2, 868 MHz) + antenna, USB-C power supply | | €30–50 |
| **BME280** module | pressure (in the node's box is fine) | €5–10 |
| **Bresser 7-in-1** outdoor sensor — the node needs **no base station**, the spare sensor alone is enough (sold as replacement sensor for the *Comfort Wetter-Center 7003300*) | wind, rain, temperature, humidity, UV, light; 3× AA | €75–100 (with base station €130–170) |
| *Optional:* **Ecowitt WH57** lightning sensor | received in the same timed reception | €65–70 |

**What you see**
- **Interval:** one packet with **all values about every 12 s**.
- **Wind:** the **sensor's own gust per packet**, the highest one per interval; with a 12-s rhythm a short gust between two packets may be lower than at a Davis or WS80 station. Good enough for a garden station and trends; for exposed sites or exact gust records prefer A or B.
- **Temperature:** the unit's own radiation shield is small; in strong sun with little wind it reads higher than a ventilated shield. A separate shaded sensor (e.g. Ecowitt WN32 in a shield, or a Bresser thermo-hygro sensor) can be chosen as the temperature source.
- **Mixed with Ecowitt:** Bresser and Ecowitt sensors can be received together — the node switches between the two radio settings exactly when each sensor is due (*timed switching*, [Wireless Sensors](wireless-sensors.md#several-sensor-families-at-once)).

## Radiation shields

Temperature and humidity are only as good as the shield around the sensor. Experienced users in the German weather station forum and the [Ecowitt wiki](https://meshka.eu/Ecowitt/dokuwiki/doku.php?id=start#passive_solar_radiation_shields_shelters_-_stevenson_screens) (section 18) agree:

- **Ecowitt's small RS0001 is not recommended** — it keeps the rain off, but it is so small that the sensor inside heats up by several degrees in direct sun.
- Better: a larger, well-ventilated multi-plate shield such as the **Davis 7714** (~€175–190), professional ones (Barani MeteoShield, MetSpec) or a careful self-build.
- The node's own 3D-printed cases are **enclosures, not radiation shields** — the [Minimal build case](../3dprint/README.md) keeps the BME280 away from the Heltec's warmth, but only under a roof or eave.
- Coming: Ecowitt's **WS39** (successor of the WH32-EP) is a temperature / humidity sensor with its own shield; whether the node receives it is not known yet.

The WS80 / WS90 / Bresser arrays have their own small shields; for an exact air temperature a separate sensor in a good shield is the better source (*Value sources → Temperature / humidity*).

## Which one?

- **Exact wind and rain, long life, a mast with cable:** A (Davis).
- **Good value, many add-ons (lightning, soil, air quality, leaks), no cables:** B (Ecowitt).
- **Quick start, smallest budget:** C (Bresser) — and add sensors later; nothing needs to be replaced when you grow.

Outputs are the same for all three: dashboard, Home Assistant (MQTT), LoRaWAN, weather networks, alarms — see the [README](../README.md#documentation).
