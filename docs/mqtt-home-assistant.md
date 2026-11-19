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

**Connection:** broker host and port (`1883` plain, `8883` TLS), optional user/password, optional topic prefix (default: device name), and the security mode — `0` plain, `1` TLS without certificate check, `2` TLS verified against a CA certificate you paste in.

## MQTT Topics

| Topic | Direction | Content |
|-------|-----------|---------|
| `<prefix>/data` | node → broker | Measurement JSON, every MQTT interval (default: like the LoRaWAN interval); wind as configured (*interval* or *10 min*) |
| `<prefix>/cmd` | broker → node | Commands (only with MQTT commands enabled) |
| `<prefix>/status` | node → broker | `online` / `offline` (retained, via last will) |

**Measurement JSON** (example with wind + BME280). Fields only appear for sensors that are enabled/detected, and each value has a sibling `*Unit` field:

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
  "nextCycleInSec": 2,
  "cpuTemp": 41.8,
  "cpuTempUnit": "C",
  "resetReason": "power-on"
}
```

- `ds18b20` contains the current scanned probe slots, with `null` for a slot whose temperature read fails. Slots follow ROM-address search order, not physical cable order; removing/adding probes can change which physical probe occupies an index. The array is omitted when no probes are found.
- `nextCycleInSec`: seconds until the next measurement. In debug mode, `cycle`, `freeHeapKiB` and `uptimeH` are added.
- `cpuTemp` / `cpuTempUnit`: ESP32-S3 chip temperature, typically 10–20 K above ambient — for spotting an overheating enclosure, not an air temperature. `resetReason`: cause of the last restart (`power-on`, `software restart`, `crash (panic)`, `watchdog (task)`, `brownout (supply voltage dip)`, …).
- `status.pcbRev`: 12, 13 or 20 (v2.0).
- PCB v2.0, only while the function is enabled: `auxAnalogV`, `auxPulses` / `auxPulsesTotal`, `heater` (`mode`, `on`, `dutyPct`), `fan` (`mode`, `on`, `dutyPct`, with the tacho also `rpm`, `stalled`), `victron` (`valid`, `batteryV`, `batteryA`, `pvV`, `pvW`, `yieldTodayKWh`, `socPct`, `loadA`, `chargeState`, `error` — each only if the device sends it), and `status.batteryLowLockout` while the battery lockout holds the outputs off:

```json
{
  "heater": {"mode": "auto", "on": true, "dutyPct": 100},
  "fan": {"mode": "on", "on": true, "dutyPct": 100, "rpm": 2940, "stalled": false},
  "victron": {"valid": true, "batteryV": 13.28, "batteryA": 1.2, "pvV": 37.5, "pvW": 45,
              "yieldTodayKWh": 0.12, "chargeState": 3, "error": 0}
}
```

**Derived values** ([details](values.md#derived-values)), each only while its inputs exist: `dewPoint`, `feelsLike` (temperature unit), `cloudBase` + `cloudBaseUnit` (m / ft above the station), `windDirText` (`N` … `NNW`), `pressureTrend3h` (pressure unit), `pressureTendency` (`steady`, `rising slowly`, …), `weatherTrend` (`improving`, `worsening`, `storm warning`, `settled`, `unsettled`, `no change`), `tempTrend1h` (K, °F in imperial), `rain1h`, `rain24h`, `rainToday`, `rainYesterday`, `lastRain` (ISO 8601, UTC), `sunshineTodayMin` / `sunshineYesterdayMin`, `warnings` (`frost`, `fog`, `sultry`: true/false) and `firmwareUpdate` (a newer version, with the update check on).

This is a partial v2.0 example, not the complete telemetry document. UV adds `uvEnabled`, `uvIndexCalibrated`, `uvSensorVolts` and, when calibrated/enabled, `uvIndex`; wind 2 adds the `wind2` object. With wireless sensors on, `rfSensors`, `sources`, `radioBatteryLow` and — from the selected sources — `lightLux` and a wireless `uvIndex` are added; `temp` / `humidityPct` / wind / rain then carry the [selected source](wireless-sensors.md#value-sources).

**Commands** on `<prefix>/cmd`:

```json
{"cmd": "resetRain"}
```

Available: `resetRain`, `resetAuxPulses`, `restart`, `debugOn`, `debugOff`, `portal`, `factoryReset` — the same actions as the [LoRaWAN downlinks](lorawan.md#downlink-commands).

```json
{"params": [{"id": 1, "value": 300}]}
```

Sets parameters with the same IDs as [downlink `0x10`](settings.md#parameter-update-via-downlink-0x10) — e.g. heater on: `{"params": [{"id": 18, "value": 1}]}`, back to auto: value `2`.

## Home Assistant

With MQTT enabled, the node registers itself in Home Assistant automatically (MQTT discovery) — no YAML needed.

- All sensors appear under one device.
- The retained status/last will marks entities unavailable when the broker detects a disconnect; an abrupt power/network loss can take until the MQTT keepalive timeout.
- The 3 DS18B20 entities always exist; a disconnected probe shows as unavailable.
- PCB v2.0: AUX, UV voltage/calibrated index, wind 2, heater/fan power, fan speed and the Victron values (battery voltage/current, PV power, yield today, state of charge, load current) appear while their functions are enabled.
- With wireless sensors on, temperature, humidity, wind, rain, *UV Index* and *Illuminance* are always announced (they follow the [value sources](wireless-sensors.md#value-sources) and show as unavailable while no source delivers them).
- Derived values: *Dew Point*, *Feels Like*, *Cloud Base*, *Temperature Change (1 h)*, *Pressure Change (3 h)*, *Pressure Tendency*, *Weather Trend*, *Wind Direction (compass)*, *Rain Last Hour / Last 24 h / Today / Yesterday / This Week / This Month / This Year*, *Vapour Pressure Deficit*, *Last Rain* (timestamp), with wireless sensors *Sunshine Today / Yesterday*, and the warnings *Frost*, *Fog / Dew* and *Sultry* as binary sensors — ready for automations (e.g. a frost notification). *Firmware Update* with the update check on.
- If you switch a sensor on or off, the entity list updates on the next MQTT reconnect.
- The device ID (MQTT client ID, discovery `node_id`, `unique_id`) is the *device name* reduced to `A–Z a–z 0–9 _ -` (other characters become `_`); without a name `weathernode-<MAC>` (full 12-digit MAC). Since 2.0.0-beta.2 names with spaces or other characters, and unnamed nodes, get a new ID — Home Assistant then shows a new device; delete the old one.
- If the broker is unreachable, reconnects back off from 15 s to at most 5 min.
