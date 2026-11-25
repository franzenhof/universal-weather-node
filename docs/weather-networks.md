# Weather Networks

Uploads straight from the node to AWEKAS, Weather Underground, Weathercloud, Wetternetzwerk.pro, Windy, openSenseMap, Met Office WOW and CWOP — and to any other HTTP service with a [custom upload](#custom-upload).

[← back to README](../README.md)

<!-- help:skip -->
<p>
  <a href="https://www.awekas.at"><img src="../pictures/badges/awekas.svg" alt="AWEKAS" height="40"></a>
  <a href="https://www.wunderground.com/pws/overview"><img src="../pictures/badges/wunderground.svg" alt="Weather Underground" height="40"></a>
  <a href="https://weathercloud.net"><img src="../pictures/badges/weathercloud.svg" alt="Weathercloud" height="40"></a>
  <a href="https://wetternetzwerk.pro"><img src="../pictures/badges/wetternetzwerk-pro.svg" alt="Wetternetzwerk.pro" height="40"></a>
</p>
<!-- /help:skip -->

Share your data with the big community weather networks — **straight from the node over Wi-Fi**, no extra server needed.

**Setup:**

1. Register your station with the network and note its station ID and key/password.
2. Open **Settings → Weather networks** on the node.
3. Enter the ID and key, click *Save*. That's it — the first upload follows after one interval.
4. Check **Status**: the network's dot turns 🟢 after the first accepted upload.

| Network | What you enter | Interval (default) |
|---------|----------------|--------------------|
| [AWEKAS](https://www.awekas.at) | User name + password | 300 s (minimum) |
| [Weather Underground](https://www.wunderground.com/pws/overview) | Station ID + station key | 300 s |
| [Weathercloud](https://weathercloud.net) | ID + key (register the device as type "Other") | 600 s (free accounts) |
| [Wetternetzwerk.pro](https://wetternetzwerk.pro) | Station ID + key | 60 s |
| [Windy](https://stations.windy.com) | Station ID + station password (*My stations*, API v2) | 300 s (minimum) |
| [openSenseMap](https://opensensemap.org) | senseBox ID, access token and the sensor IDs (see below) | 300 s |
| [Met Office WOW](https://wow.metoffice.gov.uk) | Site ID + authentication key (6-digit PIN) | 300 s (minimum) |
| [CWOP](http://www.wxqa.com) (APRS-IS) | CWOP ID (e.g. `CW1234`) or a ham callsign + APRS-IS passcode (`-1` for CWOP IDs); needs latitude / longitude | 600 s (minimum 300) |

**What gets sent:** wind as **10-minute WMO values** — 10-min mean, highest 3-s gust and vector-mean direction of the last 10 minutes, independent of the upload interval, so the station compares with others (see [Measurement and Aggregation](values.md#measurement-and-aggregation)) — air pressure (sea level, from the selected pressure source), humidity, temperature, **rain since midnight** and, for AWEKAS/Weathercloud, the rain rate. Also, where the network takes them: **dew point** (Weather Underground, Wetternetzwerk.pro, Weathercloud, Windy, openSenseMap), **rain of the last hour** (Weather Underground, Wetternetzwerk.pro, Windy, openSenseMap), wind chill / heat index (Weathercloud) the **UV index** (AWEKAS, Weather Underground, Weathercloud, Windy, openSenseMap), and for AWEKAS also brightness, sunshine hours, solar radiation and the pressure tendency (table below). Wind, rain, temperature, humidity and UV come from the selected [value source](wireless-sensors.md#value-sources) (board sensor or a wireless sensor). Latitude/longitude are optional and only sent to AWEKAS.

**AWEKAS fields** (upload API `eingabe_pruefung.php`, one `val` parameter with 25 semicolon-separated positions; empty = not available):

| # | Field | Sent |
|---|-------|------|
| 1–4 | user, password (MD5), date, time | always (local date/time) |
| 5–8 | temperature °C, humidity %, air pressure hPa (sea level), rain today mm | from the selected [value sources](wireless-sensors.md#value-sources) |
| 9, 10, 16 | wind km/h, direction °, gust km/h | 10-min WMO values |
| 11–13 | weather condition, warning text, snow height | empty |
| 14 | language | `en` |
| 15 | pressure tendency (−2 strongly falling … 2 strongly rising) | from the 3-h tendency: *steady* 0, *slowly* / normal ±1, *quickly* / *very rapidly* ±2 (needs 3 h of pressure history) |
| 17 | solar radiation W/m² | only a measured value: a Davis solar radiation sensor as the light source |
| 18 | UV index | UV source (wireless or calibrated board UV) |
| 19 | brightness lux | light source (not for the Davis solar sensor, whose lux is only an approximation) |
| 20 | sunshine hours today | the node's sunshine duration (needs a light source and the clock) |
| 21 | soil temperature | empty |
| 22 | rain rate mm/h | rain source |
| 23–25 | software, longitude, latitude | firmware version; position if set |

**Met Office WOW** ([data formats](https://wow.metoffice.gov.uk/support/dataformats)): `GET http://wow.metoffice.gov.uk/automaticreading` with `siteid`, `siteAuthenticationKey`, `dateutc` (UTC), `softwaretype` and WU-style imperial values: `tempf`, `humidity`, `dewptf`, `baromin` (QNH), `windspeedmph`, `windgustmph`, `winddir` (10-min WMO wind), `dailyrainin`. Success = HTTP 200. `rainin` ("rain since the previous observation") is not sent. *WOW has announced that it will close and no longer accepts new registrations.*

**CWOP** (Citizen Weather Observer Program): one APRS weather report per interval over a short TCP session to `cwop.aprs.net:14580` — login `user CW1234 pass -1 vers UWN <version>`, then the packet, then close. Format (APRS 1.01 complete weather report with position and UTC timestamp, path `TCPIP*`):

```
CW1234>APRS,TCPIP*:@201230z4748.00N/01302.40E_220/004g010t045r000p012P005h85b10132UWN
```

`_ddd/sss` wind direction / 10-min mean (mph), `g` gust (mph), `t` temperature (°F, `t-05` below 0 °F), `r` / `p` / `P` rain of the last hour / 24 h / since local midnight (hundredths of an inch), `h` humidity (`00` = 100 %), `b` pressure (QNH, tenths of hPa); a missing wind or temperature is sent as dots, other missing groups are left out. Accepted when the server answers the login (`# logresp`). The position is the node's latitude / longitude (rounded to 0.01 minute, about 20 m).

**Not supported** (no publicly documented upload API that could be verified): **PWSWeather** (said to accept the Weather Underground protocol, but there is no official specification), **Windguru** (its upload API page is no longer reachable) and **Windfinder** (uploads only through partner hardware such as Meteobridge). A [custom upload](#custom-upload) can reach any of them whose protocol you know.

**openSenseMap:** create a senseBox with the sensors you want and copy each sensor's ID into *openSenseMap sensor IDs* as `key=sensorId`, comma-separated, e.g. `temp=5f1c…,hum=5f1d…,press=5f1e…`. Keys and units: `temp` °C, `hum` %, `press` hPa (sea level), `wind` / `gust` m/s, `dir` °, `rain` mm (last hour), `rainday` mm (today), `dew` °C, `uv` index, `lux` lx. The access token is sent as the `Authorization` header (boxes with authentication, the default for new boxes).

**Good to know:**

- 🕐 **Needs the correct time.** The node gets it via NTP as soon as Wi-Fi is up (time zone and server are in Settings, default Central Europe). Until then it collects data but doesn't send.
- 🌧️ **Rain today** (and the other rain and sunshine statistics) survives restarts and firmware updates, and — with *Keep rain statistics over a power cut* (default on, see [Derived Values](values.md#derived-values)) — also a power cut, saved to flash at most once per hour.
- ⚡ **Never slows the station down.** Uploads run in the background; an unreachable server doesn't affect measuring or LoRaWAN.
- ⚠️ **Enable each network in only one place.** The same uploads also exist in the separate *weather-data-bridge* project (for LoRaWAN-only stations, fed via MQTT). If both send to the same station, every reading arrives twice.

## Custom Upload

Up to **two custom uploads** (*Settings → Outputs → Custom uploads*) send to any HTTP(S) service: a **URL template**, **GET** or **POST** with an optional **body template**, and an **interval** (60–3600 s). The values are inserted from [placeholders](#placeholders); a body that starts with `{` or `[` is sent as `application/json` with the values JSON-escaped, any other body as `text/plain`. Example (a Weather Underground-style server):

```
https://example.org/update?ID=MYSTATION&PASSWORD=secret&dateutc={time_utc}&tempf={temp:f}&humidity={hum}&baromin={qnh:inhg}&windspeedmph={wind:mph}&windgustmph={gust:mph}&winddir={dir}&dailyrainin={rain_day:in}
```

A placeholder whose value is not available (no source, stale sensor) is left empty. Wind is the **10-minute WMO wind** like for the networks above; the first upload follows one interval after enabling or a restart; success = HTTP 2xx. The status page lists each custom upload with its last result (also `/api/info` `outputs.status.custom1` / `custom2`). The URL may carry a key: it is treated like a password in the [settings backup](settings.md#backup-and-restore) and never logged.

### Placeholders

`{name}` or `{name:suffix}`; the suffixes are a **unit** and/or **decimals** `.0`–`.4` in any order, e.g. `{temp:f}`, `{wind:kmh:.0}`, `{temp:.2}`. Without a unit suffix the value is metric. Values are URL-encoded in the URL and JSON-escaped in a JSON body; braces that are not a placeholder (`{"a":1}`) stay as they are; unknown names render as nothing.

| Placeholder | Value | Unit suffixes (default first) | Default decimals |
|-------------|-------|-------------------------------|------------------|
| `{temp}`, `{dew}`, `{feels}` | air temperature, dew point, feels like | `c`, `f` | 1 |
| `{hum}` | relative humidity % | – | 0 |
| `{qnh}`, `{press}` | sea-level pressure (QNH), station pressure | `hpa`, `inhg` (2 decimals) | 1 |
| `{ptend3h}` | pressure change in 3 h | `hpa`, `inhg` | 1 |
| `{wind}`, `{gust}` | 10-min mean wind, highest 3-s gust | `ms`, `kmh`, `mph`, `kn` | 1 |
| `{dir}` | wind direction ° | – | 0 |
| `{rain_day}`, `{rain_1h}`, `{rain_24h}` | rain today (local midnight), last hour, last 24 h | `mm`, `in` (2 decimals) | 1 |
| `{rain_rate}` | rain rate | `mm` (mm/h), `in` (in/h) | 1 |
| `{uv}`, `{lux}`, `{solar}` | UV index, illuminance lx, solar radiation W/m² (Davis) | – | 1, 0, 0 |
| `{time_utc}`, `{time_local}` | `YYYY-MM-DD hh:mm:ss` (UTC / local) | – | – |
| `{epoch}` | Unix time (s) | – | – |
| `{station}`, `{lat}`, `{lon}` | device name, latitude, longitude (as entered) | – | – |
| `{rfN_temp}`, `{rfN_hum}`, `{rfN_wind}`, `{rfN_gust}`, `{rfN_dir}`, `{rfN_rain}`, `{rfN_uv}`, `{rfN_lux}`, `{rfN_batt}` | wireless sensor *N* (1–32, the number on the status page, or its 8-digit ID, e.g. `{rf300007DD_temp}` — keeps meaning that sensor when the list changes): temperature, humidity, wind, gust, direction, rain total (the sensor's counter), UV, lux, battery (1 = OK) | as above | as above |

The [alarm messages and webhook](alarms.md#messages-and-placeholders) use the same placeholders plus the rule's own.

*Logos above are simple illustrations made for this project, not the networks' official logos.*
