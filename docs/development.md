# Developer Notes

Building and flashing from source, host-side tests, robustness measures, config storage, the project structure, libraries and technical notes.

[← back to README](../README.md)

## Build & Flash

Install [PlatformIO](https://platformio.org/install) (VS Code extension or CLI), clone/download this repository and run commands from its root. Use a USB **data** cable, and select the serial device if more than one is connected.

Environment `heltec_wifi_lora_32_V3` (`espressif32 @ 6.4.0`, Arduino core 2.x):

```bash
pio run                 # build
pio run -t upload       # build + flash over USB (bootloader, partition table and app)
pio device monitor      # serial log, 115200 baud
```

- **A new board must be flashed over USB once** — this writes the custom partition table. After that, use OTA.
- [`firmware/`](../firmware/) contains per release the **application image** `universal-weather-node-<version>.bin` (OTA, or flashed at `0x10000`) and, from 2.0.0-beta.3, the **factory image** `universal-weather-node-<version>-factory.bin` (bootloader + partition table + boot_app0 + application, flashed at `0x0` — see [Installing the Firmware](installation.md#installing-the-firmware-browser-no-software)). The build writes it with `esptool merge_bin` ([`tools/version.py`](../tools/version.py)); `pio run` leaves it in `.pio/build/<env>/firmware-factory.bin`. Manual esptool flashing with the separate parts, for this project's 8 MB layout:

```bash
esptool.py --chip esp32s3 write_flash --flash_size 8MB \
  0x0     bootloader.bin \
  0x8000  partitions.bin \
  0xe000  boot_app0.bin \
  0x10000 firmware.bin          # or firmware/universal-weather-node-<version>.bin
```

Or everything in one go with the factory image: `esptool.py --chip esp32s3 write_flash 0x0 firmware/universal-weather-node-<version>-factory.bin`.

- **Versioning:** [`tools/version.py`](../tools/version.py) stamps the build with `git describe` and generates `include/generated_version.h`, `include/decoder_js.h` and `include/readme_md.h` (git-ignored). Only release builds — an exact tag on a clean tree, or `FW_VERSION=1.2.3 pio run` — are copied to `firmware/`. Tags are written without a leading `v` (e.g. `2.0.0-beta.1`); a leading `v` is stripped from the version anyway.
- **Documentation in the firmware:** `tools/version.py` combines README.md, the `docs/` pages and CHANGELOG.md into one Markdown page for the built-in help (`include/readme_md.h`). Links between the files become in-page anchors, links to other repository files become absolute GitHub URLs. Blocks between `<!-- help:skip -->` and `<!-- /help:skip -->` (pictures, badges) are left out — the renderer supports plain Markdown only.

## Tests

Host-side, no hardware needed: Node.js and a C++11 compiler (`c++`, `g++` or `clang++`). Run a normal PlatformIO build first to restore ArduinoJson for the runtime regressions.

```bash
node --test test/*.test.js           # all six test files: firmware runtime regressions and PCB v2.0 BOM/CPL checks
```

The suite covers decoding/encoder round trips, VE.Direct parsing, PCB v2.0 design/BOM/CPL consistency (needs python3), and five real-module C++ regression programs with deterministic hardware fakes (sensors, I/O, LoRa transport, telemetry and payloads). See [test/README](../test/README) for individual commands. It does not qualify real radio timing, wiring, thermal behavior or physical board compatibility.

## Robustness

- **Watchdog** (120 s): the main loop subscribes after setup and unsubscribes before entering the blocking portal. A radio call that hangs during normal operation can still trigger it.
- **Automatic rejoin** after 10 consecutive send failures, or after 288 uplinks in a row without any downlink (see [Join and Session](lorawan.md#join-and-session)): clear the session and retry after 60 s without rebooting or clearing chart history.
- **EU868 duty cycle** (1 %) is enforced by RadioLib on top of the interval stretching; an uplink that would exceed it is deferred to the next cycle and not counted as a send failure.
- **LoRaWAN session** (frame counter, nonces) is saved after every uplink.
- **Stored settings** are re-checked at boot; out-of-range values are clamped or reset to their default.
- **MQTT** reconnects back off from 15 s to at most 5 min while the broker is unreachable, so the main loop is not blocked by repeated connect timeouts.
- **Button** via interrupt — a short press is never missed, even during LoRa airtime or after low-power sleep.
- **Display** is off in normal operation (turns on for 20 s with a short press) and stays on during startup to show errors.
- **ADC averaging** over 8 samples for wind direction and battery.

## Debug Mode

For bench tests and diagnostics. Adds cycle counter, CPU temperature, send failure counter, free heap and uptime to the LoRaWAN payload (channels 200+) and to the MQTT/REST JSON. `sendFailCount` is always included in the JSON. Off by default to save airtime.

## Config Storage and the cfgdata Partition

Settings live in their own NVS partition, `cfgdata`, separate from the Wi-Fi credentials and from other firmware projects that use the same namespace name.

OTA updates never rewrite the partition table. A board OTA-updated from firmware older than `cfgdata` therefore doesn't have it: the display shows **"cfgdata missing! USB reflash needed"** and the node halts (the SDK forbids writing the partition table at runtime). Fix: flash once over USB, then re-check the settings in the portal.

## Project Structure

```
├── src/
│   ├── main.cpp             # setup/loop, sampling, button, LoRaWAN send
│   ├── lora_transport.h/.cpp # Radio/join retries, session reconnect, joined status
│   ├── config.h/.cpp        # Config struct, NVS load/save
│   ├── partition_check.h/.cpp  # Detects a missing "cfgdata" partition
│   ├── globals.h            # Shared live state between the modules
│   ├── sensors.h/.cpp       # Pins, pulse counters, ADC, sensor reads
│   ├── board_io.h/.cpp      # PCB v2.0: revision ID, AUX inputs, heater/fan, tacho, VE.Direct
│   ├── vedirect.h/.cpp      # Victron VE.Direct text-protocol parser (host-testable)
│   ├── payload.h            # Measurements struct + compact binary layout
│   ├── payload_binary.cpp   # Compact binary encoder (host-testable)
│   ├── lora_payload.h/.cpp  # CayenneLPP encoder
│   ├── history.h/.cpp       # In-RAM history for the dashboard
│   ├── wind_window.h/.cpp   # Wind ring buffer, 3-s gust, per-path windows (host-tested)
│   ├── rf_sensors.h/.cpp    # Bresser 868 MHz wireless sensors on the shared SX1262
│   ├── sources.h/.cpp       # value sources: board or wireless sensor per measured group
│   ├── derived.h/.cpp       # dew point, feels like, cloud base, tendency, warnings, version compare (pure)
│   ├── derived_now.h/.cpp   # snapshot of the derived values from history and daily totals
│   ├── serial_log.h/.cpp    # Serial output + in-RAM log for /api/log
│   ├── portal.h/.cpp        # Config portal (/decoder, /help, /calibrate)
│   ├── params.h/.cpp        # Parameter IDs + commands (LoRaWAN, MQTT, web)
│   ├── json_payload.h/.cpp  # Measurement JSON (MQTT + REST)
│   ├── units.h/.cpp         # Metric/Imperial conversion
│   ├── wifi_mqtt.h/.cpp     # Wi-Fi/MQTT, Home Assistant discovery, MQTT commands
│   ├── web_api.h/.cpp       # REST API, status page, settings page
│   ├── time_sync.h/.cpp     # NTP + time zone
│   └── wx_upload.h/.cpp     # Weather network uploads, daily totals, firmware update check
├── include/
│   ├── chart_page.h         # Dashboard (/)
│   ├── log_page.h           # Log viewer (/api/log)
│   ├── help_page.h          # Help page renderer (/help)
│   ├── calibrate_page.h     # Wind-direction calibration (/calibrate)
│   └── generated_version.h, decoder_js.h, readme_md.h   # generated, git-ignored
├── lib/BresserWeatherSensorReceiver/  # Fork (MIT) of the Bresser 868 MHz decoder, see FORK.md
├── lib/TfaWeatherSensorReceiver/     # TFA / LaCrosse IT+ 868 MHz receiver + decoder (own, MIT), see its README
├── lib/FineOffsetSensorReceiver/    # Fine Offset (Ecowitt / Froggit) 868 MHz receiver + decoder (own, MIT)
├── chirpstack/decoder.js    # Payload decoder (ChirpStack v3/v4, TTN)
├── test/                    # Decoder, round-trip, VE.Direct and firmware runtime tests
├── tools/version.py         # Pre-build script: version, decoder, help (README + docs) headers
├── partitions.csv           # Partition table (with cfgdata)
├── firmware/                # Release binaries (OTA)
├── pictures/                # Photos, screenshots, badges; overview.png is rendered from tools/overview/ (render.sh, headless Chrome)
├── 3dprint/                 # Rev 1.2/1.3 cases + v20/ indoor case, STL/STEP/CAD and assembly guide
├── pcb/v2.0/                # KiCad project, renders, schematic PDF; production/ = Gerber, JLCPCB BOM/CPL
├── pcb/v1.3/                # Fritzing, schematic, layout, BOM, Gerber (Rev 1.2/1.3)
├── platformio.ini
├── docs/                    # Documentation pages (linked from the README)
├── CHANGELOG.md
└── README.md
```

## Libraries

Versions below are the declared constraints in `platformio.ini`, not locked installed versions; `^` allows compatible updates.

| Library | Version constraint | Purpose |
|---------|---------|---------|
| RadioLib | ^7.6.0 | LoRaWAN stack |
| Heltec_ESP32_LoRa_v3 | ^0.9.2 | Board support (display, temperature) |
| LoRaWAN_ESP32 | ^1.2.0 | LoRaWAN session/nonce storage |
| CayenneLPP | ^1.6.1 | Payload encoding |
| WiFiManager | ^2.0.16-rc.2 | Config portal |
| Adafruit BME280 + Unified Sensor | ^2.2.4 / ^1.1.15 | BME280 driver |
| Adafruit BusIO | ^1.16.1 | I²C/SPI abstraction used by the sensor driver |
| OneWire, DallasTemperature | ^2.3.8 / ^4.0.6 | DS18B20 |
| ArduinoJson | ^7.2.0 | JSON (MQTT, REST, discovery) |
| PubSubClient | ^2.8 | MQTT client |
| BresserWeatherSensorReceiver | fork of 0.44.0 in `lib/` | Bresser 868 MHz sensor decoding (MIT, see `lib/BresserWeatherSensorReceiver/FORK.md`) |
| TfaWeatherSensorReceiver | 0.1.0 in `lib/` (own) | TFA / LaCrosse IT+ 868 MHz sensor receiving and decoding (MIT) |
| FineOffsetSensorReceiver | 0.1.0 in `lib/` (own) | Fine Offset (Ecowitt / Froggit) 868 MHz sensor receiving and decoding (MIT; format from the rtl_433 documentation) |
| WiFi, WiFiClientSecure, WebServer, HTTPClient | ESP32 core | Wi-Fi, TLS, web server, uploads |

## Technical Notes

- **CPU frequency ≥ 80 MHz:** the pulse counter's filter runs on the APB clock, which only stays at 80 MHz in PLL mode.
- **1-Wire objects are created at runtime** (`new`), because the pin depends on the PCB revision.
- **ADC linearity:** the ESP32-S3 ADC is non-linear near its range limits. Wind-direction accuracy depends on the sensor, supply and calibration; there is no qualified universal error bound. The battery measurement uses a calibration factor.
- **Tasks:** web serving and weather-network uploads run on core 0; sampling and LoRaWAN run on core 1. Web-server access, history, logs and upload state use mutexes; the LoRa joined flag is atomic. Some live configuration fields are `volatile`, which does not by itself provide general thread synchronization.
- **Local access:** the web server uses unencrypted HTTP. Digest authentication protects the login password, not settings-form contents. The setup portal avoids sending secrets through your LAN, but its access point is open and HTTP is still unencrypted; configure it in a trusted local environment. MQTT TLS mode 2 verifies the broker; mode 1 only encrypts.
- **Weather Underground uses HTTPS** without certificate verification (encrypted, not authenticated — same as MQTT TLS mode 1).
