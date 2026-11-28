# LoRaWAN

LoRaWAN is optional (off by default) — for sites without Wi-Fi. This page covers the payload decoder for ChirpStack and TTN, join and session handling, downlink commands, the payload formats and the low-power mode. Parameter changes by downlink (`0x10`) are listed in [Settings](settings.md#parameter-update-via-downlink-0x10).

[← back to README](../README.md)

## ChirpStack Decoder

[`chirpstack/decoder.js`](../chirpstack/decoder.js) decodes both payload formats for ChirpStack v3 and v4. You can also download it from the config portal (*Payload decoder (JS)*) — always matching the running firmware.

1. ChirpStack: *Device Profiles → Codec → JavaScript codec functions*
2. Paste the contents of `decoder.js` and save.

**Example output:**

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

> `rain_since_start_mm` is a running total. The difference between two packets is the rain in between — even if packets were lost.

## The Things Network (TTN)

Works the same way, no firmware changes needed:

1. Set the **region** in the portal (Europe: `EU868`).
2. In TTN, create an **OTAA** device (LoRaWAN 1.0.x, or 1.1 with NwkKey = AppKey) and copy DevEUI / JoinEUI / AppKey into the portal.
3. Use the logic of `decoder.js` as the TTN payload formatter.

**Fair use:** TTN's public Sandbox limits each node to **30 seconds uplink airtime and 10 downlinks per day**. Choose the interval from the actual payload size and data rate using an [airtime calculator](https://avbentem.github.io/airtime-calculator/ttn/eu868); **60–300 s is not automatically compliant**, especially at high spreading factors. Disable unused sensors and debug fields. The 34-byte binary payload (wind + rain + BME280 + 3 × DS18B20 + LiPo) fits every EU868 data rate, but not every region/data rate. Private networks still have regional duty-cycle limits. See [TTN's policy](https://www.thethingsnetwork.org/docs/lorawan/duty-cycle/).

## Join and Session

- **Join:** OTAA. The AppKey is used as both AppKey and NwkKey, so LoRaWAN **1.0.x and 1.1** network servers both work.
- Join nonces are persisted to avoid reuse across normal restarts. Erasing flash can remove that state; coordinate a reset/reprovisioning with the network server rather than assuming a flash erase is harmless.
- **Every restart performs a new join** (the node never deep-sleeps, and the session is only kept across deep sleep). In normal operation the session stays in RAM.
- **Silent session → rejoin:** with ADR on, the node asks for a network answer after 64 uplinks without a downlink and the network server must reply within 32 more. After **288 uplinks in a row without any downlink** (3 × (64 + 32)) the session is treated as broken and the node rejoins (log: `No downlink for 288 uplinks …`). At a 300 s interval that is 24 h, at 60 s about 5 h. If the rejoin does not help (still no downlink at all), the threshold doubles each time up to 2304 uplinks (8 days at 300 s); any received downlink resets it to 288. Before this, a session that stopped receiving downlinks stayed stuck at SF12 until the next restart.
- Sensors, I/O and Wi-Fi start **before** the first join attempt. Radio initialization,
  provisioning or join failures do not trap the node in a startup retry loop:
  measurements, history and Wi-Fi/MQTT remain available. Failed joins are retried
  after 1, 2, 4, 8 … minutes, at most every 60 min, and back to 1 min after a
  successful join (each attempt costs airtime and a DevNonce; a fixed 1-min retry
  during a network outage would break the LoRaWAN join duty cycle and use up the
  65 536 nonces in about 45 days). Each individual RadioLib join exchange still blocks
  the main loop for several seconds, like an uplink; pulse counters keep running.
  Logs report failures; hold the button 3 s to check keys and region.
- After 10 consecutive uplink failures, the node schedules a new join instead
  of restarting the whole device and losing its chart history.

## Downlink Commands

Send application commands on **FPort 1**, as **unconfirmed Class-A downlinks**. The node listens after its next uplink; reception is not guaranteed. Do not enable Class B/C for this firmware.

**FPort 0 is reserved for LoRaWAN MAC commands**, not application control. Commands on any other FPort are ignored. In ChirpStack's queue, select FPort `1` and HEX payload `03` to open the portal (equivalent Base64: `Aw==`). Binary uplinks on FPort 3 do not change the command port.

Single-byte commands:

| Byte | Action |
|------|--------|
| `0x01` | Reset the rain total ("since start") |
| `0x02` | Restart (new join) |
| `0x03` | Open the config portal (after the current cycle) |
| `0x04` | Debug mode on (saved) |
| `0x05` | Debug mode off (saved) |
| `0x06` | Reset the AUX pulse total (PCB v2.0) |
| `0xFF` | Clear project settings in `cfgdata`, restart into the first-boot portal; not a full flash erase or a guarantee of cleared Wi-Fi/LoRaWAN persistence |

## Payload Formats

Choose with **Payload format** in the settings or downlink parameter `10`. The [decoder](../chirpstack/decoder.js) handles both automatically (by FPort) and produces the same JSON fields.

| FPort | Format | Size (wind + rain + BME280 + 3 × DS18B20 + LiPo) |
|-------|--------|------------------------------------------------|
| 3 | **Compact binary (default)** | **34 bytes** — fits every EU868 data rate, even DR0/SF12 |
| 1 | CayenneLPP | 62 bytes — needs DR3/SF9 or faster on EU868 |

EU868 payload limits: DR0–DR2 (SF12–SF10) 51 bytes, DR3 (SF9) 115 bytes, DR4–DR5 222 bytes (US915 DR0: only 11 bytes). UV adds 2 bytes per value (voltage, index) to compact binary and 7 bytes to CayenneLPP for voltage/status, or 11 bytes with the index.

**Fitting into the data rate:** before every uplink the node asks the LoRaWAN stack how many bytes the current data rate allows (ADR may switch to SF12 at any time; pending MAC answers are taken into account). If the payload is larger, it leaves out the least important field groups until it fits, in this order: debug → AUX pulses → AUX analog → UV → wind 2 → fan speed → output power → I/O status → Victron → LiPo → DS18B20 #2, #1, #0 → BME280 → rain → wind. The status byte always stays. Missing fields simply don't appear in the decoded JSON for that uplink; the rain total since start is a counter, so a skipped value is caught up by the next uplink. The log and the status page (`/status`, LoRaWAN row) show what the last uplink left out. CayenneLPP generally drops more fields than binary — use binary unless your backend needs CayenneLPP.

New nodes start with the binary format; nodes that already have a stored setting keep it.

### CayenneLPP Channels

| Channel | Type | Content | Unit (Metric / Imperial) | Present when |
|---------|------|---------|--------------------------|--------------|
| Ch 1 | Direction | Wind direction | ° | wind on |
| Ch 1 | Analog Input | Average wind speed | m/s or km/h / mph | wind on |
| Ch 2 | Analog Input | Wind gust | m/s or km/h / mph | wind on |
| Ch 3 | Analog Input | Rain rate | mm/h / in/h | rain on |
| Ch 1 | Distance | Rain this cycle (0.001 resolution) | mm / in | rain on |
| Ch 2 | Distance | Rain since start (0.001 resolution) | mm / in | rain on |
| Ch 2 | Digital Input | Status bits (BME280, 1-Wire, send failures, wind unit, imperial) | bitfield | always |
| Ch 200 | Digital Input | Cycle counter | 0–255 | debug |
| Ch 201 | Temperature | CPU temperature | °C / °F | debug |
| Ch 202 | Digital Input | Send failure counter | 0–255 | debug |
| Ch 203 | Generic Sensor | Free heap | KiB | debug |
| Ch 204 | Generic Sensor | Uptime (0.1 h) | h | debug |
| Ch 1 | Voltage | LiPo voltage | V | LiPo on |
| Ch 1 | Rel. Humidity | Humidity (selected [source](wireless-sensors.md#value-sources)) | % | source delivers humidity |
| Ch 1 | Baro. Pressure | Pressure QNH (selected [source](wireless-sensors.md#value-sources): BME280 or wireless) | hPa / inHg ×10 | source delivers pressure |
| Ch 2 | Baro. Pressure | Pressure absolute (station pressure of the selected source) | hPa / inHg ×10 | source delivers pressure |
| Ch 1 | Temperature | Temperature (selected source: BME280 or wireless) | °C / °F | source delivers temperature |
| Ch 2–4 | Temperature | DS18B20 probe 0–2 | °C / °F | probe found |
| Ch 3 | Digital Input | I/O status bits (see below) | bitfield | any v2.0 function on |
| Ch 5 | Analog Input | AUX analog input | V | AUX analog on |
| Ch 10 / 11 | Generic Sensor | AUX pulses this cycle / since power-on | count | AUX pulse on |
| Ch 2 | Direction | 2nd wind vane | ° | 2nd wind sensor, mode 2 |
| Ch 6 / 7 | Analog Input | 2nd anemometer speed / gust | km/h, m/s or mph | 2nd wind sensor on |
| Ch 12 | Voltage | UV sensor terminal voltage | V | board UV source, valid voltage |
| Ch 13 | Analog Input | UV index (calibrated board index or wireless sensor) | index, resolution 0.01 | index available |
| Ch 14 | Digital Input | UV flags: bit 0 board voltage present, bit 1 index present | bitfield | UV group present |
| Ch 15 | Generic Sensor | Illuminance | lx | light source delivers |
| Ch 16 | Generic Sensor | Value sources: 2 bits per group (bits 0–1 temp/hum, 2–3 wind, 4–5 rain, 6–7 UV, 8–9 light, 10–11 pressure: 0 none, 1 board, 2 wireless), bit 15 wireless battery low. Older firmware sends 0 in bits 10–11 (decoded as `none`) | bitfield | wireless sensors on |
| Ch 20 / 21 | Digital Output | Heater / fan power | % | heater or fan not off |
| Ch 22 | Generic Sensor | Fan speed | rpm | tacho on |
| Ch 30 | Voltage | Victron battery voltage | V | VE.Direct data |
| Ch 30 | Analog Input | Victron battery current (negative = discharge) | A | VE.Direct data |
| Ch 31 | Analog Input | Victron load output current | A | MPPT reports it |
| Ch 31 | Voltage | Victron PV voltage | V | VE.Direct data |
| Ch 30 | Power | Victron PV power | W | VE.Direct data |
| Ch 30 | Energy | Victron yield today | kWh | VE.Direct data |
| Ch 30 | Percentage | Victron state of charge | % | BMV / SmartShunt |
| Ch 30 / 31 | Digital Input | Victron charge state (`CS`) / error code (`ERR`) | code | VE.Direct data |

- Rain uses the CayenneLPP *Distance* type, but the values are **mm (or in), not meters** — use the provided decoder.
- In imperial mode the pressure field carries inHg ×10 (0.01 inHg resolution); the decoder divides by 10 again.
- The decoder keeps the field names `bme280_pressure_qnh_hpa` / `bme280_pressure_abs_hpa` (and `bme280_temperature_c` …) for compatibility; the values come from the selected [pressure source](wireless-sensors.md#value-sources), which may be a wireless sensor (`sources.pressure`).
- All values are rounded and clamped to the field's range (never wrapped). Analog Input covers ±327.67.
- Sizes: minimal 7–15 bytes, full 62 bytes, full with debug 84 bytes. The v2.0 I/O block adds up to 62 bytes (every function on, all Victron fields), wireless sensors add up to 12 bytes (Ch 15 light, Ch 16 sources) — use the binary format on slow data rates.
- **I/O status bits** (Ch 3 / binary bit 20): bit 0 heater on, bit 1 fan on, bit 2 fan stalled, bit 3 battery lockout active, bit 4 VE.Direct on but no data. The decoder returns them as `io_status.*`; the Victron charge state additionally as text (`victron_charge_state_text`: off, fault, bulk, absorption, float, …).

### Compact Binary Format

Layout (version 4): `[version: 1 byte][presence mask: 1–6 bytes][fields...]`. The mask has a **variable length**: each byte carries 7 field bits (bit 0–6 = fields 7k…7k+6), bit 7 set = another mask byte follows; bytes after the highest present field are left out. Only fields whose bit is set are included, in bit order; values are big-endian, rounded and clamped. **A value that is not available takes no space** — e.g. a station with only wind, rain and a BME280 needs a 2-byte mask, a Victron charger without SOC sends no SOC field. The firmware always sends version 4.

| Bit | Field | Encoding | Present when |
|-----|-------|----------|--------------|
| 0 | Wind direction | `u16`, ° | wind on |
| 1 | Wind speed (avg) | `i16`, ×100 | wind on |
| 2 | Wind gust | `i16`, ×100 | wind on |
| 3 | Rain rate | `i16`, ×100, mm/h or in/h | rain on |
| 4 | Rain (cycle) | `u16`, ×10 mm / ×1000 in | rain on |
| 5 | Rain since start | `u32`, ×10 mm / ×1000 in | rain on |
| 6 | Status byte | `u8`, same bits as CayenneLPP | always |
| 7 | Battery voltage | `u16`, ×100, V | LiPo on |
| 8 | Humidity | `u8`, ×2, % | selected source delivers humidity |
| 9 | Pressure QNH | `u16`, ×10 hPa / ×100 inHg | pressure source delivers |
| 10 | Pressure absolute | `u16`, ×10 hPa / ×100 inHg | pressure source delivers |
| 11 | Temperature (selected source) | `i16`, ×10, °C or °F | source delivers temperature |
| 12–14 | DS18B20 probe 0–2 | `i16`, ×10, °C or °F | probe found |
| 15 | Cycle counter | `u8` | debug |
| 16 | CPU temperature | `i16`, ×10 | debug |
| 17 | Send failure counter | `u8` | debug |
| 18 | Free heap | `u16`, KiB | debug |
| 19 | Uptime | `u16`, ×10, hours | debug |
| 20 | I/O status | `u8`, bits as Ch 3 | any v2.0 function on |
| 21 | AUX analog | `u16`, ×1000, V | AUX analog on |
| 22 | AUX pulses (cycle) | `u16`, clamped at 65535 | AUX pulse on |
| 23 | AUX pulses total | `u32` | AUX pulse on |
| 24 | Heater / fan power | 2 × `u8`, % | heater or fan not off |
| 25 | Fan speed | `u16`, rpm | tacho on |
| 26 | Victron battery voltage (v1–v3: whole VE.Direct block, see below) | `u16`, ×100, V | VE.Direct data |
| 27 | Victron load output current | `i16`, ×100, A | MPPT reports it |
| 28 | 2nd wind vane direction | `u16`, ° | 2nd wind sensor, mode 2 |
| 29 / 30 | 2nd anemometer speed / gust | `i16`, ×100 | 2nd wind sensor on |
| 31 | UV sensor terminal voltage | `u16`, ×1000, V | board UV source, valid voltage |
| 32 | UV index (calibrated board index or wireless sensor) | `u16`, ×100 | index available |
| 33 | Value sources | `u16`, same bits as CayenneLPP Ch 16 | wireless sensors on |
| 34 | Illuminance | `u32`, lx | light source delivers |
| 35 | Victron battery current (negative = discharge) | `i16`, ×100, A | VE.Direct data |
| 36 / 37 | Victron PV voltage / PV power | `u16` ×100 V / `u16` W | VE.Direct data |
| 38 / 39 | Victron charge state (`CS`) / error code (`ERR`) | `u8` each | VE.Direct data |
| 40 | Victron yield today | `u16`, ×100, kWh | VE.Direct data |
| 41 | Victron state of charge | `u16`, ×10, % | BMV / SmartShunt |

Typical sizes: wind + rain + BME280 + 3 × DS18B20 + LiPo 34 bytes; everything incl. debug, the v2.0 I/O block, all Victron values, UV, light and sources at most 90 bytes (encoder buffer 96).

**Older versions** (firmware up to 2.0.0-beta.1), still read by the [decoder](../chirpstack/decoder.js): a fixed 4-byte little-endian mask; bit 26 was one 14-byte VE.Direct block (battery V/A, PV V/W, charge state, error, yield, SOC; missing values `0xFFFF` / `0x8000` / `0xFF`); **version 3** added a 5-byte UV group at bit 31 (voltage, index, flags), packets without UV stayed **version 2**. Update the decoder on ChirpStack/TTN **before** installing firmware that sends version 4.

> **Version 1** (firmware ≤ 1.0.4) used ×10 for imperial rain/pressure too (a 0.2 mm tip became 0) and `u16` for rain since start. The decoder still reads version 1; metric values are identical in both.

## Low-Power Mode (LoRa only)

For battery/solar installs without Wi-Fi: with **`lowPowerEn = 1`** (portal, web settings, downlink param 31) the node no longer idles between sub-samples but goes into **light sleep** until the next one. RAM, the LoRaWAN session and the chart history stay intact; the SX1262 sleeps between uplinks.

- **Wind speed and rain** wake the chip on every edge of the reed contacts and are counted in software while it sleeps (the hardware pulse counters are not clocked in light sleep). Works on every PCB revision.
- **Samples** (wind, gust, direction every wind sample interval; BME280 every 10 s) run as before. With 3-s wind sampling the node wakes less often.
- **Wind-vane supply** (PCB v2.0): with **`vanePowerSave = 1`** (default) the potentiometer is only powered for ~100 ms around each reading (the filter, τ ≈ 15 ms, settles in that time, spent in light sleep). Set it to `0` for vanes that need continuous power.
- **VE.Direct:** the node stays awake for the last ~2.5 s before each uplink to catch a frame.
- Only active while **Wi-Fi is off**, **wireless sensors are off** (the LoRa chip would have to keep listening) and neither heater nor fan is running (their PWM stops in light sleep). Wind, rain and AUX pulses (counter or 2nd anemometer) are counted while sleeping. The user button still wakes the node and turns the display on.
- The power saving still has to be measured on real hardware.
