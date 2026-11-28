#pragma once
// Encode a LaCrosse / TFA IT+ frame - the inverse of itPlusDecode - from the
// node's own values, for the virtual sensors (src/vsensor.h, TX29 / TX35
// style). Same hard limits as FineOffsetEncoder.h: only a temperature /
// humidity reading is encoded, there is no raw "send these bytes" path, and
// the 6-bit ID is supplied by the caller (vsBuildFrame(): derived from the
// node, never a foreign sensor's). Frame layout: ItPlusDecoder.h.
//
// Round-trip tested against itPlusDecode (test/firmware_itplus.cpp).
#include "ItPlusDecoder.h"
#include <stddef.h>
#include <stdint.h>

// Range of the originals: -40.0 .. +59.9 degC (3 BCD digits of degC + 40).
constexpr float ITPLUS_TEMP_MIN = -40.0f, ITPLUS_TEMP_MAX = 59.9f;

// Writes the 5 bytes (CRC-8 included) into out and returns ITPLUS_FRAME_LEN,
// or 0 on invalid input (sensorId > 63, temperature not a number). The
// temperature is rounded to 0.1 degC and clamped to the range above; a present
// humidity is clamped to 1..99 % (never 0x6A / 0x7D); hasHumidity false sends
// the "no humidity sensor" code 0x6A. The weak-battery flag is never set.
size_t itPlusEncode(uint8_t* out, uint8_t sensorId, bool newBattery, float tempC,
                    bool hasHumidity, uint8_t humidity);
