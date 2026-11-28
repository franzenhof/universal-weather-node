# Troubleshooting

Symptoms and what to check first, plus the known limits of the current hardware and firmware.

[← back to README](../README.md)

Start with a short button press to view the display and `pio device monitor` for the serial log. With Wi-Fi/web API and an admin password configured, use **Status** and **Log** in the browser. Log messages and a complete measurement cycle are more useful than changing several settings at once.

| Symptom | Check first |
|---------|-------------|
| Nothing boots / USB not detected | USB data cable, stable power, correct serial port; on v2.0 disconnect external supplies and remove JP8 before USB |
| Display says `cfgdata missing` | Perform a full [USB flash](development.md#build--flash) with the current partition table; OTA alone cannot fix it |
| Portal does not open | Hold the button for 3 s **after boot**, connect to `WeatherNode`, enter `http://192.168.4.1` in a normal browser; disable mobile-data/VPN routing if it interferes |
| No measurements sent yet | Wait for the output's interval (LoRaWAN / MQTT default 300 s; the first measurement cycle completes 30 s after the start); check transport enable flags. An active portal pauses normal operation |
| LoRaWAN remains “not joined” | DevEUI/JoinEUI/AppKey, region, network profile and gateway coverage; inspect the logged error. Retries back off from 1 to 60 min (`retry in N min` in the log); a restart starts again at 1 min |
| Uplinks arrive, command does not run | Queue HEX `03` on **FPort 1**, wait for the next uplink; inspect gateway TX_ACK and device logs. FPort 0 traffic is MAC control, not this command; this firmware uses Class A |
| Wind/rain stays at zero | Correct manual Rev 1.2/1.3 selection, sensor enabled, numbered pinout, reed contact; debug log shows pulse counts |
| BME280 missing | Actual BME280 (not BMP280), 3.3 V supply, address 0x76, SDA/SCL wiring, cable length and pull-ups |
| DS18B20 missing / timeout | Three-wire VDD/GND/DQ wiring, 3.3 V supply, cable/contact faults; on Rev 1.3/v2.0 power is switched only for conversions |
| Settings/Log cannot be opened | Enable Wi-Fi + web API and set an admin password in the portal; browser login user is `admin` |
| MQTT or weather uploads absent | Enable the service, verify credentials and network reachability; weather uploads also require NTP time. Broker TLS mode must match its listener |
| Heater/fan stays off | Correct supply and jumper, mode, first valid temperature in auto, VE.Direct battery/PV interlocks; entering the portal latches outputs off until restart |
| Wireless sensor not listed | *Wireless sensors* on, sensor within range (RSSI on the status page; above about −100 dBm is fine), ID filter empty or containing its ID; only Bresser, TFA / LaCrosse IT+, Fine Offset (Ecowitt / Froggit) and Davis (experimental) 868 MHz sensors are supported — *Wireless sensor type* must match (for IT+ try *both data rates*). *Paused for a LoRaWAN uplink* on the status page is normal |
| Wireless sensor not recognised, wrong values, received only sometimes | Capture a [radio trace](wireless-sensors.md#capturing-a-radio-trace-for-us) on the log page (`/api/log`) (*only unknown / failed packets*, or *only this ID*) and send us the downloaded file — it shows every packet the receiver got, also those that failed their checks |
| UV voltage or index is wrong | Correct J17 pin numbers, one sensor supply, JP10 and matching scale; a UV index additionally needs the sensor-specific transfer function and both calibration/index switches |

**Memory and HTTPS:** every HTTPS request (notifications, Weather Underground / Windy / openSenseMap, custom uploads to `https://`, the update check) needs about 45–55 KB of heap at its peak — the firmware's TLS library has fixed 16 KB receive and send buffers that cannot be made smaller with this toolchain. So a request only starts with at least **70 KiB free** in total and **24 KiB in one free block** (the two record buffers are separate ~16.7 KB allocations; the rest of the handshake comes in small pieces), — on a V4 with PSRAM the two record buffers go there and **32 KiB** of free internal RAM are enough — and not while a settings, status, wizard or backup page is being built (it waits a few seconds); otherwise it is retried with a back-off. While one runs with less than 50 KiB free, those pages answer *busy, try again* (HTTP 503) for a moment. Plain HTTP uploads and MQTT without TLS are not affected. Watch `free`, `minimum` and `largest block` on the status page or in `/api/info` `system`; with *Debug mode* on, the log shows the heap before and after every HTTPS request.

**Known limits:** v2.0 has not been qualified on real hardware. Current targets are not lightning/surge certification or guaranteed thermal ratings. An externally powered analog sensor can backfeed an unpowered controller; avoid powered signals with the node off. Chart history survives restarts and updates (a power cut loses the 5-min values and up to a day of hourly ones), and RTC-backed rain/AUX totals are lost on power loss. Keep the device on a trusted local network; neither the HTTP interface nor the open setup portal provides encrypted configuration transport. Any device on the network can also make the node restart: the Arduino web server parses request arguments before the login check and without a limit, so one request with thousands of arguments exhausts the memory (status page: *Last restart: crash (panic)*; settings and data are kept). The firmware cannot intercept this; the 4.x line for the Heltec V4 has its own web server with limits.
