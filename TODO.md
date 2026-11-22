# To-do / Roadmap

Open work and ideas for the Universal Weather Node. Done items move to the [Changelog](CHANGELOG.md).

## Roadmap (agreed 2026-11-22)

**Two lines:** 2.x stays the firmware for the Heltec **V3 and V4**; the next major version is **V4 only** (ESP32-S3R2 with 2 MB PSRAM) and gets the new framework, the better web server and a much bigger dashboard. The V3 has no PSRAM — it is what held back both the framework update and the web server (see [background](#next-major-version-background-framework-and-web-server-experiments)).

### 2.0.0 — stabilise sensors and system

**Released 2026-11-22** (decided with most exit criteria met; the open ones — tester field tests, PSRAM on a real V4, long heap runs — continue on 2.0.x). `main` stays the 2.x line until the own Heltec V4 arrives; then the switch to the next major version (branch `2.x` from the 2.0.0 tag as planned below).

Exit criteria (2.0.0 final only when all are met):
- [ ] Tester field tests done: Ecowitt values compared with the console, WS90 reception rate explained (rc.9 trace), Davis on site
- [ ] no open crash / restart report
- [ ] free heap stable over several days on the test node (HTTPS uploads, many wireless sensors, radio trace)
- [ ] PSRAM confirmed running on a real V4 (status page / `system.psram`)
- [ ] docs and README match the firmware

**Done in 2.0.0: custom series** ("eigene Messreihen") — values that are not one of the main weather values (soil moisture, lightning, leaf wetness, PM2.5, CO₂, soil EC, a DS18B20 …) get a chart: a free-text name, a source (wireless sensor ID + value, or a board probe) and a fixed aggregation per value (mean; lightning: strikes per bucket from the running counter; distance: minimum). Rings only for defined series (int16, ~0.9 KB each for 24 h + 7 days), V3 up to 4, V4 with PSRAM up to 8; the 7-day tier is saved with the history as now (cfgdata NVS, 16 KB reserve). Then the last rc.9 or 2.0.0 directly.

Afterwards: branch **`2.x`** from the 2.0.0 tag — small fixes there, or back-ported from the next major version (cherry-pick). `main` becomes the next major version. V3 users get fixes on 2.x for a stated time (decide: e.g. 12 months).

### Next major version (3.0.0 or 4.0.0 — see versioning) — V4 only

Milestones in this order:
1. **V4-only image on Arduino-ESP32 3.x** (pioarduino; the port exists on branch `framework-arduino3`), framework libraries built with our own sdkconfig (`custom_sdkconfig`): system-task stacks (esp_timer, lwIP, IPC), Wi-Fi / lwIP allowed in PSRAM, `esp_http_server` header limit. **Measure first** (internal RAM, largest block, PSRAM use with a full sensor set) — internal RAM stays 512 KB, IDF 5 needs ~35 KB more of it; the figures decide how far the dashboard can go.
2. **Update path:** the update check filters by board (a V3 is offered 2.x only), the new image refuses to run on a V3 with a clear message; OTA migration from 2.x on a V4 tested (old IDF 4.4 bootloader + IDF 5 app worked in the 2026-11-21 experiment). **Decide:** keep the 8 MB partition table (OTA from 2.x possible) or use the 16 MB flash (bigger OTA slots, file system for dashboard assets — one USB flash or an own migration step).
3. **Web server → `esp_http_server`** with a larger request-header limit (≥ 2048 B), all 42 routes ported, own Digest login, raw-body uploads, URL-decoded form bodies.
4. **Dashboard and history:** weeks of history in PSRAM, more charts and figures, assets from flash, several connections at once. **Custom series (from 2.0.0) extended:** more series (e.g. 16–32), longer history (weeks / months) in an **own flash partition** for the history (16 MB flash: e.g. 1–2 MB; partition table change = one USB flash), series also to MQTT / Home Assistant and as placeholders, own aggregations selectable. The node stays LAN-only; public pages remain the separate project pulling the REST API.

Hardware alongside: at least **two own V4 (S3R2)** as test nodes before milestone 1; PCB v2.0 keeps working (V4 socket), a later v2.1 could drop the V3 socket.

### Versioning

- Normal semantic versions, **not** tied to the Heltec board generation (a future board without a firmware break, or a firmware break on the same board, would not fit; "V4" also includes V4.2 / V4.3 but not V4-R8).
- **Board in the file names**, e.g. `universal-weather-node-2.0.3-heltec-v3-v4.bin`, `universal-weather-node-3.0.0-heltec-v4.bin` (and `-factory.bin`).
- **Compatibility table** at the top of the README: Heltec V3 → 2.x · Heltec V4 (S3R2) → next major (2.x runs too) · V4-R8 → not supported.
- Status page / portal show the board and the firmware line.
- **Decide:** next major **3.0.0** or **4.0.0** (once, as a "V4 ↔ 4" memory hook — no promise for later versions).

## Done in rc.5 (details in the [Changelog](CHANGELOG.md#200-rc5))

- Settings in tabs, wireless sensor table (★ mine / ○ received / ✕ hidden, "Only my sensors"), whole web interface in five languages, setup wizard
- Package A: calibration, records, send status per output, settings backup / restore, sun and moon
- Package B: alarm rules with ntfy / Pushover / Telegram / MQTT / webhook, custom HTTP uploads, Met Office WOW, CWOP (PWSWeather, Windguru, Windfinder not added: no verifiable public upload specification)
- Package C: Davis phantom-rain filter, OLED weather screens and *Display* settings tab

## Waiting for testers / information

- **Ecowitt field test (Heltec V4.2):** compare values with the console (dew point, feels-like, relative pressure, rain amounts). Radio trace of 21 Nov (forum #41, rc.8, 21 sensors) evaluated: every decoder gives stable, plausible values (real frames are now host tests); WN32 / WN32P confirmed as 8-byte frames with only the sum at byte 6 (byte 7 constant per sensor, meaning unknown); DP70 / WH55 arrives at the normal 17.24 kbit/s; send intervals match the nominal table. Still open: a DP30 trace.
- **WS90 reception only ~30 %** (forum trace: 32 of ~103 transmissions in 15 min, 132 in 74 min) although it is one of the strongest sensors (−70 dBm) and no WS90 frame failed — the others arrive at ~100 %. Losses come in runs (up to 114 s) while other sensors are received. rc.9 traces show the expected-vs-received rate per sensor and the read delay (packet interrupt → firmware); next: a trace from the WS90 owner, then look at frame variants / timing.
- **Davis Vantage Pro2 / Vue field test** (frequency offset, channel table, supercap / solar scaling)
- **WS85 / WS90 rain:** raw total since reset, rain-start / wet bit, 16-bit counter overflow, rate-dependent piezo gain
- **WN20 rain correction** (Ecowitt wiki, section WN20, read 2026-11-21): real tip volume ≈ 0.28 mm, but each tip is counted as 0.3 mm and every sixth as 0.4 mm (over-read); the sensor sends this 0.1 mm total (`be16 × 0.1` in our decoder), so we inherit it. Wiki workaround: gain **0.908** up to 5 mm/h, **0.773** at 5–10 mm/h, higher rates still being measured (a linear decrease is expected). Funnel 0.00523 m². → in rc.5: per-sensor gains (rain, wind, UV, light) and an optional rate-dependent WN20 preset (default off); rc.6 docs: plain factor 0.908 is the recommended way, the preset is experimental. Open: factors above 10 mm/h once the wiki has them.
- **Bresser and Ecowitt / Froggit together:** timed switching built in rc.6 (`src/rf_schedule.cpp`, simulation 98–100 % of the packets) — field test with both brands pending (forum #16 / #20, luftstrom)
- **WS90 / WS80 pressure:** arrays with the newer SHT30 T/RH/P module also report pressure (Ecowitt wiki) — not decoded yet; need a radio trace from an owner
- **WH57 counter semantics:** the wiki says "number per transmission interval" (and the 79-s interval restarts at a strike); testers' values (89 / 137 / 242) look like a running counter, and the 21 Nov trace confirms it: 89 in every packet for 15 min without a storm (distance 255) — it does not drop to 0. Lighter-spark test would still settle whether one strike adds exactly 1
- **LDS01 (laser distance):** found in the 21 Nov trace: family `0x54`, 19-byte frames (CRC-8 at 17, sum at 18), every 80 s, ID `54802801` — recognised and listed since rc.9, values not decoded. Bytes after the ID: `8C 64 | 0529 | 4000 | 0007→000E (slowly rising: heater count?) | 093F–094C | 1 varying byte | 1A66 (constant)`. Need the console's LDS01 readings (air distance, depth, total height, heater count) at a time inside a trace to map the fields.
- ~~Ecowitt WH41, WH45, WH55, WN35 not received with rc.4~~ — all four received and decoded with rc.8 (21 Nov trace); remove after rc.9.
- **AWEKAS API V2** (JSON) instead of the legacy upload string — spec from AWEKAS support

## Hardware

PCB v2.0 with the double socket for Heltec V3 / V4, the LM66100 ideal diodes and the V4 firmware support are done (rc.5; JLCPCB assembly preview confirmed 2026-11-21). Open:
- **First prototype** — checks in [`pcb/v2.0/CHECKLIST.md`](pcb/v2.0/CHECKLIST.md): 5 V at the V4 during 27 dBm TX, back-feed through the LM66100, USB flashing with the board fitted, GPIO45 low at power-up, P4 / J21 position and pin length on a real V4, the 0.3 mm tracks between the socket pins, V4 enclosure (`3dprint/v20-v4/`) and minimal-build enclosure fit.
- **Measure on a real V4:** `espefuse.py summary` (VDD_SPI_FORCE, DIS_PAD_JTAG, UART_PRINT_CONTROL), FEM marking and the GPIO43 series resistor, whether the 2×2 P4 pins come with the board; firmware on the board: ID on GPIO15, pulses on 44 / 16, 1-Wire on 43, vane on 34, VE.Direct 33 / 45, USB log, display without `heltec_setup()`.
- **Fan blip at boot (V3 and V4):** GPIO39 has its weak pull-up at reset (JTAG eFuse default) against R37 100k → ~2.3 V on the fan MOSFET gate until the firmware runs — check on a v2.0 board; if it blips, lower R37 (e.g. 10k) in a later revision.

## Firmware: use the Heltec V4's extra memory (after rc.5)

The V4 (ESP32-S3R2) has the same 512 KB internal RAM as the V3 but adds **2 MB PSRAM** and **16 MB flash** (V3: ESP32-S3FN8, 8 MB flash, no PSRAM). One firmware image serves both boards (8 MB partition layout); since rc.9 it starts the PSRAM when present (`BOARD_HAS_PSRAM`). Only the **S3R2** counts: the V4-R8 (ESP32-S3R8, 8 MB octal PSRAM) needs GPIO33–37 for the PSRAM, which the board and the PCB use — not supported, and the firmware says so.

*Status rc.9:* up to rc.8 the PSRAM was never started (`-D BOARD_HAS_PSRAM` missing: without it the core compiles `psramInit()` out). Now set; the core tries on every board and adds a PSRAM to the heap (malloc ≥ 4 KB prefers it; V3: "PSRAM ID read error" on UART0, then normal); history rings and wind window use `bigAlloc()` (`src/psram.h`), memory guards count internal RAM, status page / `/api/info` / boot log show the PSRAM or why it did not start. Open: confirm on a real V4 (status page / `/api/info` `system.psram`, boot log); then items 3–5.

1. **Check whether one image can use PSRAM when present:** build with PSRAM support and "ignore if not found" (ESP-IDF `CONFIG_SPIRAM_IGNORE_NOTFOUND` in the Arduino-ESP32 sdkconfig for the S3 — verify for our platform version), confirm a V3 still boots normally and a V4 reports the PSRAM (`psramFound()`, `ESP.getPsramSize()`); otherwise a separate V4 image (own env, own `-factory.bin`).
2. **Move memory hogs to PSRAM when available** (`heap_caps_malloc(…, MALLOC_CAP_SPIRAM)` with internal-RAM fallback): TLS buffers (mbedTLS can allocate from PSRAM via `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` — check), history rings, log buffer, page/JSON buffers, radio trace. Keep DMA/ISR-critical buffers internal.
3. **Longer history on PSRAM boards** (e.g. 30 days of 1-h values, 48 h of 5-min values), advertised in `/api/info` so the dashboard adapts.
4. **16 MB flash:** only with a separate V4 image and partition table (bigger OTA slots, room for gzip'd assets) — weigh against the simplicity of one image.
5. Test on a real V4 (heap figures in `/api/info`, TLS notifications while loading pages, OTA with the factory/app image filter).

Until this is done, the README recommends the V4 for its better reception (front end with LNA), not for memory.

## Next major version: background (framework and web server experiments)

Measurements behind the roadmap; both items return in the next major version (milestones 1 and 3).


1. **Web server → `esp_http_server`: postponed, the Arduino `WebServer` stays for now.** Prototype 2026-11-22 on Arduino-ESP32 2.0.11 (which ships `esp_http_server` from ESP-IDF 4.4), next to the WebServer on port 8080, 8-KB task stack, at most 4 connections:
   - RAM is no obstacle: `httpd_start` 13 KB (8 KB stack + ~5 KB), ~1 KB static; minus the `webApi` task (8 KB) and the WebServer object about 3–5 KB more in total. 12 parallel 20-KB pages plus old-server pages: all answered, ≥ 88 KB free in the handlers.
   - limits work: header too long → 431, URI > 512 B → 414, 100 KB of header garbage rejected without restart or heap loss.
   - blocker: the request-header limit of 1024 B is compiled into the 2.0.11 library and applies to the whole header block (2 × 600 B → 431). A Chrome request with Digest login is ~990 B — a longer Referer / URL or one more header would lock the settings pages out with 431. Way out: vendor `esp_http_server` from ESP-IDF 4.4 (Apache-2.0) with e.g. 2048 B (~1 KB RAM more); untested whether it links cleanly next to the built-in library.
   - porting work (42 routes): own Digest authentication (MD5 from mbedTLS, nonces), no multipart parser (OTA / backup upload: send the raw file body instead), POST form bodies read and URL-decoded ourselves, response header strings must live until the response is sent. The setup portal keeps WiFiManager's WebServer.
   - if picked up again: first the vendored library with the larger header limit, then the routes.
2. **Arduino-ESP32 3.x (ESP-IDF 5): postponed while Heltec V3 nodes are supported.** Experiment 2026-11-21 (branch `framework-arduino3`, pioarduino 55.03.313 = Arduino 3.3.13 / ESP-IDF 5.5, kept as reference, not merged): ported (PCNT on the new `pulse_cnt` driver, LEDC, task watchdog, miniz, HWCDC, `WiFi.h`), builds without warnings, host tests pass, runs on the V3 test node (old and new bootloader, erased first start, wireless reception, web pages). Rejected because of RAM on the V3 (no PSRAM), measured with the same diagnostics in both builds, 90 s after the start:
   - free heap 137 KB → 102 KB (about 94 KB after hours), largest free block 127 KB → 61 KB (about 35 KB after hours); web-task stack 1.5 KB tighter.
   - where it goes: ~5.5 KB smaller heap from the start (static RAM / IRAM of the framework), ~7.7 KB larger system-task stacks in the 3.x sdkconfig (esp_timer 4 → 8 KB of which over 7 KB unused, lwIP 2.5 → 4 KB, IPC 2 × 1 → 2 KB), ~14 KB more used during `setup()` (not narrowed down: Arduino 3.x network layer, new IDF 5 drivers, pin manager), ~7 KB more for Wi-Fi bring-up and runtime. Wi-Fi buffer settings are the same in both. Bluetooth is no lever: 3.x already releases the BT memory at the start (2.0.11 did not).
   - recoverable at best ~10–15 KB (system-task stacks via pioarduino `custom_sdkconfig` = rebuilding the framework libraries, smaller own task stacks), not the 2.x level.
   - also found: once after the update the stored Wi-Fi config allowed WPA3 SAE with H2E only → reason 210 against the WPA2/WPA3 access point (guard `fixLegacySaePwe()` on the branch); pioarduino overwrote `.gitignore` on the first build; 2.0.11 and 3.x share `~/.platformio/packages/framework-arduinoespressif32` (switching branches reinstalls the framework).
3. **Arduino-ESP32 2.0.11 → 2.0.17 (ESP-IDF 4.4.7, `espressif32 @ 6.11.0`): done in rc.9 (2026-11-22).** The last 2.x release; builds unchanged, same diagnostics on the V3 test node: 1–4 KB more free internal RAM (boot 247 → 256 KB, lowest 128 → 132 KB, largest block 123 → 127 KB). Tested: OTA-like app-only flash on the old bootloader, erased first start (portal), wireless reception, web pages (gzip and plain fallback), 2 h without restart. The measurements above were made on 2.0.11.
   - Reconsider when only V4 nodes (PSRAM) are left, or for a V4-only build.

- **The framework's `WebServer` has no request limits** (URI / header length, number of arguments, one request at a time, no login throttling). An oversized request can make the node restart; an uncaught allocation failure inside the library can't be caught from our code. Until then: keep the node in its own network (IoT VLAN), never reachable from the internet; public pages come from a separate project that pulls the REST API. Long-term: ESP-IDF's `esp_http_server` (see item 1 above: postponed).

## Watch

- Free heap on the test node over days (HTTPS uploads, 32 wireless sensors, radio trace)
