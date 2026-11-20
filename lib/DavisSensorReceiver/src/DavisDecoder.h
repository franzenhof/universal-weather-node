#pragma once
// Decoder for the wireless packets of Davis Instruments Vantage Pro2 / Vantage
// Vue ISS and the anemometer transmitter kit (6332) - pure C++ without
// RadioLib, host-testable. Protocol notes and sources: README.md.
//
// On air (19.2 kbit/s GFSK, every byte LSB first) a packet is
//   carrier, preamble AA AA AA AA, sync CB 89, 10 bytes:
//   [0] type << 4 | battery-low << 3 | (transmitter ID - 1)
//   [1] wind speed (mph)   [2] wind direction (raw 0-255)
//   [3..5] value of the message type   [6..7] CRC-16/CCITT of [0..5]
//   [8..9] FF FF (repeater info; a repeater's CRC also covers them)
// An MSB-first receiver (SX1262, RFM69) delivers every byte bit-reversed:
// davisDecode() takes the bytes as received ("air order") and reverses them.
#include <stddef.h>
#include <stdint.h>

constexpr size_t DAVIS_PACKET_LEN = 10;

// Message types (upper nibble of byte 0)
constexpr uint8_t DAVIS_MSG_SUPERCAP  = 0x2;   // Vue (and newer ISS): super-capacitor voltage
constexpr uint8_t DAVIS_MSG_UNKNOWN3  = 0x3;   // unidentified, follows the sun (see README)
constexpr uint8_t DAVIS_MSG_UV        = 0x4;
constexpr uint8_t DAVIS_MSG_RAINRATE  = 0x5;   // time between bucket tips
constexpr uint8_t DAVIS_MSG_SOLAR     = 0x6;
constexpr uint8_t DAVIS_MSG_SOLARCELL = 0x7;   // Vue: solar panel voltage
constexpr uint8_t DAVIS_MSG_TEMP      = 0x8;
constexpr uint8_t DAVIS_MSG_GUST      = 0x9;   // highest wind speed of the last 10 min
constexpr uint8_t DAVIS_MSG_HUMIDITY  = 0xA;
constexpr uint8_t DAVIS_MSG_UNKNOWNC  = 0xC;   // unidentified (anemometer transmitter kit, T/H station)
constexpr uint8_t DAVIS_MSG_RAIN      = 0xE;   // bucket tip counter (7 bit)
constexpr uint8_t DAVIS_MSG_SOILLEAF  = 0xF;   // soil / leaf station (not decoded)

// Wind vane formula: the Vantage Pro2 vane is a potentiometer with a dead band
// around north, the Vue uses a Hall sensor.
enum DavisModel : uint8_t { DAVIS_VP2 = 0, DAVIS_VUE = 1 };

enum DavisCrc : uint8_t {
    DAVIS_CRC_OK = 0,        // CRC of bytes 0-5 matches bytes 6-7 (direct packet)
    DAVIS_CRC_REPEATER,      // matches only over bytes 0-5 + 8-9 (packet relayed by a repeater)
    DAVIS_CRC_FAIL,
};

struct DavisPacket {
    uint8_t  bytes[DAVIS_PACKET_LEN] = {};   // bit-reversed ("firmware order", as the console shows them)
    DavisCrc crc = DAVIS_CRC_FAIL;
    // Header and wind (every packet) - valid when crc != DAVIS_CRC_FAIL
    uint8_t  type = 0;              // message type, upper nibble of byte 0
    uint8_t  txId = 0;              // transmitter ID 1-8 (the DIP switch setting)
    bool     batteryLow = false;
    uint8_t  windMph = 0;           // byte 1: wind speed, mph (an integer, sampled per packet)
    uint8_t  windDirRaw = 0;        // byte 2
    bool     hasWindDir = false;    // false: speed and direction both 0 (no vane, e.g. a T/H station)
    float    windDirDeg = 0.0f;     // 0..360
    uint16_t raw10 = 0;             // (byte3 << 2) | (byte4 >> 6): the 10-bit value of most types
    // Value of the message type (only the one of `type`, if the sensor is present)
    bool  hasTemp = false;          float tempF = 0.0f, tempC = 0.0f;
    bool  tempAnalog = false;       // old analog (thermistor) temperature: not decoded, hasTemp false
    bool  hasHumidity = false;      float humidity = 0.0f;   // % (analog sensor: linear approximation)
    bool  hasRainCount = false;     uint8_t rainCount = 0;   // 0..127, wraps
    bool  hasRainRate = false;      float rainTipSec = 0.0f; // seconds between tips; 0 = no rain
    bool  hasUv = false;            float uvIndex = 0.0f;
    bool  hasSolar = false;         float solarWm2 = 0.0f;
    bool  hasGust = false;          uint8_t gustMph = 0, gustIndex = 0;
    bool  hasSupercap = false;      float supercapV = 0.0f;  // raw10 / 300 (scaling unverified)
    bool  hasSolarCell = false;     float solarCellV = 0.0f; // raw10 / 300 (scaling unverified)
};

uint8_t  davisReverseBits(uint8_t b);
// CRC-16/CCITT as Davis uses it: polynomial 0x1021, initial value `crc`
// (0 for a packet), no reflection, no final XOR.
uint16_t davisCrc16(const uint8_t* data, size_t len, uint16_t crc = 0);

// Decodes a packet as received by an MSB-first receiver (bytes bit-reversed).
// Always fills p.bytes and p.crc; the other fields only if the CRC is OK
// (direct or repeater). Returns p.crc != DAVIS_CRC_FAIL.
bool davisDecode(const uint8_t air[DAVIS_PACKET_LEN], DavisPacket& p, DavisModel model = DAVIS_VP2);
// Same for bytes already in firmware order (e.g. from the console or a log).
bool davisDecodeOrdered(const uint8_t bytes[DAVIS_PACKET_LEN], DavisPacket& p, DavisModel model = DAVIS_VP2);

// Wind direction in degrees for a raw byte 2 (see DavisModel).
float davisWindDirDeg(uint8_t raw, DavisModel model);

// Bits of bytes 8-9 that are 0 (a direct packet has FF FF; a few bit errors
// there are tolerated, more means a foreign or repeated packet).
uint8_t davisTrailerZeroBits(const uint8_t bytes[DAVIS_PACKET_LEN]);

// Packet interval of transmitter ID 1-8: (40 + id) / 16 s, i.e. 2.5625 s
// for ID 1 ... 3.0 s for ID 8. 0 for an invalid ID.
uint32_t davisIntervalUs(uint8_t txId);

// Rain rate for a time between tips (DavisPacket::rainTipSec; 0 = no rain).
float davisRainRateMmH(float tipSec, float mmPerTip);

// Short name of a message type ("temp", "rain", ...; "type N" for others).
const char* davisTypeName(uint8_t type);

// Bucket-tip counter (7 bit, wraps at 128) -> rain since the first packet.
// The first count only sets the reference. A jump of more than maxStep tips
// between two counts is taken as a counter reset (ISS restarted, battery
// changed), not as rain: 64 tips = 12.8 mm in the ~10 s between two rain
// packets - and beyond half the range the wrap is ambiguous anyway.
class DavisRainCounter {
public:
    explicit DavisRainCounter(uint8_t maxStep = 64) : maxStep_(maxStep) {}
    // Returns the number of new tips (0 for the first count or a reset).
    uint8_t update(uint8_t count);
    bool    valid() const { return valid_; }
    uint32_t totalTips() const { return total_; }
    void    reset() { valid_ = false; total_ = 0; }
private:
    uint8_t  maxStep_;
    bool     valid_ = false;
    uint8_t  last_ = 0;
    uint32_t total_ = 0;
};
