# Network Sensors

*Settings → Sensors → Network sensors* (shown with *Show advanced options*). Firmware 4.x, Heltec V4.

A network sensor takes values that arrive **over the network** — an MQTT topic or a URL that returns JSON — and puts them into the sensor table next to the wireless sensors. From there they work like any other sensor: value sources, ★ / custom ID / display name, placeholders, history, dashboard, `/api/data`, MQTT out, Home Assistant, weather networks, LoRaWAN and [virtual sensors](virtual-sensors.md).

The node stays a measuring station and works fully without them. Network sensors are off by default and are a normal, opt-in source: what one of them feeds is your choice, made in the value sources like for a wireless sensor. Two things are built in:

- **Visible origin:** the protocol is shown as **MQTT** or **REST** instead of a radio protocol everywhere the sensor appears (status page, settings sensor table, value-source select, dashboard sensor list and sensor page, `rfSensors[].protocol` in the API, the Home Assistant device model).
- **Staleness:** every network sensor has a freshness window (default **10 min**, 1–1440 min). A value that has not been refreshed within it is **stale**, like a wireless sensor that went silent: it is no longer used as a source and shows as stale. A dropped VPN, broker or server therefore never freezes an old value as current.

## Use case

A second station of your own, 200 km away and far too far for LoRa, connected over a VPN: its node publishes `<prefix>/data` to your broker (or its `/api/data` is reachable). On the home node, network sensor 1 = MQTT, format *Universal Weather Node*, topic `rannungen/data` — and the remote station shows on the home dashboard with its own name, history and Home Assistant device.

Other examples: a Zigbee or ESPHome sensor that already publishes to your broker, a sensor of a home automation system with a REST API, a public weather API for comparison.

## Settings

Up to **4** network sensors. Per sensor:

| Setting | Meaning |
|---|---|
| Source | off / **MQTT** / **REST (HTTP GET)** |
| Format | **JSON, a key per value** · **Plain number, a topic per value** (MQTT only) · **Universal Weather Node** (preset) |
| Topic / URL | MQTT: the exact topic (no `+` / `#`). Plain numbers: the part in front of each value's topic (may be empty). REST: an `http://` or `https://` URL |
| Fresh for (min) | 1–1440, default 10: longer than the send / poll interval of the source |
| Poll every (s) | REST only, 30–3600, default 60 |
| Temperature, humidity, pressure, wind speed, gust, wind direction, rain (running total), UV index, illuminance | JSON: the key, a dotted path for nested keys (`temp`, `main.temperature`, `list.0.main.temp` — a number indexes an array). Plain numbers: the rest of the topic (no `+` / `#`). Empty = not taken. Not shown for the preset |

The ID is fixed per slot: **E1000001 … E1000004**. Switching a slot on makes it a **★ sensor** at once (value sources, custom ID, Home Assistant device, *only my sensors*); give it a display name and custom ID in the sensor table as usual. Settings backup: like every setting — a token in a REST URL goes into the backup too.

**Units:** °C, %, hPa (station pressure), m/s, ° and mm. **Rain is a running total** (like a rain gauge's counter); the node counts the increase, a smaller total counts as a restart of the counter. The *Universal Weather Node* preset reads the other node's unit fields and converts imperial, km/h and inHg itself.

**Wind** needs the mean speed. Plain topics (one value per message): a gust or direction is kept for the next speed value. JSON, REST and the preset: each message stands alone — a message without a gust has the gust = mean speed, one without a direction has 0°; nothing is carried over from an earlier message.

**Time stamp:** the time the message or reply arrived. A **retained** MQTT message counts as received when the node subscribes (after every reconnect) — publish without retain, or the value looks fresh for one freshness window after a reconnect.

## MQTT

- Needs the **MQTT output** to be on (*Settings → Outputs → MQTT*): the node subscribes on the same broker. The status page says so when it is off.
- **JSON:** one topic, the values as keys of one JSON object. Only the mapped keys are kept while parsing, so a large message costs little memory; messages up to 8 KB (the buffer grows to that while an MQTT network sensor is on; it is taken from the PSRAM).
- **Plain number:** one topic per value, the payload just a number (`21.4`). Topic = *Topic / URL* + the value's key, e.g. prefix `esphome/garden/` + `temperature/state`.
- **Universal Weather Node:** the `<prefix>/data` topic of another node of this firmware. Taken: `temp`, `humidityPct`, `pressureAbs` (station pressure of the other station), `windSpeed`, `windGust`, `windDirDeg`, `rainSinceStart`, `uvIndex`, `lightLux` with `tempUnit`, `windUnit`, `rainUnit`, `pressureUnit`. That node's MQTT interval (*Settings → Outputs*, default = the LoRaWAN interval, 5 min) must be shorter than the freshness window.

## REST

- The node fetches the URL every *Poll every* seconds (first right after switching on) in its upload task — never in the measurement loop — with the same HTTP client and limits as the weather-network uploads (connect and reply time-outs of a few seconds). `https://` is encrypted but not verified (like the uploads) and starts only with enough free memory (it waits otherwise).
- The reply must be **HTTP 200** with a JSON body; it is parsed while it streams in, kept to the mapped keys. A non-200 reply, invalid JSON or a reply without any mapped value is counted as failed and shown on the status page (`HTTP 404`, *no valid JSON*, *no mapped value*); the values stay as they were and go stale after the freshness window.
- **Universal Weather Node:** `http://<other node>/api/data` (no login needed; firmware 2.x and 4.x).
- The URL is not written to the log; the status page shows only its host.

## Status and API

The status page lists each network sensor under *Wireless sensors*: source, topic (or the host of the URL), messages received and the last one, the last error. Its values appear in the sensor list with their age (no signal strength).

In `/api/data` → `rfSensors[]` a network sensor has `"protocol": "MQTT"` or `"REST"`, `id` `E100000n`, `rssi` 0 and `batteryOk` true; a value older than the freshness window is listed in `stale`. Its Home Assistant device has no *Signal Strength*, *Battery* or *Reception* entity (a network sensor reports none of them). With 868 MHz reception switched off, network sensors alone keep the sensor table (and its outputs) in use.
