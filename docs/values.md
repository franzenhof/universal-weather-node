# Measurement, Derived Values and Units

How the node measures and aggregates each value, what it derives from them, which units it uses and how it checks for new firmware.

[← back to README](../README.md)

## Measurement Principles

**Wind speed:** the anemometer's reed switch closes once per revolution; the ESP32's hardware pulse counter counts without CPU load. Davis: 1 revolution/s = 2.25 mph = 3.62 km/h. The **gust** is the highest **3-second mean** (WMO) — see [Measurement and Aggregation](#measurement-and-aggregation).

**Wind direction:** the Davis vane is a 20 kΩ potentiometer; the voltage (0–3.3 V) maps linearly to 0–359°, plus the north offset. A 10 kΩ / 1 µF filter (τ on the order of 15 ms including the vane resistance) reduces noise; component references differ between PCB revisions. The cycle value is a **vector average** (sin/cos), so 350° and 10° average to 0°, not 180°.

```
raw (0–4095) → map to 0–359° → + offset → mod 360
```

**Rain:** one pulse per bucket tip (Davis: 0.2 mm).

```
Rain [mm]        = pulses × rainMmPerTip
Rain rate [mm/h] = rain [mm] / (cycle length [ms] / 3,600,000)
```

"Rain since start" survives restarts, watchdog resets and OTA updates; only a power cut or downlink `0x01` resets it.

**Temperature probes (DS18B20):** up to 3 probes, found automatically every cycle (hot-pluggable). The order follows their ROM addresses. A conversion that is still incomplete after 1 s is aborted: probe readings are marked unavailable, the switched bus supply is turned off on Rev 1.3/v2.0, and the next cycle searches/retries normally. A stuck bus cannot indefinitely stop all other measurements or telemetry.

**Air pressure (BME280):** converted to sea level (QNH) using the configured altitude:

```
QNH = absolute pressure × (1 - 0.0000226 × altitude[m])^(-5.255)
```

The BME280 is **hot-pluggable**: while missing it is re-probed every sub-sample and re-initialized as soon as it answers.

## Measurement and Aggregation

The node samples once and aggregates **separately for every transmission path**, so LoRaWAN, MQTT and the weather networks can use different intervals without disturbing each other.

**Sampling and resolution per sensor**

| Sensor | Sampled | Resolution | Notes |
|---|---|---|---|
| Wind speed | every **wind sample interval** (1 s or 3 s): pulses counted by hardware since the last sample | Davis: 1 pulse/s = 1.006 m/s; over a 3-s gust window 0.34 m/s | pulses are never lost between samples; means are pulse-weighted (pulses / time) |
| Wind direction | with every wind sample | 1° (pot. vane, 12-bit ADC averaged) or the calibrated table steps | vector average (sin/cos): 350° and 10° → 0° |
| Rain | pulses counted continuously, read with every wind sample | 1 tip (Davis 0.2 mm) | one running total (`rainSinceStart`); each path reports the difference since its own last send |
| BME280 (temperature, humidity, pressure) | every 10 s, at least once per base cycle | 0.01 °C / 0.008 %rH / 0.18 Pa (sensor) | average of the readings in the base cycle |
| DS18B20 | once per base cycle (~750 ms conversion) | 0.0625 °C | |
| UV, AUX analog, 2nd anemometer (PCB v2.0) | with every wind sample | 12-bit ADC | average / gust per base cycle |
| Battery, CPU temperature | once per base cycle | | |

**Base cycle:** the shortest active send interval — the LoRaWAN interval, or the MQTT interval if MQTT is on and faster. At its end the status page, REST API (`/api/data`), display and dashboard history get the base cycle's values, and every path that is due sends.

**Gust:** the highest 3-second mean (WMO-No. 8). With 1-s sampling that is the pulses of the last three samples, with 3-s sampling one sample. A single 1-s spike is therefore averaged out, as at official stations. 3-s sampling uses fixed, non-overlapping windows: slightly fewer wake-ups (low-power mode), but a peak that straddles two windows reads a little lower. Faster sampling than 1 s does not improve the 3-s gust; at 0.5 s one pulse would already be 2 m/s.

**Aggregation per path**

| Path | Interval | Wind speed / gust / direction | Rain | BME280, DS18B20, other |
|---|---|---|---|---|
| LoRaWAN | LoRaWAN interval (stretched for 1 % airtime) | *Interval*: mean, highest 3-s gust and vector-mean direction since the last uplink — or *10 min* (setting) | since the last uplink; rate = amount / window | latest base-cycle values |
| MQTT | MQTT interval | *Interval* since the last publish — or *10 min* (setting) | since the last publish | latest base-cycle values |
| Weather networks | each network's upload interval | always **10-min WMO values**: 10-min mean, highest 3-s gust and vector-mean direction of the last 10 minutes | rate averaged over the upload window, daily total | averaged over the upload window |
| Status page, REST, dashboard history | base cycle | base cycle (interval) | since the last base cycle | base cycle |

- **Interval** is the right choice when the data is aggregated further downstream (e.g. Grafana computing 10-minute means: average the means, take the `max()` of the gusts, vector-average the direction — and use only one path per measurement, LoRaWAN *or* MQTT).
- **10 min** delivers ready-to-use values comparable with official stations, for receivers without their own aggregation (e.g. Home Assistant). Do **not** average these again downstream — that smears the means and counts gusts several times.
- The wind ring keeps the last 10 minutes of wind samples (about 4 KB). After a restart the 10-min values cover the buffered span only.
- **Timing:** the node only waits the remaining time of each interval, so measuring and sending don't add up. The minimum configurable LoRaWAN interval (5 s) is not a legal transmit interval: the node stretches it to keep **1 % airtime** at the current data rate (SF7, 27 bytes: ~7 s; SF12: ~150 s; shown on the status page). On TTN also apply the [daily fair-use budget](lorawan.md#the-things-network-ttn).

## Derived Values

Computed on the node from the measured values — the same numbers in JSON/MQTT, Home Assistant, on the status page, the dashboard and (where supported) the weather networks. LoRaWAN carries no extra bytes: the [decoder](../chirpstack/decoder.js) computes dew point, feels like, cloud base, the compass text and the warnings from the decoded values itself (`dew_point`, `feels_like`, `cloud_base_m`, `wind_direction_text`, `warnings`).

| Value | How | Needs |
|-------|-----|-------|
| Dew point | Magnus formula (17.62 / 243.12 °C) | temperature + humidity |
| Feels like | wind chill (JAG/TI, at ≤ 10 °C and wind > 4.8 km/h), else heat index (NWS, from 26.7 °C), else the air temperature | temperature (+ wind / humidity) |
| Cloud base | 125 m per K of temperature − dew point spread, above the station (cumulus estimate) | temperature + humidity |
| Pressure tendency (3 h) | QNH now minus 3 h ago: *steady* (< 0.1 hPa), *slowly* (< 1.6), normal (< 3.6), *quickly* (< 6), *very rapidly* | BME280, 3 h of history after a start |
| Weather trend | barometer rule of thumb: *storm warning* (falling ≥ 6 hPa / 3 h), *worsening* (≥ 1.6), *improving* (rising ≥ 1.6), else *settled* (QNH ≥ 1020), *unsettled* (< 1000) or *no change* — not a forecast model | as above |
| Temperature trend (1 h) | now minus 1 h ago | 1 h of history (shown after about 1 h, the pressure tendency after about 3 h) |
| Rain last hour / 24 h | sum of the 5-min history buckets that lie completely inside the window, plus the current one — never longer than 1 h / 24 h, at most one bucket shorter (since start if shorter) | rain source |
| Rain this week / month / year | ISO week (Monday start), calendar month and year, local time (NTP); kept like the daily values below | rain source, time |
| Rain today / yesterday, last rain | local days (NTP); kept in RTC RAM and — with *Keep rain statistics over a power cut* (default on) — saved to flash at most once per hour and before every planned restart or update, so updates lose nothing and a power cut at most the last hour (after a cut of two or more days *yesterday* is unknown); rain before the first time sync after a restart counts as today | rain source, time |
| Sunshine today / yesterday | time with the selected light source ≥ 15 000 lx — an estimate of the WMO 120 W/m² threshold; a lux sensor sees global, not direct radiation | Bresser 7-in-1 (light) |
| Vapour pressure deficit (VPD) | saturation minus actual vapour pressure (Magnus), kPa — for plants / greenhouses | temperature + humidity |
| Warnings | *frost* (T ≤ 0 °C), *fog/dew* (spread ≤ 1 K and RH ≥ 95 %), *sultry* (dew point ≥ 16 °C) | temperature (+ humidity) |

Longer statistics (summer/frost days, records, multi-year series) are a job for your database — e.g. InfluxDB/Grafana or Home Assistant's long-term statistics fed by MQTT; the node keeps only 7 days in RAM.

## Units (Metric/Imperial)

One switch, **Units**, sets the unit for everything the node sends and shows:

| Units | Wind | Rain | Temperature | Pressure |
|-------|------|------|-------------|----------|
| Metric | km/h or m/s | mm, mm/h | °C | hPa |
| Imperial | mph | in, in/h | °F | inHg |

- Wind direction, humidity and battery voltage are always °, % and V.
- **LoRaWAN:** values are converted before sending; a status bit tells the decoder which units are in use. Imperial rain and pressure use a finer resolution on the wire.
- **MQTT/REST:** field names stay the same; the `*Unit` fields tell you the unit.
- **Home Assistant:** units update on the next MQTT reconnect.
- The dashboard has its own Metric/Imperial toggle that only affects your browser.
- Internally the node always calculates in m/s, mm, °C and hPa; the weather networks always get the units their protocol expects.

## Firmware Update Check

With *Check daily for newer firmware* on (web settings, *Firmware update check*; off by default), the node asks a version list once a day — by default this project's Gitea tags API, any Gitea `…/tags` or GitHub `…/releases` URL works — and shows a newer version on the status page, as `firmwareUpdate` in JSON and in Home Assistant. Installing stays manual ([OTA](installation.md#firmware-update-ota)). Pre-releases count: `2.0.0-beta.3` is newer than `2.0.0-beta.2`, `2.0.0` newer than any `2.0.0-beta` (tags compared case-insensitively, a `-dirty` build counts as the release). GitHub release lists are read with `per_page=5` and streamed, so long release notes don't exhaust the heap.
