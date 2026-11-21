# To-do / Roadmap

Open work and ideas for the Universal Weather Node. Done items move to the [Changelog](CHANGELOG.md).

## Done in rc.5 (details in the [Changelog](CHANGELOG.md#200-rc5))

- Settings in tabs, wireless sensor table (★ mine / ○ received / ✕ hidden, "Only my sensors"), whole web interface in five languages, setup wizard
- Package A: calibration, records, send status per output, settings backup / restore, sun and moon
- Package B: alarm rules with ntfy / Pushover / Telegram / MQTT / webhook, custom HTTP uploads, Met Office WOW, CWOP (PWSWeather, Windguru, Windfinder not added: no verifiable public upload specification)
- Package C: Davis phantom-rain filter, OLED weather screens and *Display* settings tab

## Waiting for testers / information

- **Ecowitt field test (Heltec V4.2):** compare values with the console; radio traces of WN32, WN32P, DP30 and DP70 (WH55 data rate)
- **Davis Vantage Pro2 / Vue field test** (frequency offset, channel table, supercap / solar scaling)
- **WS85 / WS90 rain:** raw total since reset, rain-start / wet bit, 16-bit counter overflow, rate-dependent piezo gain
- **WN20 rain correction** (Ecowitt wiki, section WN20, read 2026-11-21): real tip volume ≈ 0.28 mm, but each tip is counted as 0.3 mm and every sixth as 0.4 mm (over-read); the sensor sends this 0.1 mm total (`be16 × 0.1` in our decoder), so we inherit it. Wiki workaround: gain **0.908** up to 5 mm/h, **0.773** at 5–10 mm/h, higher rates still being measured (a linear decrease is expected). Funnel 0.00523 m². → in rc.5: per-sensor gains (rain, wind, UV, light) and an optional rate-dependent WN20 preset (default off). Open: factors above 10 mm/h once the wiki has them.
- **Receive Bresser and Ecowitt / Froggit together without missing packets:** today only alternating (packets lost). Idea: a scheduler that learns each known sensor's transmit interval and switches the SX1262 to that sensor's data rate shortly before its expected packet (like the Davis hop tracking); falls back to alternating while searching (tester with both brands, forum #16 / #20)
- **WS90 / WS80 pressure:** arrays with the newer SHT30 T/RH/P module also report pressure (Ecowitt wiki) — not decoded yet; need a radio trace from an owner
- **WH57 counter semantics:** the wiki says "number per transmission interval" (and the 79-s interval restarts at a strike); testers' values (89 / 137 / 242) look like a running counter. Test: trigger a spark (lighter) near the sensor and check whether the value rises by 1 and stays, or drops back to 0 at the next packet
- **LDS01 (laser distance):** wiki fields air distance / total height / depth / heater count, 80-s interval — suggests a normal Ecowitt frame rather than the rtl_433 draft format; need a radio trace (tester owns one)
- **Ecowitt WH41, WH45, WH55, WN35 not received** with rc.4 by a tester who owns them (decoders from published frames only) — waiting for a radio trace ("only unknown / failed", 15 min)
- **AWEKAS API V2** (JSON) instead of the legacy upload string — spec from AWEKAS support

## Hardware

PCB v2.0 with the double socket for Heltec V3 / V4, the LM66100 ideal diodes and the V4 firmware support are done (rc.5; JLCPCB assembly preview confirmed 2026-11-21). Open:
- **First prototype** — checks in [`pcb/v2.0/CHECKLIST.md`](pcb/v2.0/CHECKLIST.md): 5 V at the V4 during 27 dBm TX, back-feed through the LM66100, USB flashing with the board fitted, GPIO45 low at power-up, P4 / J21 position and pin length on a real V4, the 0.3 mm tracks between the socket pins, V4 enclosure (`3dprint/v20-v4/`) and minimal-build enclosure fit.
- **Measure on a real V4:** `espefuse.py summary` (VDD_SPI_FORCE, DIS_PAD_JTAG, UART_PRINT_CONTROL), FEM marking and the GPIO43 series resistor, whether the 2×2 P4 pins come with the board; firmware on the board: ID on GPIO15, pulses on 44 / 16, 1-Wire on 43, vane on 34, VE.Direct 33 / 45, USB log, display without `heltec_setup()`.
- **Fan blip at boot (V3 and V4):** GPIO39 has its weak pull-up at reset (JTAG eFuse default) against R37 100k → ~2.3 V on the fan MOSFET gate until the firmware runs — check on a v2.0 board; if it blips, lower R37 (e.g. 10k) in a later revision.

## Firmware: use the Heltec V4's extra memory (after rc.5)

The V4 (ESP32-S3R2) has the same 512 KB internal RAM as the V3 but adds **2 MB PSRAM** and **16 MB flash** (V3: ESP32-S3FN8, 8 MB flash, no PSRAM). Today one firmware image serves both boards (8 MB partition layout, PSRAM off), so the V4 runs with the V3's limits.

1. **Check whether one image can use PSRAM when present:** build with PSRAM support and "ignore if not found" (ESP-IDF `CONFIG_SPIRAM_IGNORE_NOTFOUND` in the Arduino-ESP32 sdkconfig for the S3 — verify for our platform version), confirm a V3 still boots normally and a V4 reports the PSRAM (`psramFound()`, `ESP.getPsramSize()`); otherwise a separate V4 image (own env, own `-factory.bin`).
2. **Move memory hogs to PSRAM when available** (`heap_caps_malloc(…, MALLOC_CAP_SPIRAM)` with internal-RAM fallback): TLS buffers (mbedTLS can allocate from PSRAM via `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` — check), history rings, log buffer, page/JSON buffers, radio trace. Keep DMA/ISR-critical buffers internal.
3. **Longer history on PSRAM boards** (e.g. 30 days of 1-h values, 48 h of 5-min values), advertised in `/api/info` so the dashboard adapts.
4. **16 MB flash:** only with a separate V4 image and partition table (bigger OTA slots, room for gzip'd assets) — weigh against the simplicity of one image.
5. Test on a real V4 (heap figures in `/api/info`, TLS notifications while loading pages, OTA with the factory/app image filter).

Until this is done, the README recommends the V4 for its better reception (front end with LNA), not for memory.

## Watch

- Free heap on the test node over days (HTTPS uploads, 32 wireless sensors, radio trace)
