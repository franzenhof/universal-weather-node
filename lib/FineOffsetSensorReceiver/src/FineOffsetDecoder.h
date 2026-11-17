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
                  FO_FAMILY_WS90 = 0x90;   // WS90 ultrasonic 7-in-1 with piezo rain

constexpr uint8_t FO_LIGHTNING_NO_DISTANCE = 0xFF;

struct FoFrame {
    uint8_t     family = 0;          // FO_FAMILY_*
    const char* model = "";          // "WH65", "WS69", "WH24", "WH31", "WS90", ... (static string)
    uint32_t    id = 0;              // sensor ID as the console / rtl_433 shows it (8 to 24 bit)
    uint8_t     channel = 0;         // WH31: 1..8 (DIP switches); 0 = sensor has no channel
    bool        batteryOk = true;
    uint16_t    batteryMv = 0;       // 0 = not reported
    bool  hasTemp = false;     float tempC = 0.0f;
    bool  hasHumidity = false; uint8_t humidity = 0;
    bool  hasWind = false;     float windAvgMs = 0.0f, windGustMs = 0.0f; uint16_t windDirDeg = 0;
    bool  hasRain = false;     float rainMm = 0.0f;      // the sensor's running total
    bool  hasUv = false;       float uvIndex = 0.0f;
    bool  hasLight = false;    float lightLux = 0.0f;
    bool  hasPressure = false; float pressureHpa = 0.0f; // WS69 with pressure module
    bool  hasMoisture = false; uint8_t moisture = 0;     // WH51, %
    bool  hasLightning = false;                          // WH57
    uint8_t lightningState = 0;      // 0 start-up, 1 interference, 4 noise, 8 strike
    uint8_t lightningCount = 0;      // strikes counted by the sensor (8 bit, wraps)
    uint8_t stormDistanceKm = FO_LIGHTNING_NO_DISTANCE;
};

uint8_t foCrc8(const uint8_t* data, size_t len);
uint8_t foSum8(const uint8_t* data, size_t len);

// Bytes after the sync word that a frame of this family needs (CRC and sum
// included); 0 = unknown family. WH65: 17 (the WS69 sends 25 with pressure).
size_t foFrameLength(uint8_t family);

// b: the bytes after the 0x2D 0xD4 sync, len: how many of them were received
// (FO_MAX_FRAME_LEN in the receiver's fixed-length mode).
// false = not a supported Fine Offset frame (unknown family, too short, CRC or sum).
// wh24Units: the 0x24 family exists in two scalings that cannot be told apart
// on the air without the preamble length - WH65/WS69 (0.51 m/s wind step,
// 0.254 mm (0.01 in) per rain count, the default) and the older WH24 / HP1000
// array (1.12 m/s, 0.3 mm).
bool foDecode(const uint8_t* b, size_t len, FoFrame& out, bool wh24Units = false);
