# MQTT and Home Assistant

Publishing the measurements to an MQTT broker (plain or TLS) and automatic discovery in Home Assistant: switches, topics, JSON fields and commands.

[← back to README](../README.md)

Wi-Fi and the web API are **on by default**, MQTT is off until you enter a broker. Five switches in the config portal and the web settings control them:

| Switch | Meaning |
|--------|---------|
| LoRaWAN enabled (`loraEn`) | Off by default; switch on for sites without Wi-Fi |
| Wi-Fi enabled (`wifiEn`) | Master switch for everything below |
| MQTT enabled (`mqttEn`) | Publish measurements to an MQTT broker |
| Web API enabled (`webApiEn`) | Web interface and REST API |
| MQTT commands (`mqttCtrlEn`) | Accept commands on the command topic |

Any combination works. If the network is down, the MQTT message is simply skipped — measuring and LoRaWAN carry on.

**Connection:** broker host and port (`1883` plain, `8883` TLS), optional user/password, optional topic prefix (default: device name), and the security mode — `0` plain, `1` TLS without certificate check, `2` TLS verified against a CA certificate you paste in (without a CA certificate the node does not connect in mode `2` — the status page shows *verified TLS: no CA certificate*; it never falls back to an unverified connection).

## MQTT Topics

| Topic | Direction | Content |
|-------|-----------|---------|
| `<prefix>/data` | node → broker | Measurement JSON, every MQTT interval (default: like the LoRaWAN interval); wind as configured (*interval* or *10 min*) |
| `<prefix>/rf/<custom ID>` | node → broker | One wireless sensor: a ★ sensor under its **custom ID** (e.g. `garden/rf/greenhouse`; the custom ID stays across a battery change, see [Wireless sensors](wireless-sensors.md#mine-received-hidden)), a sensor that is only received as `rf<radio ID>` (e.g. `garden/rf/rf9000342B` — never equal to a custom ID). Payload: its `rfSensors[]` object from the measurement JSON (★: with `customId`, `internalId`, `displayName`) plus `radioId`, right after each `<prefix>/data`, for every received sensor (up to 32), not retained |
| `<prefix>/cmd` | broker → node | Commands (only with MQTT commands enabled) |
| `<prefix>/status` | node → broker | `online` / `offline` (retained, via last will) |

**Measurement JSON** (example with wind + BME280). Fields only appear for sensors that are enabled/detected, and each value has a sibling `*Unit` field. It is the same document as [`/api/data`](api.md) — every field with type, unit and presence rule is in the [JSON Schema](api/schemas/data.schema.json):

```json
{
  "apiVersion": 1,
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
  "nextCycleInSec": 2,
  "cpuTemp": 41.8,
  "cpuTempUnit": "C",
  "resetReason": "power-on"
}
```

- `series` lists the defined [custom series](settings.md#custom-series) with their last cycle's `value` (fields as in [`/api/data`](api.md#fields-units-and-numbers)); absent without any.
- `ds18b20` contains the current scanned probe slots, with `null` for a slot whose temperature read fails. Slots follow ROM-address search order, not physical cable order; removing/adding probes can change which physical probe occupies an index. The array is omitted when no probes are found.
- `nextCycleInSec`: seconds until the next measurement. In debug mode, `cycle`, `freeHeapKiB` and `uptimeH` are added.
- `cpuTemp` / `cpuTempUnit`: ESP32-S3 chip temperature, typically 10–20 K above ambient — for spotting an overheating enclosure, not an air temperature. `resetReason`: cause of the last restart (`power-on`, `software restart`, `crash (panic)`, `watchdog (task)`, `brownout (supply voltage dip)`, …).
- `status.pcbRev`: 20 = V4 Full, 21 = V4 Light, 0 = Heltec V4 without PCB (wireless station).
- V4 Full, only while the function is enabled: `auxAnalogV`, `auxPulses` / `auxPulsesTotal`, `heater` (`mode`, `on`, `dutyPct`), `fan` (`mode`, `on`, `dutyPct`, with the tacho also `rpm`, `stalled`), `victron` (`valid`, `batteryV`, `batteryA`, `pvV`, `pvW`, `yieldTodayKWh`, `socPct`, `loadA`, `chargeState`, `error` — each only if the device sends it), and `status.batteryLowLockout` while the battery lockout holds the outputs off:

```json
{
  "heater": {"mode": "auto", "on": true, "dutyPct": 100},
  "fan": {"mode": "on", "on": true, "dutyPct": 100, "rpm": 2940, "stalled": false},
  "victron": {"valid": true, "batteryV": 13.28, "batteryA": 1.2, "pvV": 37.5, "pvW": 45,
              "yieldTodayKWh": 0.12, "chargeState": 3, "error": 0}
}
```

**Derived values** ([details](values.md#derived-values)), each only while its inputs exist: `dewPoint`, `feelsLike` (temperature unit), `cloudBase` + `cloudBaseUnit` (m / ft above the station), `windDirText` (`N` … `NNW`), `pressureTrend3h` (pressure unit), `pressureTendency` (`steady`, `rising slowly`, …), `weatherTrend` (`improving`, `worsening`, `storm warning`, `settled`, `unsettled`, `no change`), `tempTrend1h` (K, °F in imperial), `rain1h`, `rain24h`, `rainToday`, `rainYesterday`, `lastRain` (ISO 8601, UTC), `sunshineTodayMin` / `sunshineYesterdayMin`, `warnings` (`frost`, `fog`, `sultry`: true/false) and `firmwareUpdate` (a newer version, with the update check on).

This is a partial V4 Full example, not the complete telemetry document. UV adds `uvEnabled`, `uvIndexCalibrated`, `uvSensorVolts` and, when calibrated/enabled, `uvIndex`; wind 2 adds the `wind2` object. With wireless sensors on, `rfSensors`, `sources`, `radioBatteryLow` and — from the selected sources — `lightLux` and a wireless `uvIndex` are added; `temp` / `humidityPct` / wind / rain then carry the [selected source](wireless-sensors.md#value-sources).

**Commands** on `<prefix>/cmd`:

```json
{"cmd": "resetRain"}
```

Available: `resetRain`, `resetAuxPulses`, `restart`, `debugOn`, `debugOff`, `portal`, `factoryReset` (= [Reset settings](settings.md#reset), the node stays reachable) — the same actions as the [LoRaWAN downlinks](lorawan.md#downlink-commands).

```json
{"params": [{"id": 1, "value": 300}]}
```

Sets parameters with the same IDs as [downlink `0x10`](settings.md#parameter-update-via-downlink-0x10) — e.g. heater on: `{"params": [{"id": 18, "value": 1}]}`, back to auto: value `2`. An entry without `value` is refused (never applied as 0).

Send commands **without the retain flag**. A retained message on `<prefix>/cmd` is ignored: the broker would deliver it again at every reconnect (a retained `restart` would otherwise restart the node over and over). With *MQTT commands* off, nothing on the command topic is executed.

## Home Assistant

With MQTT enabled, the node registers itself in Home Assistant automatically (MQTT discovery) — no YAML needed.

- All sensors appear under one device.
- The retained status/last will marks entities unavailable when the broker detects a disconnect; an abrupt power/network loss can take until the MQTT keepalive timeout.
- The 3 DS18B20 entities always exist; a disconnected probe shows as unavailable.
- V4 Full: AUX, UV voltage/calibrated index, wind 2, heater/fan power, fan speed and the Victron values (battery voltage/current, PV power, yield today, state of charge, load current) appear while their functions are enabled.
- With wireless sensors on, the groups temperature, humidity, pressure, wind, rain, *UV Index* and *Illuminance* (with *Sunshine*) are announced **once a source has delivered them** — a temperature-only sensor gives no empty wind or rain entities; a sensor with more values announces its new groups within one cycle (the [value sources](wireless-sensors.md#value-sources) decide what counts). The node remembers the groups it has seen (also across restarts), so a silent sensor or a restart never removes entities and with them your names and areas in Home Assistant; a group goes only when its value source is set to *Off* (at once) or with *Reset settings*. The derived values follow their group (dew point, cloud base, fog and sultry warnings need humidity; pressure trend and weather trend need pressure).
- Derived values: *Dew Point*, *Feels Like*, *Cloud Base*, *Temperature Change (1 h)*, *Pressure Change (3 h)*, *Pressure Tendency*, *Weather Trend*, *Wind Direction (compass)*, *Rain Last Hour / Last 24 h / Today / Yesterday / This Week / This Month / This Year*, *Vapour Pressure Deficit*, *Last Rain* (timestamp), with wireless sensors *Sunshine Today / Yesterday*, and the warnings *Frost*, *Fog / Dew* and *Sultry* as binary sensors — ready for automations (e.g. a frost notification). *Firmware Update* with the update check on.
- Records: *Temperature Min Today*, *Temperature Max Today* (with a temperature source) and *Gust Max Today* (with a wind source) — today since local midnight; the full set (yesterday, month, year, all time) is in `GET /api/records` ([Records](values.md#records)).
- Sun and moon (only with latitude / longitude set): *Sunrise*, *Sunset*, *Moonrise*, *Moonset* (device class `timestamp`, today's event, unavailable on a day without it), *Moon Phase* (`new`, `waxingCrescent`, `firstQuarter`, `waxingGibbous`, `full`, `waningGibbous`, `lastQuarter`, `waningCrescent`) and *Moon Illumination* (%) — see [Sun and Moon](values.md#sun-and-moon).
- Custom series ([settings](settings.md#custom-series)): one sensor per defined series, named like the series, object id / `unique_id` `<device ID>_series_<slot>` (stable per slot, so renaming the series keeps the entity), unit and device class from the value (e.g. soil moisture `moisture` %, CO₂ `carbon_dioxide`, temperatures °C / °F), state class `measurement`. The value comes from the data topic's `series[]` (the last cycle's; lightning: strikes since the cycle before). Saving the series re-announces them at once; a freed slot's entity is removed (empty retained config).
- All values are the calibrated ones ([Calibration](settings.md#calibration)).
- If you switch a function on or off (wind, rain, LiPo, wireless sensors, V4 Full functions, update check, debug mode, latitude / longitude), the entity list updates on the next MQTT reconnect: entities of a switched-off function are removed (empty retained config). Entities of the BME280 stay while it is not detected (unavailable).
- Numeric entities have a state class (`measurement`; running sums such as *Rain Today / This Week / Month / Year*, *Rain Since Start*, *Sunshine Today*, *AUX Pulses Total* and *Victron Yield Today* `total_increasing`), so Home Assistant keeps long-term statistics for them; wind speeds have the device class `wind_speed`, the rain rate `precipitation_intensity`.
- Wireless sensors additionally get **their own devices** — see [below](#wireless-sensors-in-home-assistant).
- The device ID (MQTT client ID, discovery `node_id`, `unique_id`) is the *device name* reduced to `A–Z a–z 0–9 _ -` (other characters become `_`); without a name `weathernode-<MAC>` (full 12-digit MAC). Renaming the node gives a new ID — Home Assistant then shows a new device; delete the old one. The node's network hostname (Wi-Fi/DHCP and mDNS `<hostname>.local`) is a different, DNS-safe form of the name: lower case with `-` (see [Device name](settings.md#settings-reference)).
- If the broker is unreachable, reconnects back off from 15 s to at most 5 min. A connection that ends within 60 s counts as failed too (e.g. two nodes with the same device name, which share one client ID and kick each other off the broker), so it never turns into a reconnect storm; only a connection that stayed up for 60 s resets the backoff.

### Alarms

Every [alarm rule](alarms.md) is announced as a **binary sensor** *Alarm &lt;name&gt;* (device class `problem`, entity `binary_sensor.<device>_alarm_<n>`), fed from the retained topic `<prefix>/alarm/<n>` (`ON` / `OFF`). Renaming, adding or deleting a rule re-announces the entities (a deleted rule's entity is removed). Rules with the MQTT channel and *MQTT alarm events* on also publish each event to `<prefix>/alarm` (JSON, not retained) — e.g. for a Home Assistant automation that sends its own notification.

### Wireless sensors in Home Assistant

Every wireless sensor marked ★ **Mine** (up to 32, [Settings → Sensors](wireless-sensors.md#mine-received-hidden), or the ★ button on the status page) becomes **its own Home Assistant device**, e.g. *Greenhouse*, *WS90 9000342B*, *Bresser weather station 39582376* — linked to the node (*connected via*). Sensors that are only ○ received get **no** device, so a neighbour's station never shows up in Home Assistant (✕ hidden ones are not even stored); with no ★ sensor there are no per-sensor devices at all (the status page says so). The node's own entities (temperature, wind, rain … from the [value sources](wireless-sensors.md#value-sources)) are unchanged.

- **Entities only for what the sensor sends**, added as its values arrive: *Temperature*, *Humidity*, *Wind Speed*, *Wind Gust*, *Wind Direction*, *Rain Today* (from the counter's increments since local midnight, `total_increasing` — the reset at midnight starts a new cycle), *Rain Total* (the sensor's own counter, `total_increasing` — a battery change starts a new cycle), *UV Index*, *Illuminance*, *Solar Radiation* (Davis), *Soil Moisture* (WH51), *Lightning Strikes Today* (`total_increasing`), *Last Lightning* (timestamp), *Lightning Distance* and the diagnostic *Lightning Counter* (the WH57's raw 8-bit counter), *Pressure* (`atmospheric_pressure`, WN32P), *PM2.5*, *PM10*, *PM1*, *PM4* (µg/m³), *CO2* (ppm) (air quality sensors), *Leak* (binary sensor, device class `moisture`, on = water detected; WH55), *Leaf Wetness* (%, WN35) and *Soil Conductivity* (µS/cm, WH52); diagnostics *Battery* (binary sensor, on = low; radio sensors), *Signal Strength* (dBm, radio sensors), *Last Packet Age* (s), *Reception* (% of the expected packets in the last hour, radio sensors; unknown until learned; attribute `id_collision` = true while two transmitters send with this radio ID, the state is unknown then — see [Reception watchdog](wireless-sensors.md#reception-watchdog)) and *Supercap Voltage* (WS85/WS90, Davis Vue). Units follow the unit settings like the node's other entities.
- **Device name:** the sensor's name (sensor table), else *&lt;model&gt; &lt;custom ID&gt;*. A new name is announced within seconds.
- **State:** each entity reads the topic `<prefix>/rf/<custom ID>`. A sensor not heard for 5 minutes shows as unavailable (diagnostics stay available), a single value not received for 5 minutes (`stale`) as unknown; all of them go unavailable with the node (last will).
- **IDs:** built from the sensor's **internal ID** (`L` + 6 hex digits, shown in the sensor table; drawn once, never changes): device identifier `<device id>_rf_<internal ID>`, unique IDs `<device id>_rf_<internal ID>_<entity>` (`temp`, `humidity`, `wind_speed`, `wind_gust`, `wind_dir`, `rain_total`, `uv_index`, `light`, `solar`, `soil_moisture`, `lightning_count`, `lightning_distance`, `supercap`, `battery`, `rssi`, `age`, `pressure`, `pm25`, `pm10`, `pm1`, `pm4`, `co2`, `leak`, `leaf_wetness`, `conductivity`, `rain_today`, `lightning_today`, `last_lightning`, `distance`, `heat_count`, `reception`), config topics `homeassistant/sensor/<device id>/rf_<internal ID>_<entity>/config` (`binary_sensor` for the battery and the leak).
- **Custom ID change, battery change:** the entities stay — same unique IDs, same history. After a new **custom ID** the node announces every entity again with the new state topic (and clears the old topic with an empty retained message); after a **re-bind** to the new radio ID (Bresser, IT+, many Ecowitt sensors get one with every battery change) it announces them again for the new reading. Renaming works the same way.
- **Removing a sensor:** set it to ○ Received or ✕ Hidden — the node clears its retained discovery configs (empty retained payloads) and Home Assistant removes the device. The node remembers which sensors it announced (also across restarts and while MQTT is down), so this works whenever it is connected next. Taking a sensor ★ again later draws a new internal ID: a new device.
- **From a build before 4.0.0** (devices by radio ID, `…_rf_<ID>_…`): the ★ list starts empty after the update and the old devices are removed once at the first connection; the ★ sensors appear as new devices (new entity IDs, the old history stays with the removed entities).
- The configs go out 4 per second after connecting, so `loop()` is never blocked: a typical sensor has 4–12 entities (e.g. a WH31 5, a WS90 12), so 10 sensors appear within about 30 s; 32 sensors with 16 entities each (512 configs, more than any real sensor mix) take a little over 2 minutes; removing an ID clears all 31 possible configs.
