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
| **Sensors** | Wireless receiver (on/off, sensor type, *Only my sensors*), the **wireless sensor table** (★ Mine / ○ Received / ✕ Hidden, order of the ★ sensors — see [Wireless sensors](wireless-sensors.md#mine-received-hidden)), [Network sensors](network-sensors.md) (advanced), Value sources, [Custom series](#custom-series), Measurement (interval, board wind / rain, rain statistics, LiPo), [GNSS module](#gnss-module) (Heltec V4 without PCB), Sensor calibration (incl. the link to the [wind-direction calibration wizard](#wind-direction-calibration)), Inputs & outputs (V4 Full), Davis (diagnostics) |
| **Outputs** | Send intervals and wind values (MQTT, LoRaWAN), MQTT / Home Assistant, Weather networks, Custom uploads, Loxone, InfluxDB / VictoriaMetrics push, LoRaWAN (incl. the payload decoder download), [Virtual sensors](virtual-sensors.md) (advanced) |
| **Alarms** | Alarm rules (12 slots, ready-made rules) and notification channels (ntfy, Pushover, Telegram, MQTT, webhook) with test buttons — see [Alarms](alarms.md). This tab has its own *Save* button (it is a separate form) |
| **Display** | The built-in OLED: when it is on, which screens, rotation, brightness, night-off hours — see [Display](#display) |
| **System** | Network (Wi-Fi, static IP), Own access point, Firmware update check, Web UI (dashboard options), Hardware (board, PCB, CPU frequency, debug mode), Switch off Wi-Fi / web interface (advanced, see [below](#switching-wi-fi-or-the-web-interface-off)), Setup wizard (start it again), [HTTPS](#https) (optional, own certificate), Backup and restore, Firmware update (upload a `.bin`, needs an admin password — see [Firmware Update](installation.md#firmware-update-ota)), Reset values (records, chart history, rain, sunshine, AUX pulses), [Reset](#reset) (settings / erase everything), Diagnostics (log, radio trace, crash report with the core dump download, test crash — see [Storage layout](design/storage-layout.md#crash-report)) |

**Language:** the whole web interface — dashboard, status, settings, log, the radio-trace and firmware-update pages and the config portal's own labels — is available in English, German, French, Spanish and Portuguese. *Settings → Basics → Language* (parameter 57, portal field `uilang`): **Auto** (default; each browser gets its own language from `Accept-Language`, e.g. `de-AT` → German, anything unsupported → English) or one fixed language for everybody. Every browser can still pick its own language in the menu at the top of each page or on the dashboard (stored in that browser as a cookie `wnLang`; *Language: device setting* removes it). Order: the browser's own choice, then a fixed setting, then the browser language. Technical names (JSON keys, MQTT topics, parameter names, units, sensor models) are never translated; the help page (this documentation) stays English.

**Show advanced options** (above the tabs, remembered in the browser) shows the expert fields, hidden by default: the whole *Davis (diagnostics)* section, CPU frequency, debug mode, the hidden sensor list and the names / minimum signals of received-only sensors as text, the AUX analog and UV divider scale factors, tacho pulses per revolution, the version list URL of the update check and the section *Switch off Wi-Fi / web interface*. Hidden fields are still saved with their current value.

- Sections marked **live** apply immediately: measurement, inputs & outputs (V4 Full only), units, calibration, weather networks, time, admin password.
- Sections marked **restart** are only read at boot: LoRaWAN, network & MQTT, PCB revision. If you change one of them, the node saves and restarts; the chart history is saved before and restored after the restart.
- **Login:** user `admin`, HTTP Digest (the password itself never crosses the network). Wrong passwords are braked per client address: after 5 wrong ones every further try must wait 1 s, then 2, 4 … up to a minute; a try that comes too early gets `429 Too Many Requests` with `Retry-After`, other clients are not slowed down. Wait a moment and try again; a restart of the node does not log the browser out (it logs in again by itself).
- **Passwords and keys are never shown.** Leave a password field empty to keep the stored value; optional ones have a *remove* checkbox.
- Invalid values are listed after saving and keep their old value; everything else is saved. Fields that are not sent keep their value (scripts: send checkboxes explicitly as `name=on` / `name=0`). Posts from another web site (`Origin`/`Referer` not matching the node) are refused with 403.
- **Wi-Fi network** (name and password, *System → Network*): a change is **tested for 60 s** without a restart. If the node connects, the new network is stored; if not, it goes back to the current one and the settings page shows a notice — a typo can't lock you out. The test only counts as passed if the new network also answers (gateway ping or a DNS lookup within 10 s) — a network that blocks both rolls back; use *Store without test* for it. Switching to another network needs DHCP: switch a static IP off in the same step (or before). **Store without test (pre-provisioning)** stores a network that is not in range yet right away: the node leaves the current network immediately and keeps trying the new one (reachable again via that network, downlink `0x03` for the portal, or the button).
- **Payload decoder** (*Outputs → LoRaWAN*): downloads `weather-node-decoder.js`, the ChirpStack / TTN decoder matching this firmware — the same file as the portal's. `GET /decoder`, no login (it holds no secrets).

### Wind-direction calibration

*Settings → Sensors → Sensor calibration → Calibration table → Calibrate (guided)* opens `/calibrate`, the same 8- or 16-position wizard as in the config portal (see [Wind Vanes](pcb.md#wind-vanes) for when a vane needs it): turn the vane to each shown direction, the page shows the live ADC reading, *Capture* records it, *Save Calibration* stores the table and switches the sensor type to the calibrated table — immediately, without a restart. Admin login like the settings page (no admin password: refused, as the settings), the save is same-origin only. Only offered on a board with a wind-vane input (not on the Heltec V4 wireless build). API: `GET /calibrate/sample` (`{"adc":N}`), `POST /calibrate/save` ([OpenAPI](api/openapi.yaml)).

### Switching Wi-Fi or the web interface off

*Settings → System → Switch off Wi-Fi / web interface* (shown with *Show advanced options*): **Wi-Fi enabled** off makes a LoRaWAN-only node (no web interface, MQTT, weather networks, NTP); **Web interface enabled** off keeps Wi-Fi, MQTT and the uploads but closes all web pages and the REST API. After saving the node restarts and is **reachable only via the config portal** (PRG button 3 s) **or a LoRaWAN downlink** (`0x03` opens the portal; parameter changes via `0x10`). To be safe the save is refused — the switches stay on, with a notice — unless
- the confirmation checkbox is ticked, and
- LoRaWAN is enabled (and stays enabled in the same save) **and joined** right now.

Switching them on again is only possible in the config portal (fields `wifien`, `webapien`). Form fields for scripts: `wifien=0` / `webapien=0` with `radiooffok=1`; a restored backup never switches them off.

### Own access point

*Settings → System → Own access point* (applies at once, no restart): the node opens a Wi-Fi network of its own, for places without Wi-Fi — pilots at a model airfield open the dashboard on their phones directly at the node. How-to: [At the airfield](airfield.md).

It is set here in the web interface — **not** in the config portal (which is itself a separate, temporary access point `WeatherNode`, see [Config Portal](installation.md#config-portal)). A visitor on the node's own access point always gets the dashboard, never the [setup wizard](installation.md#setup-wizard), also while the setup is not finished.

| Field | Meaning | Default |
|-------|---------|---------|
| Access point | **off**; **fallback** — opens when the Wi-Fi network has been unreachable for 2 min (from the start, or since it was last connected); the station network is still tried every 2 min (instead of every 30 s: each attempt scans the channels and takes the access point off its channel for a moment), and the access point closes once the network has been back for 1 min; **always** — the access point always runs; a stored Wi-Fi network is used alongside it (AP + station: internet for NTP, uploads, MQTT), without one the node is access point only. Needs Wi-Fi and the web interface on | off |
| Network name (SSID) | 1–32 characters; empty = `WeatherNode-<last 4 MAC hex digits>` (shown as the field's hint) | empty |
| Password (WPA2) | 8–63 printable ASCII characters, **required** — the access point never runs open; a mode without a valid password is refused on saving. It is shown on the display (QR code) and, to devices on the access point, on the status page: it only protects *viewing*, the settings stay behind the admin login. Don't reuse a password of something else | – |
| Channel | 1–13, or **auto**: the station network's channel when it is connected (station and access point share the radio — the driver moves the access point to the station's channel anyway), else 1 | auto |

Fixed: address **192.168.4.1/24**, DHCP from .2 (8 leases), the node as DNS server and router; the DHCP offer also carries the captive-portal address (RFC 8910). If your home network itself uses 192.168.4.x, don't use *always* together with it. Up to **8 devices** at a time (the DHCP server's leases); the live stream (SSE) serves 3 of them, the others poll at the dashboard's refresh interval (default 10 s).

While it runs, for devices on the access point only (the station LAN sees no difference):
- a **DNS server** answers every name with 192.168.4.1, so any address typed in leads to the node;
- **connectivity checks** of the phones (Android `/generate_204`, Apple `/hotspot-detect.html`, Windows `/connecttest.txt` …) and every request for a foreign host name get a **redirect to `http://192.168.4.1/`** — the phone treats the network as one with a sign-in page and opens the dashboard in its sign-in sheet ([why a redirect](airfield.md#why-phones-show-a-sign-in-sheet));
- the **dashboard sends the phone's time** once per page load: without internet the node takes it as its clock (source *phone*, after [GNSS](#gnss-module) / NTP / DCF77 sensor) — see [LoRaWAN → Time sources](lorawan.md#time-sources-and-network-time-devicetimereq).

Status page: *Access point* (name, address, channel, devices) and the **join QR code** — the latter only for visitors connected through the access point (it carries the password; the status page is public in the LAN); `/api/info` → `outputs.accessPoint`, `accessPointActive`, `accessPointClients`. The display has its own **WiFi QR code** screen (*Display → Screens*). The log notes the free internal RAM before and after every start.

### GNSS module

*Settings → Sensors → GNSS module* — shown only on a Heltec V4 **without** PCB (the V4 Full / V4 Light use the GNSS connector's pins themselves). Wiring and the BME280 move to GPIO47/48: [Minimal build → GNSS module](minimal-build.md#gnss-module-optional).

| Field | Meaning | Default |
|-------|---------|---------|
| GNSS module connected | Powers the module on the V4's GNSS connector and reads its NMEA output (RMC, GGA, ZDA). **Restart.** While on, GPIO41/42 are never used for a BME280 | off |
| GNSS baud rate | *automatic* tries 9600 → 115200 → 38400 (3 s each) until valid sentences arrive (L76K: 9600, UC6580: 115200 out of the box), or a fixed rate. **Restart** | automatic |
| Take the time | The clock follows the GNSS time — the best time source, before NTP, DCF77 sensor, phone and LoRaWAN; fresh for 1 h after the last report. Taken only with a fix (RMC status *A*): without one a module may report the time of its own RTC. With the PPS pulse (GPIO41) to about 20 ms, without it to about 1 s. Live; switched off, the other sources take over at once | on |
| Take latitude / longitude | Writes latitude / longitude when the mean of 5 min of good fixes (≥ 4 satellites, HDOP ≤ 2) lies more than 100 m from the stored position (or none is stored) — saves the flash from a write per wander. Live | on |
| Take the altitude | Writes the station altitude (sea-level pressure, QNH) when that 5-min mean differs by more than 5 m (0–4000 m). Live | on |

Status page: *GNSS* (fix, satellites, HDOP, baud rate, PPS), *Position (GNSS)* with the altitude, *Last time from GNSS*, and the BME280's pins; `/api/info` → `hardware.gnss` (its `lat` / `lon` only for a request with the admin login — the station position stays private), `hardware.bme280Pins`, `hardware.bme280OldPins`. The log notes the rate found, the first time taken (with or without PPS) and every position / altitude written.

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
| Low-power: switch wind-vane supply off | V4 Full / V4 Light: vane potentiometer only powered around each reading | on |
| **Measurement** | | |
| Measurement cycle | Seconds between evaluations of the values (10–600): status page, REST API, display, dashboard history, records, alarms. Independent of the send intervals below — see [base cycle](values.md#measurement-and-aggregation) | 30 |
| LoRaWAN uplink interval | Seconds between LoRaWAN uplinks (5–3600); stretched automatically if the data rate would exceed 1 % airtime | 300 |
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
| Only my sensors | On: only the ★ *Mine* sensors are received; off: every sensor except the hidden ones, the others numbered after yours. No effect while no sensor is ★. Portal field `rfonly`, parameter 55 | off |
| Wireless sensor table | Per sensor ★ *Mine* / ○ *Received* / ✕ *Hidden* and the order of the ★ sensors (arrows); a ★ row also has the **custom ID**, the radio ID, the internal ID and a *Re-bind* select (battery change) — and a *replaces?* notice above the table — see [Mine, Received, Hidden](wireless-sensors.md#mine-received-hidden). **Live:** while the page is open, newly received sensors are appended every 5 s and signal / values refreshed; fields you are editing (name, state) are left alone. Sensors that went silent stay until the page is reloaded | – |
| Hidden (IDs as text) *(advanced)* | The ✕ list, max 32: never stored, numbered or output. Portal field `rfignore`; parameter 56 (value 0) empties it | – |
| Source: temperature / humidity, wind, rain, UV, light, pressure | Which sensor feeds each value into all outputs: *Auto* (board first, then the ★ sensors in their order; without ★: wireless 1–4), *Board sensor*, *Wireless 1–32* (the sensors received or ★, with name and ID), *Off* — see [Value Sources](wireless-sensors.md#value-sources). Portal field `srcs`: six values `air,wind,rain,uv,light,pressure` (a value left out keeps its setting) | Auto |
| Wind / rain sensor (board) | The PCB's anemometer / rain gauge — switch on when one is connected (a wireless sensor delivers wind/rain independently, see value sources) | off |
| Keep rain statistics over a power cut | Saves rain today / yesterday / week / month / year, sunshine today / yesterday and the last-rain time (one 68-byte record) to flash at most once per hour when changed, at the day change and before every planned restart or update — so they survive power cuts and firmware updates; off = RTC RAM only (lost on power loss) | on |
| LiPo measurement | Battery voltage measurement | off |
| Sunshine duration | *Illuminance ≥ 15000 lx* or *solar radiation ≥ 0.4 × clear-sky maximum* (WMO-like; needs a W/m² sensor — Davis solar radiation — as the light source and latitude / longitude, otherwise the illuminance threshold is used) — see [Sun and Moon](values.md#sun-and-moon). Parameter 65 | illuminance |
| Altitude | Station altitude in m, for sea-level pressure (QNH); kept up to date by a [GNSS module](#gnss-module) when *Take the altitude* is on | 560 |
| Heltec board | Shown only: Heltec WiFi LoRa 32 V4 (the firmware runs on nothing else) | – |
| PCB revision | Shown only: *V4 Full (detected)* or *V4 Light (detected)* from the revision ID on GPIO15 (see [Board detection](pcb.md#inputs-and-outputs-v4-full)), otherwise none — the node then runs as a wireless station (no board wind / rain sensors, no PCB inputs and outputs) | – |
| CPU frequency | 240 MHz (faster web interface) or 80 MHz (saves power, e.g. on battery / solar) | 240 |
| Debug mode | Adds diagnostic fields (cycle counter, free heap, uptime; CPU temperature in the LoRaWAN payload — the JSON/MQTT data always contain it) | off |
| **Units & calibration** | | |
| Units | Metric or Imperial — see [Units](values.md#units-metricimperial) | Metric |
| Wind speed unit | km/h or m/s (metric only) | m/s |
| Wind direction offset | North correction in degrees (−180 to +180) | 0 |
| Rain per tip / wind per pulse | Sensor ratios — see [Using Other Sensors](pcb.md#using-other-sensors) | Davis values |
| Temperature / humidity / pressure offset | Added to the main value **after the value source** — board sensor or wireless alike: temperature −10…+10 K, humidity −20…+20 % (result kept within 0–100 %), pressure −20…+20 hPa on the **station pressure before** the sea-level (QNH) reduction. Parameters 59 / 60 / 61 (tenths) | 0 |
| Rain factor / wind speed factor | 0.5–2 (1 = unchanged); multiplies every rain increment / wind speed and gust of the selected source. Parameters 62 / 63 (thousandths) | 1 |
| Per wireless sensor | Offsets and factors per sensor ID: `ID:temperature:humidity[:rain:wind:UV:light[:wn20 or :wh40][:ad=AD0-AD100]]`, comma-separated (offsets in K and %, factors 0.5–2; humidity and the factors optional), e.g. `1A2B3C4D:-0.4:+3`, `900181A2:0:0:0.95:1.05:1:1`, `20001234:0:0:0.908` (WN20); up to 32 sensors (about 15 bytes each, 40 with factors) — see [Calibration](#calibration) | – |
| **Inputs & outputs (V4 Full)** — hidden on the V4 Light and without PCB | Details: [Inputs and Outputs](pcb.md#inputs-and-outputs-v4-full) | |
| AUX analog input, scale | GPIO4; scale = V at the terminal per V at the pin (1.66667 with R25 = 15 kΩ) | off, 1.0 |
| UV sensor, divider scale | GPIO3; terminal voltage per ADC voltage, matching JP10 (1–2: 0–3.3 V, 1.1; 2–3: 0–5 V, 1.66534) | off, 1.1 |
| UV index enabled, calibration verified | Index is reported only when both switches are on and calibration is valid | both off |
| UV index slope, offset | Linear conversion of terminal volts into index; slope 0.001–30 index/V, offset −10…+10 | 1.0, 0.0 (not calibrated) |
| AUX pulse counter | GPIO48 | off |
| 2nd wind sensor, vane offset | off / speed (anemometer at AUX `PULSE`) / speed + direction (vane at AUX `AIN`) — see [Second Wind Sensor](pcb.md#second-wind-sensor-v4-full); takes over the AUX inputs it uses | off, 0° |
| Heater mode, power, thresholds | off / on / auto; auto: on below the switch-on temperature with the start power, rising linearly to full power at the full-power temperature | off, 100 %, 4.0 °C / 10 %, −10.0 °C / 100 % |
| Fan mode, speed, threshold, minimum PV power | off / on / auto (from the threshold); PWM 1–100 %; °C; W (VE.Direct, 0 = ignore) | off, 100 %, 15.0 °C, 0 |
| Fan tacho, pulses per revolution | GPIO40 | off, 2 |
| Victron VE.Direct | GPIO33 (RX; TX GPIO45, unused) | off |
| Minimum battery voltage for heater/fan | V (VE.Direct), 0 = no limit | 0 |
| **Loxone (UDP)** | see [Loxone](loxone.md) | |
| Loxone UDP | on/off, Miniserver IP or host name, UDP port, name prefix (`A–Z a–z 0–9 _ . -`, max 16), interval (`0` = every cycle, else 2–3600 s) | off, –, 7000, –, 0 |
| **Wi-Fi + MQTT** | | |
| Wi-Fi / web API | On/off switches; off from the web settings only with a confirmation and LoRaWAN joined — see [Switching Wi-Fi or the web interface off](#switching-wi-fi-or-the-web-interface-off) | on |
| MQTT / MQTT commands | On/off switches — see [MQTT and Home Assistant](mqtt-home-assistant.md) | off |
| Device name | Hostname for Wi-Fi/DHCP and mDNS (`<hostname>.local`): lower case, characters outside `a–z 0–9` → `-` (runs collapsed, none at the start or end), at most 31 characters (`"Weather Mast"` → `weather-mast`); without a name `weathernode-<last 6 MAC hex digits>`. Also the MQTT client ID (characters outside `A–Z a–z 0–9 _ -` become `_`) and default topic prefix | – |
| Wi-Fi network and password | Portal (*Configure WiFi*) or web settings (tested for 60 s with rollback, or *store without test* for pre-provisioning) | – |
| Static IP, gateway, subnet, DNS | Optional fixed LAN address; leave disabled to use DHCP. Configure in the portal's Wi-Fi form or the web settings page (restart required) | DHCP; subnet 255.255.255.0; DNS defaults to gateway |
| Own access point | Off / fallback / always, network name, password, channel — see [Own access point](#own-access-point) | off |
| MQTT broker, port, user, password, topic prefix, TLS mode, CA certificate | Broker connection | port 1883, no TLS |
| Web settings admin password | Required for the settings page and the log | – |
| **Web settings page only** | | |
| Weather networks | Station IDs, keys and intervals (AWEKAS, Weather Underground, Weathercloud, Wetternetzwerk.pro, Windy, openSenseMap, Met Office WOW, CWOP) — see [Weather Networks](weather-networks.md) | off |
| Custom uploads 1–2 | URL template, GET / POST, body template, interval 60–3600 s — see [Custom Upload](weather-networks.md#custom-upload) | off, 300 s |
| InfluxDB / VictoriaMetrics push | Write URL (v1 `…/write?db=`, v2 `…/api/v2/write?org=&bucket=`, VictoriaMetrics `…/write`), Authorization (a bare token gets `Token ` in front), measurement (wireless sensors: `<measurement>_rf`; custom series as fields `series_<slot>` of the main line), interval 10–3600 s. `rain_total` is a running total on both lines — main line: the selected rain source since the start (0 after a rain reset; same as `/metrics` `uwn_rain_total_mm`), `_rf` lines: the sensor's own counter — so the rain of a period is its increase (`difference()` / `non_negative_difference()`), not the sum of the points. Line protocol with the timestamp of the moment the values were taken. **Backfill:** every push goes through a 512 KB queue in the PSRAM; when the database or the network is unreachable (no Wi-Fi, no reply, HTTP 5xx, 401 / 403 / 404 / 408 / 429) the pushes stay there and are retried after 30 s, doubling up to 10 min; once it answers, the backlog is sent oldest first in requests of up to 8 KB (one per second) with the original timestamps, then normal pushes go on. A push the database refuses (other 4xx, e.g. 400 bad line) is dropped and logged. A full queue drops the oldest push — at ~1 KB per push (main line + a few wireless sensors) it holds about 8 h at 60 s, 1.5 h at 10 s, longer at longer intervals. The queue is in RAM only: a restart or power cut loses it (no flash writes). Status page (*Weather networks → InfluxDB*) and `/api/info` `outputs.status.influxdb.backlog` show its size, the oldest push's age and the dropped counts | off, –, –, `weather`, 60 s |
| Network sensors | Advanced (*Sensors* tab). Up to 4: source off / MQTT (a topic on the broker of the MQTT output) / REST (a polled URL), format (JSON with a key per value, plain number per topic, or the *Universal Weather Node* preset), topic / URL, freshness 1–1440 min, REST poll interval 30–3600 s, a key per value; IDs E1000001–E1000004, made ★ when switched on — see [Network Sensors](network-sensors.md). In the backup like every setting | off, 10 min, 60 s |
| Virtual sensors | Advanced. On / off, transmit power 0–14 dBm (default 10), up to 8 sensors: type (WH31 with channel 1–8, WH40, WN32P, WH65, IT+ TX29 / TX35), on / off, and a value name per field; the ID is derived from the node and shown read-only — see [Virtual Sensors](virtual-sensors.md). In the backup like every setting | off, 10 dBm |
| Alarms | Rules and channels — see [Alarms and Notifications](alarms.md); stored in their own flash records (namespaces `alarms`, `cupload` for the custom uploads) | none |
| Firmware update check, version list URL | Daily check for a newer version — see [Firmware Update Check](values.md#firmware-update-check) | off; this project's GitHub releases |
| Latitude / longitude | Decimal degrees; a decimal comma is accepted (`49,88` = `49.88`, also in the setup wizard). Sun and moon times computed on the node ([Sun and Moon](values.md#sun-and-moon)) and sent to AWEKAS; nowhere else. Kept up to date by a [GNSS module](#gnss-module) when *Take latitude / longitude* is on | – |
| Reset values | Buttons (with a confirmation, at once, no *Save*): *Reset records (today to this year)*, *Reset all records (incl. all-time)* — also parameter 64 — *Delete chart history* (all ranges — 24 h, 35 days, 2 years — also the copy in flash; includes the values of the [custom series](#custom-series), their definitions stay) and *Reset rain statistics* (today, yesterday, week, month, year, last rain and *rain since start* to 0, also the flash copy; the sunshine duration and the per-sensor rain of wireless sensors are not affected), *Reset sunshine duration* (today, yesterday) and, on the V4 Full, *Reset AUX pulse total* — see [Records](values.md#records) | – |
| Backup and restore | see [Backup and Restore](#backup-and-restore) | – |
| Time zone, NTP server | POSIX time zone string and time server. The time zone also applies without Wi-Fi (LoRaWAN / DCF77 time); the time sources and their order (GNSS, NTP, DCF77 sensor, phone, LoRaWAN network time): [LoRaWAN → Time](lorawan.md#time-sources-and-network-time-devicetimereq); a phone on the [own access point](airfield.md) sets the time too. Status page: *Clock* row (source, age) | Central Europe, `pool.ntp.org` |

### Custom series

*Settings → Sensors → Custom series* — values that are none of the weather sources (soil moisture, lightning, leaf wetness, particulate matter, CO₂, a temperature probe …) get their **own chart** on the dashboard (24 h, 35 days and 2 years, like the other histories) and go out like the other values. **Up to 32 series**; each slot is a collapsible block — the defined ones open, the free ones closed; the title shows the name and the placeholder. Per series:

| Field | Meaning |
|-------|---------|
| Name | free text, the chart title and the Home Assistant entity name (empty: the value's name) |
| Source | a ★ wireless sensor (plus a configured one that is no longer ★), or *Board* for the DS18B20 probes; *off* frees the slot |
| Value | temperature, humidity, soil moisture, leaf wetness, **lightning** (strikes per bucket — from the sensor's running counter; the chart title shows the range total), **storm distance** (closest per bucket), PM2.5, PM10, CO₂, soil conductivity, pressure, illuminance, UV index, battery voltage, super capacitor, DS18B20 probe 1–3, laser distance (LDS01). The hint shows the value now — "no value" if the sensor does not send it |
| Aggregation | how the measurement cycles are combined into a bucket (5 min, 1 h), the hours into a day and the buckets into one chart point on long ranges — the same rule in every step: **mean**, **minimum**, **maximum** or **sum**. Preselected: the value's default (lightning: sum, storm distance: minimum, else mean); only a different choice is stored |

**Outputs** (the value of the last measurement cycle; lightning: the strikes since the cycle before): placeholder **`{series_<slot>}`** in custom uploads, alarm texts and OLED screens (slot 1–32 as on the settings page; temperatures take `:c` / `:f`, pressure `:hpa` / `:inhg`, all `.0`–`.4` — see [Placeholders](weather-networks.md#placeholders); `series` is therefore not allowed as a custom ID), `/api/data` and the MQTT data topic as `series[]` ([API](api.md#fields-units-and-numbers)), one Home Assistant sensor per series ([MQTT / Home Assistant](mqtt-home-assistant.md#home-assistant)), the InfluxDB field `series_<slot>` and the `/metrics` gauge `uwn_series{slot,name,field,unit}`.

**Memory:** the rings take 3.6 KB of PSRAM per series that is set up (116 KB for 32; 4 series without a running PSRAM); the definitions live in the PSRAM too and are saved with the settings. The history of the series is saved with the rest of the history (each tier when it has changed, the 5-min values every 30 min, and before a planned restart). **Resetting:** one series — set its source to *off* and save (its values and memory are freed), or rename it / change its source, value or aggregation (its history starts empty, the other series keep theirs); all series at once — *Settings → System → Reset values → Delete chart history* (this also empties the charts of the main values; the series' definitions stay). In `/api/history` the series come as `series` ([schema](api/schemas/history.schema.json)).

### Display

*Settings → Display* — the Heltec's built-in 128 × 64 OLED. All live.

| Setting | Meaning | Default |
|---------|---------|---------|
| Display on | **for 20 s after PRG** (as before), **always** (except the night hours), or **for N minutes after PRG** | 20 s after PRG |
| Minutes after PRG | for the third mode, 1–120 | 5 |
| Screens | **custom screens 1 and 2** (see below) and **classic status** (wind, rain, temperature, battery, LoRaWAN uplink result or IP / receiver state — the screen of older firmware), **temperature** (big, with humidity, dew point, feels like), **wind** (mean, gust, compass point and degrees), **rain** (today, last hour, rate), **pressure** (QNH, 3-h change with an arrow and the weather trend word), **status** (IP and RSSI, `.local` name, number of wireless sensors and active alarms), **WiFi QR code** (only while the [own access point](#own-access-point) runs: the join code on the left, name, password, address and devices on the right; it comes first when the display is switched on) | classic + WiFi QR code |
| Next screen every | rotation through the ticked screens, 2–60 s; while the display is on, a short PRG press shows the next screen | 5 s |
| Brightness | OLED contrast 1–255 | 200 |
| Orientation | **normal** (as older firmware drew it) or **rotated by 180°** — e.g. a Heltec V4 held with the RST button bottom left, as Meshtastic shows it | normal |
| Battery voltage top right | the voltage of a LiPo on the Heltec module's battery connector at the end of the title line of every screen (*USB* without a battery; on the classic screen in its battery line). Measured once per send cycle, also with the LiPo measurement (*Sensors*) off — that one still decides whether LoRaWAN / MQTT send it | off |
| Status LED | the Heltec's white LED (GPIO35, not used by the PCBs), also with the display off: **heartbeat (2 s)** — a 40-ms flash every 2 s while the firmware's main loop runs (a hang or crash stops it); **reception of my sensors (★)** — a short flash for every valid packet of a ★ sensor (dark while no sensor is ★); **every reception** — a flash for every valid packet, neighbours' included. Valid = checksum ok and not hidden; Davis mode: every ISS packet. LoRaWAN does not use the LED | off |
| Screensaver | takes over the display now and then so the always-lit pixels rest (burn-in; a shifted layout would not help, a full-width line stays in almost the same row): **off**, **brief blank**, **moving dot**, **walking figure** or **weather icons** (sun → cloud → rain, drifting across). The takeover itself is what rests the pixels; the figure is the fun variant. Only while the display is on (*always* or *N minutes* mode) and not in the night hours; a PRG press ends it at once. *Preview screensaver* plays a takeover on the settings page with the current values | off |
| Screensaver every / lasts | minutes between two takeovers, 1–60, and the length of one, 3–30 s | 5 / 8 |
| Night off from / until | local hours 0–23 (e.g. 22 and 6); equal = never. Against burn-in and to save power in *always* mode; a PRG press still switches the display on for 20 s | 0 / 0 |

#### Screensaver: the walking figure

A small figure walks across the display and reacts to what the node measures; for each condition there is a pool of animations, one is picked at random, and about one in ten takeovers is a rare surprise (dancing, a UFO). Several conditions at once: one of them is picked at random; a storm (lightning sensor) always wins; *night* (21–6 h) and *mild* only show when nothing else applies.

| Condition | Holds when | Animations |
|---|---|---|
| Frost | temperature < 0 °C | scarf, shivering and breath puffs · snowball throw |
| Thaw | above 0 °C (up to 8 °C) within 12 h after a frost | jumping in the slush · waving at a melting snowman |
| Mild | 0–27 °C, or no temperature | whistling under a drifting cloud · waving next to a flower |
| Hot | ≥ 27 °C | fanning himself, sweat drops · cocktail with sunglasses |
| High UV | UV index ≥ 6 (wireless sensor) | sunscreen under a hat |
| Rain | rain rate ≥ 0.1 mm/h or last hour ≥ 0.2 mm | umbrella · catching drops in the palm |
| Wind | gust ≥ 10 m/s or mean ≥ 7 m/s | leaning into it, scarf and leaves blowing · the hat flies off |
| Lightning | WH57 reports a storm front (heard within 15 min) | running under a stormy sky, bolts and a screen flash |
| Fog | humidity ≥ 93 % | torch with a beam |
| Bad air | PM2.5 > 35 µg/m³ or CO₂ > 1500 ppm (WH41 / WH43 / WH45 / WH46) | coughing · putting on a mask |
| Dry soil | a WH51 below 25 % | watering a wilted plant |
| Night | 21–6 h | moon and yawn · sleepwalking in a nightcap with Zzz |

#### Custom screens

Two screens with lines of your own, each with a **title** and a **layout**: *title + 5 small lines*, or *title + large value + 2 lines* (line 1 in large type, about 8 characters). Tick them under *Screens* to include them in the rotation. Below each one a **preview** shows it with the current values, also before saving: drawn when the tab is opened and redrawn shortly after you stop typing (or with *Update preview*).

A line is text with placeholders, as for the [custom uploads](weather-networks.md): `Garden {garten_temp:.1}{u_temp}  {garten_hum:.0} %` (`garten` = the custom ID of a ★ sensor).

| Placeholders | |
|---|---|
| `{temp} {hum} {dew} {feels} {qnh} {press} {ptend3h}` | the main values (selected sources) |
| `{wind} {gust} {dir}` · `{rain_day} {rain_1h} {rain_24h} {rain_rate}` · `{uv} {lux} {solar}` | |
| `{<custom ID>_temp} {<custom ID>_hum} {<custom ID>_wind} {<custom ID>_gust} {<custom ID>_dir} {<custom ID>_rain} {<custom ID>_uv} {<custom ID>_lux} {<custom ID>_batt}` | a ★ wireless sensor by its custom ID (e.g. `{17000008_temp}`). Saved as `{@<internal ID>_temp}` and shown with the custom ID again, so it keeps meaning that sensor when the order or its custom ID changes; *Available placeholders* inserts this form. Still understood: `{rf<ID>_temp}` (8-digit radio ID, the form for a sensor that is not ★) and `{rfN_temp}` (number 1–32 in the sensor list) — a ★ sensor's is converted on the next save |
| `{clock} {date} {date_full} {ip}` | local time HH:MM, date (DD.MM. / MM/DD), date with year (DD.MM.YYYY / MM/DD/YYYY), IP address (display only) |
| `{u_temp} {u_wind} {u_rain} {u_rate} {u_press}` | the device's unit labels (display only) |

Above the first screen's fields, *Available placeholders* lists every placeholder that has a value right now, with that value; a click inserts it into the field used last. The custom uploads (*Outputs*) and the alarm texts have the same list.

Unlike the uploads, values come **in the device's units** (*Basics → Units*). A suffix sets decimals or another unit: `{temp:.0}`, `{wind:kmh}`, `{qnh:inhg:.1}`. A value that is not there right now — or a misspelt name — shows as `--`. A small line holds about 20 characters (128 pixels, proportional font); the preview shows what fits. Title up to 23, lines up to 63 bytes of template text.

Texts on the screens follow the device language (*Settings → Basics → Language*; *Auto* = English). Existing nodes keep the old behaviour until something is changed here.

### Web UI

*Settings → System → Web UI.* Options for the node's own dashboard in the browser; both are **off** by default and apply when the dashboard is reloaded (the dashboard reads them once from `/api/info` → `ui`). The *Live* / *every N s* badge next to *Updated* is always shown (and nothing with auto-refresh *Off*).

| Setting | Meaning | Default |
|---------|---------|---------|
| Flash tiles on change | a tile briefly highlights (about 1 s) when its displayed value changes, from the live stream or a refresh; not on the first display, not after a unit or range switch, and not when the browser asks for reduced motion | off |
| Show age of values | each tile shows how old its value is ("3 s ago", "2 min ago"), counting up live. A value from a wireless sensor uses that sensor's last packet; a board value has no time stamp of its own and uses the time since the last measurement cycle | off |

### HTTPS

*Settings → System → HTTPS* (restart). Optional: the web interface also on **`https://`** (port 443) — with a certificate you provide, or without one with a **self-signed certificate the node generates** itself. **HTTP stays the default** and is never taken away by a broken certificate. What HTTPS adds in the own LAN is modest: the admin password never crosses the network anyway (Digest login), but settings you type — Wi-Fi, MQTT and weather-network passwords — and the pages themselves then travel encrypted, and the browser counts the page as a *secure context* (e.g. it may offer the location in the [setup wizard](installation.md#setup-wizard)). Over plain HTTP the login itself is guarded — every Digest nonce count is accepted once, so a recorded admin request cannot be sent again, and wrong passwords are braked per client address (after 5 wrong ones each further try must wait 1 s, then 2, 4 … up to a minute; a try that comes too early gets `429` with `Retry-After`, without slowing anyone else down) — but whoever can read your LAN traffic still sees every page and every value you send. On a network you do not trust (shared or guest Wi-Fi, other people's devices in the same LAN) administer the node over HTTPS.

| Field | Meaning | Default |
|-------|---------|---------|
| HTTPS on port 443 | starts the HTTPS server at the next start — with your certificate below if it is usable, else with a [generated one](#generated-certificate); only with enough free memory; otherwise the node stays on HTTP and the status page says why | off |
| HTTP on port 80 | **stays available** — both work. **Redirects browser pages** — a browser opening a page (a `GET` that accepts `text/html`) gets a `307` to the same address on `https://`; the API, `/metrics`, scripts, form posts and firmware uploads keep working on `http://`. **HTTPS only** — *every* plain request is redirected, the API and a firmware upload too (answered before the upload is read); integrations that read `http://` (Home Assistant, Prometheus, the public-dashboard puller, scripts) then need an `https://` address and the certificate (your CA, or the generated one). `307` is temporary on purpose: switch HTTPS off again and the browser goes back to HTTP — no HSTS either. Devices on the [own access point](#own-access-point) and the config portal always stay on HTTP, and **while HTTPS does not run, port 80 serves everything** whatever is set here | stays available |
| Active certificate | (while HTTPS is enabled) which certificate runs — *uploaded* or *self-signed (generated)* — with its name (CN), its names (SAN), end date, the port-80 mode, or why HTTPS does not run. A generated one adds **Download CA certificate** and **Generate new certificate** | – |
| Certificate (PEM) | your certificate, then any intermediate certificates (up to 8 KB). Its **Subject Alternative Names** must contain every address you type in the browser (`weathernode.local`, the IP …). Emptying the field removes certificate and key (the node then uses a generated certificate) | – |
| Private key (PEM) | the matching private key, **without a password** — EC P-256 (recommended: fast handshake) or RSA 2048–4096 (up to 4 KB). Write-only like a password: never shown again, empty keeps the stored one; *remove certificate and key* deletes both | – |

On *Save* the pair is checked before anything is stored: PEM blocks complete, no private key in the certificate field (it would be shown on this page and land in every backup), no password-protected key, and then by mbedTLS — the certificate and the key parse and **the key belongs to the certificate**. A failed check names the reason (and the mbedTLS code) and keeps the stored pair. The field *Uploaded certificate* shows its name (CN), key type and end date (*expired* once the clock is set and the date has passed — the node still uses it; the browser will warn).

**Make a certificate.** Without one the node uses a [generated self-signed certificate](#generated-certificate) — fine for one node and a few devices, but each device has to trust it, and again after every new one. Better for several nodes or names: a **small CA of your own** — you trust it once on each device, and then every certificate it signs is accepted, including the next one for this node. With OpenSSL (Linux, macOS, Windows with Git Bash):

```sh
# once: your own CA (keep ca.key private and safe; 10 years)
openssl req -x509 -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 3650 \
  -keyout ca.key -out ca.crt -subj "/CN=Weather node CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"

# the node: key + certificate signed by the CA - put every name / IP you use into subjectAltName
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
  -keyout node.key -out node.csr -subj "/CN=weathernode"
printf "subjectAltName=DNS:weathernode.local,IP:192.168.1.20\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n" > node.ext
openssl x509 -req -in node.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 825 -extfile node.ext -out node.crt
```

Paste `node.crt` into *Certificate* and `node.key` into *Private key*, tick *HTTPS on port 443*, save — the node restarts. 825 days is the longest validity Apple devices accept; make the next one before it ends. A certificate from a real CA (e.g. Let's Encrypt with a DNS challenge for a name you own) needs no trust step: paste its *fullchain* as the certificate.

**Trust the CA** (`ca.crt`, never `ca.key`) once per device: *Windows* — double-click → *Install certificate* → *Local machine* → *Trusted Root Certification Authorities*; *macOS* — open it in Keychain Access, add to *System*, then *Get Info → Trust → Always Trust*; *Linux* — copy to `/usr/local/share/ca-certificates/` and run `sudo update-ca-certificates` (Firefox: *Settings → Privacy & Security → Certificates → Import*); *Android* — *Settings → Security → Encryption & credentials → Install a certificate → CA certificate*; *iOS / iPadOS* — send the file to the device, install the profile (*Settings → General → VPN & Device Management*), then switch it on under *Settings → General → About → Certificate Trust Settings*. For scripts: `curl --cacert ca.crt https://weathernode.local/api/data`.

#### Generated certificate

Enable HTTPS without uploading a certificate (or with one that is not usable) and the node makes its own at the start: an **EC P-256 key and a self-signed certificate**, named **"Universal Weather Node <device name>"** (organisation "Universal Weather Node" — that is how certificate managers and phones list it once trusted), Subject Alternative Names **`<hostname>.local`** and the node's current **Wi-Fi address**, valid 825 days, a random serial. It is stored next to the settings (never over an uploaded pair) and used again at every start as long as it still names the host name, the current address and the current device name and has not expired; otherwise the next start makes a new one. It is made only once the node has its Wi-Fi address (while only the [own access point](#own-access-point) is up, HTTPS waits and starts as soon as Wi-Fi connects). If the address changes later (a new DHCP lease), the status line says *the certificate does not name the current address* — press **Generate new certificate** (or give the node a fixed address / DHCP reservation, so this never happens). Generating takes about 0.25 s on the V4.

**The browser warns** about a self-signed certificate (*not private* / *not secure*) until you trust it, and Chrome registers no service worker for such a page (no [app install, no offline cache](web-dashboard.md#install-as-an-app-home-screen)). **Trust it once per device** — then the warning is gone and the app install works. Get the file with **Download CA certificate** in *Settings → System → HTTPS* (shown only for the generated certificate — an uploaded one comes from your own CA, trust that instead), or from `http://<node>/api/https/certificate` (public, it is only the certificate; the private key never leaves the node), then:

- **Windows:** double-click `Universal-Weather-Node-<host>-CA.crt` → *Install certificate* → *Local machine* → *Place all certificates in the following store* → *Trusted Root Certification Authorities*. Chrome and Edge use it; restart the browser.
- **macOS:** double-click it (Keychain Access opens) → add to *System* → double-click the entry → *Trust* → *When using this certificate: Always Trust*. Safari and Chrome use it.
- **Linux:** `sudo cp Universal-Weather-Node-<host>-CA.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates` (Debian / Ubuntu; Fedora: `/etc/pki/ca-trust/source/anchors/` + `sudo update-ca-trust`). Chrome on Linux uses its own NSS store: *Settings → Privacy and security → Security → Manage certificates → Authorities → Import*.
- **Android:** *Settings → Security → Encryption & credentials → Install a certificate → CA certificate* → pick the file (the menu path varies by maker). Chrome uses it; most apps do not.
- **iOS / iPadOS:** open the download link in Safari → *Allow* the profile → *Settings → General → VPN & Device Management* → install it → then *Settings → General → About → Certificate Trust Settings* → switch it on.
- **Firefox** has its own store: *Settings → Privacy & Security → Certificates → View Certificates → Authorities → Import* — or accept the warning once as an exception.
- **Scripts:** `curl --cacert Universal-Weather-Node-<host>-CA.crt https://<host>.local/api/data`.

The certificate is marked as its own CA (as Android and iOS require for a trusted certificate) and is good for nothing but this node — but whoever has a device that trusts it and could get the node's key could impersonate the node to that device. A **new** certificate (after *Generate new certificate*, a new name or address, *Reset settings*) has to be trusted again; remove the old one from the device. Not in the [settings backup](#backup-and-restore): a restored or replaced node generates its own. For many devices, or names the node cannot know (a reverse proxy, a DNS name of your own), use [your own CA](#https) above.

**If HTTPS does not start** (waiting for the Wi-Fi address, generating a certificate failed, too little free memory, the server start failed) the web interface runs on plain HTTP as before, port 80 does not redirect, and the status page shows an *HTTPS* line with the reason (also `/api/info` → `outputs.https`: `off`, `on`, `waitingForNetwork`, `generateFailed`, `lowMemory`, `startFailed`; `outputs.httpsCertificate`: `uploaded` / `generated` while it runs). An uploaded pair that does not parse or match is not used: the node runs with a generated one and the line says so. Shut out anyway (*redirects* or *HTTPS only* and a certificate your browser refuses)? Most browsers still let you proceed past the warning (the node sends no HSTS); else open `http://192.168.4.1/` from a device on the [own access point](#own-access-point) and switch HTTPS off, or as the last resort [Reset settings](#reset) — it also deletes certificate and key, the generated pair too.

**Limits and memory.** HTTPS runs as a second server task next to the plain one: about **21 KB internal RAM** while it runs (its 12 KB stack, the server itself; measured on a V4: 106 → 85 KB free), started only with at least 52 KB free; each open HTTPS connection adds ~2.4 KB internal RAM and ~37 KB PSRAM (the TLS buffers). At most **5 HTTPS connections** at a time (the browser reuses them; the oldest idle one makes room), of them at most **2 live streams** (SSE) — a third dashboard over HTTPS refreshes by polling. A new connection costs a TLS handshake (EC P-256: about 0.4 s on the V4; RSA longer). Requests of both servers are handled one at a time, as before. TLS 1.2. Not in the [settings backup](#backup-and-restore) without *include passwords and keys*: the uploaded certificate is in every backup, its key only with that box ticked; a restore checks the pair like a manual save. The generated pair is never in a backup. Generating a certificate costs no lasting memory (about 6 KB of the web task's stack for 0.25 s, ~1 KB PSRAM; the code adds ~20 KB to the firmware). The setup (config) portal is always plain HTTP. The node announces `_https._tcp` over mDNS while HTTPS runs.

### Calibration

*Settings → Sensors → Sensor calibration.* The main-value corrections (offsets for temperature, humidity and station pressure, factors for rain and wind speed) are applied **after the value source was chosen**, so they correct whatever feeds the value — the board sensors or a wireless sensor — and **every output sees the corrected value**: status page, dashboard and history, `/api/data`, MQTT / Home Assistant, LoRaWAN, Loxone, the weather networks, the records and the derived values (dew point, QNH, rain statistics …). The pressure offset is added to the station pressure; QNH is then computed from the corrected value.

The **per-sensor** offsets (`ID:temperature:humidity`) and the **wind, UV and light factors** are applied when a packet of that sensor is stored. The per-sensor **rain factor** multiplies the counter's *increments* — the main rain path when that sensor is the rain source, and the sensor's rain today / last hour — never the raw counter that is shown. For a **WN20** the normal way is the rain factor **0.908** (`20001234:0:0:0.908`); **`wn20`** switches on the experimental rate-dependent WN20 correction from the Ecowitt wiki instead of a fixed rain factor (see [Wireless sensors](wireless-sensors.md#names-ecowitt-ids-rain-and-lightning-per-sensor)). Entries saved by older firmware (`ID:t:h`) keep working. They therefore also correct that sensor's raw values: `rfSensors` in `/api/data`, `<prefix>/rf/<key>`, its Home Assistant device and the status page. If the sensor is the selected source, the main offset comes on top — use one or the other for a sensor that is a value source. Wind direction, rain per tip, wind per pulse and the UV calibration stay where they were.

### Backup and Restore

*Settings → System → Backup and restore.*

- **Download settings** saves `uwn-settings-<host>-<date>.json` with every field of the settings page (`settings`: the form field names of this page with their values; `format`, `version` = 2, `firmware`, `host`, `date`, `secrets`) and the ★ sensors (`sensors`: per [logical sensor](wireless-sensors.md#mine-received-hidden) `internalId`, `customId`, `displayName`, `radioId`, `minRssiDbm` if set). Value sources, alarms and custom series refer to them by `internalId`. Passwords and keys — Wi-Fi, the own access point, admin, MQTT, the weather networks' keys, the alarm channels' tokens, the LoRaWAN AppKey, and the webhook / custom-upload URLs (they may carry a key) — are only included if you tick **include passwords and keys** (default off): the file then holds them in plain text, keep it private.
- **Restore settings from a file** uploads such a file. It is checked like a manual *Save* (same limits, invalid values are listed and keep their old value), the next settings page lists **what changed** (old → new; secrets only as `***`), and the node restarts if a restart field changed. The ★ sensors of the file replace the current ones first (entries that do not fit — a damaged ID, a key or radio ID twice — are skipped and counted). A **version-1** file (2.x, 3.x and 4.x before the logical sensors) restores everything else; its ★ list and its references by radio ID (value sources, wireless custom series) are ignored, not converted — those keep their current setting. A file from a newer format (`version` > 2) or another file is refused without changing anything. Fields that this node's page does not have (other hardware, older firmware) are ignored; a backup without secrets keeps the stored passwords. A different Wi-Fi network in the file is tested like a manual change (rollback after 60 s).
- Not in the backup: the LoRaWAN session and DevNonces (stay untouched), the wind-direction calibration table ([wizard](#wind-direction-calibration)), the rain statistics and records. API: `GET /api/settings/backup[?secrets=1]`, `POST /api/settings/restore` (field `backup`), both admin login and same-origin.

Saved settings survive ordinary restarts and application-only OTA updates. *Reset settings*, *Erase everything* or a full flash erase remove project settings; upgrades from the older storage layout may require re-entering them. Keep a private backup of keys, sensor calibration and settings before changing firmware or partitions.

### Reset

*Settings → System → Reset* — two steps, both admin login and a typed word (nothing happens on a stray click), both end in a restart about a second after the answer:

| | **Reset settings** (type `RESET`) | **Erase everything — hand the device on** (type `ERASE`) |
|---|---|---|
| Settings | all back to their defaults — value sources, ★ sensors, custom series, virtual and network sensors, calibration (also the wind-direction table), display, MQTT, weather networks, Loxone, InfluxDB, HTTPS certificate **and key** (uploaded and generated), alarm rules **and channels** (tokens), custom uploads | erased |
| Kept | **how the node is reached and who may administer it:** Wi-Fi network and password, static IP, device name, own access point, admin password, language, LoRaWAN switch / keys / region and the LoRaWAN session — the node comes back at its address; the [setup wizard](installation.md#setup-wizard) is offered again | nothing |
| Measurement data | kept (records, rain statistics, chart history — saved before the reset as before any restart) | erased (also the rain totals in RTC RAM) |
| Diagnostics | kept (restart history, core dump) | erased (the core dump holds RAM contents, passwords too) |
| Afterwards | normal start with the kept network settings | starts **like a new device**: the config portal (access point `WeatherNode`) opens |

Also via [downlink](lorawan.md#downlink-commands) `0xFF` and the [MQTT command](mqtt-home-assistant.md) `factoryReset`: both mean *Reset settings* — remotely the node is never wiped down to an unreachable state. *Erase everything* is on the settings page only. It also deletes the LoRaWAN session and DevNonce counter: before the node joins again with the same DevEUI, delete and re-create the device on the network server (or reset its DevNonces). The firmware itself, and the reserved `files` partition (unused by 4.x), stay.

API: `POST /api/system/reset` (`what=settings` + `confirm=RESET`, or `what=everything` + `confirm=ERASE`), admin login, same-origin. While a reset runs, no setting or measurement store can be written any more (so a save racing the reset cannot bring old data back).

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
| 32 | `0x20` | `vanePowerSave` | `0` / `1` — low-power mode also switches the wind-vane supply off (V4 Full / V4 Light) | 0 | `10 20 00 00` |
| 33 | `0x21` | `wind2Mode` | `0` off, `1` 2nd anemometer at AUX `PULSE`, `2` plus vane at AUX `AIN` (V4 Full) | 2 | `10 21 00 02` |
| 34 | `0x22` | `wind2DirOffsetDeg` | `−180`…`180`, north offset of the 2nd vane | -90 | `10 22 FF A6` |
| 35 | `0x23` | `uvEn` | `0` / `1`, UV sensor voltage (V4 Full) | 1 | `10 23 00 01` |
| 36 | `0x24` | `uvAinScale` | ×1000, `1000..5000`; terminal V per ADC V | 1100 (1.1 default; use 1665 for JP10 2–3) | `10 24 04 4C` |
| 37 | `0x25` | `uvIndexEn` | `0` / `1`, publish the calibrated index | 1 | `10 25 00 01` |
| 38 | `0x26` | `uvCalibrated` | `0` / `1`, explicit confirmation of sensor calibration | 1 | `10 26 00 01` |
| 39 | `0x27` | `uvIndexSlope` | index/V ×1000, `1..30000` | 2000 (= 2 index/V) | `10 27 07 D0` |
| 40 | `0x28` | `uvIndexOffset` | index ×1000, `−10000..10000` | −500 (= −0.5) | `10 28 FE 0C` |
| 41 | `0x29` | `mqttIntervalSec` | `0` (= LoRaWAN interval) or `2..3600` | 5 s | `10 29 00 05` |
| 42 | `0x2A` | `loraWindAgg` | `0` interval, `1` 10 min (WMO) | 0 | `10 2A 00 00` |
| 43 | `0x2B` | `mqttWindAgg` | `0` interval, `1` 10 min (WMO) | 1 | `10 2B 00 01` |
| 44 | `0x2C` | `rfEn` | `0` / `1`, 868 MHz wireless sensors (IDs only via web settings / portal) | 1 | `10 2C 00 01` |
| 45–49 | `0x2D`–`0x31` | value sources: temperature/humidity, wind, rain, UV, light | `0` auto, `1` board, `2`–`33` wireless 1–32 (n + 1), `34` off — see [Value Sources](wireless-sensors.md#value-sources) | 2 (wind from wireless 1) | `10 2E 00 02` |
| 50 | `0x32` | `rfProto` | wireless sensor type: `16` Auto (search, stores `1` / `10` / `4` after the first sensor heard), or a bit mask: `1` Bresser, `2` IT+ 17.241 kbit/s, `4` IT+ 9.579 kbit/s, `6` IT+ both, `7` Bresser + IT+, `8` Fine Offset / Ecowitt, `10` IT+ 17.241 + Fine Offset (same data rate, no alternating), `14` IT+ both + Fine Offset, `9` Bresser + Fine Offset, `15` all types (keep listening to everything); `17` Davis Vantage Pro2 / Vue (exclusive: LoRaWAN paused, experimental); any combination `1..15` (Bresser together with others = timed switching between the data rates, see [several sensor families](wireless-sensors.md#several-sensor-families-at-once)) | 6 | `10 32 00 06` |
| 51 | `0x33` | `rainPersist` | keep rain and sunshine statistics (today, yesterday, week, month, year, last rain) over power cuts and updates (`0` off / `1` on, at most one flash write per hour plus one per planned restart) | 1 | `10 33 00 01` |
| 52 | `0x34` | `loxEn` | Loxone UDP output `0` off / `1` on | 0 | `10 34 00 01` |
| 53 | `0x35` | `loxSec` | Loxone interval in s: `0` = every measurement cycle, else 2–3600 | 0 | `10 35 00 3C` |
| 54 | `0x36` | value source: pressure | `0` auto (BME280 first, then wireless 1–4 with a barometer), `1` board (BME280), `2`–`33` wireless 1–32, `34` off — see [Value Sources](wireless-sensors.md#value-sources) | 0 | `10 36 00 02` |
| 55 | `0x37` | `rfOnlyMine` | Only my sensors: `1` = accept only the ★ *Mine* sensors (no effect while there are none), `0` = all except hidden | 0 | `10 37 00 01` |
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

IDs 15–30, wind-2 mode (33) and the UV sensor (35) belong to the [V4 Full I/O block](pcb.md#inputs-and-outputs-v4-full); on the V4 Light and without PCB they are rejected except for disabling a function. Calibration values can be prepared where accepted, but do not provide the missing hardware.

Triplets are processed in order, not as an atomic transaction: valid changes are saved even if another triplet is invalid. An uplink or gateway TX_ACK does not confirm that a parameter was applied; check the device log or settings.

**Several at once:** interval 300 s plus debug on → `10 01 01 2C 06 00 01`
