// Uplink payload contract shared by both wire formats: the values one
// cycle puts on air, already converted to the configured output units
// (units.h). Plain C++ (no Arduino), so the binary encoder can be compiled
// on the host and checked against chirpstack/decoder.js
// (test/payload_roundtrip.test.js). lora_payload.cpp fills it from the
// sensor globals and also holds the CayenneLPP encoder.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct Measurements {
    // Status byte: BME280 Bit0, Bus1 Bit1, send-fail count Bit2-5, imperial
    // units Bit6, wind unit Bit7 (0=km/h, 1=m/s; ignored when Bit6 set).
    uint8_t status = 0;
    bool    imperial() const { return (status & 0x40) != 0; }

    bool     hasWind = false;
    uint16_t windDirDeg = 0;
    float    windSpeed = 0.0f, windGust = 0.0f;          // m/s, km/h or mph

    bool     hasRain = false;
    float    rainRate = 0.0f;                             // mm/h or in/h
    float    rainCycle = 0.0f, rainSinceStart = 0.0f;     // mm or in

    bool     hasBattery = false;
    float    batteryV = 0.0f;

    bool     hasBme = false;
    float    humidityPct = 0.0f;
    float    pressureQnh = 0.0f, pressureAbs = 0.0f;      // hPa or inHg
    float    bmeTemp = 0.0f;                              // °C or °F

    bool     hasDs18b20[3] = {};
    float    ds18b20[3] = {};                             // °C or °F

    bool     debug = false;
    uint8_t  dbgCycle = 0, dbgSendFail = 0;
    float    dbgCpuTemp = 0.0f;                           // °C or °F
    float    dbgHeapKib = 0.0f, dbgUptimeH = 0.0f;
};

// ---- Compact binary format (FPort 3) ---------------------------------------
// [version:1][presence bitmask:4, little-endian][field data...] - only fields
// whose bit is set are present, in ascending bit order, big-endian.
//
// Version 2 (current) vs. 1: in imperial mode rain amounts are x1000 (0.001
// in) and pressures x100 (0.01 inHg) instead of x10 - at x10 a single 0.2 mm
// tip (0.008 in) encoded as 0 and pressure had 3.4 hPa resolution. Rain
// since start is uint32 (uint16 x10 wrapped at 6553.5 mm). Metric scaling is
// unchanged. The decoder reads the scale from the status byte's imperial bit.
constexpr uint8_t BIN_PROTO_VERSION = 2;
constexpr size_t  BIN_MAX_SIZE      = 64;   // all fields incl. debug: 43 bytes

enum BinField : uint8_t {
    BIN_WIND_DIR = 0, BIN_WIND_SPEED, BIN_WIND_GUST,
    BIN_RAIN_RATE, BIN_RAIN_MM, BIN_RAIN_SINCE,
    BIN_STATUS, BIN_BATTERY,
    BIN_BME_HUMIDITY, BIN_BME_PRESS_QNH, BIN_BME_PRESS_ABS, BIN_BME_TEMP,
    BIN_DS18B20_0, BIN_DS18B20_1, BIN_DS18B20_2,
    BIN_DBG_CYCLE, BIN_DBG_CPU_TEMP, BIN_DBG_FAIL_COUNT, BIN_DBG_FREE_HEAP, BIN_DBG_UPTIME,
};

// Returns the payload length (at most BIN_MAX_SIZE; out must hold that).
size_t encodeBinary(const Measurements& m, uint8_t* out);
