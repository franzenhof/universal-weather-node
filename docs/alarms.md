# Alarms and Notifications

Rules that watch a value and notify you — by push message (ntfy, Pushover, Telegram), MQTT / Home Assistant or a webhook — when it crosses a threshold.

[← back to README](../README.md)

**Setup:** *Settings → Alarms* (own *Save* button on that tab). Pick a ready-made rule or fill in a free slot, enter at least one channel below, tick the channel in the rule, *Save*. Each channel has a **test button** (it saves the tab first); the result appears on the status page in the *Alarms* box.

## Rules

Up to **12 rules**. A rule watches **one value**:

| Value | Unit (as in the unit setting) |
|-------|-------------------------------|
| Temperature, feels like, dew point | °C / °F |
| Humidity | % |
| Pressure (QNH), pressure change 3 h | hPa / inHg |
| Wind speed, gust | m/s, km/h or mph (the last cycle's mean / gust) |
| Rain rate, rain today, rain last hour | mm/h, mm / in/h, in |
| UV index | – |
| Frost / fog-dew / sultry warning | 1 = warning on ([Warnings](values.md#derived-values)) |
| **Wireless sensor value** (by sensor ID) | temperature, humidity, wind, gust, UV, illuminance, **leak** (1 = water), **lightning distance** (km, 999 = none), **soil moisture** %, **CO2** ppm, **PM2.5** µg/m³, **silent for** (minutes since the last packet), **battery low** (1 = low) |

All values are the calibrated ones of the selected [value sources](wireless-sensors.md#value-sources).

**The form shows only what applies** (with JavaScript; without it all fields stay visible and work the same): a free slot only *Monitor*; *Sensor* and *Measurement* only for *Wireless sensor …*; the sensor list offers the ★ sensors first, then the received ones and any ID already used in a rule, as *name or model · Ecowitt ID or ID* (*other ID* opens a field for a sensor not in range now); *Measurement* lists only what that sensor provides (plus *silent for* and *battery low*); no thresholds for yes/no values (warnings, leak, battery low — they raise at 1 and clear at 0). The server still checks every combination.

| Setting | Meaning |
|---------|---------|
| Raise when the value is above / below the threshold | the comparison |
| Raise threshold / clear threshold | **Hysteresis:** raised when the value passes the raise threshold, cleared only when it reaches the clear threshold (above: value ≤ clear; below: value ≥ clear). For warnings and yes/no values use 0.5 for both. |
| Minimum duration | the condition must hold this long (minutes) before the alarm is raised — e.g. gusts above 20 m/s for 5 min |
| Repeat every | while raised, notify again after this many minutes (0 = once) |
| Priority | 1 (lowest) … 5 (urgent): ntfy priority 1–5; Pushover −2 … 2 (5 = emergency, repeated until acknowledged, see *retry / expire*); Telegram silent for 1–2 |
| Channels | ntfy, Pushover, Telegram, MQTT, webhook |
| Notify the all clear | also send a message when the alarm clears |
| Name, message, all-clear message | texts; empty = default text in the device language (*Settings → Basics → Language*; *Auto* = English) |

Evaluated every **30 s**. A value that is **not available** — wireless sensor silent for more than 5 minutes, no source selected, clock not yet synced for *rain today* — never raises and never clears an alarm, and a pending *minimum duration* starts over. The exception is *silent for*, which keeps counting while a sensor is gone. Alarm states live in RAM: after a restart a rule raises again if the condition still holds.

**Ready-made rules** (*Add a ready-made rule*, goes into the first free slot): **frost**, **fog / dew**, **sultry** (the node's [warnings](values.md#derived-values)), **storm** (pressure falling ≥ 6 hPa in 3 h, clears at −3 hPa, priority 4) and **rain starts** (rain rate above 0.1 mm/h, cleared when it stops, no all clear). Adjust them afterwards.

Parameter **66** switches rules on and off by LoRaWAN downlink (bit n = rule n + 1, see [parameters](settings.md#parameter-update-via-downlink-0x10)); everything else is set on the web page (or by a [settings backup](settings.md#backup-and-restore)).

### Messages and placeholders

Message templates may use **`{name}`** (rule name), **`{value}`**, **`{unit}`**, **`{threshold}`** (the raise threshold; the clear threshold in an all-clear message), **`{sensor}`** (wireless sensor ID), **`{state}`** (`raised` / `cleared` / `test`), **`{priority}`**, **`{rule}`**, **`{time}`** (local) and every placeholder of the [custom upload](weather-networks.md#placeholders), e.g. `{temp}`, `{wind:kmh}`. Default: `{name}: {value} {unit} (limit {threshold} {unit})`.

## Channels

| Channel | What you enter | Request |
|---------|----------------|---------|
| [ntfy](https://ntfy.sh) | server (default `https://ntfy.sh`), topic, optional access token, tags (comma-separated, e.g. `warning,snowflake`) | `POST` JSON `{topic, title, message, priority, tags}` to the server root; token as `Authorization: Bearer` |
| [Pushover](https://pushover.net) | application token, user key; retry / expire for emergency priority (≥ 30 s / ≤ 10800 s) | `POST https://api.pushover.net/1/messages.json` (form) |
| [Telegram](https://core.telegram.org/bots/api#sendmessage) | bot token (from @BotFather), chat ID | `POST https://api.telegram.org/bot<token>/sendMessage` JSON `{chat_id, text}` |
| MQTT | *MQTT alarm events* on | see below |
| Webhook | URL (GET), or URL + JSON body (POST) — both with placeholders | URL placeholders URL-encoded, body placeholders JSON-escaped, `Content-Type: application/json` |

Example webhook body: `{"alarm":"{name}","state":"{state}","value":{value},"unit":"{unit}","station":"{station}"}`. `{message}` is inserted whole (up to 199 characters). With the values filled in, the URL may have up to 319 characters and the body up to 639; a longer one is not sent (status *too long, not sent*) rather than sent cut off.

- **One request at a time**, sent by the same background task as the weather networks — never in parallel with an upload. HTTPS needs a lot of memory (see [Memory and HTTPS](troubleshooting.md)): a request starts only with at least 70 KiB free and 24 KiB in one free block (V4 with PSRAM: 32 KiB of internal RAM), and not while a settings / status page is being built — **a notification can therefore be delayed** by a few seconds (busy page) or, with little memory, longer (retries after 10 s, doubling up to 5 min). A failed request is tried up to 4 times in all (back-off from 15 s, doubling up to 5 min); the queue holds 6 messages (the oldest is dropped when full).
- The status page (*Alarms* box) and `/api/info` `outputs.status` (`ntfy`, `pushover`, `telegram`, `webhook`) show the last success / error of each channel.
- **Tokens and keys are write-only** fields like the other passwords, never shown, never logged (a log line names only the channel and the HTTP status) and not in `/api/alarms`. In the [settings backup](settings.md#backup-and-restore) they — and the webhook URL, which may carry a key — are only included with *include passwords and keys*.

## MQTT and Home Assistant

With MQTT on, every rule's state is published **retained** to `<prefix>/alarm/<n>` (`ON` / `OFF`; empty when the rule is deleted), and Home Assistant gets one **binary sensor** per rule (*Alarm &lt;name&gt;*, device class `problem`) — independent of the rule's channels. With *MQTT alarm events* on, rules with the MQTT channel also publish each event to `<prefix>/alarm` (not retained):

```json
{"rule": 1, "name": "Frost", "state": "raised", "value": -0.40, "unit": "°C", "priority": 3}
```

`state` is `raised`, `repeat` or `cleared`; a test sends `{"rule":0,"name":"test","state":"test"}`. Details: [MQTT and Home Assistant](mqtt-home-assistant.md#alarms).

## API and pages

- `GET /api/alarms` — rules, thresholds (configured units), state and which channels are configured (yes / no only) ([schema](api/schemas/alarms.schema.json)).
- `/api/data` `alarms` — the active rules `[{n, name, priority}]` (only while one is active); the dashboard shows them as badges next to the frost / fog / storm warnings.
- Status page: *Alarms* box with the active rules (since when) and each configured channel's send status.
