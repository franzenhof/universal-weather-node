#pragma once
// Fine Offset Electronics 868 MHz FSK sensors - also sold as Ecowitt, Froggit,
// Ambient Weather (915 MHz), Misol, Sainlogic, ...
//
// Air frame (all families below): 17.24 kbit/s FSK (58 us per bit), NRZ,
// preamble 0xAA..., sync 0x2D 0xD4, then
//   FF  family code (= model number, e.g. 0x90 = WS90)
//   ..  family specific payload
//   CC  CRC-8, polynomial 0x31, init 0x00, over all bytes before it
//   SS  8-bit sum (mod 256) of all bytes before it (including CC)
// The frame length follows from the family code (foFrameLength()).
// Exception: the WH25 / WH32 family (see FO_FAMILY_WH25 below).
//
// Independent implementation from the published protocol descriptions (the
// rtl_433 decoder documentation for these devices); no third-party code included.
#include <stddef.h>
#include <stdint.h>

constexpr size_t FO_MAX_FRAME_LEN = 32;   // WS90

// Family codes (first payload byte)
constexpr uint8_t FO_FAMILY_WH65 = 0x24,   // WH24 / WH65 / WS69 7-in-1 array (WH2600, WH3000SE, WS2902, ...)
                  FO_FAMILY_WN20 = 0x20,   // WN20 rain gauge
                  FO_FAMILY_WH31 = 0x30,   // WH31 / WH31E / WN31 / DP50 thermo-hygrometer
                  FO_FAMILY_WH31B = 0x37,  // WH31B thermo-hygrometer
                  FO_FAMILY_WN34 = 0x34,   // WN34S/L/D, DP150, DP35 temperature probe
                  FO_FAMILY_WN38 = 0x38,   // WN38 black globe thermometer
                  FO_FAMILY_WH40 = 0x40,   // WH40 / WH5360 / DP80 rain gauge
                  FO_FAMILY_WH51 = 0x51,   // WH51 / DP100 soil moisture
                  FO_FAMILY_WH57 = 0x57,   // WH57 / WH31L / DP60 lightning
                  FO_FAMILY_WS68 = 0x68,   // WS68 anemometer with light / UV
                  FO_FAMILY_WS80 = 0x80,   // WS80 ultrasonic array
                  FO_FAMILY_WS85 = 0x85,   // WS85 ultrasonic wind + piezo rain
                  FO_FAMILY_WS90 = 0x90,   // WS90 ultrasonic 7-in-1 with piezo rain
                  FO_FAMILY_WN35 = 0x35,   // WN35 / DP10 leaf wetness
                  FO_FAMILY_WH41 = 0x41,   // WH41 PM2.5 (PM10 estimate)
                  FO_FAMILY_WH0290 = 0x42, // WH0290 / Ambient Weather PM25 / Misol PM25: same frame as WH41
                  FO_FAMILY_WH43 = 0x43,   // WH43 PM2.5 (PM10 estimate)
                  FO_FAMILY_WH45 = 0x45,   // WH45 / WH0295 / DP250: CO2, PM2.5, PM10, temperature, humidity
                  FO_FAMILY_WH46 = 0x46,   // WH46: as WH45 plus PM1 and PM4
                  FO_FAMILY_WH55 = 0x55,   // WH55 / DP70 water leak
                  FO_FAMILY_WH52 = 0xA2;   // WH52 soil moisture / temperature / EC

// The thermo-hygro(-baro) sensors WH25 / WH32 / WH32B / WN32B / WN32P / WN32
// send an 8-byte frame without a family byte: a 4-bit type (0xE indoor with
// pressure, 0xD outdoor) and an 8-bit sum instead of CRC + sum. Their
// FoFrame::family is one of these codes (not on the air):
constexpr uint8_t FO_FAMILY_WH25 = 0x25,   // type 0xE: WH25 / WH32B / WN32B / WN32P (T, H, pressure)
                  FO_FAMILY_WH32 = 0x32;   // type 0xD: WH32 / WN32 / WN32EP outdoor (T, H)
constexpr size_t FO_WH25_FRAME_LEN = 8;

constexpr uint8_t FO_LIGHTNING_NO_DISTANCE = 0xFF;

struct FoFrame {
    uint8_t     family = 0;          // FO_FAMILY_*
    const char* model = "";          // "WH65", "WS69", "WH24", "WH31", "WS90", ... (static string)
    uint32_t    id = 0;              // sensor ID as the console / rtl_433 shows it (8 to 24 bit)
    uint8_t     channel = 0;         // WH31: 1..8 (DIP switches); 0 = sensor has no channel
    bool        batteryOk = true;
    uint16_t    batteryMv = 0;       // 0 = not reported
    uint16_t    moistureAd = 0xFFFF; // WH51: raw 9-bit AD of the moisture probe, 0xFFFF = none
    bool  hasTemp = false;     float tempC = 0.0f;
    bool  hasHumidity = false; uint8_t humidity = 0;
    bool  hasWind = false;     float windAvgMs = 0.0f, windGustMs = 0.0f; uint16_t windDirDeg = 0;
    bool  hasRain = false;     float rainMm = 0.0f;      // the sensor's running total
    bool  hasUv = false;       float uvIndex = 0.0f;
    bool  hasLight = false;    float lightLux = 0.0f;
    bool  hasPressure = false; float pressureHpa = 0.0f; // WS69 with pressure module
    bool  hasMoisture = false; uint8_t moisture = 0;     // WH51, %
    bool  hasSupercap = false; float supercapV = 0.0f;  // WS85 / WS90: solar-charged super capacitor
    bool  hasLightning = false;                          // WH57
    uint8_t lightningState = 0;      // 0 start-up, 1 interference, 4 noise, 8 strike
    uint8_t lightningCount = 0;      // strikes counted by the sensor (8 bit, wraps)
    uint8_t stormDistanceKm = FO_LIGHTNING_NO_DISTANCE;
    // air quality (WH41 / WH0290 / WH43 / WH45 / WH46), ug/m3 and ppm
    bool  hasPm25 = false;     float pm25 = 0.0f;
    bool  hasPm10 = false;     float pm10 = 0.0f;
    bool  pm10Estimated = false;                         // WH41 / WH0290 / WH43: derived from PM2.5 by the sensor
    bool  hasPm1 = false;      float pm1 = 0.0f;         // WH46
    bool  hasPm4 = false;      float pm4 = 0.0f;         // WH46
    bool  hasCo2 = false;      uint16_t co2Ppm = 0;      // WH45 / WH46
    bool  extPower = false;                              // air quality sensors: battery level 6 = USB powered; WS80 / WS90: 0 V = external supply
    // WH55 water leak
    bool  hasLeak = false;     bool leak = false;        // leak = alarm active (water detected)
    bool  leakHighSensitivity = false;
    uint16_t leakRaw = 0;                                // raw capacitance value (lower = wetter)
    bool  hasLeafWetness = false; uint8_t leafWetnessPct = 0;      // WN35
    bool  hasConductivity = false; float conductivityUsCm = 0.0f;  // WH52, uS/cm
};

uint8_t foCrc8(const uint8_t* data, size_t len);
uint8_t foSum8(const uint8_t* data, size_t len);

// Bytes after the sync word that a frame of this family needs (CRC and sum
// included); 0 = unknown family. WH65: 17 (the WS69 sends 25 with pressure).
// Not for FO_FAMILY_WH25 / FO_FAMILY_WH32 (no family byte; FO_WH25_FRAME_LEN).
size_t foFrameLength(uint8_t family);

// b: the bytes after the 0x2D 0xD4 sync, len: how many of them were received
// (FO_MAX_FRAME_LEN in the receiver's fixed-length mode).
// false = not a supported Fine Offset frame (unknown family, too short, CRC or sum,
// implausible values of a WH25 / WH32 frame).
// wh24Units: the 0x24 family exists in two scalings that cannot be told apart
// on the air without the preamble length - WH65/WS69 (0.51 m/s wind step,
// 0.254 mm (0.01 in) per rain count, the default) and the older WH24 / HP1000
// array (1.12 m/s, 0.3 mm).
bool foDecode(const uint8_t* b, size_t len, FoFrame& out, bool wh24Units = false);
