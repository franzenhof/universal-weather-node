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

    bool     hasBme = false;                              // temperature (+ humidity) of the selected source
    bool     hasHumidity = true;                          // with hasBme
    bool     hasPressure = false;                         // BME280 pressure
    float    humidityPct = 0.0f;
    float    pressureQnh = 0.0f, pressureAbs = 0.0f;      // hPa or inHg
    float    bmeTemp = 0.0f;                              // °C or °F

    bool     hasDs18b20[3] = {};
    float    ds18b20[3] = {};                             // °C or °F

    // V4 Full I/O block. Victron values: NAN / -1 = not sent by the device.
    bool     hasIoStatus = false;
    uint8_t  ioStatus = 0;                                // IO_* bits, board_io.h
    bool     hasAuxAin = false;
    float    auxAinV = 0.0f;
    bool     hasAuxPulses = false;
    uint32_t auxPulses = 0, auxPulsesTotal = 0;
    bool     hasWind2 = false, hasWind2Dir = false;      // 2nd anemometer / vane on AUX
    uint16_t wind2DirDeg = 0;
    float    wind2Speed = 0.0f, wind2Gust = 0.0f;        // m/s, km/h or mph
    bool     hasLight = false;
    float    lightLux = 0.0f;                            // wireless sensor
    bool     hasSources = false;
    uint16_t sources = 0;                                // srcBits() (sources.h)
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
// Version 4: [version:1][presence mask: 1-6 bytes][field data...]
// The mask is variable-length: each byte carries 7 field bits (bit 0-6 =
// fields 7k..7k+6), bit 7 = another mask byte follows; trailing empty bytes
// are left out. Only fields whose bit is set are present, in ascending bit
// order, big-endian. A value that is not available takes no space at all.
//
// Older firmware sent version 1-3 (fixed 4-byte little-endian mask; 3 = with
// a 5-byte UV group at bit 31, VE.Direct as one 14-byte block at bit 26). The
// decoder still reads them; this firmware always sends version 4.
// Version 2 vs. 1: imperial rain x1000 / pressure x100, rain since start u32.
constexpr uint8_t BIN_PROTO_VERSION = 4;
constexpr size_t  BIN_MAX_SIZE      = 96;   // all fields incl. debug, V4 Full I/O, UV, light, sources

enum BinField : uint8_t {
    BIN_WIND_DIR = 0, BIN_WIND_SPEED, BIN_WIND_GUST,
    BIN_RAIN_RATE, BIN_RAIN_MM, BIN_RAIN_SINCE,
    BIN_STATUS, BIN_BATTERY,
    BIN_BME_HUMIDITY,   // u8 x2 - humidity of the selected temperature/humidity source
    BIN_BME_PRESS_QNH, BIN_BME_PRESS_ABS,
    BIN_BME_TEMP,       // i16 x10 - temperature of the selected source (BME280 or wireless)
    BIN_DS18B20_0, BIN_DS18B20_1, BIN_DS18B20_2,
    BIN_DBG_CYCLE, BIN_DBG_CPU_TEMP, BIN_DBG_FAIL_COUNT, BIN_DBG_FREE_HEAP, BIN_DBG_UPTIME,
    // V4 Full I/O block
    BIN_IO_STATUS,      // u8, IO_* bits
    BIN_AUX_AIN,        // u16, V x1000
    BIN_AUX_PULSES,     // u16, pulses this cycle (clamped)
    BIN_AUX_TOTAL,      // u32, pulses since power-on
    BIN_OUTPUT_DUTY,    // u8 heater %, u8 fan %
    BIN_FAN_RPM,        // u16
    BIN_VE_BATT_V,      // u16, Victron battery V x100 (v1-3: the whole 14-byte VE.Direct block)
    BIN_VE_LOAD,        // i16, MPPT load output current A x100
    BIN_WIND2_DIR,      // u16, 2nd wind vane (AUX analog), degrees
    BIN_WIND2_SPEED,    // i16 x100, 2nd anemometer (AUX pulse), same unit as wind speed
    BIN_WIND2_GUST,     // i16 x100
    BIN_UV_VOLTS,       // u16, board UV sensor terminal voltage x1000 (v3: 5-byte UV group)
    BIN_UV_INDEX,       // u16, UV index x100 (calibrated board index or wireless sensor)
    BIN_SOURCES,        // u16, srcBits(): 2 bits per group (temp/hum, wind, rain, UV, light:
                        //      0 none, 1 board, 2 wireless), bit 15 wireless battery low
    BIN_LIGHT,          // u32, illuminance, lux
    BIN_VE_BATT_A,      // i16, Victron battery current A x100
    BIN_VE_PV_V,        // u16, PV voltage x100
    BIN_VE_PV_W,        // u16, PV power W
    BIN_VE_CS,          // u8, charge state (CS)
    BIN_VE_ERR,         // u8, error code (ERR)
    BIN_VE_YIELD,       // u16, yield today kWh x100
    BIN_VE_SOC,         // u16, state of charge % x10
    BIN_FIELD_COUNT
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
