# Loxone

Feeding a Loxone Miniserver by UDP push or HTTP polling.

[← back to README](../README.md)

The node can feed a **Loxone Miniserver** in two ways — no LoxBerry needed.

## Virtual UDP Input (recommended)

The node pushes every value as its own UDP datagram `name=value` (ASCII, `.` as decimal separator, booleans as `0`/`1`) to the Miniserver after each Loxone interval.

1. **Node** (web settings, section *Loxone (UDP)*): switch on, enter the Miniserver's IP (or host name), the UDP port (default **7000**), optionally a **prefix** (e.g. `g75_`, to tell several nodes apart on one port) and the interval (`0` = every measurement cycle, else 2–3600 s).
2. **Loxone Config:** add a *Virtual UDP Input* with the same port (sender address = the node's IP), and below it one *Virtual UDP Input Command* per value with the command recognition `temp=\v` — or `g75_temp=\v` with a prefix.

The names are exactly the keys of [`/api/data`](web-dashboard.md#rest-api-endpoints) (units follow the unit setting):

| Group | Names |
|-------|-------|
| Weather | `temp`, `humidityPct`, `pressureAbs`, `pressureQnh`, `windSpeed`, `windGust`, `windDirDeg`, `rainRate`, `rain` (amount since the last Loxone send), `rainSinceStart`, `uvIndex`, `lightLux` |
| Derived | `dewPoint`, `feelsLike`, `cloudBase`, `vpd`, `pressureTrend3h`, `tempTrend1h`, `rain1h`, `rain24h`, `rainToday`, `rainYesterday`, `rainWeek`, `rainMonth`, `rainYear`, `sunshineTodayMin`, `sunshineYesterdayMin`, `warnFrost`, `warnFog`, `warnSultry`, `tempMinToday`, `tempMaxToday`, `gustMaxToday` |
| Sun and moon (with latitude / longitude, [Sun and Moon](values.md#sun-and-moon)) | `astroSunrise`, `astroSunset`, `astroCivilDawn`, `astroCivilDusk`, `astroSolarNoon`, `astroMoonrise`, `astroMoonset`, `astroMoonTransit` as **minutes since local midnight** (392 = 06:32; not sent on a day without the event), `astroDayLengthMin`, `astroSunElevation` (°), `astroClearSkyWm2`, `astroMoonIlluminationPct`, `astroMoonPhaseIndex` (0 new … 4 full … 7 waning crescent) |
| Wireless sensors (S = a ★ sensor's **custom ID**, e.g. `greenhouse` — it stays across a battery change and does not move when the ★ order changes; a sensor that is only received: `rf` + its radio ID, e.g. `rf17000023` — never equal to a custom ID) | `S_temp`, `S_humidity`, `S_battery` (1 = OK), `S_rssi`, `S_age`, and as far as the sensor sends them `S_rainToday`, `S_rain1h`, `S_lightningToday`, `S_lightning1h`, `S_batteryV`, `S_windSpeed`, `S_windGust`, `S_windDirDeg`, `S_rainTotal`, `S_uvIndex`, `S_lightLux`, `S_soilMoisturePct`, `S_supercapV`, `S_lightningCount`, `S_lightningDistKm`, `S_pressure`, `S_pm25`, `S_pm10`, `S_pm10Estimated` (1), `S_pm1`, `S_pm4`, `S_co2Ppm`, `S_externalPower` (1), `S_leak` (1 = water), `S_leakHighSensitivity`, `S_leakRaw`, `S_leafWetnessPct`, `S_conductivityUsCm`, `S_mine` (1 = ★); `radioBatteryLow`. Custom IDs consist of `a-z A-Z 0-9 _ -`. Changing a custom ID in the sensor table renames that sensor's inputs. Up to 3.x the names were `rfN_…` (N = position in the sensor list) — after the update to 4.0.0 rename the Loxone inputs once (`rf1_temp` → `<custom ID>_temp`) |
| PCB / device | `ds18b20_1`…, `batteryVolt`, `uvSensorVolts`, `auxAnalogV`, `auxPulses`, `auxPulsesTotal`, `wind2Speed`, `wind2Gust`, `wind2DirDeg`, `heaterOn`, `heaterDutyPct`, `fanOn`, `fanDutyPct`, `fanRpm`, `fanStalled`, `victron…` (BatteryV, BatteryA, PvV, PvW, YieldTodayKWh, SocPct, LoadA, ChargeState, Error, Valid), `cpuTemp`, `nextCycleInSec` |

- A value that is currently not valid (sensor missing, stale wireless value) is **not sent** — Loxone keeps its last value.
- Wind in the Loxone send is mean and gust since the last Loxone send (own aggregation window, like MQTT).
- Every received wireless sensor (up to 32) is sent, about 5–12 datagrams each — mark your sensors ★ *Mine* so they are sent under their custom ID (stable across a battery change), and hide (✕) the neighbours' sensors or switch on *Only my sensors* so they are not sent ([Mine, Received, Hidden](wireless-sensors.md#mine-received-hidden)).
- The status page shows *Loxone UDP* under *Connections* (target, last send, values, errors).
- If a command picks up the wrong value (e.g. `temp=\v` also reacting to `garden_temp=`), set a prefix — every name then starts with it.
- *Not yet tested with a real Miniserver.*

## Virtual HTTP Input (works with any version)

Alternatively the Miniserver polls the node: *Virtual HTTP Input* with the URL `http://<node-ip>/api/data` and a polling interval (e.g. 60 s), and one *Virtual HTTP Input Command* per value with the recognition `\i"temp":\i\v`, `\i"windGust":\i\v`, `\i"rainToday":\i\v` … (any key of [`/api/data`](web-dashboard.md#rest-api-endpoints)). The top-level values come before the wireless-sensor list in the JSON, so `\i"temp":\i\v` always finds the station temperature.
