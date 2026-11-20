# BresserWeatherSensorReceiver – universal-weather-node fork

Upstream: https://github.com/matthias-bs/BresserWeatherSensorReceiver
Base: commit `8c03c8627c90749dfc1f6b1e37e81aa351e152ad` (2026-11-13, library version 0.44.0), MIT license (see `LICENSE`).

Only the files needed for receiving and decoding are included:
`WeatherSensor.*`, `WeatherSensorCfg.h`, `WeatherSensorConfig.cpp`, `WeatherSensorDecoders.cpp`, `WeatherUtils.*`.
Not included: `RainGauge`, `Lightning`, `RollingCounter`, `InitBoard`, examples, tests, docs.

## Changes

1. **No own radio object.** Upstream creates a global `RADIO_CHIP radio` from board pin macros. The fork keeps a
   pointer (`WeatherSensorReceiver::radio`) that the application sets with `WeatherSensor::setRadio(&radio)`, so the
   SX1262 of the Heltec WiFi LoRa 32 V3 is shared with RadioLib's LoRaWAN stack (no second `Module`, no pin conflict).
2. **RadioLib 7.x** (the firmware uses 7.7.1) instead of the exact `RadioLib (=8.0.0)` pin: `ConfigFSK_t` in RadioLib 7
   units (MHz / kbps / kHz instead of Hz), `setPacketReceivedAction()` instead of `setIrqAction()`. Radio parameters
   unchanged: 868.3 MHz, 8.21 kbps, 57.136 kHz deviation, 234.3 kHz RX bandwidth (SX1262), 32-bit preamble,
   sync `AA 2D`, fixed packet length, no CRC.
3. **`startReceive()`** split out of `begin()`: configures FSK and starts interrupt-driven reception. The application
   calls it again after every LoRaWAN uplink (RadioLib reconfigures the chip for LoRa). `begin()` and
   `startReceive()` return a RadioLib error code instead of blocking (`RADIOLIB_ERR_NULL_POINTER` without a radio).
4. **`setSensorIds(inc, exc)`** sets the include/exclude filters directly; the firmware keeps them in its own
   configuration and calls `begin(n, false)` so the library's `Preferences` lists are not used.
5. Default exclude list emptied (upstream excluded one specific sensor ID).
6. PlatformIO board define `ARDUINO_heltec_wifi_lora_32_V3` mapped to `ARDUINO_HELTEC_WIFI_LORA_32_V3`
   (`WeatherSensorCfg.h`), so the SX1262 is selected.
7. **`lastSlot`**: index of the slot written by the last successful decode (set in `findSlot()`), so the
   application can copy exactly the sensor that was just received.
8. **`msg_ok`** (`MSG_OK_*` bits) per slot: the values carried by the *last* message (reset in `findSlot()`, set by the
   5/6/7-in-1 decoders). The 6-in-1 decoder ORs its per-message flags into `w.*_ok`, which are only cleared by
   `clearSlots()`, so the application could not tell which values a packet actually contained (per-value freshness).
   The cumulative `w.*_ok` behaviour is unchanged.

9. **`setRawHook()`**: optional callback with every frame read in `getMessage()` (raw bytes, RSSI, decode result),
   also for frames that fail their digest/checksum or match no decoder - for the firmware's radio trace.

Decoders (`WeatherSensorDecoders.cpp`) are otherwise unchanged.
