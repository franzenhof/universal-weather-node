// Uplink payload contract shared by both wire formats: the values one
// cycle puts on air, already converted to the configured output units
// (units.h). Plain C++ (no Arduino), so the binary encoder can be compiled
// on the host and checked against chirpstack/decoder.js
// (test/payload_roundtrip.test.js). lora_payload.cpp fills it from the
// sensor globals and also holds the CayenneLPP encoder.
#pragma once
#include <math.h>
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

    // PCB v2.0 I/O block. Victron values: NAN / -1 = not sent by the device.
    bool     hasIoStatus = false;
    uint8_t  ioStatus = 0;                                // IO_* bits, board_io.h
    bool     hasAuxAin = false;
    float    auxAinV = 0.0f;
    bool     hasAuxPulses = false;
    uint32_t auxPulses = 0, auxPulsesTotal = 0;
    bool     hasWind2 = false, hasWind2Dir = false;      // 2nd anemometer / vane on AUX
    uint16_t wind2DirDeg = 0;
    float    wind2Speed = 0.0f, wind2Gust = 0.0f;        // m/s, km/h or mph
    bool     hasUv = false;
    float    uvSensorVolts = 0.0f;                       // terminal voltage, V
    float    uvIndex = NAN;                              // absent unless explicitly calibrated/enabled
    bool     hasOutputs = false;
    uint8_t  heaterDuty = 0, fanDuty = 0;                 // %
    bool     hasFanRpm = false;
    float    fanRpm = 0.0f;
    bool     hasVe = false;
    float    veBattV = 0.0f, veBattA = 0.0f, vePvV = 0.0f, vePvW = 0.0f;
    float    veYieldKwh = 0.0f, veSocPct = 0.0f;
    float    veLoadA = NAN;                               // MPPT load output current (NAN = not sent)
    int      veChargeState = -1, veError = -1;

    bool     debug = false;
    uint8_t  dbgCycle = 0, dbgSendFail = 0;
    float    dbgCpuTemp = 0.0f;                           // °C or °F
    float    dbgHeapKib = 0.0f, dbgUptimeH = 0.0f;
};

// ---- Compact binary format (FPort 3) ---------------------------------------
// [version:1][presence bitmask:4, little-endian][field data...] - only fields
// whose bit is set are present, in ascending bit order, big-endian.
//
// Version 2 vs. 1: in imperial mode rain amounts are x1000 (0.001
// in) and pressures x100 (0.01 inHg) instead of x10 - at x10 a single 0.2 mm
// tip (0.008 in) encoded as 0 and pressure had 3.4 hPa resolution. Rain
// since start is uint32 (uint16 x10 wrapped at 6553.5 mm). Metric scaling is
// unchanged. Version 3 is selected only when UV is present and appends the UV
// group at bit 31; all prior fields and v1/v2 packets remain byte-identical.
constexpr uint8_t BIN_PROTO_VERSION = 2;
constexpr uint8_t BIN_PROTO_VERSION_UV = 3;
constexpr size_t  BIN_MAX_SIZE      = 85;   // all fields incl. debug, v2.0 I/O and UV

enum BinField : uint8_t {
    BIN_WIND_DIR = 0, BIN_WIND_SPEED, BIN_WIND_GUST,
    BIN_RAIN_RATE, BIN_RAIN_MM, BIN_RAIN_SINCE,
    BIN_STATUS, BIN_BATTERY,
    BIN_BME_HUMIDITY, BIN_BME_PRESS_QNH, BIN_BME_PRESS_ABS, BIN_BME_TEMP,
    BIN_DS18B20_0, BIN_DS18B20_1, BIN_DS18B20_2,
    BIN_DBG_CYCLE, BIN_DBG_CPU_TEMP, BIN_DBG_FAIL_COUNT, BIN_DBG_FREE_HEAP, BIN_DBG_UPTIME,
    // PCB v2.0 I/O block (added without a version change: older decoders
    // stop before these trailing fields)
    BIN_IO_STATUS,      // u8, IO_* bits
    BIN_AUX_AIN,        // u16, V x1000
    BIN_AUX_PULSES,     // u16, pulses this cycle (clamped)
    BIN_AUX_TOTAL,      // u32, pulses since power-on
    BIN_OUTPUT_DUTY,    // u8 heater %, u8 fan %
    BIN_FAN_RPM,        // u16
    BIN_VE_DIRECT,      // 14 bytes: battery V x100 (u16), battery A x100 (i16), PV V x100 (u16),
                        // PV W (u16), charge state (u8), error (u8), yield today kWh x100 (u16),
                        // SOC % x10 (u16); not sent: 0xFFFF / 0x8000 / 0xFF
    BIN_VE_LOAD,        // i16, MPPT load output current A x100 (only if the charger reports it)
    BIN_WIND2_DIR,      // u16, 2nd wind vane (AUX analog), degrees
    BIN_WIND2_SPEED,    // i16 x100, 2nd anemometer (AUX pulse), same unit as wind speed
    BIN_WIND2_GUST,     // i16 x100
    BIN_UV,             // 5 bytes: sensor mV (u16), index x100 (u16; FFFF absent), flags (bit0 volts, bit1 calibrated index)
};

// Returns the payload length (at most BIN_MAX_SIZE; out must hold that).
size_t encodeBinary(const Measurements& m, uint8_t* out);

// ---- Fitting a payload into the current LoRaWAN data rate --------------------
// At slow data rates (EU868 SF12-SF10: 51 bytes, US915 DR0: 11 bytes) not every
// field fits. dropNextGroup() removes the least important remaining field group
// (debug, AUX pulses, AUX analog, UV, wind 2, fan speed, output power, I/O status, Victron,
// LiPo, DS18B20 #2/#1/#0, BME280, rain, wind - the status byte always stays) and
// returns its name, or nullptr when nothing is left to drop. Used by both
// encoders: encode, and while too long, drop the next group and encode again.
const char* dropNextGroup(Measurements& m);

// Binary encoding that fits into `limit` bytes; `dropped` (optional) receives a
// comma-separated list of the groups left out ("" if everything fit).
size_t encodeBinaryFitted(Measurements m, uint8_t* out, size_t limit, char* dropped = nullptr, size_t droppedSize = 0);
