# Changelog

Release notes of the Universal Weather Node firmware, newest first. Binaries are in [`firmware/`](firmware/).

[← back to README](README.md)

## 2.0.0-rc.7

Firmware: [`universal-weather-node-2.0.0-rc.7.bin`](firmware/universal-weather-node-2.0.0-rc.7.bin) (OTA / USB at `0x10000`) and [`universal-weather-node-2.0.0-rc.7-factory.bin`](firmware/universal-weather-node-2.0.0-rc.7-factory.bin) (first installation, USB at `0x0`). A small fix release on top of rc.6; nothing to check after the update.

- **Settings storage with data of another firmware:** if *cfgdata* is not usable as settings storage — e.g. after Meshtastic, whose file system lies at the same flash address, and flashing only the factory image without erasing — the node now erases that partition at the start and comes up like a new one (setup portal) instead of failing to start. Erasing the whole chip when flashing (ESPHome web flasher: *Erase device*) still is the cleanest way.

## 2.0.0-rc.6

Firmware: [`universal-weather-node-2.0.0-rc.6.bin`](firmware/universal-weather-node-2.0.0-rc.6.bin) (OTA / USB at `0x10000`) and [`universal-weather-node-2.0.0-rc.6-factory.bin`](firmware/universal-weather-node-2.0.0-rc.6-factory.bin) (first installation, USB at `0x0`). Update from rc.5 under Settings → System → Firmware update; from rc.4 or older through the config portal (in a normal browser, see [installation](docs/installation.md#firmware-update-ota)).

**Check after the update** (release candidate: no conversion of old settings):
- a **value source set to *Off*** has to be set again (the value changed from `6` to `34`; otherwise it reads *Wireless 5*);
- value sources on ***Auto*** with ★ sensors now follow the order of your ★ list (arrows) — put the sensor that should feed the outputs first;
- wrong **rain-rate records** from a short MQTT / Loxone cycle can be cleared under Settings → System → Records.

- **Radio trace moved to the log page:** start, stop, download and discard are now on the password-protected log page (`/api/log`, box *Radio trace*); the status page keeps one compact row with the state and progress of a capture and a link there, so visitors no longer see buttons that end in a password prompt. After start / stop / discard the browser returns to the log page. The hex-ID field only appears when the filter *only this ID* is chosen. `/api/rf/trace` and `/api/davis/log` are unchanged.
- **Chart history kept across restarts and updates:** the 7-day (hourly) values are saved to the settings flash every 24 h, both ranges (24 h and 7 days) before every planned restart — firmware update, settings that need a restart, restart command. After the start they are restored as soon as the clock is synced; the time the node was off shows as gaps. The 24-h values are only a hand-over (removed after the restore), so a power cut loses them and at most a day of hourly values. The settings always come first: the history is not saved if less than 16 KB of the 64-KB settings partition would stay free. New in `/api/info`: `system.settingsStoreUsed` / `settingsStoreFree`.
- **Rain rate over at least 5 minutes:** with a short cycle (MQTT or Loxone every few seconds) the rate was the rain of that cycle divided by its length, so one 0.2-mm tip in a 10-s cycle read 72 mm/h (two tips 144 mm/h) — in the status page, records, chart history, MQTT, weather networks, alarms and on the display. The rate now uses a rolling window of at least 5 minutes (a send window that is already 5 minutes or longer keeps its own); amounts are unchanged. Wrong rain-rate records from before can be cleared under Settings → System → Records (reset).
- **Readable links in dark mode:** links (also visited ones) use the teal accent colour instead of the browser's default blue / purple on all pages; the help page and the wind-direction calibration now follow the dark theme too (help: also the theme chosen on the dashboard), and form controls and scrollbars match the theme (`color-scheme`).
- **Bresser together with Ecowitt / Froggit or IT+ (timed switching):** instead of plain alternating (about half of the packets missed) the receiver learns each sensor's send interval from its packets and switches to that sensor's data rate about 1 s before its next packet is due; in between the data rates are listened to in turn as before (finds new sensors). With sensors marked ★ only those are followed. Simulated 98–100 % of the packets for a Bresser station with a WH65, WS80 or WH31; field test pending. Status page: *timed reception for n sensors*. New module `src/rf_schedule.cpp` (host-tested).
- **Wireless sensor table updates itself:** under Settings → Sensors newly received sensors are appended every 5 s and signal / values refreshed while the page is open (new `GET /api/settings/rfsensors`, the table alone); cells with a field — a name being typed, a changed state — are never replaced. It only refreshes while the *Sensors* tab is shown and no firmware upload runs (the web server keeps a single login nonce; a refresh during an upload made the upload start over and ask for the password).
- **Wireless sensor type *All types*:** keeps listening to Bresser, IT+ and Fine Offset with timed switching and stores nothing (unlike *Auto*, which stops at the first type it finds) — to see everything in range or for sensors of several brands. Parameter 50 value `15`.
- **Value sources: every wireless sensor 1–32** (was 1–4): the source lists offer every sensor that is received or ★ (with its name and ID, no empty placeholders); *Auto* still only looks at 1–4. **Changed parameter value:** *Off* is now `34` (was `6`; Wireless n = n + 1, i.e. `2`–`33`) — **a value source set to *Off* has to be set again once after the update** (it would otherwise read Wireless 5).
- **Value source *Auto* follows your ★ order:** with ★ sensors *Auto* takes, after the board sensor, the first ★ sensor (in the order of the ★ list, arrows on the settings page) that delivers the value — all ★ sensors, not only the first four, and the order you set is the priority (before: wireless 1–4 in Ecowitt's sensor order). Sensors that are not ★ are no longer used by *Auto* while any sensor is ★. Without ★ sensors nothing changes (wireless 1–4, Ecowitt's order). **Check the value sources after the update** if you rely on *Auto* with several sensors delivering the same value.
- **Pulse inputs settle after the start:** wind, rain and the AUX pulse input / 2nd anemometer are not counted for the first 10 s after the start (a burst on the wind line right after an update showed as a 226-km/h gust in the chart history; the records were not affected, the clock was not set yet).
- **Reset values with buttons:** under Settings → System → *Reset values* buttons (with a confirmation, effective at once, no *Save*): reset the records (today to this year, or all incl. all-time), the new **Delete chart history** (both ranges, also the copy in flash — e.g. to remove an outlier), **Reset rain statistics** (today … year, last rain, rain since start, also the flash copy), **Reset sunshine duration** and, on PCB v2.0, **Reset AUX pulse total** (before only by downlink / MQTT command). New `POST /api/settings/reset`; parameter 64 for the records is unchanged.
- **Docs: WN20 rain correction** — the plain per-sensor rain factor **0.908** is now the recommended way (`ID:0:0:0.908`, wiki factor up to 5 mm/h); the rate-dependent `wn20` preset is marked experimental (no wiki figures above 10 mm/h yet). The WH40 preset equals the plain factor 1.016.
- **Docs: updating from the config portal on a phone** — use Chrome / Safari at `http://192.168.4.1/update`, not the automatic sign-in window: with an admin password set, Android's window can't show the password prompt and only reports *Webpage not available* (`ERR_HTTP_RESPONSE_CODE_FAILURE`) (forum report, update rc.4 → rc.5).

## 2.0.0-rc.5

Firmware: [`universal-weather-node-2.0.0-rc.5.bin`](firmware/universal-weather-node-2.0.0-rc.5.bin) (OTA / USB at `0x10000`) and [`universal-weather-node-2.0.0-rc.5-factory.bin`](firmware/universal-weather-node-2.0.0-rc.5-factory.bin) (first installation, USB at `0x0`).

Payload format unchanged except two new bits in the value-source field; existing nodes keep their settings and behave as before (pressure source *Auto* = the BME280 when one is detected).

**Portal-only functions in the web settings** (the config portal stays for the first setup and for nodes without Wi-Fi / web interface):
* **Wind-direction calibration:** *Settings → Sensors → Sensor calibration → Calibrate (guided)* opens the portal's 8- / 16-position wizard (`/calibrate`, live ADC reading, saved without a restart). Admin login, same-origin save; only on boards with a wind-vane input. See [Settings](docs/settings.md#wind-direction-calibration).
* **Payload decoder download** (ChirpStack / TTN, matching the firmware) in *Settings → Outputs → LoRaWAN* (`/decoder`, no login, as in the portal).
* **Switch Wi-Fi or the web interface off** (LoRaWAN-only node) in *Settings → System* (advanced) with a warning and a confirmation checkbox; refused unless LoRaWAN is enabled and joined, so the node stays reachable via the config portal (PRG 3 s) or a LoRaWAN downlink. See [Settings](docs/settings.md#switching-wi-fi-or-the-web-interface-off).
* New API routes in [OpenAPI](docs/api/openapi.yaml): `GET /decoder`, `GET /calibrate`, `GET /calibrate/sample`, `POST /calibrate/save`.
* The *Firmware update* section shows no live / restart tag (it is an action, not a setting).

**From the Ecowitt wiki (by Gyvate):**
* **Freshness per sensor type:** sensors that send only every 10 minutes (WH41 / WH43, WH45 / WH46 on battery) stay valid for 25 minutes (2.5 × their interval) instead of 5 — values, stale marks, Home Assistant availability and their table place; they never end the *Auto* search.
* **WS90 / WS80 on an external supply** (firmware ≥ 1.6.0 reports 0 V): *external power* instead of *battery low*.
* **WH40 preset** (`…:wh40`, ×1.016: its 0.1016 mm tips are reported as 0.1 mm).
* **Auto value sources** among wireless 1–4 follow Ecowitt's sensor order (e.g. wind WS90 > WS80 > WS85 > WS69 > WS68, rain WH40 > WN20 > WS69 > piezo).
* **WH51 raw AD** (`soilMoistureAd`) and an optional two-point soil-moisture calibration per sensor (`…:ad=AD0-AD100`).

**Easier forms:** fields that do not apply are hidden while you edit (static IP, MQTT, weather networks, Loxone, LoRaWAN, custom upload body, webhook body, display timing, wizard MQTT) — without JavaScript everything stays visible. The alarm form: *Monitor* / *Measurement* labels, a list of known sensors (name or model, Ecowitt ID) instead of typing the hex ID, measurements limited to what the sensor provides, no thresholds for yes/no values.

**Firmware update in the web interface:** *Settings → System → Firmware update* uploads a firmware file (`universal-weather-node-<version>.bin`, or the `-factory.bin` from 2.0.0-rc.1 on) with a progress bar, then restarts with a countdown back to the status page — no detour through the config portal anymore. Same checks as the portal (wrong files, a different partition table or low memory are refused before anything is written); only with an admin password (Digest login, same-origin), otherwise the portal remains the way. HTTPS requests, notifications, uploads and LoRaWAN uplinks wait during the upload. The status page links there when a newer version is available. Behind a reverse proxy allow uploads of ~2.5 MB. See [Firmware Update](docs/installation.md#firmware-update-ota).

**Install as an app (home screen):** app icons, a web app manifest (name = device name) and the head tags on every page — *Add to Home Screen* on iOS, *Install app* on Android / desktop. Over https (e.g. behind a reverse proxy) a small service worker keeps the last dashboard and its data available offline (*Offline – last data from …*); settings, setup, log, OTA and POSTs are never cached, and a firmware update renews the cache. See [Install as an App](docs/web-dashboard.md#install-as-an-app-home-screen).

**Per-sensor factors and WN20 correction:** the per-sensor calibration (*Settings → Sensors → Sensor calibration*) also takes factors for rain, wind (mean and gust), UV index and illuminance (`ID:t:h:rain:wind:uv:light`; older `ID:t:h` entries keep working). The rain factor applies to the counter's increments, never to the raw counter shown. Optional **WN20 preset** (`…:wn20`) after the Ecowitt wiki: 0.908 up to 5 mm/h, 0.773 above (verified up to 10 mm/h).

**Auto search no longer locks onto a neighbour's weak sensor:** the type is saved only after 3 valid packets from one sensor within 10 min with a mean signal ≥ −105 dBm (V4: −85 dBm); after 30 min with only weak sensors the best one is taken and marked *signal very weak*. The status page shows the search candidates; the wizard shows the type found with *Search again*. (A field node had saved the type after one packet of a neighbour's IT+ sensor at −114 dBm — the first confirmed real IT+ reception.)

**HTTPS with a memory margin:** a field node fell to 21 KiB free while a Pushover test ran next to a settings save. HTTPS requests (notifications, https weather networks, custom uploads, update check) now start only with ≥ 70 KiB free and ≥ 24 KiB in one free block (the TLS record buffers are two separate ~16.7 KB allocations), and not while a heavy page (settings, status, wizard, backup / restore) is being built; meanwhile those pages answer *busy, try again* (503) when a TLS request runs with little memory. The debug log shows the heap before / after each HTTPS request. (The TLS buffers themselves are fixed at 16 KB in the prebuilt libraries of this toolchain.)

**Heltec V4 on PCB v2.0 (firmware part):** pin map for the V4 sockets (wind direction GPIO6, wind speed GPIO44, rain GPIO16, 1-Wire GPIO43, vane supply GPIO34, VE.Direct RX 33 / TX 45), revision ID on GPIO15 (a V4 without it stays the wireless build), log over native USB (UART0 stays off on every V4). Status page: *v2.0 detected · Heltec V4 sockets*. Not yet tested on real hardware.

**PCB v2.0: Heltec V3 or V4** — see [Heltec V3 or V4 on the PCB](docs/pcb.md). The board has a second socket set for the Heltec WiFi LoRa 32 V4 (J19 / J20, 2.54 mm toward J1, plus the 1×2 socket J21 for the V4's GPIO15 / 16); fit **one** set only. On a V4 the board uses GPIO6 (wind direction), 44 (wind speed), 16 (rain), 43 (1-Wire), 34 (vane supply), 15 (revision ID) and 45 (VE.Direct TX, 100k pull-down R65); the V4's RF-front-end and USB pins stay unconnected. The V4's 5V pin (its USB VBUS) is fed through two LM66100 ideal diodes (U8 / U9: ≈ 4.9 V at the V4, no reverse current into a running USB host, USB-only mode powers the board); keep "remove JP8 and VIN before USB" for switched-off computers and always on the V3. GPIO19 / J16 pin 5 is now labelled VANE. Moved: JP8, JP12, J14 and the VE.Direct isolation gap; U2 / U3 now use the UMW SN74LVC2G14DBVR second source (the TI part was nearly out of stock); new V4 enclosure variant `3dprint/v20-v4/`. DRC 0 errors. *Not yet built — verify the points in `pcb/v2.0/CHECKLIST.md` on the first prototype.*

**Dashboard link stays visible in the navigation (GitHub #2):** the dashboard now has the same five links as the other pages (Dashboard highlighted) and the language menu at the same place.

**Less static RAM (about 10 KB):** the device log keeps the last 4 KB (was 8 KB, the log page shows correspondingly fewer lines); the history rings use 32 instead of 36 bytes per bucket (same JSON); the config portal's CA-certificate buffers (4.4 KB) are taken from the heap only while the portal runs.

**Smaller firmware:** the built-in help and the dashboard page are stored gzip-compressed and sent with `Content-Encoding: gzip` (the browser decompresses them; clients without gzip get them inflated on the node) — about 260 KB less flash.

**From tester feedback (rc.4)**
* **Rain per wireless sensor:** the sensor's own counter is now labelled *counter* (it counts since the sensor's last battery change, e.g. 6898.6 mm on a WH65); new **rain today** and **rain in the last hour** per sensor from the counter's increments (`rainToday`, `rain1h` in `rfSensors`, MQTT `<prefix>/rf/<ID>`, Home Assistant *Rain Today* `total_increasing`, Loxone `rfN_rainToday`). RAM only.
* **Lightning (WH57):** strikes today / last hour and the time of the last strike from the counter's increments (`lightningToday`, `lightning1h`, `lastLightning`; HA *Lightning Strikes Today*, *Last Lightning*); the raw counter is only a diagnostic now (HA *Lightning Counter*).
* **Wind direction** on the dashboard cards and the status page as compass point + degrees (*NE 45°*) instead of an arrow that was read as a trend.
* **Ecowitt IDs:** shown next to ours (*ID 900181A2 (Ecowitt 181A2)*, JSON `ecowittId`); the short form is accepted in the Mine / Hidden lists and replaced by the full ID when the sensor is heard.
* **Sensor names** (e.g. *Hochbeet*, 24 characters) in the sensor table and the setup wizard: dashboard, status page, JSON `name`, Home Assistant device *&lt;name&gt; (&lt;model&gt;)*; in the settings backup. The wizard's sensor list no longer reloads while you type.
* **Sensor battery voltage** (`batteryV`) where the sensor sends it (WH51, WN34, WH40 …).
* **Fix:** a save could be refused with "static IP (invalid address)" although static IP was not touched — a static IP stored without a valid gateway (config portal) failed every later save. Now an unchanged or switched-off static IP is never checked, an empty address means DHCP, and the portal only stores a complete static address.
* **Battery on the display:** no more "Bat: 0.000V" — with LiPo measurement off nothing is shown, below 2.5 V *USB* (also on the status page).
* **PCB v2.0 on a V3:** with VE.Direct off, its TX pin (GPIO34) is driven high — the ADuM121N isolator draws about 1 mA instead of 3.9 mA with a floating input.
* **`/api/info` `system`:** uptime, free heap, minimum free heap and largest free block (bytes); the status page shows the largest block too — for watching a node over days.
* Less static RAM: the config portal's firmware-upload buffers (6 KB) are allocated only when an upload starts.
* Installation: tips for downloading the `.bin` (not the GitHub page), Chrome/Edge, the first DHCP and reopening the portal.

**Davis phantom-rain filter** — see [Settings](docs/settings.md#settings-reference) (*Davis (diagnostics)*).
* An implausible jump of the Davis tip counter (a corrupt packet that passed the CRC, a decrement that is not a 7-bit wrap, a counter reset) is held until the next counter packet confirms or contradicts it: a corrupt value is dropped, a confirmed jump is rain if it fits the per-minute limit, else the counter is re-based without counting it. Logged to the device log and the radio trace. Settings (advanced): filter on / off, tips per packet without confirmation (8), max. tips per minute (50). Heavy rain passes unchanged.

**OLED weather screens** — see [Display](docs/settings.md#display).
* New settings tab *Display*: on for 20 s after PRG (default, as before), always on, or N minutes after PRG; screens classic status, temperature, wind, rain, pressure (with tendency arrow and trend word), status (IP, `.local` name, RSSI, sensors, active alarms) with rotation; brightness; night-off hours. A short PRG press while the display is on shows the next screen. Texts in the device language.

**Less static RAM:** the log download buffer (8 KB) and the settings handler's copy of the configuration (5 KB) are now taken from the heap only while needed.

**Alarms and notifications** — see [Alarms and Notifications](docs/alarms.md).
* Up to 12 alarm rules on any main or derived value, the frost / fog / sultry warnings or a value of a wireless sensor (leak, lightning distance, soil moisture, CO2, PM2.5, silent for N minutes, battery low …): above / below, raise and clear threshold (hysteresis), minimum duration, repeat interval, all clear, priority 1–5, message templates with placeholders in the device language. Ready-made rules: frost, fog / dew, sultry, storm (pressure falling ≥ 6 hPa / 3 h), rain starts. Unavailable (stale) values never raise or clear an alarm.
* Channels: ntfy (server, topic, token, tags), Pushover (incl. emergency priority with retry / expire), Telegram, MQTT (retained `<prefix>/alarm/<n>` state per rule, events to `<prefix>/alarm`, one Home Assistant binary sensor per rule) and a webhook (GET / POST with a JSON body template). One request at a time on the upload task, waits for enough memory for TLS, retries with back-off; a test button per channel. Tokens are write-only, never logged, and only in a settings backup with *include passwords and keys*.
* New settings tab *Alarms* (own form), parameter 66 (rules on / off), `GET /api/alarms` ([schema](docs/api/schemas/alarms.schema.json)), `/api/data` `alarms` (active rules, shown as dashboard badges), status-page box *Alarms*, send status per channel in `/api/info` `outputs.status`.

**Custom uploads and more weather networks** — see [Weather Networks](docs/weather-networks.md).
* Two custom HTTP uploads: URL template, GET / POST with a body template, interval; placeholders such as `{temp}`, `{wind:kmh}`, `{qnh:inhg}`, `{rain_day:in}`, `{temp:.1}`, `{time_utc}`, `{rf3_temp}` (URL-encoded in URLs, JSON-escaped in JSON bodies) — the same engine as the alarm messages and webhook.
* **Met Office WOW** (WU-style, per its data-format page; WOW has announced its closure) and **CWOP** (APRS weather report over APRS-IS `cwop.aprs.net:14580`, passcode −1 for CWOP IDs, position from latitude / longitude). Not added, as no public upload specification could be verified: PWSWeather, Windguru, Windfinder.

**Smaller firmware:** the built-in help (documentation) was stored twice in flash; now once (about 290 KB less).

**Calibration** — see [Calibration](docs/settings.md#calibration).
* Offsets for temperature (K), humidity (%) and the station pressure (hPa, before the QNH reduction), factors for rain and wind speed / gust — applied after the value source, for board and wireless sensors alike, so every output (status, dashboard, `/api/data`, MQTT / Home Assistant, LoRaWAN, Loxone, weather networks, records, derived values) sees the corrected value. Parameters 59–63, *Settings → Sensors → Sensor calibration*.
* Per wireless sensor: temperature / humidity offsets by sensor ID (`ID:temperature:humidity`, up to 32), applied when a packet is stored — they also correct that sensor's own values (`rfSensors`, `<prefix>/rf/<ID>`, its HA device).

**Records** — see [Records](docs/values.md#records).
* Min / max with time of temperature, humidity, pressure (QNH), max gust and rain rate, and the largest daily rain — today, yesterday, this month, this year, all time; local-midnight roll-over; one flash record written at most hourly and before planned restarts. New `GET /api/records` ([schema](docs/api/schemas/records.schema.json)), dashboard panel *Records*, `/api/data` `tempMinToday` / `tempMaxToday` / `gustMaxToday` (also MQTT, three HA entities, Loxone). Reset in *Settings → System* (parameter 64, all-time separately). The dashboard's former *Records (min · avg · max)* panel is now called *Chart range*.

**Send status per output** — status page and `/api/info` `outputs.status`: for LoRaWAN, MQTT, Home Assistant discovery, Loxone and each weather network the last success, the last error (time, code, short text — no URLs or keys) and the failures in a row.

**Settings backup and restore** — see [Backup and Restore](docs/settings.md#backup-and-restore).
* *Settings → System*: download all settings as `uwn-settings-<host>-<date>.json` (passwords and keys only when ticked, with a warning) and restore from such a file — validated like a manual save, the changes are listed, restart if needed; files of a newer major format are refused; the LoRaWAN session stays untouched. `GET /api/settings/backup`, `POST /api/settings/restore`.

**Sun and moon** — see [Sun and Moon](docs/values.md#sun-and-moon).
* Computed on the node from latitude / longitude, no internet: sunrise, sunset, civil twilight, solar noon, day length, sun elevation, clear-sky solar maximum, moonrise / moonset / transit, moon illumination and phase; polar day / night and days without moonrise or moonset handled. `/api/data` `astro`, dashboard panel *Sun & moon* (with a hint when the location is missing, also in the settings), HA timestamp entities *Sunrise / Sunset / Moonrise / Moonset*, *Moon Phase*, *Moon Illumination*, Loxone `astro…` (minutes since midnight). Tested against U.S. Naval Observatory values (sun ±1 min, moon ±3 min).
* Sunshine duration optionally by solar radiation ≥ 0.4 × clear-sky maximum (WMO-like) for a W/m² sensor (Davis solar radiation); parameter 65.
* Latitude / longitude are now used on the node (and still only sent to AWEKAS).

**Setup wizard** — see [Setup Wizard](docs/installation.md#setup-wizard).
* After the first start or a factory reset the dashboard opens a step-by-step wizard (`/setup`, five languages): language → wireless station (live list, mark ★ / hide ✕, "I have no wireless sensors") → location (name, altitude, optional position and time zone from the browser) → units → admin password (strongly recommended) → optional outputs (MQTT / Home Assistant, weather networks, Loxone) → summary. Skip, back and "later" on every step; it never blocks the dashboard (once per browser session, gone for good when finished or closed). Each step posts to the normal settings handler; restart fields are applied by one restart at the end. Open without login only while no admin password is set and the setup is not done; can be started again in *Settings → System*. New parameter 58 (setup done; existing nodes count as done), `POST /setup/done`. The portal's start text now points to the wizard.
* The language step preselects *Auto* (follows the browser) instead of the detected language.
* The Wi-Fi-change result banner and the radio-trace start errors are now translated; German "Freier Heap" → "Freier Arbeitsspeicher".

**Web interface in five languages** — see [Language](docs/settings.md#web-settings-page) and [Translations](docs/development.md#translations-web-interface).
* Status, settings (all tabs, hints, sensor table, messages after saving), log, radio-trace and firmware-update pages, the wind-calibration page and the setup portal's parameter labels in **English, German, French, Spanish and Portuguese**; technical names (JSON keys, topics, parameter names, units, sensor models) stay as they are. The help page (documentation) stays English; WiFiManager's built-in portal pages too.
* New setting **Language** (*Settings → Basics*, parameter **57**, portal field `uilang`): *Auto* (each browser's `Accept-Language`, region tags such as `de-AT` count) or a fixed language. A language menu on every page (and the dashboard's) sets a per-browser choice (cookie `wnLang`), shared between the dashboard and the other pages; the dashboard otherwise follows the device setting. `/api/info` `device.language`.
* One source for all texts: `i18n/*.json`, compiled into flash at build time (`tools/i18n_gen.py`); a test fails on missing keys or differing placeholders.
* The settings, status and log pages are now streamed in small chunks instead of being built as one large string (the settings page needed ~36 KB of contiguous heap and answered 503 on a fragmented heap).

**Settings in tabs, wireless sensors Mine / Received / Hidden** — see [Web Settings Page](docs/settings.md#web-settings-page) and [Mine, Received, Hidden](docs/wireless-sensors.md#mine-received-hidden).
* The settings page has tabs **Basics**, **Sensors**, **Outputs**, **System** (deep links `#sensors` …; one *Save*, back on the same tab; without JavaScript all tabs are shown). **Show advanced options** (remembered in the browser) reveals the expert fields, hidden by default: Davis diagnostics, CPU frequency, debug mode, the sensor ID lists as text, I/O scale factors, the update version-list URL. All fields keep their meaning; the intervals of MQTT / LoRaWAN wind values moved to *Outputs*.
* **Wireless sensor table** (tab Sensors): every sensor with number, ID, kind, signal, age and values, and its state — ★ **Mine** (the former ID list: numbered first in this order, wireless 1–4 = value sources, Home Assistant device; arrows change the order), ○ **Received** (output everywhere, numbered after the ★ ones, no HA device) or ✕ **Hidden** (new hidden list: never stored, numbered or output — REST, MQTT, HA, Loxone, sources). Hidden sensors still in range are listed collapsed, with *unhide*.
* **Only my sensors** (new switch): on = only ★ sensors are received, like an ID list before; off = all except hidden, ★ first. **Existing nodes with an ID list start with it on (identical behaviour), without one off.** With it off and some ★ sensors, the others now get numbers after them (before, a list excluded them).
* Status page: ★ / ✕ buttons per sensor and "*n* hidden sensors in range". New `POST /api/rf/sensor` (`id`, `action=mine|received|hide|up|down`), parameters **55** (only my sensors) and **56** (empty the hidden list), portal fields `rfignore` / `rfonly`, `rfSensors[].mine` in `/api/data` / MQTT, `/api/info` `wireless` (counts). A ★ sensor always gets a table place (it takes the one of the other sensor heard longest ago).

**Pressure as a value source** — see [Value Sources](docs/wireless-sensors.md#value-sources).
* New setting *Source: pressure* (web settings, portal field `srcs` as sixth value, **parameter 54**): *Auto* (BME280 first, then wireless 1–4 that send pressure — indoor sensors are fine here), *Board*, *Wireless 1–4*, *Off*. Wireless barometers: Ecowitt / Froggit **WS69** with pressure module and **WN32P / WH25 / WH32B**; Bresser, IT+ and Davis transmitters send none.
* The sensors' station pressure is reduced to sea level with the configured altitude by the same formula as the BME280 path, so `pressureAbs` / `pressureQnh`, the 3-h tendency and weather trend, history, Home Assistant (pressure entities now also announced with wireless sensors on), weather networks, LoRaWAN and the display work with either source. Freshness per value as for the other groups (5 min); per-sensor `stale` can now list `pressure`.
* JSON / `/api/info`: `sources.pressure`; `/api/info` `available.pressure` follows the source. LoRaWAN value-source field (CayenneLPP Ch 16 / binary field 33): bits 10–11 = pressure (existing bits unchanged; older firmware leaves them 0 → `none`); decoder updated.

**AWEKAS: more fields** — see [Weather Networks](docs/weather-networks.md).
* Now also sent (positions per the AWEKAS upload API): **UV index** (UV source), **brightness** in lux (light source), **sunshine hours today** (the node's sunshine statistics, with a light source), **pressure tendency** (−2…2 from the 3-h tendency classes) and **solar radiation** in W/m² — only measured (Davis solar radiation sensor as the light source; then no brightness, the Davis lux value is only an approximation). Soil temperature stays empty.

## 2.0.0-rc.4

Firmware: [`universal-weather-node-2.0.0-rc.4.bin`](firmware/universal-weather-node-2.0.0-rc.4.bin) (OTA / USB at `0x10000`) and [`universal-weather-node-2.0.0-rc.4-factory.bin`](firmware/universal-weather-node-2.0.0-rc.4-factory.bin) (first installation, USB at `0x0`). Payload format unchanged; existing nodes keep their settings (also the sensor ID list and the *Wireless 1–4* value-source choices).

**Up to 32 wireless sensors** — see [Wireless sensors](docs/wireless-sensors.md).
* The node now receives up to **32** wireless sensors at the same time (was 4) and *Wireless sensor IDs* takes up to 32 IDs — a fixed table reserved at start, no per-sensor memory allocation at run time (no heap fragmentation). Each sensor has a number 1–32 (`rfSensors[].slot`): its position in the ID list, else the reception order. Only 1–4 can be value sources — *Wireless 1–4*, *Auto* and parameters 45–49 keep their meaning; sensors 5–32 are reported everywhere but never feed the main values.
* All of them appear on the status page (*Wireless n · ID …*), as dashboard cards (with `#n`), in `/api/data` / MQTT `rfSensors[]` (`slot` 1–32, at most 32 items), on Loxone as `rf1_…` … `rf32_…` (was limited to slots 1–9 in the code, 1–4 in practice) and in the settings hint of received IDs.
* MQTT: the data message is streamed into the connection instead of being serialized into a copy first (~5 KB with 32 sensors), so the MQTT client buffer no longer grows to the message size.
* An ID list saved by an older firmware loads unchanged (the stored string is the same; the field only got longer). After a downgrade, a list longer than 47 characters would be read as empty (= accept any sensor).

**Home Assistant: one device per wireless sensor** — see [Wireless sensors in Home Assistant](docs/mqtt-home-assistant.md#wireless-sensors-in-home-assistant).
* New MQTT topic **`<prefix>/rf/<ID>`** per received sensor (its `rfSensors[]` object), published after each data message.
* Every sensor whose ID is in *Wireless sensor IDs* gets its own HA device (*"<model> <ID>"*, connected via the node) with entities only for the values it sends: temperature, humidity, wind speed / gust / direction, rain total (`total_increasing`), UV, illuminance, solar radiation, soil moisture, lightning count / distance; diagnostics battery low, signal strength, last packet age, supercap voltage. Unavailable when the sensor has not been heard for 5 min. Configs go out 4 per second (worst case 32 sensors × 16 entities: a little over 2 min). Sensors not in the list (neighbours) get no device; with an empty list there are none (the status page and the settings hint say so).
* Removing an ID from the list deletes its device in HA (retained configs cleared); the announced IDs are remembered across restarts.

**More Ecowitt / Froggit sensors (decoded from published frames, not yet tested with real sensors)** — see [Wireless sensors](docs/wireless-sensors.md).
* Received in the same 17.24 kbit/s phase as the other Fine Offset sensors (no switching): air quality **WH41**, **WH0290**, **WH43**, **WH45 / DP250**, **WH46** (PM1 / PM2.5 / PM4 / PM10, CO₂, temperature, humidity, USB power), leak **WH55 / DP70** (alarm, sensitivity, raw value, channel), leaf wetness **WN35 / DP10**, soil **WH52** (temperature, moisture, EC), and the thermo-hygro(-baro) family without a family code **WN32P** (WH25 / WH32B / WN32B, with station pressure) and **WN32** / WN32EP (WH32 / DP40).
* New per-sensor values in `rfSensors[]` / `<prefix>/rf/<ID>`: `pressure` (node pressure unit), `pm25`, `pm10`, `pm10Estimated`, `pm1`, `pm4`, `co2Ppm`, `externalPower`, `leak`, `leakHighSensitivity`, `leakRaw`, `leafWetnessPct`, `conductivityUsCm`; WH55 also `channel`. They are per sensor only (no new value-source groups); a WS69 with pressure module now also reports its `pressure`.
* Home Assistant: new per-sensor entities *Pressure*, *PM2.5*, *PM10*, *PM1*, *PM4*, *CO2*, *Leak* (binary sensor, `moisture`), *Leaf Wetness*, *Soil Conductivity* (25 possible entities per sensor, all cleared when an ID is removed). Loxone `rfN_pm25`, `rfN_co2Ppm`, `rfN_leak`, … Status page and dashboard cards (all five languages; new kind *Leaf wetness sensor*).
* A WH31 frame with 0 % humidity (outside the WH31's range, e.g. from a temperature-only WN30) no longer reports humidity.

**Radio trace ("Funk-Mitschnitt") for support** — see [Capturing a radio trace for us](docs/wireless-sensors.md#capturing-a-radio-trace-for-us).
* Status page, box *Wireless sensors*: **Start capture** for 5 / 15 / 60 min, recording all packets, only unknown / failed packets, or one sensor ID; progress (packets, buffer fill, time left), *Stop*, *Discard* and **Download diagnostic file** (`uwn-rftrace-<hostname>-<YYYYMMDD-HHMM>.txt`).
* Every packet of every receive phase — Bresser, IT+ 17.241 / 9.579 kbit/s, Fine Offset (+ IT+) 17.241 kbit/s, Davis — with uptime / local time, phase, RSSI, result (OK / IGNORED / FAILED / UNKNOWN), the failed check (CRC, sum, digest, family code), decoded model, ID and values, and the raw bytes; also frames that fail their checks or come from unknown families. Header with firmware, board, receive settings, ID list, sensor table and statistics per phase and per ID over the whole capture — never Wi-Fi, MQTT, admin, LoRaWAN or weather-network credentials (host-tested); it does contain the neighbours' sensors in range, which the page and the file say.
* About 26 KB of RAM, allocated only when a capture starts (refused if the free heap is too low), kept for the download until *Discard*, a new start or a restart.
* REST: `POST /api/rf/trace` (start / stop / clear) and `GET /api/rf/trace` (file; `?status` = progress JSON), Digest, same-origin POST, no CORS.
* The Davis sniffer capture is now this trace: Davis mode starts an open-ended one (unless a trace is running); `/api/davis/log` keeps working and returns the same text. Davis diagnostics settings unchanged.
* Receiver libraries: `TfaWeatherSensor::getRaw()` (IT+ frames before decoding) and a raw-frame hook in the Bresser fork (`WeatherSensor::setRawHook()`, see its FORK.md).

**Davis Vantage Pro2 / Vue wireless (experimental, not yet tested with a real station)** — see [Davis wireless](docs/wireless-sensors.md#davis-wireless-experimental).
* New *Wireless sensor type* **Davis (exclusive)** (parameter 50 / portal `rfproto` = `17`): receives an EU 868 MHz Vantage Pro2 / Vue ISS or anemometer transmitter 6332 with the SX1262 and follows its 5-channel hop sequence (acquisition with frequency-offset scan, tracking with period PLL, re-acquisition after 12 misses, optional software AFC). Exclusive: **LoRaWAN is paused** (status page: *LoRaWAN paused: Davis mode*) and no other wireless sensors are received; *Auto (search)* never selects it.
* Values (temperature, humidity, wind every 2.5 s, rain from the bucket-tip counter with wrap, UV, solar radiation, battery flag, Vue supercap) appear as wireless sensor `DA0000ii` and feed the value sources; new `rfSensors[].solarWm2`.
* Field-test tools, all live without reflashing (web settings *Davis (diagnostics)*): transmitter ID, frequency offset, offset scan, AFC, RX window / guard, channel table, receiver bandwidth, rain collector, station model, **sniffer** (every packet with raw bytes, CRC, decoded value and timing error to the log, per-minute summary) and **raw capture** on one channel; status page box *Davis*, `davis` object in `/api/data` / MQTT, full capture at the new `GET /api/davis/log`.
* New library `lib/DavisSensorReceiver` (own, MIT): decoder and hop state machine host-tested against published sample packets and a simulated transmitter.

**HTTP API for your own displays and apps** — the JSON endpoints are now a documented, versioned contract: [Building Your Own Display or App](docs/api.md).
* **Contract files:** [OpenAPI 3.1](docs/api/openapi.yaml) for every HTTP endpoint and JSON Schemas (2020-12) for [`/api/data`](docs/api/schemas/data.schema.json) (= MQTT data topic), [`/api/history`](docs/api/schemas/history.schema.json) and [`/api/info`](docs/api/schemas/info.schema.json) — every field with type, unit (or unit field), range and when it is present. Example responses of five setups in [`docs/api/examples/`](docs/api/examples/), generated by the real firmware serializers; a contract test fails when the firmware emits an undocumented field or the examples drift.
* **`apiVersion`** (`1`) in `/api/data`, the MQTT data topic, `/api/history` and `/api/info`. Within one version fields are only added; renames, removals or type/unit changes need version 2. Not sent to Loxone (UDP).
* **New `GET /api/info`:** device name and mDNS host name, firmware version, board / PCB revision, units of every value, enabled outputs (Wi-Fi, MQTT, LoRaWAN, Loxone, weather networks — no addresses, IDs or keys), which value groups are available and their sources, cycle length, history ranges. No authentication.
* **CORS:** `/api/data`, `/api/history` and `/api/info` send `Access-Control-Allow-Origin: *` and `Cache-Control: no-store` and answer `OPTIONS` preflights — a page from any origin can read the node. Settings and log stay same-origin with Digest authentication.
* **mDNS:** the node is reachable as `<device-name>.local` (lower case, other characters as `-`; unnamed: `weathernode-<MAC>`) and announces `_http._tcp` and `_weathernode._tcp` with TXT `api=1`, `path=/api/info`, while Wi-Fi is up and the web API is on.
* **Tools:** a [mock server](tools/api-mock/README.md) (Python standard library) serves the examples with drifting values, and [`examples/simple-display/index.html`](examples/simple-display/index.html) is a dependency-free template display.
* Other JSON output unchanged. New link check for the documentation (`test/doc_links.test.js`).

**Fixes**
* Value sources: *Auto* no longer takes the temperature / humidity from an **indoor sensor** (Ecowitt WH45 / WH46, WN32P / WH25) in wireless 1–4 — without an ID list it could otherwise feed the outdoor temperature. *Wireless n* can still select one explicitly.
* Status page: the wireless receiver no longer says "listening … between LoRaWAN uplinks" when LoRaWAN is off.
* Dashboard: the Ecowitt **WN34** shows as *Temperature probe* (DE *Temperaturfühler*, all five languages) and the **WN38** as *Black globe thermometer* instead of *Pool thermometer*; Bresser pool sensors unchanged. The JSON `type` stays `3` (documented: use `model`).
* **History buckets covered twice their span:** a bucket only closed with the first cycle at least 5 min (1 h) after its first cycle, so with the default 300-s cycle every 5-min bucket held two cycles and the "24 h" chart reached back ~48 h (the 7-day one ~8 days at 1-h cycles; trends and rain 1 h/24 h were based on these wider buckets). Buckets are now fixed windows — on the wall clock once it is synced, on the uptime before — and a cycle counts in the window it ends in; one point per window (windows without a cycle are gaps), so 288 points are exactly 24 h and 168 exactly 7 days.
* **Hostname:** the Wi-Fi/DHCP hostname is now the same sanitised name as mDNS (`weather-mast` for "Weather Mast", `weathernode-<MAC>` without a name; was the raw device name, and in practice often the ESP default because it was set after the Wi-Fi mode), also in the Wi-Fi change test and the setup portal; shown on the status page.

## 2.0.0-rc.3

Firmware: [`universal-weather-node-2.0.0-rc.3.bin`](firmware/universal-weather-node-2.0.0-rc.3.bin) (OTA / USB at `0x10000`) and [`universal-weather-node-2.0.0-rc.3-factory.bin`](firmware/universal-weather-node-2.0.0-rc.3-factory.bin) (first installation, USB at `0x0`). Payload format and settings unchanged.

**Heltec WiFi LoRa 32 V4**
* Same firmware image for V3 and V4; the board is detected by flash size (setting *Heltec board*: Auto / V3 / V4). The V4's RF front end (V4.2 GC1109 / V4.3 KCT8103L, detected at boot) is switched for receive and transmit like Meshtastic does, put to sleep with the radio, and the radio's TX power is reduced by the amplifier gain so LoRaWAN stays at the regional limit (EU868 16 dBm).
* V4 = build without PCB only: PCB revision, board wind / rain, 1-Wire and the v2.0 I/O are disabled (GPIO2/5/7 drive the front end); status page and JSON (`status.board`, `status.boardFem`) show the board. V4-R8 is reported as not supported. *Not yet tested on real hardware.*

**Documentation**
* README reduced to a short landing page with a Quick Start and an "I want to …" signpost; topics moved to [`docs/`](docs/) (installation, wireless sensors, minimal build, web dashboard, MQTT / Home Assistant, weather networks, Loxone, values, settings, LoRaWAN, PCB, troubleshooting, development), changelog to `CHANGELOG.md`. The device's built-in help page joins README, docs and changelog into one offline page.

## 2.0.0-rc.2

Firmware: [`universal-weather-node-2.0.0-rc.2.bin`](firmware/universal-weather-node-2.0.0-rc.2.bin) (OTA / USB at `0x10000`) and [`universal-weather-node-2.0.0-rc.2-factory.bin`](firmware/universal-weather-node-2.0.0-rc.2-factory.bin) (first installation, USB at `0x0`). Payload format unchanged (no decoder update needed); existing nodes keep their settings.

**Easier start for a Wi-Fi station with a wireless sensor**
* **New defaults for new nodes** (existing nodes keep their settings): Wi-Fi and web dashboard **on**, LoRaWAN **off**, wireless sensors **on**, board wind / rain sensors **off**. Enter the Wi-Fi in the portal — done.
* **Wireless sensor type *Auto (search)*** (new default, parameter 50 value `16`): cycles through Bresser, 17.241 kbit/s (Ecowitt / Froggit + IT+) and IT+ 9.579 kbit/s and stores the type of the first sensor it hears (respecting the ID list). The display shows the node's URL and the search state when LoRaWAN is off.
* Portal and web settings start with the basic fields; LoRaWAN moved to the end as *optional*.
* README restructured: Quick Start for the wireless station first, PCB and LoRaWAN further down.

**New values**
* **Rain this week / month / year** (`rainWeek`, `rainMonth`, `rainYear`; ISO week, local time) in JSON/MQTT, Home Assistant (`total_increasing`), status page and dashboard.
* **Rain and sunshine statistics survive power cuts and firmware updates** (setting *Keep rain statistics over a power cut*, parameter 51, default on): today, yesterday, week, month, year, sunshine and last rain are saved to flash in one 68-byte record at most once per hour and before every planned restart (OTA, settings, `0x02`) — a power cut loses at most the last hour. All restarts go through one function that saves first. RTC data in one fixed block, so new firmware builds no longer reset the daily values.
* **Vapour pressure deficit** `vpd` (kPa).

**Dashboard redesign**
* Warning badges, a large "now" block (temperature with feels-like, trend, today's min/max; humidity with dew point and VPD; pressure with tendency arrow; wind; rain; UV with WHO colour), rain panel from rate to year, derived values panel, wireless sensor cards (model, ID, signal, age, battery, supercap, stale values, source chips), dew point in the temperature chart, rain bars per hour / day; everything without data is hidden. All languages updated.

**Loxone**
* **UDP push** to a Loxone Miniserver (one `name=value` datagram per value, optional prefix, own interval; parameters 52/53) and documented **Virtual HTTP Input** recognition for `/api/data`. `rfSensors` entries now carry `slot` (1–4); `radioBatteryLow` is always present while wireless sensors are on.

## 2.0.0-rc.1

Firmware: [`universal-weather-node-2.0.0-rc.1.bin`](firmware/universal-weather-node-2.0.0-rc.1.bin) (OTA / USB at `0x10000`) and [`universal-weather-node-2.0.0-rc.1-factory.bin`](firmware/universal-weather-node-2.0.0-rc.1-factory.bin) (first installation, USB at `0x0`). Release candidate for 2.0.0; settings and payload format unchanged since 2.0.0-beta.3.

* **OTA with either file:** the portal's firmware upload also accepts the `-factory` image and writes only its firmware part (from `0x10000`), after checking that its partition table matches the node's; otherwise refused with a hint to flash over USB. Nodes on 2.0.0-beta.2 or older still need the file without `-factory`.
* **Fine Offset WS85 / WS90:** the voltage of the solar-charged super capacitor as `supercapV` in `rfSensors` and on the status page — only for these stations, other sensors don't get the key.
* Docs: prototype disclaimers removed; PCB and LoRaWAN marked optional in the introduction (Heltec V3 + a wireless station is enough, BME280 recommended); 3D-printable enclosure for the minimal wireless build (`3dprint/case_minimal.py`, 12.5 × 4.5 mm cable slot for the Dupont plugs).

## 2.0.0-beta.3

Firmware binary: [`universal-weather-node-2.0.0-beta.3.bin`](firmware/universal-weather-node-2.0.0-beta.3.bin) (application image for OTA or USB). Same hardware support and beta status as 2.0.0-beta.2; existing settings keep their meaning, payload format unchanged (no decoder update needed).

**Ecowitt / Froggit (Fine Offset)**
* **868 MHz sensors:** WH65/WS69, WS68, WS80, WS85, WS90, WH31/DP50, WN34, WN38, WH40, WN20, WH51, WH57 — received in the same 17.241 kbit/s phase as IT+ (*Wireless sensor type* values 8, 10, 14, 9). New library `lib/FineOffsetSensorReceiver` (own, MIT). `rfSensors` gains `model`, `channel`, `soilMoisturePct`, `lightningCount`, `lightningDistKm`. Not yet tested with real sensors.

**Docs**
* **Installing in the browser:** step-by-step guide with [web.esphome.io](https://web.esphome.io) and the new **factory image** `universal-weather-node-<version>-factory.bin` (bootloader, partition table and firmware in one file, flashed at `0x0`) — no PlatformIO needed for the first installation.
* **OTA file check:** the portal's firmware upload (`/update`) refuses a `-factory` image or any non-firmware file before writing anything (a factory image in the app slot would leave a node that doesn't boot); the upload page names the right file.
* Wiring graphic for the [minimal wireless build](docs/minimal-build.md) (Heltec V3 + BME280 on four wires, USB-C power); architecture note: sensors are read directly, Wi-Fi is output only.

## 2.0.0-beta.2

Firmware binary: [`universal-weather-node-2.0.0-beta.2.bin`](firmware/universal-weather-node-2.0.0-beta.2.bin) (application image for OTA or USB). Same hardware support and beta status as 2.0.0-beta.1. **Update the payload decoder on ChirpStack/TTN first** (binary format version 4). New settings keep the previous behavior by default (MQTT interval 0 = like the LoRaWAN interval, wind values *interval*, value sources *Auto*, wireless sensors off); the wind sample interval default changes to 1 s for new installations, stored values are mapped to 1 s or 3 s.

**Wireless sensors and value sources**
* **Wireless sensors (Bresser, 868 MHz):** the LoRa chip receives Bresser 5/6/7-in-1 stations and other Bresser sensors between LoRaWAN uplinks (setting *Wireless sensors*, parameter 44, optional ID filter). Values on the status page and as `rfSensors` in `/api/data` / MQTT. Uses a fork of BresserWeatherSensorReceiver (MIT) in `lib/`, adapted to the shared transceiver and RadioLib 7.
* **Value sources:** per group (temperature/humidity, wind, rain, UV, light) *Auto* (board first, then wireless 1–4 with values not older than 5 min), *Board*, *Wireless 1–4* or *Off* — for LoRaWAN, MQTT/JSON, Home Assistant, weather networks, history and display (settings, portal field `srcs`, parameters 45–49). Wireless wind is aggregated per path like the anemometer (interval / 10 min); rain only adds increases of the selected source, so switching never jumps. New: `sources`, `lightLux`, `radioBatteryLow` in JSON; CayenneLPP Ch 15/16; wireless UV index; temperature/humidity no longer require a BME280 (pressure still does). Decoder updated (versions 1–4).

**LoRaWAN**
* **Compact binary format version 4** (always sent; decoder reads versions 1–4 — update it on ChirpStack/TTN **before** the firmware): variable-length presence mask (1–6 bytes instead of 4), UV voltage and index as separate fields (bits 31/32, no flag byte), new fields value sources (33) and illuminance (34), Victron values as separate fields (26, 35–41) instead of a fixed 14-byte block. A value that is not available takes no space; no existing value was removed. Typical station 1–3 bytes smaller than in 2.0.0-beta.1, minimal packet 3 instead of 6 bytes.
* Rejoin automatically after 288 uplinks in a row without any downlink (3 × the ADR acknowledgement window); if that does not help, the threshold doubles up to 2304 uplinks, any downlink resets it. A session that had stopped receiving downlinks — uplinks fine, ADR stuck at SF12, queued commands never delivered — previously stayed that way until the next restart.
* Failed joins are retried after 1, 2, 4 … minutes, at most every 60 min, instead of every minute — protects the join duty cycle and the DevNonce budget during network outages.
* **LoRaWAN airtime:** the uplink interval is stretched automatically to keep 1 % airtime at slow data rates (shown on the status page).

**Measurement and aggregation**
* **Separate aggregation per path:** LoRaWAN, MQTT and the weather networks keep their own windows; one send no longer resets the values of another. Rain: one running total, each path reports the difference since its own last send (interface unchanged).
* **MQTT interval** (setting, parameter 41): MQTT can publish faster than LoRaWAN, e.g. every 5 s; `0` keeps the old behavior (like the LoRaWAN interval).
* **Gust = highest 3-s mean** (WMO) instead of the highest single sample; **wind sample interval** 1 s (new default) or 3 s — other values map to a step (parameter 2 keeps working).
* **Wind values per path** (parameters 42/43): *interval* (since the last send, for downstream aggregation such as Grafana) or *10 min* (WMO values of the last 10 minutes from a 10-min ring buffer).
* **Weather networks** always get 10-min WMO wind values, independent of their upload interval.
* BME280 read every 10 s (at least once per base cycle) instead of every sub-sample. Docs: new section *Measurement and Aggregation*.

**Derived values, statistics and networks (all computed on the node)**
* **Derived values:** dew point, feels like (wind chill / heat index), cloud base, compass wind direction, 3-h pressure tendency with a weather trend, 1-h temperature trend, rain last hour / 24 h / yesterday and the time of the last rain, sunshine duration today / yesterday (Bresser 7-in-1 light), frost / fog / sultry warnings — in JSON/MQTT, Home Assistant (warnings as binary sensors), status page and dashboard tiles. The decoder adds dew point, feels like, cloud base, compass text and warnings from the decoded values (payload unchanged).
* **Weather networks:** Windy (PWS API v2) and openSenseMap (senseBox, sensor-ID mapping); dew point, rain of the last hour and UV index to the networks that take them; wind chill / heat index to Weathercloud.
* **Firmware update check** (optional, daily): newer version on the status page, in JSON and Home Assistant.
* History stores the rain per bucket in 0.01 mm (was 0.1 mm).

**TFA / LaCrosse IT+ wireless sensors**
* *Wireless sensor type* (web settings, portal `rfproto`, parameter 50): Bresser or TFA / LaCrosse IT+ temperature/humidity sensors (17.241 and/or 9.579 kbit/s, alternating every 5 s with both; Bresser + IT+ alternating is selectable but not recommended). IDs `170000xx` / `090000xx`, `protocol` in `rfSensors`. New library `lib/TfaWeatherSensorReceiver` (own, MIT, same pattern as the Bresser fork). Decoder checked against a published frame; not yet tested with real sensors. WeatherHub / Mobile Alerts not supported yet.

**Hardware (PCB v2.0)**
* **J18 DC jack** 5.5 × 2.1 mm (optional THT, LCSC C381116) parallel to J3 for 12 V / 24 V plug-in supplies, centre positive, up to 24 V / 5 A; F7, D1 and R45 moved beside it, VIN paths 1.5 mm wide. Case: knock-out in the left wall. Gerber/BOM/CPL/THT list regenerated, DRC clean.

**Web settings**
* **Wi-Fi network on the web settings page:** name and password can be changed in normal operation. The new network is tested for 60 s without a restart and only stored if the node connects; otherwise it returns to the previous network and shows a notice. *Store without test* pre-provisions a network that is not in range yet.

**Robustness (review of the whole firmware)**
* **Wireless sensors:** freshness is judged **per value** (a station that keeps sending wind but loses its temperature part drops only the temperature after 5 min; `stale` in `rfSensors`, *(stale)* on the status page) — also for the Bresser 6-in-1, whose library flags were cumulative (fork change 8). Sensor places are freed after 5 min of silence and when the ID list changes; IT+ battery changes are re-learned. Rain after a reception gap is spread instead of spiking the rain rate; values stay stale across the 49-day `millis()` wrap.
* **LoRaWAN:** the session is saved after every uplink; the EU868 1 % duty cycle is enforced by RadioLib (a too-early uplink is deferred, not counted as a failure; safe across the `millis()` wrap); send paths are at most 2 s early instead of half a cycle (LoRaWAN no longer sends more often than set); downlink commands only on FPort 1; the binary and CayenneLPP encoders leave out non-finite values; exact 32-bit AUX totals in CayenneLPP; the decoder reports truncated or unknown payloads in `errors`.
* **Inputs:** the user button works again after low-power sleep (the wake-up left its interrupt level-triggered) and press/release events can't be lost; no double count of a pulse just before light sleep; any number of PCNT counter wraps; rain reset (`0x01`) no longer shortens per-uplink rain amounts; BME280 readings outside its range (−40…85 °C, 300…1100 hPa, 0…100 %) are dropped; VE.Direct `---` means unavailable; wind fields are left out when a window has no valid sample; wind 2 gust is a 3-s gust, its mean pulse-weighted.
* **Settings:** stored values are re-checked at boot (out-of-range values reset or clamped); heater full-power temperature ≤ switch-on temperature; time-zone changes are applied on the main task.
* **Web / Wi-Fi / MQTT:** Wi-Fi change keeps the previous network from the driver (a short disconnect could store the untested one), needs DHCP for another network and a gateway/DNS answer before it is kept; portal values with `'` are no longer cut off, secrets are not prefilled, the admin password protects `/update` and ArduinoOTA; settings posts from other sites are refused and missing fields keep their value; MQTT/HA IDs sanitised, full MAC for unnamed nodes; MQTT reconnect back-off up to 5 min with shorter timeouts; update-check tag validated and escaped, GitHub lists streamed; weather-network uploads after an outage restart their averaging window; rain before the first time sync counts as today; dashboard wind unit follows the device setting.
* **Derived values:** rain 1 h / 24 h never longer than the window; 1-h / 3-h trends only after enough history; version compare ignores `-dirty` and case.

**Status page and diagnostics**
* Status page: new **LoRaWAN** box (data rate/SF, channel, TX power, device address, frame counter, last downlink with RSSI/SNR, uplinks since the last downlink vs. the rejoin limit, rejoins since start) and in **Device** the CPU temperature, the cause of the last restart and the minimum free heap.
* `/api/data` and MQTT JSON: `cpuTemp`/`cpuTempUnit` are now always included (previously debug mode only), plus `resetReason`; Home Assistant discovery offers both (*CPU Temperature*, *Last Restart*) without debug mode.
* Status page: the wireless receiver's pause during a LoRaWAN uplink is shown as normal (*paused for a LoRaWAN uplink*) instead of as an error.

## 2.0.0-beta.1

Firmware binary: [`universal-weather-node-2.0.0-beta.1.bin`](firmware/universal-weather-node-2.0.0-beta.1.bin) (application image for OTA or USB with the existing compatible partition table). First firmware for the PCB v2.0 prototype order; still supports Rev 1.2 and 1.3 (select the revision manually on older boards). Beta: nothing has been tested on v2.0 hardware yet; a release candidate follows after prototype bring-up and a field test.

**Changes since 1.3.0-beta.2:**
* Heater output J12: 0.1 Hz time-proportional control (10 s cycle, 1 % steps) from an independent timer task instead of 100 Hz PWM; the cycle keeps running during blocking sensor/LoRa calls, duty changes keep the cycle phase, the portal/OTA still latches both outputs off. Fan PWM stays at 100 Hz.
* Dashboard: the rain tile and the rain amount chart show the total over the selected range (Σ 24 h / Σ 7 days, or the buffered span after a restart) instead of the last cycle's amount.
* UV input default divider scale 1.1 for JP10 1-2 (0–3.3 V), matching the changed v2.0 divider (R52 100k); 1.66534 for JP10 2-3 is unchanged. Existing saved settings are kept.
* PCB v2.0 fabrication data finished for prototypes: VIN + USB power with manual disconnect (JP8), fewer extended parts, separate THT list, preview-matched CPL (Q7 centroid), probe pads TP6/TP7 and a bottom S/N field.

**Validation:** PlatformIO build and all six host test files passed (firmware runtime regressions, payload/decoder round trips, VE.Direct, PCB v2.0 design/BOM/CPL checks). This does not replace physical hardware or LoRaWAN field testing.

## 1.3.0-beta.2

Firmware binary: [`universal-weather-node-1.3.0-beta.2.bin`](firmware/universal-weather-node-1.3.0-beta.2.bin) (application image for OTA or USB with the existing compatible partition table). Supports PCB Rev 1.2, 1.3 and 2.0; select **1.2** or **1.3** manually on older boards, since hardware auto-detection only identifies v2.0. If the installed partition table lacks `cfgdata`, perform a full USB flash with the current partition table rather than application-only OTA. Field testing on these boards remains outstanding.

**Reliability fixes:**
* Abort a stuck DS18B20 conversion after 1 s and retry next cycle instead of indefinitely blocking measurements and telemetry.
* Shut down and latch heater/fan outputs off before entering the configuration portal; restart to resume normal output control.
* Serialize complete REST/MQTT telemetry rather than truncating it to a fixed 1024-byte buffer.
* Initialize sensors and Wi-Fi before LoRa initialization/join; retry failed radio/join attempts no more often than every 60 s. A single join exchange remains synchronous. Ten send failures trigger a cleared-session rejoin instead of restarting the whole device.
* Apply web changes to `lowPowerEn` and `vanePowerSave` even when no restart-required setting changes.
* Include a standalone second wind sensor in both LoRa payload formats.
* Fail closed when configured battery/PV output limits cannot be evaluated because VE.Direct is disabled, invalid or stale.

**Validation:** PlatformIO build and all four host test files passed, including five real-module C++ regression programs. This does not replace physical hardware or LoRaWAN field testing.

**PCB documentation and exports:** the current v2.0 board includes 14 back-side operating-reference blocks / 100 lines at 0.8 mm or larger. Gerber/drill, BOM and CPL were refreshed on 2026-11-03; see [Custom PCB](docs/pcb.md#custom-pcb) and the [assembly checklist](pcb/v2.0/CHECKLIST.md) before ordering.

## 1.3.0-beta.1

Firmware binary: [`firmware/universal-weather-node-1.3.0-beta.1.bin`](firmware/) (OTA or USB). Untested on PCB v2.0 hardware so far; on Rev 1.2/1.3 boards it behaves like 1.2.0-beta.1 plus the payload fitting below.

**Second wind sensor** (`wind2Mode`, params 33/34, PCB v2.0): anemometer at AUX `PULSE`, optional vane at AUX `AIN`; LPP Ch 2/6/7, binary bits 28–30, JSON `wind2`. AUX pulses are now also counted in low-power mode.

**Low-power mode** (`lowPowerEn`, param 31): light sleep between sub-samples in LoRa-only operation, wind/rain pulses counted while sleeping, SX1262 asleep between uplinks; on PCB v2.0 the wind-vane supply is switched off between readings (`vanePowerSave`, param 32). PCB v2.0: switched vane supply (GPIO19, Q6), UV analog input (GPIO3), R8 populated and activated with jumper JP2, separate I²C pull-up headers JP1 (SDA) and JP11 (SCL), with one empty 2.54 mm grid row between them.

**Hardware v2.0 (new default, `pcb/v2.0/`, KiCad):** SMD redesign for JLCPCB, 100 × 100 mm, M3 holes 92 × 92 mm; protected two-pin VIN input 6–29.2 V (P-channel reverse-polarity switch Q7, bidirectional SMBJ30CA TVS) and 5 V buck (LMR36520). No external 5 V terminal input. Two-pin JP8 connects the buck to the shared 5 V rail; disconnect VIN and remove JP8 before connecting Heltec USB-C. Use one source at a time. Original Davis RJ11 jacks with their screw terminals below each (one TVS per line protects both); I²C, 1-Wire, AUX and UV screw terminals; J17 UV input (GPIO3) with a 3.3/5 V range jumper; wind-vane supply switched by GPIO19; heater (low side) and selectable 5 V/VIN fan (high side) outputs with tacho; isolated Victron VE.Direct; hardware revision ID on GPIO20, test pads and expansion header. See the current KiCad schematic and checklist under `pcb/v2.0/` and the refreshed production exports described above. The Fritzing design moved from `pcblayout/` to `pcb/v1.3/`.

**Firmware for PCB v2.0** (see [Inputs and Outputs](docs/pcb.md#inputs-and-outputs-pcb-v20)), every function off by default:
* Board detection through the revision ID on GPIO20; PCB revision `20` (manual override possible).
* AUX analog input (scalable for the 0–5 V divider) and AUX pulse counter with a total kept across restarts (downlink `0x06` / MQTT `resetAuxPulses` resets it).
* Heater and fan outputs: off / on / auto by temperature (1 K hysteresis), PWM power — the heater's auto mode scales its power with the temperature (default 10 % at 4 °C up to 100 % at −10 °C), fan auto optionally only with PV power; battery lockout from the Victron voltage.
* Fan tacho with rpm and stall detection.
* Victron VE.Direct reception (battery V/A, PV V/W, yield today, charge state, error, SOC).
* Settings in the portal, on the web settings page (live) and as parameters 15–30 (downlink `0x10`, MQTT); status page sections *Outputs* and *Victron VE.Direct*; MQTT JSON, Home Assistant discovery, CayenneLPP channels 3/5/10/11/20–22/30/31 and binary fields 20–27; decoder and tests updated.

**Payload:** the compact binary format (FPort 3) is now the default for new nodes. Both formats are fitted to the current data rate before every uplink (least important field groups are left out, see [Payload Formats](docs/lorawan.md#payload-formats)) — previously an oversized CayenneLPP payload at SF10–SF12 was rejected by the LoRaWAN stack, counted as a send failure and led to a restart after 10 of them.

**Renamed:** the project is now **universal-weather-node** (formerly lorawan-weather-node) — repository, documentation, PCB files and silkscreen, firmware binaries (`firmware/universal-weather-node-<version>.bin`), payload decoder and the Home Assistant device model. Device-side identifiers stay unchanged so existing installations keep working: Wi-Fi portal SSID `WeatherNode`, MQTT client ID / default topic prefix `weathernode-…`, OTA hostname `weather-node`.

**Docs:** BME280 RJ45 pinout corrected (SCL is pin 2, GND pins 1, 3, 4, 6 — as on the board silkscreen).

## 1.2.0-beta.1

**New:**
* Direct uploads to AWEKAS, Weather Underground, Weathercloud and Wetternetzwerk.pro over Wi-Fi (see [Weather Networks](docs/weather-networks.md)), with NTP time sync and a daily rain total kept in RTC RAM.
* `/status` reworked: status dot per connection (LoRaWAN, WiFi incl. RSSI, MQTT, NTP) and per weather network, rain today, auto-reload; same look as the dashboard.
* `/api/settings` now covers every setting except the WiFi network and calibration wizard; live vs. restart sections; secrets are write-only; Digest instead of Basic authentication.
* Dashboard: synchronized hover/touch crosshair with tooltips (time, all series, wind direction), sharp HiDPI rendering, gradient area fills.
* README restructured (getting started → usage → reference), weather network badges.

## 1.1.1

**Fixes:**
* BME280 is now hot-pluggable: it was only detected once at boot, so a sensor connected later was ignored until reset, and an unplugged one kept reporting its last values as present. While absent it is now re-probed every sub-sample and fully re-initialized when it answers again (also restoring the humidity config a power-cycled chip loses); a failed or NAN measurement marks it absent.
* BME280 cycle averages are divided by the number of valid BME280 readings instead of all sub-samples, so a sensor (un)plugged mid-cycle no longer skews the average.

## 1.1.0

**Payload changes — update `decoder.js` in ChirpStack together with this firmware** (the new decoder still decodes payloads from older firmware):
* Compact binary format **version 2**: in imperial mode rain is encoded ×1000 (0.001 in) and pressure ×100 (0.01 inHg) — previously a single 0.2 mm tip (0.008 in) was sent as 0 and pressure had ≈ 3.4 hPa resolution; rain since start is `u32` (previously wrapped above 6553.5 mm). Metric values are unchanged.
* CayenneLPP: imperial pressure is sent with 0.01 inHg resolution (decoder divides by 10 when the imperial bit is set); debug channels Ch203 (free heap) and Ch204 (uptime, 0.1 h) are now *Generic Sensor* (previously saturated at 327).
* All values are rounded instead of truncated and clamped instead of wrapped; the decoder no longer rounds rain to 0.1.

**Fixes:**
* Wind-direction calibration was lost when the portal closed after *Save Calibration* — the portal wrote its pre-calibration state back.
* DS18B20 probes were never detected on PCB Rev 1.3: the bus was searched once at boot while its supply was still switched off. It is now searched every cycle after power-up, which also makes the probes hot-pluggable as documented.
* Live web/REST rain values (`rain`, `rainRate`) were overwritten by every 3 s sub-sample (mostly 0, or 240 mm/h for a single tip); they now show the last completed cycle.
* `rainMMSinceStart` did not survive restarts as documented (`RTC_DATA_ATTR` is re-initialized on every non-deep-sleep boot); it now survives restarts and watchdog resets and only resets on power loss or downlink `0x01`.
* Dashboard history ages broke after 49.7 days of uptime (`millis()` wrap); it now uses a 64-bit uptime. Same for the debug uptime and the display timeout.
* A short button press showed stale display content; the display is now redrawn, and the boot screen shows a first real reading.
* PCNT counter wrap (every 32767 pulses) produced a negative delta: one rejected wind sample per wrap, and negative rain after ≈ 6553 mm.
* Heap leak on every failed join attempt (a new `LoRaWANNode` was allocated per retry).
* Possible race in `webApiStop()` that could restart the web server right before the config portal took port 80.
* Input validation: wind-direction offset limited to ±180° in the portal (out-of-range NVS values no longer produce 65xxx°); web settings and MQTT values outside the int16 range are rejected instead of wrapping (`65836` → `300`), and invalid web settings are reported instead of silently ignored.
* Default MQTT port is 1883, matching the default "no TLS" mode (was 8883).

**Other:** `ParamId` enum instead of magic numbers (`applyDownlinkParam()` renamed to `applyParam()`); dashboard/log HTML moved from `web_api.cpp` into `include/`; `partition_repair` renamed to `partition_check` (it only detects); compact binary encoder split out as host-testable C++ with new decoder and round-trip tests; generated headers are no longer tracked and only release builds are copied to `firmware/`; README: build & flash, tests, LoRaWAN session & join, corrected payload sizes and data-rate limits, RC filter values (incl. wind-direction filter C11 = 1 µF), rain accumulator and DS18B20 behavior.
