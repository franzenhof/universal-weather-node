# Configuration

Where settings are changed, what every setting does, and the numeric parameter IDs shared by LoRaWAN downlinks, MQTT commands and the web.

[← back to README](../README.md)

There are two places to change settings:

| | Config portal | Web settings page |
|---|---|---|
| **Where** | The node's own Wi-Fi `WeatherNode` → http://192.168.4.1 | Your home network → `http://<node-ip>/api/settings` |
| **When** | First boot, button held 3 s, or LoRaWAN downlink `0x03` | Any time, while Wi-Fi and the web API are on |
| **What** | Everything incl. the Wi-Fi network, wind-direction calibration, decoder download, firmware update — the only way in for the first setup and for a node with Wi-Fi or the web interface off | Everything, incl. the Wi-Fi network, the calibration wizard, the decoder download, weather networks and time zone |

## Web Settings Page

Open **Settings** in the web interface (user `admin`, password = the *web settings admin password* from the portal).

The page has six **tabs** (links: `/api/settings#basics`, `#sensors`, `#outputs`, `#alarms`, `#display`, `#system`); one *Save* stores all of them, and the page comes back on the tab you were on. Without JavaScript all tabs are shown one below the other.

| Tab | Sections |
|-----|----------|
| **Basics** | Language, Device (name), Location (altitude, latitude / longitude), Time, Units, Web access (admin password) |
| **Sensors** | Wireless receiver (on/off, sensor type, *Only my sensors*), the **wireless sensor table** (★ Mine / ○ Received / ✕ Hidden, order of the ★ sensors — see [Wireless sensors](wireless-sensors.md#mine-received-hidden)), Value sources, Measurement (interval, board wind / rain, rain statistics, LiPo), Sensor calibration (incl. the link to the [wind-direction calibration wizard](#wind-direction-calibration)), Inputs & outputs (PCB v2.0), Davis (diagnostics) |
| **Outputs** | Send intervals and wind values (MQTT, LoRaWAN), MQTT / Home Assistant, Weather networks, Custom uploads, Loxone, LoRaWAN (incl. the payload decoder download) |
| **Alarms** | Alarm rules (12 slots, ready-made rules) and notification channels (ntfy, Pushover, Telegram, MQTT, webhook) with test buttons — see [Alarms](alarms.md). This tab has its own *Save* button (it is a separate form) |
| **Display** | The built-in OLED: when it is on, which screens, rotation, brightness, night-off hours — see [Display](#display) |
| **System** | Network (Wi-Fi, static IP), Firmware update check, Hardware (board, PCB, CPU frequency, debug mode), Switch off Wi-Fi / web interface (advanced, see [below](#switching-wi-fi-or-the-web-interface-off)), Setup wizard (start it again), Backup and restore, Firmware update (upload a `.bin`, needs an admin password — see [Firmware Update](installation.md#firmware-update-ota)), Reset values (records, chart history, rain, sunshine, AUX pulses), Diagnostics (log, radio trace) |

**Language:** the whole web interface — dashboard, status, settings, log, the radio-trace and firmware-update pages and the setup portal's own labels — is available in English, German, French, Spanish and Portuguese. *Settings → Basics → Language* (parameter 57, portal field `uilang`): **Auto** (default; each browser gets its own language from `Accept-Language`, e.g. `de-AT` → German, anything unsupported → English) or one fixed language for everybody. Every browser can still pick its own language in the menu at the top of each page or on the dashboard (stored in that browser as a cookie `wnLang`; *Language: device setting* removes it). Order: the browser's own choice, then a fixed setting, then the browser language. Technical names (JSON keys, MQTT topics, parameter names, units, sensor models) are never translated; the help page (this documentation) stays English.

**Show advanced options** (above the tabs, remembered in the browser) shows the expert fields, hidden by default: the whole *Davis (diagnostics)* section, CPU frequency, debug mode, the sensor lists as text (*Mine* / *Hidden* IDs), the AUX analog and UV divider scale factors, tacho pulses per revolution, the version list URL of the update check and the section *Switch off Wi-Fi / web interface*. Hidden fields are still saved with their current value.

- Sections marked **live** apply immediately: measurement, inputs & outputs (PCB v2.0 only), units, calibration, weather networks, time, admin password.
- Sections marked **restart** are only read at boot: LoRaWAN, network & MQTT, PCB revision. If you change one of them, the node saves and restarts; the chart history is saved before and restored after the restart.
- **Passwords and keys are never shown.** Leave a password field empty to keep the stored value; optional ones have a *remove* checkbox.
- Invalid values are listed after saving and keep their old value; everything else is saved. Fields that are not sent keep their value (scripts: send checkboxes explicitly as `name=on` / `name=0`). Posts from another web site (`Origin`/`Referer` not matching the node) are refused with 403.
- **Wi-Fi network** (name and password, *Network & MQTT*): a change is **tested for 60 s** without a restart. If the node connects, the new network is stored; if not, it goes back to the current one and the settings page shows a notice — a typo can't lock you out. The test only counts as passed if the new network also answers (gateway ping or a DNS lookup within 10 s) — a network that blocks both rolls back; use *Store without test* for it. Switching to another network needs DHCP: switch a static IP off in the same step (or before). **Store without test (pre-provisioning)** stores a network that is not in range yet right away: the node leaves the current network immediately and keeps trying the new one (reachable again via that network, downlink `0x03` for the portal, or the button).
- **Payload decoder** (*Outputs → LoRaWAN*): downloads `weather-node-decoder.js`, the ChirpStack / TTN decoder matching this firmware — the same file as the portal's. `GET /decoder`, no login (it holds no secrets).

### Wind-direction calibration

*Settings → Sensors → Sensor calibration → Calibration table → Calibrate (guided)* opens `/calibrate`, the same 8- or 16-position wizard as in the config portal (see [Wind Vanes](pcb.md#wind-vanes) for when a vane needs it): turn the vane to each shown direction, the page shows the live ADC reading, *Capture* records it, *Save Calibration* stores the table and switches the sensor type to the calibrated table — immediately, without a restart. Admin login like the settings page (no admin password: refused, as the settings), the save is same-origin only. Only offered on a board with a wind-vane input (not on the Heltec V4 wireless build). API: `GET /calibrate/sample` (`{"adc":N}`), `POST /calibrate/save` ([OpenAPI](api/openapi.yaml)).

### Switching Wi-Fi or the web interface off

*Settings → System → Switch off Wi-Fi / web interface* (shown with *Show advanced options*): **Wi-Fi enabled** off makes a LoRaWAN-only node (no web interface, MQTT, weather networks, NTP); **Web interface enabled** off keeps Wi-Fi, MQTT and the uploads but closes all web pages and the REST API. After saving the node restarts and is **reachable only via the config portal** (PRG button 3 s) **or a LoRaWAN downlink** (`0x03` opens the portal; parameter changes via `0x10`). To be safe the save is refused — the switches stay on, with a notice — unless
- the confirmation checkbox is ticked, and
- LoRaWAN is enabled (and stays enabled in the same save) **and joined** right now.

Switching them on again is only possible in the config portal (fields `wifien`, `webapien`). Form fields for scripts: `wifien=0` / `webapien=0` with `radiooffok=1`; a restored backup never switches them off.

## Settings Reference

| Setting | Description | Default |
|---------|-------------|---------|
| **LoRaWAN** | | |
| LoRaWAN enabled | Switch on for sites without Wi-Fi (needs a gateway / network account) | off |
| DevEUI / JoinEUI | 16 hex characters each, from ChirpStack/TTN | – |
| AppKey | 32 hex characters, from ChirpStack/TTN | – |
| Region | `EU868`, `US915`, `AU915`, `AS923`, `IN865`, `KR920`, `CN470`, `RU864` | EU868 |
| Payload format | Compact binary (FPort 3) or CayenneLPP (FPort 1) — see [Payload Formats](lorawan.md#payload-formats) | binary |
| Low-power mode | Light sleep between sub-samples (LoRa only, Wi-Fi off) — see [Low-Power Mode](lorawan.md#low-power-mode-lora-only) | off |
| Low-power: switch wind-vane supply off | PCB v2.0: vane potentiometer only powered around each reading | on |
| **Measurement** | | |
| LoRaWAN / measurement interval | Seconds between LoRaWAN uplinks (5–3600); stretched automatically if the data rate would exceed 1 % airtime | 300 |
| Wind sample interval | **1 s** or **3 s** — see [Measurement and Aggregation](values.md#measurement-and-aggregation) | 1 |
| LoRaWAN wind values | *Interval* (mean, 3-s gust and direction since the last uplink) or *10 min* (WMO values of the last 10 minutes) | interval |
| MQTT interval | Seconds between MQTT publishes; `0` = like the LoRaWAN interval, else 2–3600 | 0 |
| MQTT wind values | *Interval* or *10 min*, as for LoRaWAN | interval |
| Wireless sensors | Receive 868 MHz sensors with the LoRa chip (between LoRaWAN uplinks, if LoRaWAN is on) — see [Wireless Sensors](wireless-sensors.md) | on |
| Wireless sensor type | **Auto (search)** — cycles through all families and stores the type of the first sensor it hears (Bresser → *Bresser*, Ecowitt / Froggit or IT+ 17.241 → *TFA IT+ 17.241 + Fine Offset*, IT+ 9.579 → *IT+ 9.579*); **All types** — keeps listening to every family with [timed switching](wireless-sensors.md#several-sensor-families-at-once) and stores nothing (to see everything in range, or for sensors of several brands); or fixed: *Bresser*, *TFA / LaCrosse IT+* (both data rates, alternating, or 17.241 / 9.579 kbit/s only), *Fine Offset / Ecowitt / Froggit*, *TFA IT+ 17.241 kbit/s + Fine Offset* (same data rate, received together) or *IT+ both rates + Fine Offset*; *Bresser + TFA IT+* or *Bresser + Fine Offset* are received with **timed switching** (see [several sensor families](wireless-sensors.md#several-sensor-families-at-once); still being field-tested); **Davis Vantage Pro2 / Vue (exclusive)** — experimental, pauses LoRaWAN and all other wireless sensors, never chosen by *Auto*, see [Davis wireless](wireless-sensors.md#davis-wireless-experimental) | Auto (search) |
| **Davis (diagnostics)** | shown while *Davis* is selected; all live — see [Davis wireless](wireless-sensors.md#davis-wireless-experimental) | |
| Transmitter ID | 1–8 (ISS DIP switches); 0 = the first transmitter heard | 0 |
| Station | Vantage Pro2 / 6332 (potentiometer vane) or Vantage Vue (Hall vane) — wind-direction formula | Pro2 |
| Rain collector | 0.2 mm or 0.01 in per tip | 0.2 mm |
| Frequency offset | −100…+100 kHz added to every channel | 0 |
| Search: offset scan | ±0–60 kHz in 6-kHz steps while searching | ±30 |
| AFC while tracking | follow a drifting transmitter (probes 6 kHz above / below every 4th packet) | off |
| RX window: late / early (guard) | 5–500 ms after / before the predicted packet | 25 / 40 |
| Channel table | measured 2026 or classic DavisRFM69 | measured |
| Receiver bandwidth | 58.6, 78.2, 93.8, 117.3, 156.2, 234.3 kHz | 93.8 |
| Phantom-rain filter, tips per packet, max. tips per minute | an implausible jump of the 7-bit tip counter (more than *tips per packet* since the last accepted count, a decrement that is not a 127 → 0 wrap, a counter reset) is **held** until the next counter packet: if that one continues from the old value, the held value was corrupt and is dropped; if it continues from the new one, the jump counts as rain while it fits *max. tips per minute* (over at least one minute), else it is a counter reset and only re-bases the counter. Every held / dropped / confirmed / re-based jump goes to the log and the radio trace. Off = the old behaviour (steps above 64 tips = reset). Heavy rain (8 tips = 1.6 mm per ~10 s packet, 50 tips = 10 mm per minute) passes unchanged | on, 8, 50 |
| Sniffer | every packet to the log (`/api/log`, max 30 lines/min; all of them in the [radio trace](wireless-sensors.md#capturing-a-radio-trace-for-us), `/api/davis/log`) | off |
| Raw capture, channel | park on channel 0–4 without hopping, log everything incl. CRC failures | off, 0 |
| Only my sensors | On: only the ★ *Mine* sensors are received (the behaviour of older firmware with an ID list); off: every sensor except the hidden ones, the others numbered after yours. No effect while no sensor is ★. Portal field `rfonly`, parameter 55 | off; **on** for a node updated with an ID list set |
| Wireless sensor table | Per sensor ★ *Mine* / ○ *Received* / ✕ *Hidden* and the order of the ★ sensors (arrows) — see [Mine, Received, Hidden](wireless-sensors.md#mine-received-hidden). **Live:** while the page is open, newly received sensors are appended every 5 s and signal / values refreshed; fields you are editing (name, state) are left alone. Sensors that went silent stay until the page is reloaded | – |
| Mine (IDs as text) *(advanced)* | The ★ list as hex IDs, comma-separated, max 32; its order numbers the sensors *Wireless 1–n* (the first four are the ones *Auto* looks at; any of them can be chosen as a value source); each gets a [Home Assistant device](mqtt-home-assistant.md#wireless-sensors-in-home-assistant). Portal field `rfids`. A list saved by an older firmware is kept unchanged | – |
| Hidden (IDs as text) *(advanced)* | The ✕ list, max 32: never stored, numbered or output. Portal field `rfignore`; parameter 56 (value 0) empties it | – |
| Source: temperature / humidity, wind, rain, UV, light, pressure | Which sensor feeds each value into all outputs: *Auto* (board first, then the ★ sensors in their order; without ★: wireless 1–4), *Board sensor*, *Wireless 1–32* (the sensors received or ★, with name and ID), *Off* — see [Value Sources](wireless-sensors.md#value-sources). Portal field `srcs`: six values `air,wind,rain,uv,light,pressure` (a value left out keeps its setting) | Auto |
| Wind / rain sensor (board) | The PCB's anemometer / rain gauge — switch on when one is connected (a wireless sensor delivers wind/rain independently, see value sources) | off |
| Keep rain statistics over a power cut | Saves rain today / yesterday / week / month / year, sunshine today / yesterday and the last-rain time (one 68-byte record) to flash at most once per hour when changed, at the day change and before every planned restart or update — so they survive power cuts and firmware updates; off = RTC RAM only (lost on power loss) | on |
| LiPo measurement | Battery voltage measurement | off |
| Sunshine duration | *Illuminance ≥ 15000 lx* or *solar radiation ≥ 0.4 × clear-sky maximum* (WMO-like; needs a W/m² sensor — Davis solar radiation — as the light source and latitude / longitude, otherwise the illuminance threshold is used) — see [Sun and Moon](values.md#sun-and-moon). Parameter 65 | illuminance |
| Altitude | Station altitude in m, for sea-level pressure (QNH) | 560 |
| Heltec board (restart) | *Auto* (by flash size: 8 MB = V3, 16 MB = V4) / *V3* / *V4* — only needed if detection is wrong. A V4 runs without PCB or on PCB v2.0 (detected by the revision ID on GPIO15); a V4 without that ID ignores the PCB revision, board wind / rain sensors and all PCB inputs and outputs (wireless build) | Auto |
| PCB revision | 12 = Rev 1.2, 13 = Rev 1.3, 20 = v2.0 (v2.0 is detected automatically); a Heltec V4 runs on v2.0 only (ID on GPIO15) and ignores this without it | 13 |
| CPU frequency | 80 MHz (saves power) or 240 MHz | 80 |
| Debug mode | Adds diagnostic fields (cycle counter, free heap, uptime; CPU temperature in the LoRaWAN payload — the JSON/MQTT data always contain it) | off |
| **Units & calibration** | | |
| Units | Metric or Imperial — see [Units](values.md#units-metricimperial) | Metric |
| Wind speed unit | km/h or m/s (metric only) | m/s |
| Wind direction offset | North correction in degrees (−180 to +180) | 0 |
| Rain per tip / wind per pulse | Sensor ratios — see [Using Other Sensors](pcb.md#using-other-sensors) | Davis values |
| Temperature / humidity / pressure offset | Added to the main value **after the value source** — board sensor or wireless alike: temperature −10…+10 K, humidity −20…+20 % (result kept within 0–100 %), pressure −20…+20 hPa on the **station pressure before** the sea-level (QNH) reduction. Parameters 59 / 60 / 61 (tenths) | 0 |
| Rain factor / wind speed factor | 0.5–2 (1 = unchanged); multiplies every rain increment / wind speed and gust of the selected source. Parameters 62 / 63 (thousandths) | 1 |
| Per wireless sensor | Offsets and factors per sensor ID: `ID:temperature:humidity[:rain:wind:UV:light[:wn20 or :wh40][:ad=AD0-AD100]]`, comma-separated (offsets in K and %, factors 0.5–2; humidity and the factors optional), e.g. `1A2B3C4D:-0.4:+3`, `900181A2:0:0:0.95:1.05:1:1`, `20001234:0:0:0.908` (WN20); up to 32 sensors (about 15 bytes each, 40 with factors) — see [Calibration](#calibration) | – |
| **Inputs & outputs (PCB v2.0)** | Details: [Inputs and Outputs](pcb.md#inputs-and-outputs-pcb-v20) | |
| AUX analog input, scale | GPIO4; scale = V at the terminal per V at the pin (1.66667 with R25 = 15 kΩ) | off, 1.0 |
| UV sensor, divider scale | GPIO3; terminal voltage per ADC voltage, matching JP10 (1–2: 0–3.3 V, 1.1; 2–3: 0–5 V, 1.66534) | off, 1.1 |
| UV index enabled, calibration verified | Index is reported only when both switches are on and calibration is valid | both off |
| UV index slope, offset | Linear conversion of terminal volts into index; slope 0.001–30 index/V, offset −10…+10 | 1.0, 0.0 (not calibrated) |
| AUX pulse counter | GPIO48 | off |
| 2nd wind sensor, vane offset | off / speed (anemometer at AUX `PULSE`) / speed + direction (vane at AUX `AIN`) — see [Second Wind Sensor](pcb.md#second-wind-sensor-pcb-v20); takes over the AUX inputs it uses | off, 0° |
| Heater mode, power, thresholds | off / on / auto; auto: on below the switch-on temperature with the start power, rising linearly to full power at the full-power temperature | off, 100 %, 4.0 °C / 10 %, −10.0 °C / 100 % |
| Fan mode, speed, threshold, minimum PV power | off / on / auto (from the threshold); PWM 1–100 %; °C; W (VE.Direct, 0 = ignore) | off, 100 %, 15.0 °C, 0 |
| Fan tacho, pulses per revolution | GPIO40 | off, 2 |
| Victron VE.Direct | GPIO33/34 | off |
| Minimum battery voltage for heater/fan | V (VE.Direct), 0 = no limit | 0 |
| **Loxone (UDP)** | see [Loxone](loxone.md) | |
| Loxone UDP | on/off, Miniserver IP or host name, UDP port, name prefix (`A–Z a–z 0–9 _ . -`, max 16), interval (`0` = every cycle, else 2–3600 s) | off, –, 7000, –, 0 |
| **Wi-Fi + MQTT** | | |
| Wi-Fi / web API | On/off switches; off from the web settings only with a confirmation and LoRaWAN joined — see [Switching Wi-Fi or the web interface off](#switching-wi-fi-or-the-web-interface-off) | on |
| MQTT / MQTT commands | On/off switches — see [MQTT and Home Assistant](mqtt-home-assistant.md) | off |
| Device name | Hostname for Wi-Fi/DHCP and mDNS (`<hostname>.local`): lower case, characters outside `a–z 0–9` → `-` (runs collapsed, none at the start or end), at most 31 characters (`"Weather Mast"` → `weather-mast`); without a name `weathernode-<last 6 MAC hex digits>`. Also the MQTT client ID (characters outside `A–Z a–z 0–9 _ -` become `_`) and default topic prefix | – |
| Wi-Fi network and password | Portal (*Configure WiFi*) or web settings (tested for 60 s with rollback, or *store without test* for pre-provisioning) | – |
| Static IP, gateway, subnet, DNS | Optional fixed LAN address; leave disabled to use DHCP. Configure in the portal's Wi-Fi form or the web settings page (restart required) | DHCP; subnet 255.255.255.0; DNS defaults to gateway |
| MQTT broker, port, user, password, topic prefix, TLS mode, CA certificate | Broker connection | port 1883, no TLS |
| Web settings admin password | Required for the settings page and the log | – |
| **Web settings page only** | | |
| Weather networks | Station IDs, keys and intervals (AWEKAS, Weather Underground, Weathercloud, Wetternetzwerk.pro, Windy, openSenseMap, Met Office WOW, CWOP) — see [Weather Networks](weather-networks.md) | off |
| Custom uploads 1–2 | URL template, GET / POST, body template, interval 60–3600 s — see [Custom Upload](weather-networks.md#custom-upload) | off, 300 s |
| Alarms | Rules and channels — see [Alarms and Notifications](alarms.md); stored in their own flash records (namespaces `alarms`, `cupload` for the custom uploads) | none |
| Firmware update check, version list URL | Daily check for a newer version — see [Firmware Update Check](values.md#firmware-update-check) | off; this project's GitHub releases |
| Latitude / longitude | Sun and moon times computed on the node ([Sun and Moon](values.md#sun-and-moon)) and sent to AWEKAS; nowhere else | – |
| Reset values | Buttons (with a confirmation, at once, no *Save*): *Reset records (today to this year)*, *Reset all records (incl. all-time)* — also parameter 64 — *Delete chart history* (24 h and 7 days, also the copy in flash) and *Reset rain statistics* (today, yesterday, week, month, year, last rain and *rain since start* to 0, also the flash copy; the sunshine duration and the per-sensor rain of wireless sensors are not affected), *Reset sunshine duration* (today, yesterday) and, on PCB v2.0, *Reset AUX pulse total* — see [Records](values.md#records) | – |
| Backup and restore | see [Backup and Restore](#backup-and-restore) | – |
| Time zone, NTP server | POSIX time zone string and time server | Central Europe, `pool.ntp.org` |

### Display

*Settings → Display* — the Heltec's built-in 128 × 64 OLED. All live.

| Setting | Meaning | Default |
|---------|---------|---------|
| Display on | **for 20 s after PRG** (as before), **always** (except the night hours), or **for N minutes after PRG** | 20 s after PRG |
| Minutes after PRG | for the third mode, 1–120 | 5 |
| Screens | **classic status** (wind, rain, temperature, battery, LoRaWAN uplink result or IP / receiver state — the screen of older firmware), **temperature** (big, with humidity, dew point, feels like), **wind** (mean, gust, compass point and degrees), **rain** (today, last hour, rate), **pressure** (QNH, 3-h change with an arrow and the weather trend word), **status** (IP and RSSI, `.local` name, number of wireless sensors and active alarms) | classic only |
| Next screen every | rotation through the ticked screens, 2–60 s; while the display is on, a short PRG press shows the next screen | 5 s |
| Brightness | OLED contrast 1–255 | 200 |
| Night off from / until | local hours 0–23 (e.g. 22 and 6); equal = never. Against burn-in and to save power in *always* mode; a PRG press still switches the display on for 20 s | 0 / 0 |

Texts on the screens follow the device language (*Settings → Basics → Language*; *Auto* = English). Existing nodes keep the old behaviour until something is changed here.

### Calibration

*Settings → Sensors → Sensor calibration.* The main-value corrections (offsets for temperature, humidity and station pressure, factors for rain and wind speed) are applied **after the value source was chosen**, so they correct whatever feeds the value — the board sensors or a wireless sensor — and **every output sees the corrected value**: status page, dashboard and history, `/api/data`, MQTT / Home Assistant, LoRaWAN, Loxone, the weather networks, the records and the derived values (dew point, QNH, rain statistics …). The pressure offset is added to the station pressure; QNH is then computed from the corrected value.

The **per-sensor** offsets (`ID:temperature:humidity`) and the **wind, UV and light factors** are applied when a packet of that sensor is stored. The per-sensor **rain factor** multiplies the counter's *increments* — the main rain path when that sensor is the rain source, and the sensor's rain today / last hour — never the raw counter that is shown. For a **WN20** the normal way is the rain factor **0.908** (`20001234:0:0:0.908`); **`wn20`** switches on the experimental rate-dependent WN20 correction from the Ecowitt wiki instead of a fixed rain factor (see [Wireless sensors](wireless-sensors.md#names-ecowitt-ids-rain-and-lightning-per-sensor)). Entries saved by older firmware (`ID:t:h`) keep working. They therefore also correct that sensor's raw values: `rfSensors` in `/api/data`, `<prefix>/rf/<ID>`, its Home Assistant device and the status page. If the sensor is the selected source, the main offset comes on top — use one or the other for a sensor that is a value source. Wind direction, rain per tip, wind per pulse and the UV calibration stay where they were.

### Backup and Restore

*Settings → System → Backup and restore.*

- **Download settings** saves `uwn-settings-<host>-<date>.json` with every field of the settings page (`settings`: the form field names of this page with their values; `format`, `version`, `firmware`, `host`, `date`, `secrets`). Passwords and keys — Wi-Fi, admin, MQTT, the weather networks' keys, the alarm channels' tokens, the LoRaWAN AppKey, and the webhook / custom-upload URLs (they may carry a key) — are only included if you tick **include passwords and keys** (default off): the file then holds them in plain text, keep it private.
- **Restore settings from a file** uploads such a file. It is checked like a manual *Save* (same limits, invalid values are listed and keep their old value), the next settings page lists **what changed** (old → new; secrets only as `***`), and the node restarts if a restart field changed. A file from a newer major format (`version` > 1) or another file is refused without changing anything. Fields that this node's page does not have (other hardware, older firmware) are ignored; a backup without secrets keeps the stored passwords. A different Wi-Fi network in the file is tested like a manual change (rollback after 60 s).
- Not in the backup: the LoRaWAN session and DevNonces (stay untouched), the wind-direction calibration table ([wizard](#wind-direction-calibration)), the rain statistics and records. API: `GET /api/settings/backup[?secrets=1]`, `POST /api/settings/restore` (field `backup`), both admin login and same-origin.

Saved settings survive ordinary restarts and application-only OTA updates. Factory reset or a full flash erase removes project settings; upgrades from the older storage layout may require re-entering them. Keep a private backup of keys, sensor calibration and settings before changing firmware or partitions.

## Parameter Update via Downlink `0x10`

Send on **FPort 1**: `0x10`, followed by one or more triplets `[paramId][valueHi][valueLo]` — the value is a **signed 16-bit big-endian** number (e.g. 300 = `01 2C`). Several triplets can follow one `0x10`. Valid values are saved immediately.

| ID (dec) | ID (hex) | Parameter | Range | Example | Full payload (hex) |
|---|---|---|---|---|---|
| 1 | `0x01` | `uplinkIntervalSec` | `5..3600` | 300 s | `10 01 01 2C` |
| 2 | `0x02` | `sampleSec` | wind sample interval `1` or `3` s (2 → 1, 4 and more → 3) | 1 s | `10 02 00 01` |
| 3 | `0x03` | `windEn` | `0` / `1` | 1 | `10 03 00 01` |
| 4 | `0x04` | `rainEn` | `0` / `1` | 1 | `10 04 00 01` |
| 5 | `0x05` | `lipoEn` | `0` / `1` | 1 | `10 05 00 01` |
| 6 | `0x06` | `debugMode` | `0` / `1` | 1 | `10 06 00 01` |
| 7 | `0x07` | `windDirOffsetDeg` | `-180..180` | −45 (two's complement) | `10 07 FF D3` |
| 8 | `0x08` | `windUnitMs` | `0` = km/h, `1` = m/s | 1 | `10 08 00 01` |
| 9 | `0x09` | `cpuFreqMhz` | `80` / `240` | 240 | `10 09 00 F0` |
| 10 | `0x0A` | `payloadFormat` | `0` = CayenneLPP, `1` = binary | 1 | `10 0A 00 01` |
| 11 | `0x0B` | `unitSystem` | `0` = Metric, `1` = Imperial | 1 | `10 0B 00 01` |
| 12 | `0x0C` | `rainMmPerTip` | mm/tip ×1000, `1..5000` | 200 (= 0.2 mm) | `10 0C 00 C8` |
| 13 | `0x0D` | `windMsPerPps` | (m/s per pulse/s) ×1000, `1..10000` | 1006 (≈ Davis) | `10 0D 03 EE` |
| 14 | `0x0E` | `windDirSensorType` | `0` = potentiometer, `1` = calibrated table | 1 | `10 0E 00 01` |
| 15 | `0x0F` | `auxAinEn` | `0` / `1` | 1 | `10 0F 00 01` |
| 16 | `0x10` | `auxAinScale` | ×1000, `1..20000` | 1667 (≈ 1.66667 for R25 = 15 kΩ) | `10 10 06 83` |
| 17 | `0x11` | `auxPulseEn` | `0` / `1` | 1 | `10 11 00 01` |
| 18 | `0x12` | `heaterMode` | `0` = off, `1` = on, `2` = auto | 2 | `10 12 00 02` |
| 19 | `0x13` | `heaterDutyPct` | `1..100` | 50 % | `10 13 00 32` |
| 20 | `0x14` | `heaterOnBelow` | °C ×10, `-400..400` | 40 (= 4.0 °C) | `10 14 00 28` |
| 21 | `0x15` | `fanMode` | `0` = off, `1` = on, `2` = auto | 1 | `10 15 00 01` |
| 22 | `0x16` | `fanDutyPct` | `1..100` | 100 % | `10 16 00 64` |
| 23 | `0x17` | `fanOnAbove` | °C ×10, `-400..600` | 150 (= 15.0 °C) | `10 17 00 96` |
| 24 | `0x18` | `fanMinPvW` | W, `0..5000` (0 = ignore) | 10 W | `10 18 00 0A` |
| 25 | `0x19` | `tachoEn` | `0` / `1` | 1 | `10 19 00 01` |
| 26 | `0x1A` | `tachoPpr` | `1..8` | 2 | `10 1A 00 02` |
| 27 | `0x1B` | `veDirectEn` | `0` / `1` | 1 | `10 1B 00 01` |
| 28 | `0x1C` | `outMinBattV` | V ×10, `0..600` (0 = no limit) | 124 (= 12.4 V) | `10 1C 00 7C` |
| 29 | `0x1D` | `heaterMinDutyPct` | `1..100` (auto: power at `heaterOnBelow`) | 10 % | `10 1D 00 0A` |
| 30 | `0x1E` | `heaterFullBelow` | °C ×10, `-400..400` (auto: `heaterDutyPct` from here) | −100 (= −10.0 °C) | `10 1E FF 9C` |
| 31 | `0x1F` | `lowPowerEn` | `0` / `1` — [low-power mode](lorawan.md#low-power-mode-lora-only) | 1 | `10 1F 00 01` |
| 32 | `0x20` | `vanePowerSave` | `0` / `1` — low-power mode also switches the wind-vane supply off (PCB v2.0) | 0 | `10 20 00 00` |
| 33 | `0x21` | `wind2Mode` | `0` off, `1` 2nd anemometer at AUX `PULSE`, `2` plus vane at AUX `AIN` (PCB v2.0) | 2 | `10 21 00 02` |
| 34 | `0x22` | `wind2DirOffsetDeg` | `−180`…`180`, north offset of the 2nd vane | -90 | `10 22 FF A6` |
| 35 | `0x23` | `uvEn` | `0` / `1`, UV sensor voltage (PCB v2.0) | 1 | `10 23 00 01` |
| 36 | `0x24` | `uvAinScale` | ×1000, `1000..5000`; terminal V per ADC V | 1100 (1.1 default; use 1665 for JP10 2–3) | `10 24 04 4C` |
| 37 | `0x25` | `uvIndexEn` | `0` / `1`, publish the calibrated index | 1 | `10 25 00 01` |
| 38 | `0x26` | `uvCalibrated` | `0` / `1`, explicit confirmation of sensor calibration | 1 | `10 26 00 01` |
| 39 | `0x27` | `uvIndexSlope` | index/V ×1000, `1..30000` | 2000 (= 2 index/V) | `10 27 07 D0` |
| 40 | `0x28` | `uvIndexOffset` | index ×1000, `−10000..10000` | −500 (= −0.5) | `10 28 FE 0C` |
| 41 | `0x29` | `mqttIntervalSec` | `0` (= LoRaWAN interval) or `2..3600` | 5 s | `10 29 00 05` |
| 42 | `0x2A` | `loraWindAgg` | `0` interval, `1` 10 min (WMO) | 0 | `10 2A 00 00` |
| 43 | `0x2B` | `mqttWindAgg` | `0` interval, `1` 10 min (WMO) | 1 | `10 2B 00 01` |
| 44 | `0x2C` | `rfEn` | `0` / `1`, 868 MHz wireless sensors (IDs only via web settings / portal) | 1 | `10 2C 00 01` |
| 45–49 | `0x2D`–`0x31` | value sources: temperature/humidity, wind, rain, UV, light | `0` auto, `1` board, `2`–`33` wireless 1–32 (n + 1), `34` off (up to rc.5: `6`) — see [Value Sources](wireless-sensors.md#value-sources) | 2 (wind from wireless 1) | `10 2E 00 02` |
| 50 | `0x32` | `rfProto` | wireless sensor type: `16` Auto (search, stores `1` / `10` / `4` after the first sensor heard), or a bit mask: `1` Bresser, `2` IT+ 17.241 kbit/s, `4` IT+ 9.579 kbit/s, `6` IT+ both, `7` Bresser + IT+, `8` Fine Offset / Ecowitt, `10` IT+ 17.241 + Fine Offset (same data rate, no alternating), `14` IT+ both + Fine Offset, `9` Bresser + Fine Offset, `15` all types (keep listening to everything); `17` Davis Vantage Pro2 / Vue (exclusive: LoRaWAN paused, experimental); any combination `1..15` (Bresser together with others = timed switching between the data rates, see [several sensor families](wireless-sensors.md#several-sensor-families-at-once)) | 6 | `10 32 00 06` |
| 51 | `0x33` | `rainPersist` | keep rain and sunshine statistics (today, yesterday, week, month, year, last rain) over power cuts and updates (`0` off / `1` on, at most one flash write per hour plus one per planned restart) | 1 | `10 33 00 01` |
| 52 | `0x34` | `loxEn` | Loxone UDP output `0` off / `1` on | 0 | `10 34 00 01` |
| 53 | `0x35` | `loxSec` | Loxone interval in s: `0` = every measurement cycle, else 2–3600 | 0 | `10 35 00 3C` |
| 54 | `0x36` | value source: pressure | `0` auto (BME280 first, then wireless 1–4 with a barometer), `1` board (BME280), `2`–`33` wireless 1–32, `34` off — see [Value Sources](wireless-sensors.md#value-sources) | 0 | `10 36 00 02` |
| 55 | `0x37` | `rfOnlyMine` | Only my sensors: `1` = accept only the ★ *Mine* IDs (no effect while there are none), `0` = all except hidden | 0 (1 after an update with an ID list) | `10 37 00 01` |
| 56 | `0x38` | hidden-sensor list | `0` = empty the list (unhide all); the IDs themselves are set in the web settings / portal | – | `10 38 00 00` |
| 57 | `0x39` | `uiLang` | web interface / portal language: `0` Auto (browser language; the portal: English), `1` English, `2` Deutsch, `3` Français, `4` Español, `5` Português | 0 | `10 39 00 02` |
| 58 | `0x3A` | `setupDone` | setup wizard: `1` = done, `0` = the dashboard opens the [setup wizard](installation.md#setup-wizard) again (once per browser session; without an admin password it is open in the LAN until finished) | 0 (1 for nodes updated from an older firmware) | `10 3A 00 00` |
| 59 | `0x3B` | temperature offset | 0.1 K, −100…100 (signed 16 bit), after the value source — see [Calibration](#calibration) | 0 | `10 3B FF FC` (−0.4 K) |
| 60 | `0x3C` | humidity offset | 0.1 %, −200…200 | 0 | `10 3C 00 1E` (+3 %) |
| 61 | `0x3D` | pressure offset | 0.1 hPa, −200…200, on the station pressure before QNH | 0 | `10 3D 00 0F` (+1.5 hPa) |
| 62 | `0x3E` | rain factor | ×1000, 500…2000 | 1000 | `10 3E 04 4C` (1.1) |
| 63 | `0x3F` | wind speed factor | ×1000, 500…2000 (speed and gust) | 1000 | `10 3F 03 84` (0.9) |
| 64 | `0x40` | records reset | `1` = today … this year, `2` = also the all-time records (an action, not stored) — see [Records](values.md#records) | – | `10 40 00 01` |
| 65 | `0x41` | sunshine method | `0` illuminance ≥ 15000 lx, `1` solar radiation ≥ 0.4 × clear-sky maximum (W/m² sensor + location) | 0 | `10 41 00 01` |
| 66 | `0x42` | alarm rules on / off | bit n = rule n + 1 (0–4095); the rules themselves are set on the web page — see [Alarms](alarms.md) | – | `10 42 00 05` (rules 1 and 3 on) |

IDs 15–30 and wind-2 mode (33) belong to the [PCB v2.0 I/O block](pcb.md#inputs-and-outputs-pcb-v20); on Rev 1.2/1.3 they are rejected except for disabling a function. Enabling UV (35) also requires v2.0. Calibration values can be prepared where accepted, but do not provide the missing hardware.

Triplets are processed in order, not as an atomic transaction: valid changes are saved even if another triplet is invalid. An uplink or gateway TX_ACK does not confirm that a parameter was applied; check the device log or settings.

**Several at once:** interval 300 s plus debug on → `10 01 01 2C 06 00 01`
