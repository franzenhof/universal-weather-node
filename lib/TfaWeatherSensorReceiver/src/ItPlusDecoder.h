#pragma once
// LaCrosse "IT+" (instant transmission) sensors on 868.3 MHz FSK - also sold
// as TFA Dostmann (e.g. 30.3143.IT, 30.3144.IT, 30.3147.IT, 30.3157.IT,
// 30.3159.IT at 17.241 kbit/s; 30.3155.WD, 30.3156.WD at 9.579 kbit/s),
// Technoline/StarMeteo/Conrad TX21/25/27/29/35-IT.
//
// Air frame: preamble 0xAA..., sync 0x2D 0xD4, then 5 bytes:
//   MMMM IIII  IINW TTTT  TTTT TTTT  BHHH HHHH  CCCC CCCC
//   M  model nibble, always 9
//   I  6-bit sensor ID (random, new after a battery change)
//   N  new-battery flag (set for a while after a battery change)
//   W  unused
//   T  temperature, 3 BCD digits: tens, ones, tenths of (degC + 40)
//   B  weak-battery flag
//   H  humidity in % (0x6A = no humidity sensor, 0x7D = this is the second
//      probe of a two-channel sensor, e.g. TX25U)
//   C  CRC-8, polynomial 0x31, init 0x00, over the first 4 bytes
// Independent implementation from the published protocol description
// (rtl_433 "lacrosse_tx35" notes, FHEM LaCrosse documentation).
#include <stddef.h>
#include <stdint.h>

constexpr size_t ITPLUS_FRAME_LEN = 5;
constexpr uint8_t ITPLUS_NO_HUMIDITY = 0x6A;
constexpr uint8_t ITPLUS_PROBE_FLAG  = 0x7D;

struct ItPlusFrame {
    uint8_t sensorId = 0;       // 0..63, +64 for the second probe channel
    bool    newBattery = false;
    bool    weakBattery = false;
    float   tempC = 0.0f;
    bool    hasHumidity = false;
    uint8_t humidity = 0;
};

uint8_t itPlusCrc8(const uint8_t* data, size_t len);

// b: the 5 bytes after the 0x2D 0xD4 sync. false = not an IT+ frame
// (wrong model nibble, CRC, BCD digit or value range).
bool itPlusDecode(const uint8_t* b, ItPlusFrame& out);
