# Configuration

Where settings are changed, what every setting does, and the numeric parameter IDs shared by LoRaWAN downlinks, MQTT commands and the web.

[← back to README](../README.md)

There are two places to change settings:

| | Config portal | Web settings page |
|---|---|---|
| **Where** | The node's own Wi-Fi `WeatherNode` → http://192.168.4.1 | Your home network → `http://<node-ip>/api/settings` |
| **When** | First boot, button held 3 s, or LoRaWAN downlink `0x03` | Any time, while Wi-Fi and the web API are on |
| **What** | Everything incl. the Wi-Fi network, wind-direction calibration, firmware update | Everything except the Wi-Fi network and the calibration wizard — incl. weather networks and time zone |

## Web Settings Page

Open **Settings** in the web interface (user `admin`, password = the *web settings admin password* from the portal).

- Sections marked **live** apply immediately: measurement, inputs & outputs (PCB v2.0 only), units, calibration, weather networks, time, admin password.
- Sections marked **restart** are only read at boot: LoRaWAN, network & MQTT, PCB revision. If you change one of them, the node saves and restarts — this clears the chart history, which lives in RAM.
- **Passwords and keys are never shown.** Leave a password field empty to keep the stored value; optional ones have a *remove* checkbox.
- Invalid values are listed after saving and keep their old value; everything else is saved. Fields that are not sent keep their value (scripts: send checkboxes explicitly as `name=on` / `name=0`). Posts from another web site (`Origin`/`Referer` not matching the node) are refused with 403.
- **Wi-Fi network** (name and password, *Network & MQTT*): a change is **tested for 60 s** without a restart. If the node connects, the new network is stored; if not, it goes back to the current one and the settings page shows a notice — a typo can't lock you out. The test only counts as passed if the new network also answers (gateway ping or a DNS lookup within 10 s) — a network that blocks both rolls back; use *Store without test* for it. Switching to another network needs DHCP: switch a static IP off in the same step (or before). **Store without test (pre-provisioning)** stores a network that is not in range yet right away: the node leaves the current network immediately and keeps trying the new one (reachable again via that network, downlink `0x03` for the portal, or the button).
- Only switching Wi-Fi or the web API off and the wind-direction calibration wizard are portal-only — so you can't lock yourself out.

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
| Wireless sensor type | **Auto (search)** — cycles through all families and stores the type of the first sensor it hears (Bresser → *Bresser*, Ecowitt / Froggit or IT+ 17.241 → *TFA IT+ 17.241 + Fine Offset*, IT+ 9.579 → *IT+ 9.579*); or fixed: *Bresser*, *TFA / LaCrosse IT+* (both data rates, alternating, or 17.241 / 9.579 kbit/s only), *Fine Offset / Ecowitt / Froggit*, *TFA IT+ 17.241 kbit/s + Fine Offset* (same data rate, received together) or *IT+ both rates + Fine Offset*; *Bresser + TFA IT+* or *Bresser + Fine Offset* alternating is possible but **not recommended** (packets of both are missed) | Auto (search) |
| Wireless sensor IDs | Hex IDs to accept, comma-separated, max 4; empty = any. The order defines *Wireless 1–4* for the value sources; the hint lists the IDs received so far | – |
| Source: temperature / humidity, wind, rain, UV, light | Which sensor feeds each value into all outputs: *Auto* (board first, then wireless 1–4), *Board sensor*, *Wireless 1–4*, *Off* — see [Value Sources](wireless-sensors.md#value-sources) | Auto |
| Wind / rain sensor (board) | The PCB's anemometer / rain gauge — switch on when one is connected (a wireless sensor delivers wind/rain independently, see value sources) | off |
| Keep rain statistics over a power cut | Saves rain today / yesterday / week / month / year, sunshine today / yesterday and the last-rain time (one 68-byte record) to flash at most once per hour when changed, at the day change and before every planned restart or update — so they survive power cuts and firmware updates; off = RTC RAM only (lost on power loss) | on |
| LiPo measurement | Battery voltage measurement | off |
| Altitude | Station altitude in m, for sea-level pressure (QNH) | 560 |
| Heltec board (restart) | *Auto* (by flash size: 8 MB = V3, 16 MB = V4) / *V3* / *V4* — only needed if detection is wrong. On a V4 the PCB revision, board wind / rain sensors and all PCB inputs and outputs are ignored (wireless build only) | Auto |
| PCB revision | 12 = Rev 1.2, 13 = Rev 1.3, 20 = v2.0 (v2.0 is detected automatically); ignored on a Heltec V4 | 13 |
| CPU frequency | 80 MHz (saves power) or 240 MHz | 80 |
| Debug mode | Adds diagnostic fields (cycle counter, free heap, uptime; CPU temperature in the LoRaWAN payload — the JSON/MQTT data always contain it) | off |
| **Units & calibration** | | |
| Units | Metric or Imperial — see [Units](values.md#units-metricimperial) | Metric |
| Wind speed unit | km/h or m/s (metric only) | m/s |
| Wind direction offset | North correction in degrees (−180 to +180) | 0 |
| Rain per tip / wind per pulse | Sensor ratios — see [Using Other Sensors](pcb.md#using-other-sensors) | Davis values |
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
| Wi-Fi / web API | On/off switches | on |
| MQTT / MQTT commands | On/off switches — see [MQTT and Home Assistant](mqtt-home-assistant.md) | off |
| Device name | Hostname for Wi-Fi/DHCP and mDNS (`<hostname>.local`): lower case, characters outside `a–z 0–9` → `-` (runs collapsed, none at the start or end), at most 31 characters (`"Weather Mast"` → `weather-mast`); without a name `weathernode-<last 6 MAC hex digits>`. Also the MQTT client ID (characters outside `A–Z a–z 0–9 _ -` become `_`) and default topic prefix | – |
| Wi-Fi network and password | Portal (*Configure WiFi*) or web settings (tested for 60 s with rollback, or *store without test* for pre-provisioning) | – |
| Static IP, gateway, subnet, DNS | Optional fixed LAN address; leave disabled to use DHCP. Configure in the portal's Wi-Fi form or the web settings page (restart required) | DHCP; subnet 255.255.255.0; DNS defaults to gateway |
| MQTT broker, port, user, password, topic prefix, TLS mode, CA certificate | Broker connection | port 1883, no TLS |
| Web settings admin password | Required for the settings page and the log | – |
| **Web settings page only** | | |
| Weather networks | Station IDs, keys and intervals (AWEKAS, Weather Underground, Weathercloud, Wetternetzwerk.pro, Windy, openSenseMap) — see [Weather Networks](weather-networks.md) | off |
| Firmware update check, version list URL | Daily check for a newer version — see [Firmware Update Check](values.md#firmware-update-check) | off; this project's Gitea tags |
| Latitude / longitude | Only sent to AWEKAS | – |
| Time zone, NTP server | POSIX time zone string and time server | Central Europe, `pool.ntp.org` |

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
| 45–49 | `0x2D`–`0x31` | value sources: temperature/humidity, wind, rain, UV, light | `0` auto, `1` board, `2`–`5` wireless 1–4, `6` off — see [Value Sources](wireless-sensors.md#value-sources) | 2 (wind from wireless 1) | `10 2E 00 02` |
| 50 | `0x32` | `rfProto` | wireless sensor type: `16` Auto (search, stores `1` / `10` / `4` after the first sensor heard), or a bit mask: `1` Bresser, `2` IT+ 17.241 kbit/s, `4` IT+ 9.579 kbit/s, `6` IT+ both, `7` Bresser + IT+, `8` Fine Offset / Ecowitt, `10` IT+ 17.241 + Fine Offset (same data rate, no alternating), `14` IT+ both + Fine Offset, `9` Bresser + Fine Offset; any combination `1..15` (Bresser together with others = the receiver alternates and misses packets, not recommended) | 6 | `10 32 00 06` |
| 51 | `0x33` | `rainPersist` | keep rain and sunshine statistics (today, yesterday, week, month, year, last rain) over power cuts and updates (`0` off / `1` on, at most one flash write per hour plus one per planned restart) | 1 | `10 33 00 01` |
| 52 | `0x34` | `loxEn` | Loxone UDP output `0` off / `1` on | 0 | `10 34 00 01` |
| 53 | `0x35` | `loxSec` | Loxone interval in s: `0` = every measurement cycle, else 2–3600 | 0 | `10 35 00 3C` |

IDs 15–30 and wind-2 mode (33) belong to the [PCB v2.0 I/O block](pcb.md#inputs-and-outputs-pcb-v20); on Rev 1.2/1.3 they are rejected except for disabling a function. Enabling UV (35) also requires v2.0. Calibration values can be prepared where accepted, but do not provide the missing hardware.

Triplets are processed in order, not as an atomic transaction: valid changes are saved even if another triplet is invalid. An uplink or gateway TX_ACK does not confirm that a parameter was applied; check the device log or settings.

**Several at once:** interval 300 s plus debug on → `10 01 01 2C 06 00 01`
