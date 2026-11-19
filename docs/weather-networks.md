# Weather Networks

Uploads straight from the node to AWEKAS, Weather Underground, Weathercloud, Wetternetzwerk.pro, Windy and openSenseMap.

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

**What gets sent:** wind as **10-minute WMO values** — 10-min mean, highest 3-s gust and vector-mean direction of the last 10 minutes, independent of the upload interval, so the station compares with others (see [Measurement and Aggregation](values.md#measurement-and-aggregation)) — air pressure (sea level, BME280), humidity, temperature, **rain since midnight** and, for AWEKAS/Weathercloud, the rain rate. Also, where the network takes them: **dew point** (Weather Underground, Wetternetzwerk.pro, Weathercloud, Windy, openSenseMap), **rain of the last hour** (Weather Underground, Wetternetzwerk.pro, Windy, openSenseMap), wind chill / heat index (Weathercloud) and the **UV index** (Weather Underground, Weathercloud, Windy, openSenseMap). Wind, rain, temperature, humidity and UV come from the selected [value source](wireless-sensors.md#value-sources) (board sensor or a wireless sensor). Latitude/longitude are optional and only sent to AWEKAS.

**openSenseMap:** create a senseBox with the sensors you want and copy each sensor's ID into *openSenseMap sensor IDs* as `key=sensorId`, comma-separated, e.g. `temp=5f1c…,hum=5f1d…,press=5f1e…`. Keys and units: `temp` °C, `hum` %, `press` hPa (sea level), `wind` / `gust` m/s, `dir` °, `rain` mm (last hour), `rainday` mm (today), `dew` °C, `uv` index, `lux` lx. The access token is sent as the `Authorization` header (boxes with authentication, the default for new boxes).

**Good to know:**

- 🕐 **Needs the correct time.** The node gets it via NTP as soon as Wi-Fi is up (time zone and server are in Settings, default Central Europe). Until then it collects data but doesn't send.
- 🌧️ **Rain today** (and the other rain and sunshine statistics) survives restarts and firmware updates, and — with *Keep rain statistics over a power cut* (default on, see [Derived Values](values.md#derived-values)) — also a power cut, saved to flash at most once per hour.
- ⚡ **Never slows the station down.** Uploads run in the background; an unreachable server doesn't affect measuring or LoRaWAN.
- ⚠️ **Enable each network in only one place.** The same uploads also exist in the separate *weather-data-bridge* project (for LoRaWAN-only stations, fed via MQTT). If both send to the same station, every reading arrives twice.

*Logos above are simple illustrations made for this project, not the networks' official logos.*
