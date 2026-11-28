#pragma once
// Encode a small set of Fine Offset sensor frames - the inverse of foDecode -
// from the node's own values, for the virtual-sensor / protocol-bridge feature
// (TODO 4.0.0 item 9): a wired Davis / BME280 can appear to an Ecowitt console,
// app or gateway as a standard sensor.
//
// Hard limits, by design (see the project's 868 MHz TX policy):
//   - ONLY sensor readings are encoded; there is NO raw "send these bytes" path.
//   - NO actuator / gateway command frames exist here at all.
//   - the ID is supplied by the caller (meant to be the node's own fixed ID,
//     never a foreign sensor's); the transmit path is a separate, V4-only step.
//
// Each function writes a complete frame (payload + the trailing CRC-8 / sum, or
// WH25's sum) into out (at least FO_MAX_FRAME_LEN bytes) and returns its length,
// or 0 on invalid input. Round-trip tested against foDecode (test/firmware_foencode.cpp).
#include "FineOffsetDecoder.h"
#include <cstddef>
#include <cstdint>

// WH31 / WN31 thermo-hygrometer: channel 1..8, 8-bit id, temperature in °C.
// hasHum false encodes a temperature-only WN30 frame; a present humidity is
// clamped to 1..99 % (the WH31's range).
size_t foEncodeWh31(uint8_t* out, uint8_t id, uint8_t channel, float tempC,
                    bool hasHum, uint8_t humidity, bool batteryOk);

// WH40 rain gauge: 20-bit id, running rain total in mm (0.1 mm resolution).
size_t foEncodeWh40(uint8_t* out, uint32_t id, float rainTotalMm, bool batteryOk);

// WN32P / WH32B thermo-hygro-barometer (WH25 family): 8-bit id, temperature in
// °C, humidity (0 = none), station pressure in hPa (negative = none). Ideal for
// a BME280. At least one of the three values must be present.
size_t foEncodeWn32p(uint8_t* out, uint8_t id, float tempC, uint8_t humidity,
                     float pressureHpa, bool batteryOk);

// Outdoor array WH65 / WS69 (the Ecowitt 7-in-1): wind (speed + gust in m/s,
// direction 0..359°), rain running total, temperature, humidity, UV index and
// illuminance - for a wired Davis ISS or similar all-in-one. Each has<X> false
// leaves that value out (the frame sends the sensor's "none" code); rain is
// always present in this frame (0 if there is no gauge). Pressure is not in
// this frame - send it as a WN32P.
struct FoWh65Values {
    uint8_t  id = 0;
    bool     batteryOk = true;
    bool     hasTemp = false;   float    tempC = 0.0f;
    bool     hasHum = false;    uint8_t  humidity = 0;
    bool     hasWind = false;   float    windAvgMs = 0.0f, windGustMs = 0.0f;  uint16_t windDirDeg = 0;
    bool     hasRain = false;   float    rainTotalMm = 0.0f;
    bool     hasUv = false;     uint8_t  uvIndex = 0;
    bool     hasLight = false;  float    lightLux = 0.0f;
};
size_t foEncodeWh65(uint8_t* out, const FoWh65Values& v);
