#pragma once
static const char README_MD[] PROGMEM = R"READMEMD(
# Weather Node (LoRaWAN + WiFi/MQTT)

Firmware for a custom-designed PCB based on a **Heltec WiFi LoRa 32 V3** (ESP32-S3) that measures wind speed, wind direction, rainfall, temperature, humidity, and barometric pressure.

**This is not a LoRaWAN-only project.** Measurements can be transmitted over any combination of:

- 📡 **LoRaWAN** (OTAA, region configurable; default EU868) — **CayenneLPP** or an optional **compact binary format** (~50% smaller), to a private ChirpStack server or TTN
- 📶 **Wi-Fi + MQTT** (TLS-capable) — with **Home Assistant MQTT auto-discovery**
- 🌐 **REST API** — JSON telemetry, plus a self-contained web dashboard (no CDN dependency) with trend charts, and a small settings page

All three run **independently and in parallel** — enable any subset via the on-device Wi-Fi config portal, no rebuild required. See [Parallel Wi-Fi + MQTT Transport](#parallel-wi-fi--mqtt-transport-optional) for the Wi-Fi/MQTT/REST side, or [Compact Binary Payload Format](#compact-binary-payload-format-alternative-50-smaller) for the LoRaWAN payload options.

The images below give a quick impression of the hardware journey from empty PCB to finished node and show how the measured data can be visualized in Grafana.

## Table of Contents

- [Project in Pictures](#project-in-pictures)
- [Use Case](#use-case)
- [Hardware](#hardware)
- [GPIO Pinout](#gpio-pinout)
- [Measurement Principles](#measurement-principles)
- [Generic Sensor Calibration](#generic-sensor-calibration)
- [Units (Metric/Imperial)](#units-metricimperial)
- [CayenneLPP Payload Channels](#cayennelpp-payload-channels)
- [Compact Binary Payload Format (Alternative, ~50% Smaller)](#compact-binary-payload-format-alternative-50-smaller)
- [Initial Configuration (Wi-Fi Portal)](#initial-configuration-wi-fi-portal)
- [Parallel Wi-Fi + MQTT Transport (Optional)](#parallel-wi-fi--mqtt-transport-optional)
- [User Button (GPIO 0)](#user-button-gpio-0)
- [ChirpStack Payload Decoder](#chirpstack-payload-decoder)
- [TTN (The Things Stack) Instead of Private ChirpStack](#ttn-the-things-stack-instead-of-private-chirpstack)
- [LoRaWAN Downlink Commands](#lorawan-downlink-commands)
- [Error Handling / Robustness](#error-handling--robustness)
- [Uplink Interval and Timing](#uplink-interval-and-timing)
- [Project Structure](#project-structure)
- [Libraries](#libraries)
- [Technical Notes](#technical-notes)
- [License & Open Source](#-license--open-source)


## Use Case

```
[USB-C power supply] ──► [Heltec WiFi LoRa V3]
                              │        │
                   [Davis 6410]    [BME280 via RJ45]
                   Wind sensor           │
                   │                [DS18B20 × 3]
                   [Davis AeroCone]  [LiPo optional]
                   Rain sensor
                              │
                              [LoRaWAN (default EU868)]
                              │
                       [ChirpStack server]
                              │
                (optional, parallel/instead)
                              │
                  [Wi-Fi + MQTT broker, TLS] ──► [Home Assistant / any MQTT client]
                              │
                     [REST API: /api/data]
```

The node runs continuously (no deep sleep), measures without interruption, and transmits a LoRaWAN uplink every configurable number of seconds. Typical uplink interval: 60–300 seconds.

Wi-Fi + MQTT is fully optional and disabled by default (relevant for battery-powered field installs); on mains-powered installs it can run alongside or instead of LoRaWAN.

---

## Hardware

| Component | Function |
|-----------|----------|
| Heltec WiFi LoRa 32 V3 | Microcontroller (ESP32-S3), LoRa SX1262, OLED display 128×64 |
| Davis 6410 Anemometer (default) | Wind speed (reed switch) + wind direction (potentiometer 20 kΩ) |
| Davis AeroCone / Standard, typically 6466M (default) | Rainfall (reed switch, 0.2 mm/pulse) |
| BME280 | Temperature / humidity / barometric pressure (I²C 0x76, up to 2 m RJ45 cable) |
| DS18B20 | Up to 3 temperature sensors (1-Wire, external) |
| LiPo battery | Optional; voltage measurement via voltage divider + ADC |
| Custom PCB Rev 1.2 / 1.3 | ESD protection, pull-ups, RC filter, polyfuse |

> **Other sensors:** Davis is the tested default, but rain gauges and anemometers are just reed-switch pulse counters underneath — any similar sensor works via the calibration parameters below. Wind vanes need either a continuous potentiometer (like the Davis) or a guided one-time calibration for reed-switch/resistor-ladder vanes (the common design in many cheaper weather-station clones). See [Generic Sensor Calibration](#generic-sensor-calibration).

### Custom PCB

In addition to mechanically connecting the connectors, the PCB provides the following protection functions for all external interfaces:

- **TVS diodes** (SMLVT3V3) directly at each external connector as primary ESD coarse protection  
- **BAT43 Schottky clamp diodes** as secondary protection clamps before the GPIO pins  
- **Series resistors** (330 Ω / 1 kΩ) for current limiting  
- **RC low-pass filter** for hardware debouncing of the reed switches:
  - Wind speed: 330 Ω + 4.7 kΩ + 100 nF (τ ≈ 0.5–0.9 ms) — sized against a measured ~1.5 ms minimum reed-contact closure time at full anemometer speed
  - Rain: 330 Ω + 10 kΩ + 1 µF (τ ≈ 10–15 ms) — the tipping bucket's much lower pulse rate allows a far stronger filter
- **74LVC2G14 dual Schmitt-trigger inverter** (Rev 1.3+, SOT-23-6) on wind speed and rain, after the RC filter, for a clean single-threshold digital transition (belt-and-suspenders against reed-switch bounce and cable EMI beyond what the RC filter alone catches) — see the [Revision-Dependent Pins](#revision-dependent-pins) note on the resulting PCNT edge-polarity inversion
- **Polyfuse F1** (250 mA) for short-circuit protection of the 3.3 V sensor supply  
- **Additional I²C pull-ups** (4.7 kΩ, jumper J1/J2) for longer BME280 cable runs up to 2 m

---

## GPIO Pinout

### Fixed Pins (revision-independent)

| GPIO | Function | Direction | Note |
|------|----------|-----------|------|
| **0** | User button | IN (pull-up) | Short press: display 20 s; long press (≥ 3 s): Wi-Fi portal |
| **1** | LiPo ADC | IN (ADC1_CH0) | Voltage divider R13/R14 (390 Ω / 100 Ω) |
| **2** | Wind direction ADC (ADC1_CH1) | IN | Davis potentiometer 0–3.3 V → 0–359°; fixed on both revisions |
| **37** | BAT_CTRL | OUT | HIGH enables ADC measurement path (Heltec V3 internal from board rev. 3.2) |
| **41** | I²C SDA (BME280) | IN/OUT | Wire2, 50 kHz, address 0x76 |
| **42** | I²C SCL (BME280) | OUT | Wire2 |
| **47** | 1-Wire high-side switch | OUT | HIGH = bus powered; Rev 1.3+ only |

### Revision-Dependent Pins

| GPIO Rev 1.2 | GPIO Rev 1.3 | Function |
|:------------:|:------------:|----------|
| **3** | **6** | Wind speed (PCNT Unit 0, reed switch, via 74LVC2G14 Schmitt trigger on Rev 1.3+) |
| **4** | **7** | Rain (PCNT Unit 1, reed switch, via 74LVC2G14 Schmitt trigger on Rev 1.3+) |
| **5** | **5** | 1-Wire bus (DS18B20, up to 3 sensors) |
| – | **3, 4** | unused / free |

> The active PCB revision is configured in the Wi-Fi portal (default: **13** for Rev 1.3).
>
> **Rev 1.3+ note:** wind speed and rain now pass through an inverting Schmitt-trigger buffer (74LVC2G14, SOT-23-6) after the RC low-pass filter, for reliable debouncing against reed-switch contact bounce and cable-borne EMI. Because it inverts, the PCNT units are configured to count the *falling* edge on these two channels (`PCNT_COUNT_INC` on `neg_mode`, see `configureWindPcnt()`/`configureRainPcnt()` in `sensors.cpp`) — the opposite of Rev 1.2's direct (non-inverted) wiring.

### Wind Sensor (Davis 6410) – RJ11 6-pin

| Pin | Color | Function |
|-----|-------|----------|
| 1 | – | n/c |
| 2 | Yellow | VCC 3.3 V (potentiometer supply) |
| 3 | Green | Wind direction (potentiometer wiper → ADC) |
| 4 | Red | GND |
| 5 | Black | Wind speed (reed switch → PCNT) |
| 6 | – | n/c |

### Rain Sensor (Davis 6466M) – RJ11 6-pin

| Pin | Color | Function |
|-----|-------|----------|
| 3 | Green/Yellow | Reed switch → PCNT |
| 4 | Red | GND |
| others | – | n/c |

> Wire colors 3 and 4 may be swapped depending on the cable batch – both variants work since the reed switch is polarity-independent.

### BME280 – RJ45 (solder module-side onto breakout board)

| Pins | Function |
|------|----------|
| 1, 2, 4, 6 | GND |
| 7, 8 | VCC 3.3 V |
| 3 | SCL |
| 5 | SDA |

> Jumpers J1 and J2 on the PCB activate the additional 4.7 kΩ I²C pull-ups for cable lengths over 0.5 m.

### Revision History

**Rev 1.2** is the original board revision (legacy, still supported). **Rev 1.3** is the current default (`pcbRevision = 13` in the Wi-Fi portal) and the revision shown in the pictures above.

> Rev 1.3: The 5V input on the PCB (overvoltage protected) connects directly to the **5V pin of the Heltec WiFi LoRa V3** (Pin 2 on the J2 header of the controller board). GPIO 3 and GPIO 4 are free and unused (see [Revision-Dependent Pins](#revision-dependent-pins) above).

> **Note on current firmware:** Both revisions support only **one** 1-Wire bus (Rev 1.2: GPIO 5, Rev 1.3: GPIO 5 as well — unchanged). The former second bus is no longer addressed by the software.

**Changes in Rev 1.3 vs. 1.2:**
- TVS diode (SMLVT3V3 / U4) added at rain sensor connector
- 2nd 1-Wire bus removed
- Wind speed and rain moved off GPIO 3/4 to GPIO 6/7 (GPIO 3 is an ESP32-S3 JTAG-source strapping pin — floats with no internal pull, best kept free of external sensor wiring; GPIO 4 was freed up alongside it for future expansion)
- 74LVC2G14 Schmitt-trigger inverter added on wind speed and rain (see RC filter / GPIO pinout notes above) — requires the inverted PCNT edge polarity now set in `configureWindPcnt()`/`configureRainPcnt()`

---

## Measurement Principles

### Wind Speed
The Davis 6410 anemometer contains a **reed switch** that closes once per revolution.  
The ESP32 hardware pulse counter (**PCNT**) counts edges without CPU load.  
Conversion per Davis specification:

```
1 revolution/s = 2.25 mph = 3.62 km/h
Wind speed [km/h] = pulses/s × 2.25 × 1.60934
```

The **maximum wind gust** (maximum per uplink interval) is also captured and transmitted separately.

> The pulses-per-second-to-speed ratio (`windMsPerPps`, default `1.00584`) is configurable for other anemometers — see [Generic Sensor Calibration](#generic-sensor-calibration).

### Wind Direction
The Davis 6410 contains a **potentiometer (20 kΩ)** powered directly from VCC3.3.  
The wiper delivers a linear voltage from 0–3.3 V proportional to 0–359°.  
A configurable **north offset** (–180° to +180°) corrects the mounting orientation.

```
Raw value (0–4095, 12-bit ADC) → map(0, 4095, 0, 359) → + offset → mod 360
```

The low-pass filter C6 (10 µF + potentiometer source impedance ~10 kΩ) gives τ ≈ 100 ms (settling time ~500 ms), which is appropriate for typical vane movement speeds.

> This linear mapping only applies to continuous-potentiometer vanes (`windDirSensorType = 0`). Reed-switch/resistor-ladder vanes use a calibrated lookup table instead — see [Generic Sensor Calibration](#generic-sensor-calibration).

### Rainfall
The reed switch in the tipping bucket rain gauge fires one pulse per **0.2 mm of rainfall** (Davis default).  
PCNT counts pulses in hardware. Rain rate is calculated as mm/h from the interval:

```
Rain [mm] = pulses × rainMmPerTip
Rain rate [mm/h] = rain [mm] / (interval [ms] / 3,600,000)
```

> `rainMmPerTip` (default `0.2`) is configurable for other tipping-bucket rain gauges — see [Generic Sensor Calibration](#generic-sensor-calibration).

The since-start accumulator (`rainMMSinceStart`) survives normal operation in **RTC_DATA_ATTR** (lost on power interruption, resettable via downlink `0x01`).

### Temperatures (DS18B20)
Up to **3 sensors per bus** are detected automatically (`getDeviceCount()`).  
The index corresponds to the order of ROM addresses on the bus.

### Barometric Pressure (BME280)
The measured absolute pressure is converted to **sea level (QNH)** when a station altitude is configured:

```
QNH = absolute pressure × (1 - 0.0000226 × altitude[m])^(-5.255)
```

---

## Generic Sensor Calibration

**This project is designed for and tested with the Davis 6410 (wind speed + direction) and 6466M/AeroCone (rain) sensors** — that combination is what the PCB, connectors, and firmware defaults are built around. That said, rain gauges and anemometers are, underneath, almost always just reed-switch pulse counters, and the electrical protection circuitry already on the board (see below) doesn't care which brand is wired up to it. This section documents exactly what's needed to use cheaper/other sensors, verified against the actual [PCB schematic](pcblayout/lorawan-weather-node-pcb_Schaltplan.png) — not just the firmware.

### Rain gauge and wind-speed: drop-in compatible, just recalibrate the ratio

Both the **Rain** input (RJ11 pins 3/4) and the **Wind Speed** input (RJ11 pin 5, shared with the wind connector) are wired identically on this PCB: a **4.7 kΩ pull-up to 3.3 V**, a **330 Ω series resistor** for current limiting, TVS/Schottky ESD clamps, and a **100 nF filter capacitor** — the same RC-debounced, pulled-up digital input in both cases (see `PCNT CONFIGURATION` in `src/main.cpp` and the `Connector Davis Rain` / `Connector for Davis Windsensor` blocks in the schematic). Electrically, this is exactly what **any** simple 2-wire reed-switch sensor needs (one wire to the pulse GPIO, one wire to GND) — there is nothing Davis-specific about it.

**So: any 2-wire reed-switch tipping-bucket rain gauge or cup anemometer can be wired to the existing connectors with no hardware changes at all.** You only need to tell the firmware the sensor's pulse ratio:

| Parameter | Meaning | Davis default | Set via |
|-----------|---------|----------------|---------|
| `rainMmPerTip` | mm of rain per reed-switch pulse (tip) | `0.2` | Portal, downlink `12`, MQTT |
| `windMsPerPps` | Wind speed (m/s) per pulse-per-second | `1.00584` | Portal, downlink `13`, MQTT |

Take the values from your sensor's datasheet. A commonly-seen example: the SparkFun/Argent Data Systems "Weather Meters" anemometer (widely cloned under various rebrandings) specifies roughly `2.4 km/h` per pulse-per-second, i.e. `windMsPerPps ≈ 0.667`; tipping-bucket rain gauges vary by bucket size, with `0.2`, `0.2794`, `0.3`, and `0.5` mm/tip all being common — always check your specific unit's datasheet rather than assuming one of these.

> **Tip:** if you're unsure of the exact ratio, set `debugMode = 1` and watch the Serial log (`Wind: N pulses → ... m/s`, `Rain: N pulses → ... mm`) while manually triggering the reed switch a known number of times — this reveals the raw pulse count independent of the ratio, so you can work out the correct value experimentally.

### Wind direction: potentiometer vs. resistor-ladder vanes (needs a pull-up resistor)

Wind vanes use one of two fundamentally different sensing principles, selected by **`windDirSensorType`**:

- **`0` — Continuous potentiometer** (Davis 6410 and pin-compatible replacements, e.g. Inspeed's Davis-compatible e-Vane): a 3-wire sensor (VCC, GND, wiper) whose wiper outputs a voltage linearly proportional to direction (0–3.3 V → 0–359°). **Drop-in compatible with the existing wiring, no extra parts** — the potentiometer itself is the voltage divider. No calibration needed beyond the existing `windDirOffsetDeg` north correction.
- **`1` — Reed-switch / resistor-ladder vane**: the common design in many cheaper weather-station clones (e.g. the SparkFun/Argent-style "Weather Meters" wind vane, and the many similar sensors sold under various generic/rebranded weather-station kits) — typically **8 reed switches** arranged around the vane, each wired to a *different* resistor value, all connected in parallel between one output wire and a common wire. Only one (or two adjacent) switches close at a time, producing **discrete voltage steps** per direction rather than a continuous ramp — the linear potentiometer formula above cannot decode this, and (unlike the potentiometer) **this sensor type needs a pull-up resistor that the direction ADC input doesn't have by default**, because it was designed only for a self-biased potentiometer. **PCB Rev 1.3** has an optional footprint, **R18 (4.7 kΩ)**, for exactly this — populate it when fitting a non-Davis vane; leave it unpopulated for a Davis potentiometer vane (see below).

#### Wiring a resistor-ladder vane

The wind connector's RJ11 pins are (see [Wind Sensor RJ11 pinout](#wind-sensor-davis-6410--rj11-6-pin)):

| Pin | Davis (potentiometer) use | Resistor-ladder vane use |
|-----|----------------------------|---------------------------|
| 2 (Yellow) | VCC 3.3 V → potentiometer supply | **Now used as the pull-up supply** (see below) instead of powering a pot |
| 3 (Green) | Potentiometer wiper → ADC1_CH1 | Vane's **sense** wire → ADC1_CH1 (same pin) |
| 4 (Red) | GND | Vane's **common/ground** wire → GND (same pin) |
| 5 (Black) | Wind speed reed switch → PCNT | Unaffected — wind speed wiring is unchanged |

The direction ADC line (pin 3) needs a pull-up to pin 2 (VCC 3.3 V) that isn't populated by default:

- **PCB Rev 1.3:** populate the optional **R18 (4.7 kΩ)** footprint — no extra wiring in the sensor cable needed, just plug the vane into the existing RJ11 connector.
- **PCB Rev 1.2 (no R18 footprint):** add **one external resistor between pin 2 (VCC 3.3 V) and pin 3 (sense/ADC)** instead — solder or crimp it inline in the sensor cable near the RJ11 plug (or into a small breakout/terminal block if you're not using the original RJ11 connector).

Either way, this resistor plus the vane's internal (switch-selected) resistor to ground forms the voltage divider the ADC reads:

```
Vadc = VCC × R_vane / (R_vane + R_pullup)
```

**Recommended pull-up value: 4.7 kΩ** (R18's populated value on Rev 1.3) — matching every other pull-up already used on this PCB (I²C, 1-Wire, the rain and wind-speed reed switches — see the schematic), which keeps things consistent and uses a value you likely already have on hand for a Rev 1.2 external resistor too. The exact value isn't critical for correctness — the guided calibration below captures whatever ADC values actually result — but a poorly chosen pull-up can compress the 8/16 steps too close together to reliably distinguish. If, during calibration, adjacent positions show suspiciously similar ADC readings, try a different value (common alternatives for this class of sensor: 1 kΩ–10 kΩ) and recalibrate.

> **Don't use the ESP32-S3's internal pull-up for this** — use a real external resistor. Internal pull-ups are ~45 kΩ typical (with wide manufacturing tolerance), far weaker than 4.7 kΩ; against a resistor-ladder vane's typical range (~1–120 kΩ), that skews the divider so most positions cluster near the high-voltage end, compressing the steps together. Enabling a pin's internal pull-up while also using it for `analogRead()`/ADC input isn't a well-supported combination on ESP32 either, on top of the ADC non-linearity already noted in [Technical Notes](#technical-notes).

#### Guided calibration (works with any resistor-ladder vane, regardless of vendor)

Since exact resistor values (and therefore ADC voltage steps) vary by vendor and component tolerance, this project doesn't hardcode any specific resistor table. Instead, it uses a **guided on-device calibration**:

1. Open the Wi-Fi portal (button long press, first boot, or downlink `0x03`) and click **Calibrate Wind Direction** on the start page (or open `http://192.168.4.1/calibrate` directly).
2. Choose **8** or **16** positions, matching your vane's switch count.
3. For each position, physically rotate the vane to the indicated compass direction and click **Capture** — the page reads the live raw ADC value from the device.
4. Click **Save Calibration** once all positions are captured. This immediately sets `windDirSensorType = 1` and stores the lookup table (raw ADC → degrees) in NVS — no need to also save the main parameter page.

At runtime, each reading is matched to the **nearest** calibrated ADC value (simple linear scan over ≤16 entries), then `windDirOffsetDeg` is applied on top, same as the potentiometer mode. This approach works with *any* resistor-ladder vane regardless of vendor, and also compensates for resistor tolerances and the ESP32-S3 ADC's known non-linearity (see [Technical Notes](#technical-notes)).

> `windDirSensorType` can also be toggled via downlink/MQTT parameter `14` (e.g. to revert to potentiometer mode after swapping hardware) without touching a previously-saved calibration table.

---

## Units (Metric/Imperial)

A single grouped switch, **`unitSystem`** (`0` = Metric, `1` = Imperial), controls the unit for every transmitted/displayed measurement — grouped in the portal right next to the existing wind-speed-unit field so unit-related settings aren't scattered across the page. Internally, all computation always stays in the original base units (m/s, mm, °C, hPa); conversion happens only at the point a value is transmitted or shown:

| unitSystem | Wind speed/gust | Rain | Temperature | Pressure |
|------------|------------------|------|--------------|----------|
| `0` Metric | km/h or m/s (per `windUnitMs`) | mm, mm/h | °C | hPa |
| `1` Imperial | mph | in, in/h | °F | inHg |

- Wind direction (°), humidity (%), battery voltage (V), and debug fields (free heap, uptime) are unaffected — no imperial equivalent applies.
- **LoRaWAN (both CayenneLPP and compact binary):** values are converted *before* encoding — the wire format doesn't change, only the number. The CayenneLPP/binary status byte's **Bit 6** tells the decoder which system is in use for that uplink (see [chirpstack/decoder.js](chirpstack/decoder.js)); Bit 6 set overrides Bit 7 (km/h-vs-m/s), forcing mph.
- **MQTT/REST JSON:** field names are unit-neutral (e.g. `temp`, not `tempC`); a sibling `*Unit` field (e.g. `tempUnit`) always tells you which unit that number is in — see the JSON example in [Parallel Wi-Fi + MQTT Transport](#parallel-wi-fi--mqtt-transport-optional).
- **Home Assistant discovery:** `unit_of_measurement` on each entity reflects the current setting, re-published on the next MQTT reconnect after a change.
- Settable in the Wi-Fi portal, via LoRaWAN downlink parameter `11`, via MQTT command, or via the web settings page (`/api/settings`).

---

## CayenneLPP Payload Channels

| Channel | Type | Content | Unit (Metric / Imperial) | Active when |
|---------|------|---------|---------------------------|-------------|
| Ch 1 | Direction | Wind direction | ° (always) | `windEn = 1` |
| Ch 1 | Analog Input | Avg wind speed | m/s or km/h / mph | `windEn = 1` |
| Ch 2 | Analog Input | Wind gust (maximum) | m/s or km/h / mph | `windEn = 1` |
| Ch 3 | Analog Input | Rain rate | mm/h / in/h | `rainEn = 1` |
| Ch 1 | Distance | Rain current cycle | mm / in | `rainEn = 1` |
| Ch 2 | Distance | Rain since start | mm / in | `rainEn = 1` |
| Ch 2 | Digital Input | Status bitfield (BME280, 1-Wire, send-fail count, wind unit, **imperial units flag**) | bitfield | always |
| Ch 200 | Digital Input | Cycle counter (debug) | 0–255 | `debugMode = 1` |
| Ch 202 | Digital Input | Send fail counter (debug) | 0–255 | `debugMode = 1` |
| Ch 201 | Temperature | CPU temperature (debug) | °C / °F | `debugMode = 1` |
| Ch 203 | Analog Input | Free heap (debug) | KiB (always) | `debugMode = 1` |
| Ch 204 | Analog Input | Uptime (debug) | h (always) | `debugMode = 1` |
| Ch 1 | Voltage | LiPo voltage | V (always) | `lipoEn = 1` |
| Ch 1 | Rel. Humidity | Humidity | % (always) | BME280 detected |
| Ch 1 | Baro. Pressure | Barometric pressure QNH | hPa / inHg | BME280 detected |
| Ch 2 | Baro. Pressure | Barometric pressure absolute | hPa / inHg | BME280 detected |
| Ch 1 | Temperature | BME280 temperature | °C / °F | BME280 detected |
| Ch 2–4 | Temperature | DS18B20 sensor 0–2 | °C / °F | sensor detected |

> **Units:** Which column applies is selected by the **`unitSystem`** portal parameter (see [Units (Metric/Imperial)](#units-metricimperial)) and indicated per-uplink by the status bitfield's imperial-units bit — the decoder reads this bit and labels values accordingly, no separate firmware version needed.

> **Rain encoding note:** The payload uses the CayenneLPP **Distance** type for rain values, but in this project it is interpreted as **mm or in** (`rain_cycle_mm`, `rain_since_start_mm`), not meters.
> Use the provided decoder in [chirpstack/decoder.js](chirpstack/decoder.js). Generic CayenneLPP decoders might assume meters, but you have to know the values are millimeters/inches.

> In normal operation (`debugMode = 0`) no debug telemetry is sent.

> **Payload size:**  
> Minimal config (DS18B20 only): approx. 15–30 bytes – fits in DR0 (SF12, 51 bytes).  
> Full config (wind + rain + BME280 + 3× DS18B20 + LiPo): approx. 70–75 bytes – requires **DR2 (SF10)** or higher.  
> With a private ChirpStack server and normal gateway distance this is not an issue.

---

## Compact Binary Payload Format (Alternative, ~50% Smaller)

Instead of CayenneLPP, the uplink can be sent as a compact custom binary format — same information, without CayenneLPP's per-field channel/type overhead. Select it via the **`payloadFormat`** parameter in the Wi-Fi portal (`0` = CayenneLPP, `1` = compact binary) or downlink parameter `10`.

| FPort | Format |
|-------|--------|
| 1 | CayenneLPP (default, see above) |
| 3 | Compact binary |

Both formats are decoded by the **same** [chirpstack/decoder.js](chirpstack/decoder.js) — it dispatches automatically based on the uplink's FPort and produces identical named JSON fields either way, so downstream consumers (Grafana, dashboards, MQTT/REST JSON) don't need to know which format is on the wire.

**Layout:** `[version: 1 byte][presence bitmask: 4 bytes, little-endian][field data...]` — only fields whose bit is set in the mask are included, in ascending bit-index order; values are big-endian with the same scaling as CayenneLPP.

| Bit | Field | Encoding | Active when |
|-----|-------|----------|-------------|
| 0 | Wind direction | `u16`, ° (always) | `windEn = 1` |
| 1 | Wind speed (avg) | `i16`, ×100, per unit system | `windEn = 1` |
| 2 | Wind gust | `i16`, ×100, per unit system | `windEn = 1` |
| 3 | Rain rate | `i16`, ×100, mm/h or in/h | `rainEn = 1` |
| 4 | Rain (cycle) | `u16`, ×10, mm or in | `rainEn = 1` |
| 5 | Rain since start | `u16`, ×10, mm or in | `rainEn = 1` |
| 6 | Status byte | `u8` (same bitfield as CayenneLPP Ch 2 Digital Input, incl. imperial-units bit) | always |
| 7 | Battery voltage | `u16`, ×100, V (always) | `lipoEn = 1` |
| 8 | BME280 humidity | `u8`, ×2, % (always) | BME280 detected |
| 9 | BME280 pressure QNH | `u16`, ×10, hPa or inHg | BME280 detected |
| 10 | BME280 pressure absolute | `u16`, ×10, hPa or inHg | BME280 detected |
| 11 | BME280 temperature | `i16`, ×10, °C or °F | BME280 detected |
| 12–14 | DS18B20 sensor 0–2 | `i16`, ×10 each, °C or °F | sensor detected |
| 15 | Debug: cycle counter | `u8` | `debugMode = 1` |
| 16 | Debug: CPU temperature | `i16`, ×10, °C or °F | `debugMode = 1` |
| 17 | Debug: send fail counter | `u8` | `debugMode = 1` |
| 18 | Debug: free heap | `u16`, KiB | `debugMode = 1` |
| 19 | Debug: uptime | `u16`, ×10, hours | `debugMode = 1` |

> **Payload size comparison:** Full config (wind + rain + BME280 + 3× DS18B20 + LiPo), no debug telemetry: CayenneLPP ≈ 70–75 bytes (requires DR2/SF10+) vs. compact binary ≈ 33 bytes — fits **DR0/SF12** (51-byte limit) even at maximum sensor configuration.

> **Rain accumulator resolution:** `rain_since_start_mm` uses a 16-bit field (0.1 mm resolution, max ≈ 6553 mm) instead of CayenneLPP's 32-bit Distance field — ample headroom between resets (downlink `0x01`), but not unlimited the way the CayenneLPP encoding is.

---

## Initial Configuration (Wi-Fi Portal)

### First boot
On first power-up (no DevEUI stored in NVS) the Wi-Fi portal opens **automatically**:

1. The OLED display shows **"Config Portal"**, the AP SSID, the URL and brief instructions
2. Connect your phone or laptop to the Wi-Fi network **"WeatherNode"** (no password)
3. Open **`http://192.168.4.1`** in a browser (most devices redirect automatically)
4. Enter all parameters and click *Save* → the device restarts automatically

### Re-opening the portal later
The portal can be opened at any time in two ways:

| Method | How |
|--------|-----|
| **Button long press** | Hold the user button (GPIO 0) for **≥ 3 seconds** during normal operation |
| **LoRaWAN downlink** | Send byte `0x03` on FPort 1 (takes effect after the current send cycle) |

> **Note:** GPIO 0 must **not** be pressed during power-on or reset – this would activate the ESP32 bootloader instead of the firmware. The portal is therefore triggered by a long press *during operation* only.

> **Saving:** clicking *Save* on the **LoRa-Weather-Node Parameter** page persists immediately and restarts the device — you don't also need to submit the separate "Configure WiFi" page unless you actually want to change WiFi credentials or the static IP. If the portal sits untouched for **3 minutes**, it also times out and restarts on its own (an actively-used session — browsing between pages, filling in the form — never trips this, since it counts from the *last* request, not from when the portal opened).

### Firmware update via Wi-Fi portal (OTA)
Firmware updates can be done wirelessly while the portal is open:

1. Open the Wi-Fi portal (either long press on user button for at least 3 seconds, or LoRaWAN downlink `0x03`)
2. Connect to the board Wi-Fi **WeatherNode**
3. Open **http://192.168.4.1** in a normal browser
4. On the portal start page, click **Firmwareupdate**
5. Upload the `.bin` file and wait until the board reboots

> If the captive portal page does not allow file upload, open **http://192.168.4.1/update** directly in the browser.

### Built-in Help Page

The full README is embedded in the firmware and rendered as a help page — no internet connection needed, so it also works while connected to the isolated **WeatherNode** portal Wi-Fi:

- **Portal:** click **Help / Documentation** on the portal start page, or open **http://192.168.4.1/help**
- **Persistent web server** (if `webApiEn` is on): linked from the settings page (`/api/settings`), or open `/help` directly

The page is generated at build time from this same `README.md` (single source of truth, always version-matched to the running firmware); only the picture gallery is omitted, since the referenced image files aren't embedded.

| Parameter | Description | Default |
|-----------|-------------|---------|
| DevEUI | 16 hex chars, from ChirpStack | – |
| JoinEUI | 16 hex chars, from ChirpStack | – |
| AppKey | 32 hex chars, from ChirpStack | – |
| LoRaWAN region | `EU868`, `US915`, `AU915`, `AS923`, `IN865`, `KR920`, `CN470`, `RU864` | EU868 |
| PCB revision | 12 = Rev 1.2, 13 = Rev 1.3 | 13 |
| Altitude above sea level (m) | Station altitude for QNH calculation | 560 |
| Uplink interval (s) | Target interval per measurement cycle | 300 |
| Sample interval (s) | Sub-sample interval within a cycle; ≤ uplink interval | 3 |
| Units (0=Metric, 1=Imperial) | Master unit switch — see [Units (Metric/Imperial)](#units-metricimperial) | 0 |
| Wind speed unit (0=km/h, 1=m/s) | Only applies when Units = Metric; ignored (always mph) when Units = Imperial | 1 |
| Wind direction offset (°) | North correction during installation (−180 to +180) | 0 |
| Wind sensor (1=Yes) | Davis wind sensor enabled | 1 |
| Rain sensor (1=Yes) | Davis rain sensor enabled | 1 |
| LiPo measurement (1=Yes) | Voltage measurement via voltage divider enabled | 0 |
| Debug mode (1=Yes) | Adds cycle counter, CPU temp and extra diagnostics in high LPP channels | 0 |
| CPU frequency (80/240 MHz) | 80 MHz saves power; below 80 MHz not allowed (PCNT-APB dependency) | 80 |
| Payload format (0=CayenneLPP, 1=compact binary) | See [Compact Binary Payload Format](#compact-binary-payload-format-alternative-50-smaller); FPort 1 vs. FPort 3 | 0 |
| Rain: mm per tip | For non-Davis rain gauges — see [Generic Sensor Calibration](#generic-sensor-calibration) | 0.2 |
| Wind speed: m/s per pulse-per-second | For non-Davis anemometers — see [Generic Sensor Calibration](#generic-sensor-calibration) | 1.00584 |
| Wind direction sensor (0=potentiometer, 1=calibrated table) | Set to 1 via the **Calibrate Wind Direction** portal page, not manually — see [Generic Sensor Calibration](#generic-sensor-calibration) | 0 |

> All settings are stored persistently in the **NVS (Non-Volatile Storage)** of the ESP32-S3 and survive restarts and firmware updates.

> Region input in the portal is validated. Unsupported values fall back to **EU868**.

### Config storage & upgrading a device from before `cfgdata` existed

Configuration lives in its own dedicated NVS partition, `cfgdata` (see [Project Structure](#project-structure)), rather than the ESP32's shared `nvs` partition — this keeps it fully isolated from Wi-Fi credentials and from any other firmware project that happens to use the same `config` namespace.

`cfgdata` only exists in the *partition table*, and OTA updates (via the Wi-Fi portal's Firmware update page or ArduinoOTA) only ever replace the running app image — they never rewrite the partition table. A device that was OTA-updated straight from a version *older than this partition* (i.e. one that never got a full serial reflash in between) will therefore boot into firmware expecting `cfgdata`, but not actually have it. The firmware detects this on boot and **cannot repair it from software alone**: the ESP32-Arduino SDK build used here refuses (`abort()`s) any attempt to write to the partition-table region at runtime, specifically to prevent firmware from bricking a device by accident. Neither normal operation nor the Wi-Fi portal would work correctly either way (no config can be saved without `cfgdata`, so the portal would just silently lose whatever is entered on every restart) — so instead of pretending to work, the device shows **"cfgdata missing! USB reflash needed"** on its display and halts there (a detailed explanation is also logged to the serial console).

A device in this state needs a **one-time full reflash via USB/serial** (which rewrites the partition table along with the app). Since that requires physical access anyway, it's also a natural point to re-check/re-enter that device's configured parameters through the Wi-Fi portal afterward.

### Debug mode

`debugMode` was introduced to support bench testing (for example without connected weather sensors) and fast diagnostics without affecting normal airtime.

When enabled, the node adds extra telemetry to high LPP channels (200+):

1. Cycle counter
2. CPU temperature
3. Send-fail counter
4. Free heap (KiB)
5. Uptime (hours)

When disabled (default), these values are not part of the uplink payload. The MQTT/REST JSON telemetry (see [Parallel Wi-Fi + MQTT Transport](#parallel-wi-fi--mqtt-transport-optional)) mirrors this: `cycle`, `cpuTemp`, `freeHeapKiB` and `uptimeH` (plus their Home Assistant discovery entities) are likewise only included when `debugMode = 1` — `sendFailCount` is the one field always present there regardless.

---

## Parallel Wi-Fi + MQTT Transport (Optional)

Alongside (or instead of) LoRaWAN, the node can run a second transport: **Wi-Fi STA + MQTT (TLS-capable)**, a small **REST API**, and **Home Assistant MQTT auto-discovery**. Everything here is opt-in and defaults to **off** — with all flags disabled, the firmware behaves exactly like before.

Five independent flags control this, all configurable in the Wi-Fi portal (new "WiFi + MQTT Parameter" section on the parameter page):

| Flag | Meaning |
|------|---------|
| `loraEn` | LoRaWAN transport enabled. Can be turned **off** for a Wi-Fi/MQTT-only node — no LoRaWAN gateway required. |
| `wifiEn` | Wi-Fi STA enabled — master switch; required for `mqttEn` and `webApiEn` to have any effect. |
| `mqttEn` | MQTT publish + control enabled. |
| `webApiEn` | REST API (`/api/data`) + web settings page enabled. |
| `mqttCtrlEn` | MQTT command topic enabled (subordinate to `mqttEn`). |

All four combinations of `loraEn`/`wifiEn` are valid (LoRaWAN only, Wi-Fi/MQTT only, both, or neither). Wi-Fi connect/MQTT connect are handled by a **non-blocking state machine**: if the network is unreachable, that uplink cycle's MQTT publish is simply skipped — sensor sampling and LoRaWAN uplinks are never affected.

> **Wi-Fi credentials and static IP** are configured separately via WiFiManager's own **"Configure WiFi"** menu (unchanged) — not in the parameter page.

### Device name, MQTT connection

| Parameter | Description | Default |
|-----------|-------------|---------|
| Device name | Freely chosen; used as Wi-Fi hostname, MQTT client ID, and default MQTT topic prefix | – (falls back to `weathernode-<chip-id>`) |
| MQTT broker host / port | e.g. `mqtt.example.com` / `8883` (standard ports: `1883` = plain/no TLS, `8883` = TLS) | – / `8883` |
| MQTT username / password | Optional | – |
| MQTT topic prefix | Optional override; defaults to the device name | – |
| MQTT security | `0` = plain, no TLS (required for brokers listening without TLS, e.g. the classic port 1883) · `1` = TLS, any server certificate trusted (encrypted, not authenticated) · `2` = TLS, server certificate verified against the CA below | `0` |
| MQTT Root CA certificate | PEM text, only used when MQTT security = `2` | – |
| Web settings admin password | Required to change settings via the web settings page (see below); writes are refused while empty | – |

### MQTT topics

With topic prefix `<prefix>` (device name unless overridden):

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `<prefix>/data` | node → broker | Telemetry JSON, published once per uplink cycle |
| `<prefix>/cmd` | broker → node | Commands (only if `mqttCtrlEn`) |
| `<prefix>/status` | node → broker | `online`/`offline` availability (retained, via MQTT LWT) |

**Telemetry JSON** mirrors the CayenneLPP uplink — fields are only present when the corresponding sensor is enabled/detected. Field **names** are unit-neutral (`temp`, not `tempC`); the actual unit in use is always given by a sibling `*Unit` field (same convention as the pre-existing `windUnit`), controlled by the `unitSystem` portal parameter — see [Units (Metric/Imperial)](#units-metricimperial):

Real example from a running device (Wind + BME280 enabled, no DS18B20/LiPo — those fields simply aren't present when their sensor isn't detected/enabled, e.g. `ds18b20`/`ds18b20Unit` or `batteryVolt`):

```json
{
  "windDirDeg": 345,
  "windSpeed": 3.546406,
  "windGust": 7.092811,
  "windUnit": "km/h",
  "rainRate": 0,
  "rainRateUnit": "mm/h",
  "rain": 0,
  "rainSinceStart": 0,
  "rainUnit": "mm",
  "temp": 28.51,
  "tempUnit": "C",
  "humidityPct": 49.24707,
  "pressureAbs": 949.9233,
  "pressureQnh": 1015.552,
  "pressureUnit": "hPa",
  "status": {
    "bme280Present": true,
    "bus1Present": false,
    "sendFailCount": 0
  },
  "nextCycleInSec": 2
}
```

When `bus1Present` is true, `ds18b20` is a fixed-size array — one entry per detected probe slot, `null` for a slot that's since disconnected — rather than shrinking to only the connected probes; this keeps `ds18b20[i]` meaning "probe `i`" for every consumer, notably the Home Assistant discovery templates below which hardcode that index.

`nextCycleInSec` counts down the seconds left until the current measurement cycle completes and the next uplink/publish happens (always present). `cycle` — a wrapping counter incremented once per uplink cycle, a cheap way to detect whether `/api/data` or the MQTT topic returned a new measurement vs. a repeat of the last one — is only included when `debugMode = 1`, same as the LoRaWAN payload (there to save airtime; here just to keep the default payload minimal).

**Commands** on `<prefix>/cmd` accept two JSON shapes:

```json
{"cmd": "resetRain"}
```
`resetRain` | `restart` | `debugOn` | `debugOff` | `portal` | `factoryReset` (same actions as the LoRaWAN downlink bytes `0x01`–`0x05`/`0xFF`, see [LoRaWAN Downlink Commands](#lorawan-downlink-commands)).

```json
{"params": [{"id": 1, "value": 300}]}
```
Same `paramId` scheme as LoRaWAN downlink `0x10` (see the parameter table further below) — settable via MQTT as an alternative to LoRaWAN.

### REST API

| Endpoint | Method | Auth | Purpose |
|----------|--------|------|---------|
| `/` | GET | none | Self-contained (no CDN) dashboard — live cards, wind compass, records, and trend charts (temperature, humidity, pressure, wind, rain rate, rain amount, DS18B20 probes, battery) — localized UI, Metric/Imperial and 24h/7d toggles, auto-refresh |
| `/status` | GET | none | Plain, JS-free HTML status table: last-measured values plus device/WiFi/MQTT/LoRaWAN status, with links to the pages below — this is what used to live at `/` before the dashboard took over the landing spot |
| `/chart` | GET | none | Redirects to `/` (the dashboard's old address, kept as a 302 so nothing bookmarked/linked during development breaks) |
| `/api/data` | GET | none | Current telemetry JSON (same schema as the MQTT data topic) |
| `/api/history` | GET | none | Rolling history JSON backing `/` — the fine/24h tier by default, or the coarse/7-day tier with `?range=7d` |
| `/api/settings` | GET | admin password | HTML form showing current operational settings |
| `/api/settings` | POST | admin password | Change uplink/sample interval, wind/rain sensor enable, debug mode, units (Metric/Imperial) |
| `/api/log` | GET | admin password | Rolling view of the device's own log output (auto-refreshing, dark terminal style) |
| `/api/log/raw` | GET | admin password | Plain-text snapshot of the same rolling log, polled by `/api/log`'s page |
| `/api/log/clear` | POST | admin password | Clears the rolling log buffer |

**By design, the REST API only ever exposes non-secret operational parameters.** It is deliberately **not** a route into the full Wi-Fi config portal — WiFi credentials, the LoRaWAN AppKey, and MQTT credentials/CA certificate remain reachable only via physical button-press, first boot, or LoRaWAN downlink `0x03`. This keeps physical (or LoRaWAN-authenticated) access as the trust boundary for provisioning secrets, while still allowing convenient remote tweaks to day-to-day settings. If no admin password is configured yet, all admin-password-gated endpoints refuse access outright (403) rather than prompting for one that could never match.

> **Data freshness:** `/api/data` does **not** trigger a new sensor reading — it just serializes whatever the relevant globals currently hold. How often those actually change depends on the sensor: **wind, rain, and BME280** are sub-sampled every `sampleSec` (`takeSample()` in `main.cpp` overwrites `windSpeedMs`/`bme280TempC`/`rainMM`/etc. with a fresh single-sample reading each time, only briefly replaced by the whole cycle's average/sum right when a cycle completes) — polling faster than `sampleSec` for these just returns the same value repeatedly, but polling at `sampleSec` cadence does show new readings between uplink cycles. **DS18B20 and battery/CPU temperature** are the exception: they're only measured once per full cycle (`computeFinalValues()`), so polling faster than `uplinkIntervalSec` gains nothing for those fields specifically. Check the **`nextCycleInSec`** field to see how long until the next cycle lands, or (with `debugMode = 1`) the **`cycle`** field — a wrapping counter incremented once per uplink cycle — to detect a new full-cycle measurement vs. a repeat of the last one.

> **History buffer:** `/api/history` (and the dashboard at `/` built on top of it) is a two-tier in-RAM ring buffer, RRDtool-style — a **fine tier** (288 buckets × 5 min = 24h, the default) and a **coarse tier** (168 buckets × 1h = 7 days, `?range=7d`), so the recent view keeps fine detail while the week view stays cheap in RAM (~9 KB combined) rather than needing a single flat 2000+ sample buffer. Each completed measurement cycle merges into *both* tiers' current bucket — a running average for wind speed/rain rate/temperature/humidity/pressure/battery, a running **max** for wind gust (averaging away the peak would defeat the point), and a running **sum** for rain amount (`rainMm`, an accumulated quantity, not a rate) — independent of `uplinkIntervalSec`; a bucket is only finalized into the ring once its real-world time window elapses, so a short uplink interval no longer eats into how far back the buffer reaches. Neither tier is persisted across a restart (OTA update, watchdog restart, downlink `0x02`/`0x03`/`0xFF`) — for durable long-term history, use Grafana via MQTT/ChirpStack as described above. A bucket only records the channels that were enabled/detected during it (e.g. no wind entry if `windEn = 0`), so toggling a sensor mid-run shows up as a gap, not a misleading zero. The dashboard's own "current reading" (cards, compass) intentionally comes from `/api/data`, not the last history bucket — otherwise "now" would only refresh once per bucket (up to 5 min/1h) instead of once per uplink cycle. Each response also appends the *currently open* (not-yet-finalized) bucket as one extra, newest point — otherwise the charts would show "No data yet" for the first 5 min/1h after boot, even though the node has already been measuring; this live point simply gets more representative as more uplink cycles merge into it before it eventually finalizes. There's deliberately no way to clear this buffer from the dashboard — unlike the log buffer (which can fill up with debug noise worth clearing before watching for a specific event), there's no real use case for intentionally discarding your own trend data, and the buffer is RAM-only anyway (a restart already empties it).

> **Dashboard language, units, range, time axis, and auto-refresh:** the `/` dashboard's UI text is localized (English, German, French, Spanish, Portuguese — auto-detected from the browser, overridable via the page's own language selector) and has its own Metric/Imperial toggle, both purely client-side preferences persisted in the browser's `localStorage`. Neither touches `gCfg.unitSystem` — that's a device-wide setting affecting the actual LoRaWAN payload and MQTT/Home Assistant data, whereas this is just how one visitor's browser chooses to *display* the already-fetched history. The 24h/7d range toggle is different from those two: since it selects between the two genuinely separate server-side ring-buffer tiers described above, switching it triggers a real `/api/history?range=7d` request rather than just a local re-render. Auto-refresh is a Grafana-style step dropdown (Off / 1s / 5s / 10s / 30s / 1m, default **10s**) rather than a plain on/off switch — a short interval is genuinely useful here, not just extra load, since wind/rain/BME280 readings actually do update every `sampleSec` between uplink cycles (see the "Data freshness" note above). "Off" is simply the first step, so pausing (e.g. to read a chart without it jumping mid-look) and picking a cadence share one control; the choice is persisted in `localStorage`.
>
> The trend charts' x-axis defaults to **absolute time** (e.g. "14:32", or "Tue 14:32" beyond 24h ago in the 7-day view), toggleable to **relative** ("14m ago" / "vor 14m" / "il y a 14m" / "hace 14m" / "há 14m", depending on language). The device itself has no synced wall clock (no NTP anywhere in this project) — absolute mode works anyway because it's computed entirely client-side: the browser's own `Date.now()` at render time, minus each point's `ageSec`. Locale-aware formatting (`toLocaleTimeString`/`toLocaleDateString`) follows the dashboard's selected language, not necessarily the browser's own locale.
>
> The "Rain amount" card intentionally shows a *different* live value than its own trend sparkline: the big number is `/api/data`'s `rain` field (the amount from the single most-recently-completed uplink cycle), while the sparkline/records below it track the history buffer's bucketed `rainMm` (summed per 5-min/1h bucket). "Rain amount" replaced "Rain rate" as the 5th card — rain rate keeps its own chart, just no longer a dedicated card.
>
> **DS18B20 probes:** up to 3 probes get one card (probe 0 only, to avoid crowding the row with near-identical tiles) plus one 3-line trend chart (all connected probes, one series each), sharing the device's regular temperature unit. Both history.cpp's ring buffer and `/api/data`'s `ds18b20` array preserve *slot* identity — index `i` always means "probe `i`", with `null` for a slot that's disconnected rather than the array shrinking — so a probe that drops out mid-session goes gap/unavailable instead of silently shifting a later probe's reading into an earlier one's card or Home Assistant entity.
>
> **Hidden when there's no data:** every card and chart — not just DS18B20 — disappears entirely (rather than showing a permanent "--" or an empty "No data yet" chart) for a metric that's never had a single real reading, e.g. no BME280 detected, wind/rain sensing disabled, or no DS18B20 probe wired up. This is the same rule the Records panel already applied, just generalized to the cards/charts above it. A multi-series chart (wind, DS18B20) keeps whichever series do have data and only hides the whole panel once every one of its series is empty.

### Home Assistant MQTT Auto-Discovery

When `mqttEn` is on, the node automatically publishes [Home Assistant MQTT discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) configs (retained, prefix `homeassistant`) for every currently enabled sensor, right after each successful MQTT connect. No separate toggle is needed — if you don't use Home Assistant, the extra retained config topics are harmless to any other MQTT client.

- All entities are grouped under one HA device (identified by the MQTT client ID).
- An **availability topic** (`<prefix>/status`, backed by MQTT LWT) makes entities show "unavailable" immediately if the node drops off the network, instead of freezing on the last value.
- The 3 DS18B20 probe entities are always published (hot-pluggable, auto-detected once per cycle); an unconnected probe simply shows as unavailable.
- Entities reflect which sensors were enabled **at the time of the MQTT connect**. If you toggle a sensor at runtime (downlink/MQTT command), the entity list updates on the next reconnect.

---

## User Button (GPIO 0)

| Press duration | When | Action |
|---------------|------|--------|
| — | First boot (no DevEUI stored) | Wi-Fi portal opens automatically |
| Short press (< 3 s, on release) | Normal operation | Turn on OLED display for 20 s |
| Long press (≥ 3 s) | Normal operation or setup loops (join retries etc.) | Open Wi-Fi config portal → save → restart |

Button presses are detected via a **hardware interrupt** (GPIO CHANGE), so a short press is never missed – even while the LoRa send/receive window is blocking the main task for several seconds.

The display is **off** during normal operation to protect the SSD1306 driver and save power. It turns off automatically after 20 seconds. During the startup sequence (radio init, LoRaWAN join retries) the display stays on permanently so that status and error messages remain visible.

---

## ChirpStack Payload Decoder

The file `chirpstack/decoder.js` contains the JavaScript decoder for ChirpStack v3 and v4.

> **Tip:** The decoder file can also be downloaded directly from the device via the Wi-Fi config portal (*Payload decoder (JS)* button on the start page). The downloaded file is always version-matched to the running firmware.

**Adding the decoder to ChirpStack:**
1. In the ChirpStack web interface: *Device Profiles → Codec → JavaScript codec functions*
2. Paste the contents of `chirpstack/decoder.js`
3. Save – all values will appear as named JSON fields in the uplink events

**Example output (JSON):**
```json
{
  "wind_direction_deg": 247,
  "wind_speed_avg": 12.30,
  "wind_gust": 18.50,
  "rain_rate_mmh": 0.00,
  "rain_cycle_mm": 0.0,
  "rain_since_start_mm": 14.2,
  "cycle_counter": 42,
  "status": {
    "bme280_present": true,
    "bus1_has_sensors": true,
    "send_fail_count": 0,
    "wind_speed_unit": "m/s"
  },
  "battery_v": 3.98,
  "bme280_humidity_pct": 67.5,
  "bme280_pressure_qnh_hpa": 1013.2,
  "bme280_pressure_abs_hpa": 952.1,
  "bme280_temperature_c": 21.3,
  "ds18b20_0_c": 18.6,
  "ds18b20_1_c": 19.1
}
```

**Rain accumulator evaluation:**  
`rain_since_start_mm` is a cumulative counter. The delta between two received packets gives the actual rainfall – even if packets were lost in between.

---

## TTN (The Things Stack) Instead of Private ChirpStack

The node can be used with TTN/TTS as well. No payload format change is required, because the uplink is still standard CayenneLPP and the same decoder logic can be used.

### What to configure

1. In the Wi-Fi portal, set **LoRaWAN region** to the matching regional plan of your TTN deployment (for Europe typically `EU868`).
2. In TTN, create an end device using **OTAA**, LoRaWAN 1.0.x profile, and copy **DevEUI / JoinEUI / AppKey** into the node portal.
3. Add the payload formatter (JavaScript) in TTN by reusing [chirpstack/decoder.js](chirpstack/decoder.js) logic (field names can stay the same).

### Fair-use and practical intervals (EU868)

TTN strongly favors low airtime and low uplink rates. Practical guidance for weather telemetry:

1. Prefer **60 s to 300 s** uplink intervals.
2. Avoid high-spreading-factor operation with very short intervals.
3. Keep payload compact (disable unused sensors in the portal).

For TTN community networks, very short uplink intervals are generally not appropriate and may violate fair-use expectations depending on data rate and gateway conditions.

For TTN operation, keep `debugMode` disabled except for short troubleshooting windows.

### Do you need further firmware changes for TTN?

In most cases: **no functional code changes** are required. The key points are correct region and OTAA credentials.

---



## LoRaWAN Downlink Commands

> The same commands and parameters below are also reachable via the MQTT command topic if the parallel Wi-Fi + MQTT transport is enabled — see [Parallel Wi-Fi + MQTT Transport](#parallel-wi-fi--mqtt-transport-optional).

Downlinks are sent on **FPort 1** with a single byte:

| Byte | Function |
|------|----------|
| `0x01` | Reset rain accumulator (rain since start) to 0 |
| `0x02` | Restart device (triggers a new OTAA join) |
| `0x03` | Open Wi-Fi config portal (takes effect after the current send cycle completes) |
| `0x04` | Enable debug mode (persistent) |
| `0x05` | Disable debug mode (persistent) |
| `0xFF` | Factory reset: wipe all stored configuration and restart into the first-boot config portal |

### Parameter Update via Downlink `0x10`

Parameter updates are sent on **FPort 1**, payload starting with `0x10` followed by one or more triplets:

`[paramId][valueHi][valueLo]`

- `paramId` is a single **hex byte** (see table below).
- `value` is a **signed int16, big-endian**, i.e. two bytes `valueHi valueLo` (e.g. decimal `300` → hex `0x012C` → `valueHi=0x01`, `valueLo=0x2C`).
- Every triplet is 3 bytes; you can send several triplets in one downlink by simply repeating the pattern after the leading `0x10` — no separator needed between triplets.

To avoid mixing up decimal ParamIDs from firmware/code with the hex bytes an actual downlink needs, here is every parameter with a ready-to-send example payload:

| ParamID (dec) | ParamID (hex) | Parameter | Meaning / Range | Example value | value hex (Hi Lo) | Full downlink payload (hex) |
|---|---|---|---|---|---|---|
| 1 | `0x01` | `uplinkIntervalSec` | `5..3600` | `300` (5 min) | `01 2C` | `10 01 01 2C` |
| 2 | `0x02` | `sampleSec` | `1..uplinkIntervalSec` | `3` | `00 03` | `10 02 00 03` |
| 3 | `0x03` | `windEn` | `0` or `1` | `1` | `00 01` | `10 03 00 01` |
| 4 | `0x04` | `rainEn` | `0` or `1` | `1` | `00 01` | `10 04 00 01` |
| 5 | `0x05` | `lipoEn` | `0` or `1` | `1` | `00 01` | `10 05 00 01` |
| 6 | `0x06` | `debugMode` | `0` or `1` | `1` | `00 01` | `10 06 00 01` |
| 7 | `0x07` | `windDirOffsetDeg` | `-180..180` | `-45` (negative example — two's complement) | `FF D3` | `10 07 FF D3` |
| 8 | `0x08` | `windUnitMs` | `0`=km/h, `1`=m/s | `1` | `00 01` | `10 08 00 01` |
| 9 | `0x09` | `cpuFreqMhz` | `80` or `240` | `240` | `00 F0` | `10 09 00 F0` |
| 10 | `0x0A` | `payloadFormat` | `0`=CayenneLPP/FPort 1, `1`=binary/FPort 3 | `1` | `00 01` | `10 0A 00 01` |
| 11 | `0x0B` | `unitSystem` | `0`=Metric, `1`=Imperial — see [Units (Metric/Imperial)](#units-metricimperial) | `1` | `00 01` | `10 0B 00 01` |
| 12 | `0x0C` | `rainMmPerTip` | mm/tip ×1000, `1..5000` — see [Generic Sensor Calibration](#generic-sensor-calibration) | `200` (0.2mm, Davis default) | `00 C8` | `10 0C 00 C8` |
| 13 | `0x0D` | `windMsPerPps` | (m/s per pulse/s) ×1000, `1..10000` — see [Generic Sensor Calibration](#generic-sensor-calibration) | `1006` (Davis default ≈1.006) | `03 EE` | `10 0D 03 EE` |
| 14 | `0x0E` | `windDirSensorType` | `0`=potentiometer, `1`=calibrated table — see [Generic Sensor Calibration](#generic-sensor-calibration) | `1` | `00 01` | `10 0E 00 01` |

**Combining multiple parameters in one downlink:** just concatenate the triplets after the single leading `0x10`. Example — set `uplinkIntervalSec=300` (`paramId=1`) and enable `debugMode` (`paramId=6`) in one downlink:

`10 01 01 2C 06 00 01`

Valid parameters are stored persistently in NVS immediately.

---

## Error Handling / Robustness

### Watchdog Timer
A **hardware watchdog** (120 s timeout) protects against hanging code.  
It is activated only **after a successful LoRaWAN join** – allowing slow joins and Wi-Fi portal operation to run without a WDT reset.

### Automatic Restart on Send Failures
After **10 consecutive send failures** an automatic restart is triggered to renew the LoRaWAN join (e.g. after gateway outage or session timeout).

### ADC Averaging
Wind ADC (direction) and battery ADC are averaged over **8 samples** to reduce ESP32-S3 ADC noise.

---

## Uplink Interval and Timing

The configured uplink interval is a **target value**: at the end of each cycle the code only waits the remaining time until the target interval (`millis()` delta). This prevents the measurement time (1-Wire conversion ~750 ms, LoRa airtime ~500 ms) from adding up twice.

```
Cycle start → measure → send → sleep(max(0, target − elapsed))
```

`Sample interval` and `Uplink interval` have different roles:

- **Sample interval** (`sampleSec`): defines how often sub-samples are taken within one uplink cycle.
- **Uplink interval** (`uplinkIntervalSec`): defines when one uplink is generated from the accumulated sub-samples.

Within one send cycle, values are combined as follows:

- **Wind speed (avg)**: arithmetic mean of all wind speed sub-samples.
- **Wind gust (max)**: maximum wind speed seen in all sub-samples of that cycle.
- **Wind direction**: vector average (sin/cos) across all sub-samples.
- **Rain cycle**: sum of rain pulses within the cycle.
- **Rain rate**: derived from the cycle rain amount and full cycle duration.
- **BME280 temperature/humidity/pressure**: arithmetic mean of all BME280 sub-samples.
- **DS18B20 temperatures**: measured once per uplink cycle (not sub-sampled/averaged over `sampleSec`).

**Minimum practical interval** in EU868 at DR5 (SF7): ~5 s (duty cycle restriction).  
For TTN/community networks, use significantly longer intervals (typically at least 60 s).  
**Default values:** uplink interval `300 s`, sample interval `3 s`.

---

## Project Structure

```
├── src/
│   ├── main.cpp             # Orchestration only: setup/loop, sampling, button, LoRaWAN send
│   ├── config.h/.cpp        # Config struct, NVS load/save
│   ├── partition_repair.h/.cpp # Detects a missing "cfgdata" partition after OTA from older firmware (logs; needs a reflash)
│   ├── globals.h            # Shared live state (extern) between the modules below
│   ├── sensors.h/.cpp       # Pin/PCNT/ADC setup, sensor reads (wind, rain, BME280, DS18B20, battery)
│   ├── lora_payload.h/.cpp  # CayenneLPP + compact binary uplink encoding
│   ├── portal.h/.cpp        # Wi-Fi config portal + web routes (/decoder, /help, /calibrate)
│   ├── params.h/.cpp        # Transport-agnostic command/parameter actions (LoRaWAN + MQTT)
│   ├── json_payload.h/.cpp  # Shared telemetry JSON builder (MQTT + REST API)
│   ├── units.h/.cpp         # Metric/Imperial value + label conversion
│   ├── wifi_mqtt.h/.cpp     # Wi-Fi/MQTT state machine, publish, HA discovery, MQTT commands
│   └── web_api.h/.cpp       # REST API (/api/data) + web settings page
├── chirpstack/
│   └── decoder.js           # ChirpStack v3/v4 JavaScript payload decoder
├── pcblayout/
│   ├── lorawan-weather-node-pcb.fzz                    # Fritzing Rev 1.3
│   ├── lorawan-weather-node-pcb_Schaltplan.png
│   ├── lorawan-weather-node-pcb_Leiterplatte.png
│   ├── lorawan-weather-node-pcb_bom.html
│   ├── lorawan-weather-node-pcb-gerberfile rev12.zip   # Gerber files Rev 1.2
│   ├── lorawan-weather-node-pcb-gerberfile rev13.zip   # Gerber files Rev 1.3
│   └── design_readme.txt    # Connector pin assignments
├── platformio.ini
└── README.md
```

---

## Libraries

| Library | Version | Purpose |
|---------|---------|---------|
| RadioLib | ^7.6.0 | LoRaWAN stack (OTAA, EU868) |
| Heltec_ESP32_LoRa_v3 | ^0.9.2 | Board support (display, heltec_temperature) |
| LoRaWAN_ESP32 | ^1.2.0 | LoRaWAN session management (persistence) |
| CayenneLPP | ^1.6.1 | Payload encoding |
| WiFiManager | ^2.0.16-rc.2 | Wi-Fi configuration portal |
| Adafruit BME280 | ^2.2.4 | BME280 driver |
| Adafruit Unified Sensor | ^1.1.15 | Sensor abstraction |
| OneWire | ^2.3.8 | 1-Wire protocol |
| DallasTemperature | ^4.0.6 | DS18B20 driver |
| esp_task_wdt | (IDF built-in) | Hardware watchdog |
| ArduinoJson | ^7.2.0 | Telemetry JSON, MQTT commands, Home Assistant discovery configs |
| PubSubClient | ^2.8 | MQTT client (buffer size raised via `MQTT_MAX_PACKET_SIZE` build flag) |
| WiFi / WiFiClientSecure / WebServer | (ESP32 core) | Wi-Fi STA, TLS, REST API — no extra `lib_deps` entry needed |

---

## Technical Notes

### PCNT and CPU Frequency
The hardware pulse counter (PCNT) uses the **APB clock** for its debounce filter.  
At CPU frequencies ≥ 80 MHz (PLL mode) the APB clock stays constant at 80 MHz.  
Below 80 MHz (XTAL mode) the APB clock drops proportionally → debounce filter timing becomes invalid.  
**Minimum allowed CPU frequency: 80 MHz.**

### 1-Wire with Runtime-Configured Pins
Because the GPIO numbers for 1-Wire are determined at runtime from the PCB revision, the `OneWire` and `DallasTemperature` objects are allocated on the heap with `new` (no static constructor with hard-coded pins possible).

### LoRaWAN Session Persistence
The `LoRaWAN_ESP32` library stores the LoRaWAN session (frame counters, DevAddr, session keys) in NVS. A restart does not necessarily trigger a new OTAA join.

### ADC Linearity
The ESP32-S3 ADC is non-linear (especially below 150 mV and above 3.1 V). For the wind direction potentiometer this is uncritical since values are mapped to 0–359° and the absolute measurement error remains < 5° in practice. For the battery voltage measurement a calibration factor is included in the code.

### Web Server Task and Cross-Core Concurrency
The REST/web UI (`server.handleClient()`) runs on its own FreeRTOS task pinned to **core 0**, separate from the main sensor-sampling/LoRaWAN-send loop on core 1 — see `webApiTask()` in `src/web_api.cpp`. This keeps an incoming HTTP request from waiting behind `sendLoRa()`'s blocking radio airtime, which is where it would otherwise queue up if the web server were still polled once per main-loop iteration (as it originally was).

Running two tasks means a few pieces of state are now genuinely touched from both cores, addressed as follows:

- **The `WebServer` object itself** (`begin()`/`stop()`/`handleClient()`): guarded by `serverMutex` in `web_api.cpp`. This covers more than it might look like at first: `webApiStop()` doesn't just stop the server once — it also `vTaskSuspend()`s `webApiTask` outright (guarded against a null task handle on the first-boot path, where `vTaskSuspend(NULL)` would otherwise suspend the *calling* task instead of doing nothing). Without that, the task's own poll loop would simply restart the server a few milliseconds later on its own — since it doesn't know a config portal is about to open — recreating exactly the port-80 conflict with WiFiManager's captive-portal server that `webApiStop()` exists to prevent. Every caller of `webApiStop()` (button long-press, first boot, downlink `0x03`) heads into `ESP.restart()` regardless of outcome, so a one-way suspend (never resumed) is correct rather than just a pause.
- **The dashboard's history ring buffers** (`src/history.cpp`): both tiers (fine and coarse, see below) and their in-progress accumulator buckets are guarded by one shared `historyMutex` — `pushHistorySample()` (main task, once per uplink cycle) writes into both tiers' current bucket and finalizes/appends when a bucket's window elapses, while `buildHistoryJson()` (reachable from the web task) reads them. Unlike a plain scalar, an in-progress index update read mid-write could hand back an out-of-bounds array index, not just a stale value, so this needs a real mutex rather than just a visibility guarantee. (There's no `historyClear()` — see the note above on why clearing this buffer isn't exposed at all.)
- **The device's own log ring buffer** (`SerialLog`/`dbgSerial`, `src/serial_log.h`/`.cpp`): guarded by its own mutex for the same reason as the history buffer — nearly all logging happens from the main task, but `webApiTask` also logs its own start/stop messages, and `/api/log/raw`/`/api/log/clear` (web task) read/reset the same `logHead`/`logFull` state.
- **The `Config` fields that `applyDownlinkParam()` (`src/params.cpp`) can write** — the plain `int` ones (`uplinkIntervalSec`, `sampleSec`, `windEn`, `rainEn`, `lipoEn`, `debugMode`, `windDirOffsetDeg`, `windUnitMs`, `cpuFreqMhz`, `payloadFormat`, `unitSystem`, `windDirSensorType`) plus the two `float` sensor-calibration ones (`rainMmPerTip`, `windMsPerPps`): all marked `volatile` in `config.h`. A single aligned 32-bit load/store is already atomic on this hardware, so the risk here isn't torn values — it's the compiler caching a read in a register across a loop iteration on one core while the other core writes it; `volatile` forces a fresh memory read each time. `std::atomic<int>`/`std::atomic<float>` were deliberately **not** used instead: `Config` is copied by value throughout (`loadConfig()`'s return, `portal.cpp`'s `Config c = cur`), and atomics aren't copyable, which would have broken that. One int-specific wrinkle: `volatile int` can't be passed directly into `std::min`/`std::max` alongside a plain `int` (template deduction can't unify the two cv-qualifications) — `portal.cpp`'s sample-interval clamp reads `uplinkIntervalSec` into a plain local `int` first to work around it; the two `float` fields never hit `min`/`max`/`constrain` anywhere, so no equivalent fixup was needed for them.

Everything else — the live sensor globals (`windSpeedMs`, `bme280TempC`, etc., read by `/api/data` and `/`) — is left as-is: `/api/settings` never writes them, so the only exposure is the main task writing while the web task reads, which at worst shows one stale/slightly-mismatched pair of values in a single response — self-correcting on the next refresh, not worth a lock.

**TLS/HTTPS for this server was deliberately not pursued**, after weighing it: the ESP32 Arduino `WebServer` class has no secure equivalent to drop in (would mean building a per-connection TLS handshake layer on `WiFiClientSecure` from scratch, a much bigger lift than MQTT's use of the same library in the simpler client-verifying-a-server role); the RAM/latency cost of a TLS handshake is real on this hardware; and the actual secret at risk — the web admin password — only gates non-critical operational settings, not WiFi/LoRaWAN/MQTT credentials (those stay reachable only via the physical portal). If cleartext Basic Auth ever becomes a real concern, switching to Digest Auth would address that specific risk far more cheaply than full TLS.

---

## 📄 License & Open Source

This project is fully open-source. To support both software developers and hardware makers, it is dual-licensed:

* **Software / Firmware:** Licensed under the **[MIT License](LICENSE-SOFTWARE)**. You can freely use, modify, and distribute the code.
* **Hardware (Schematics, PCB & 3D Files):** Licensed under the **[CERN-OHL-P v2](LICENSE-HARDWARE)** (CERN Open Hardware Licence – Permissive). You are free to manufacture, modify, and distribute the hardware designs.

*Developed with 🚜 and 💻 by Gerhard Massenbichler at Franzenhof.*
)READMEMD";
