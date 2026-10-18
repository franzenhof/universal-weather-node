#pragma once

#include <stdint.h>

// Parameter IDs - the wire contract of LoRaWAN downlink 0x10, the MQTT
// "set" command and the web settings page (README "Parameter IDs").
enum ParamId : uint8_t {
    PARAM_UPLINK_INTERVAL_SEC   = 1,
    PARAM_SAMPLE_SEC            = 2,
    PARAM_WIND_EN               = 3,
    PARAM_RAIN_EN               = 4,
    PARAM_LIPO_EN               = 5,
    PARAM_DEBUG_MODE            = 6,
    PARAM_WIND_DIR_OFFSET_DEG   = 7,
    PARAM_WIND_UNIT_MS          = 8,
    PARAM_CPU_FREQ_MHZ          = 9,
    PARAM_PAYLOAD_FORMAT        = 10,
    PARAM_UNIT_SYSTEM           = 11,
    PARAM_RAIN_MM_PER_TIP_X1000 = 12,
    PARAM_WIND_MS_PER_PPS_X1000 = 13,
    PARAM_WIND_DIR_SENSOR_TYPE  = 14,
};

// Applies a single (paramId, value) pair to the live Config + hardware
// state. Shared by the LoRaWAN downlink handler (cmd 0x10), the MQTT command
// topic and the web settings page. Returns false if paramId/value is
// invalid; the caller decides whether to persist via saveConfig().
bool applyParam(uint8_t paramId, int16_t value);

// Saturates a wider input (e.g. a web form value) to the int16 range, so an
// out-of-range number is rejected by applyParam() instead of wrapping into
// a valid-looking one (65836 -> 300).
inline int16_t clampToParamValue(long v)
{
    return (int16_t)(v < -32768L ? -32768L : (v > 32767L ? 32767L : v));
}

// Named, transport-agnostic command actions. Both the LoRaWAN downlink
// byte commands and the MQTT JSON commands dispatch into these.
void cmdResetRainAccumulator();
void cmdRestart();
void cmdSetDebugMode(bool enabled);
void cmdRequestConfigPortal();

// Wipes the entire "config" NVS namespace (cfgdata partition) and restarts
// into the first-boot config portal. Does not return.
void cmdFactoryReset();
