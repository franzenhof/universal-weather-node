# TfaWeatherSensorReceiver

Receiver and decoder for **TFA Dostmann / LaCrosse "IT+"** 868 MHz temperature / humidity sensors
with a **Semtech SX1262** and [RadioLib](https://github.com/jgromes/RadioLib) 7.x — written for
[universal-weather-node](../../README.md), kept self-contained so it can become a stand-alone
library (same pattern as the `BresserWeatherSensorReceiver` fork next to it). MIT license.

> **Status:** decoder verified against a published TX29-IT frame (host tests); **not yet tested
> with real sensors**. Reports welcome.

## Supported sensors

| Data rate | Sensors (examples) |
|-----------|--------------------|
| 17.241 kbit/s | TFA 30.3143.IT, 30.3144.IT, 30.3147.IT, 30.3157.IT, 30.3159.IT; LaCrosse/Technoline TX21-IT, TX25-IT, TX27-IT, TX29-IT |
| 9.579 kbit/s | TFA 30.3155.WD, 30.3156.WD; LaCrosse/Technoline TX35-IT, TX37 |

Not supported: TFA **WeatherHub** / Mobile Alerts (see below), KlimaLogg (38.4 kbit/s, NRZ-S),
433 MHz OOK sensors (the SX1262 has no OOK receiver).

## Usage

```cpp
#include <TfaWeatherSensor.h>
extern SX1262 radio;                      // the application's radio (may be shared, e.g. with LoRaWAN)
TfaWeatherSensor tfa;

void setup() {
    TfaWeatherSensor::setRadio(&radio);
    tfa.startReceive(TfaWeatherSensor::IT_PLUS_17241);   // call again after another user of the radio
}

void loop() {
    ItPlusFrame f; float rssi;
    if (tfa.getMessage(f, rssi))
        Serial.printf("ID %u: %.1f C %u %% battery %s\n", f.sensorId, f.tempC, f.humidity, f.weakBattery ? "low" : "ok");
}
```

Only one data rate is received at a time; to cover both, call `startReceive()` alternately with
both rates (the sensors repeat about every 4 s, so 5 s per rate catches each one).

## Protocol

868.3 MHz FSK (the sensors are often 50–75 kHz low; the receiver uses 868.26 MHz and 234 kHz
bandwidth), NRZ, preamble `0xAA…` (some sensors send only 6 preamble bits), sync `0x2D 0xD4`,
then 5 bytes:

```
MMMM IIII  IINW TTTT  TTTT TTTT  BHHH HHHH  CCCC CCCC
```

| Field | Meaning |
|-------|---------|
| M | model nibble, always 9 |
| I | 6-bit sensor ID — random, new after every battery change |
| N | new-battery flag (set for a while after a battery change) |
| W | unused |
| T | temperature, three BCD digits of (°C + 40) with one decimal |
| B | weak-battery flag |
| H | humidity in %; `0x6A` = no humidity sensor, `0x7D` = second probe of a two-channel sensor (reported with ID + 64) |
| C | CRC-8, polynomial `0x31`, init `0x00`, over the first 4 bytes |

Independent implementation from the published protocol descriptions (rtl_433 `lacrosse_tx35`
notes, FHEM LaCrosse documentation); no third-party code included.

## WeatherHub / Mobile Alerts (not supported yet)

TFA WeatherHub (30.3302–30.3311.02, …) and Technoline Mobile Alerts (MA10xxx) send 6000 bit/s
FSK whose bits are differentially coded (NRZ-S) and scrambled (G3RUH, x¹⁷ + x¹² + 1) before
transmission; the frame (`4B 2D D4 2B`, length, 6-byte ID, payload, CRC-32 with a per-type init)
is only visible after descrambling. The SX1262 packet engine needs a fixed on-air sync pattern,
which cannot be derived reliably without recordings. Needed to add it: a few raw recordings of a
WeatherHub sensor (e.g. `rtl_433 -f 868.25M -s 1M -S all` or a `tfrec` dump), then either a fixed
on-air sync or a raw-bit receive mode with software descrambling.
