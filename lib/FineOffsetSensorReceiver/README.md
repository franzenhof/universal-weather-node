# FineOffsetSensorReceiver

Receiver and decoder for **Fine Offset Electronics** 868 MHz FSK weather sensors (sold as **Ecowitt**,
**Froggit**, Ambient Weather (915 MHz), Misol, Sainlogic, …) with a **Semtech SX1262** and
[RadioLib](https://github.com/jgromes/RadioLib) 7.x — written for
[universal-weather-node](../../README.md), kept self-contained so it can become a stand-alone
library (same pattern as `TfaWeatherSensorReceiver` next to it). MIT license.

> **Status:** decoder verified against the published example frames of every supported family
> (host tests, `test/firmware_fineoffset.cpp`); **not yet tested with real sensors**. Reports welcome.

## Supported sensors

| Family code | Sensors (examples) | Values |
|-------------|--------------------|--------|
| `0x24` | WH65 / WS69 7-in-1 array (Ecowitt WS69, Froggit WH3000SE / WH4000SE outdoor unit, Ambient WS-2902), WH24 / HP1000 | temperature, humidity, wind, rain, UV index, light; WS69: pressure |
| `0x80` | WS80 ultrasonic array | temperature, humidity, wind, UV index, light |
| `0x90` | WS90 ultrasonic 7-in-1 with piezo rain | temperature, humidity, wind, rain, UV index, light |
| `0x85` | WS85 ultrasonic wind + piezo rain | wind, rain |
| `0x68` | WS68 anemometer with light / UV | wind, UV index, light |
| `0x30`, `0x37` | WH31 / WH31E / WN31 / Froggit DP50, WH31B | temperature, humidity, channel 1–8 |
| `0x34`, `0x38` | WN34S/L/D, Froggit DP150 / DP35 temperature probes; WN38 black globe | temperature |
| `0x40` | WH40 rain gauge (Froggit DP80) | rain |
| `0x20` | WN20 rain gauge | rain |
| `0x51` | WH51 soil moisture (Froggit DP100) | soil moisture % |
| `0x57` | WH57 lightning (Ambient WH31L, Froggit DP60) | strike count, storm distance |
| `0x41`, `0x42` | WH41; WH0290 / Ambient Weather PM25 / Misol PM25 | PM2.5, PM10 (estimated by the sensor) |
| `0x43` | WH43 | PM2.5, PM10 (estimated) |
| `0x45`, `0x46` | WH45 / WH0295 / Froggit DP250 / Ambient AQIN; WH46 | temperature, humidity, PM2.5, PM10, CO2; WH46 also PM1, PM4 |
| `0x55` | WH55 leak (Froggit DP70) | leak alarm, sensitivity, raw value, channel 1–4 |
| `0x35` | WN35 leaf wetness (Froggit DP10) | leaf wetness %, battery voltage |
| `0xA2` | WH52 soil sensor | temperature, soil moisture %, EC (µS/cm); battery not decoded |
| (type `0xE`) | WH25, WH32B, WN32B, WN32P indoor | temperature, humidity, pressure |
| (type `0xD`) | WH32, WN32, WN32EP outdoor | temperature, humidity |

The families added last (air quality, WH55, WN35, WH52, WH25 / WH32) are decoded from published frames
only and **not yet tested with real sensors**.

Only the 868 MHz (EU, "A") versions can be received; 433 / 915 MHz versions use the same format on
another band. Not supported: the older WH1080 / WH1050 (different frame) and the OOK sensors (WH2, WH5, WH0530, WH53 — the SX1262
has no OOK receiver). The WN30 probe thermometer sends the WH31 frame (a humidity of 0 or above 100 is
treated as "none"). The LDS01 laser distance sensor: distance and the low byte of the heater count (from a radio trace).

## Usage

```cpp
#include <FineOffsetSensor.h>
extern SX1262 radio;                    // the application's radio (may be shared, e.g. with LoRaWAN)
FineOffsetSensor fo;

void setup() {
    FineOffsetSensor::setRadio(&radio);
    fo.startReceive();                  // call again after another user of the radio
}

void loop() {
    FoFrame f; float rssi;
    if (fo.getMessage(f, rssi) && f.hasTemp)
        Serial.printf("%s %06lX: %.1f C, battery %s\n", f.model, (unsigned long)f.id, f.tempC, f.batteryOk ? "ok" : "low");
}
```

`foDecode()` (`FineOffsetDecoder.h`) is pure C++ without RadioLib and can be used with any other
receiver: pass it the bytes after the `2D D4` sync word.

**With LaCrosse / TFA IT+ in the same session:** IT+ uses the same bit rate, preamble and sync word.
Start with a receive configuration that covers both (`RxConfig` with `frequencyMhz = 868.28f`,
`bandwidthKhz = 312.0f`, see below), read
the raw frame with `getRaw()` and try `foDecode()` first, then the IT+ decoder (`itPlusDecode()` of
`TfaWeatherSensorReceiver`) — the WS90 family code `0x90` starts with the IT+ model nibble 9, and the
Fine Offset check (CRC + sum over up to 32 bytes) is much stronger than the IT+ one.

## Protocol

FSK, 17.24 kbit/s (58 µs per bit), NRZ, no whitening; preamble `0xAA…` (32–48 bits), sync `0x2D 0xD4`,
then the frame. Every supported family ends with the same two check bytes:

```
FF  <payload>  CC  SS
FF  family code (= model number, e.g. 0x90 = WS90)
CC  CRC-8, polynomial 0x31, init 0x00, not reflected, over all bytes before it
SS  sum of all bytes before it (incl. CC), mod 256
```

The frame length follows from the family code (`foFrameLength()`: 7 bytes for WH31 up to 32 for
WS90; WH55: 10 checked bytes plus 2 of unknown meaning); the receiver therefore reads a fixed `FO_MAX_FRAME_LEN` = 32 bytes (about 15 ms) and the
decoder checks only the family's own length. Field layouts and scalings are documented in
`FineOffsetDecoder.cpp`; notable points:

- **`0x24` family (WH24 / WH65 / WS69):** the two hardware generations use different scalings
  that can only be told apart by the preamble length, which the SX1262 does not report. The
  default is WH65 / WS69 (wind step 0.51 m/s with the mean sent ×8, rain 0.254 mm (0.01 in) per
  count); `wh24Units = true` selects WH24 / HP1000 (1.12 m/s, 0.3 mm). The WS69 appends 8 bytes
  with a 17-bit pressure (Pa) and a second CRC + sum; it is recognised by that second check.
  The UV value is converted to the console's integer UV index steps. Invalid: temperature `0x7FF`,
  humidity `0xFF`, wind direction / speed `0x1FF`, gust `0xFF`, UV `0xFFFF`, light `0xFFFFFF`.
- **WS80 / WS90 / WS85 / WS68:** 9-bit wind values (0.1 m/s, direction in degrees) with their MSBs
  in a flags byte; `0x1FF` = invalid. Temperature (WS80 / WS90) 10 bit, (°C + 40) × 10, `0x3FF` =
  invalid; light × 10 lux; UV index × 10. WS90 / WS85 rain: 16-bit total in 0.1 mm.
- **Rain totals** are the sensors' own 16-bit counters (WH65: 0.254 mm per count, wraps after
  about 16.6 m; WH40 / WN20 / WS90 / WS85: 0.1 mm, wraps after 6553.5 mm) and restart at 0 after a
  battery change; the application has to difference them.
- **Battery:** WH65 / WH31: low-battery bit. Voltage-based (assumed thresholds): WS80 / WS90 / WS85
  > 2.4 V, WH40 ≥ 1.0 V, WH51 ≥ 1.3 V, WN34 > 1.2 V, WN20 > 1.8 V, WH57 level ≥ 1 of 0–2, WS68 raw
  > 0x20.
- **Air quality (WH41 / WH0290 / WH43 / WH45 / WH46):** PM2.5 and PM10 as 14-bit values in 0.1 µg/m³;
  the battery level 0–5 is split over the top bits of both (6 = USB power), ≤ 1 = low. WH41 / WH0290 /
  WH43 use a Honeywell sensor whose PM10 is derived from PM2.5 (`pm10Estimated`). WH45 / WH46: 11-bit
  temperature (°C + 40) × 10, humidity, CO2 in ppm (16 bit); WH46 adds PM1 and PM4 (0.1 µg/m³).
- **WH55 leak:** byte 1 holds two flag bits and the channel (0–3 → CH1–4), the 20-bit ID as the
  Ecowitt app shows it includes these bits; battery level 1–5 (a frame with a level above 6 is
  rejected — an extra guard against WH1050 / TFA 30.3151 frames that also start with 0x55 and carry a
  CRC-8 at byte 8); raw value (lower = wetter); byte 7 bit 7 = high sensitivity, bit 6 = alarm (water
  detected). Byte 9 is the usual sum (found while checking the published frames).
- **WN35:** 24-bit raw A/D, leaf wetness %, battery 20 mV steps.
- **WH52:** temperature 13 bit (°C + 40) × 10 (bits 7–5 of byte 4: a transmission boost counter),
  moisture %, EC = 20-bit value / 25.6 µS/cm (empirical); the battery byte / scaling are still
  disputed upstream and are not decoded.
- **WH25 / WH32 family:** no family byte — `TI IT TT HH PP PP SS XX`: type nibble (0xE with pressure,
  0xD outdoor), 8-bit ID, low-battery and invalid flags, 10-bit temperature (°C + 40) × 10, humidity,
  pressure in 0.1 hPa (0xFFFF = none), SS = 8-bit sum of the first 6 bytes, XX = their XOR with the
  nibbles swapped (WH25 only; missing on WH32B / WN32B, so not required). With only an 8-bit sum,
  frames with implausible values (temperature above 60 °C, humidity above 100 %, pressure outside
  300–1100 hPa) are rejected; `FoFrame::family` is `0x25` / `0x32` (not on the air).
- **IDs:** WH65 and WH31 use an 8-bit ID that changes with every battery change (the WH31 also
  sends its channel); the others use 16–24-bit IDs.

### Frequency

The EU sensors transmit on nominally 868.3 MHz (the same nominal frequency as Bresser and IT+).
`startReceive()` uses 868.3 MHz with the SX1262's 234.3 kHz filter (868.18–868.42 MHz). Together
with IT+ (whose sensors are often 50–75 kHz low) 868.28 MHz with the 312 kHz filter covers both at
the cost of about 1.2 dB sensitivity.

## Sources and license

Independent implementation from the published protocol descriptions: the device documentation in
the comments of the [rtl_433](https://github.com/merbanan/rtl_433) decoders `fineoffset.c`,
`fineoffset_ws80.c`, `fineoffset_ws85.c`, `fineoffset_ws90.c`, `fineoffset_wh31l.c`,
`fineoffset_wn34.c`, `ambientweather_wh31e.c`, `fineoffset_wh45.c`, `fineoffset_wh46.c`,
`fineoffset_wh43.c`, `fineoffset_wh55.c`, `fineoffset_wh52.c` and the WH0290 / WH25 sections of
`fineoffset.c`, the readme of `rtl_433_tests/tests/fineoffset/fineoffset_wh0290`, the issues
[#2756](https://github.com/merbanan/rtl_433/issues/2756) (WH55), [#3614](https://github.com/merbanan/rtl_433/issues/3614)
(WH52), [#3665](https://github.com/merbanan/rtl_433/issues/3665) and pull request
[#3679](https://github.com/merbanan/rtl_433/pull/3679) (WN35), whose example frames are used as test
vectors (each one re-checked: CRC + sum).
rtl_433 is GPL-2.0; **no rtl_433 code is included or translated** — the frame layouts were taken
from its documentation and implemented from scratch. Thanks to the rtl_433 contributors who
analysed these devices.

MIT License, Copyright (c) 2026 Gerhard Massenbichler — see `LICENSE`.
