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
    // PCB v2.0 I/O block (rejected with "invalid" on Rev 1.2/1.3 boards,
    // except switching a function off)
    PARAM_AUX_AIN_EN            = 15,
    PARAM_AUX_AIN_SCALE_X1000   = 16,
    PARAM_AUX_PULSE_EN          = 17,
    PARAM_HEATER_MODE           = 18,
    PARAM_HEATER_DUTY_PCT       = 19,
    PARAM_HEATER_ON_BELOW_DC    = 20,
    PARAM_FAN_MODE              = 21,
    PARAM_FAN_DUTY_PCT          = 22,
    PARAM_FAN_ON_ABOVE_DC       = 23,
    PARAM_FAN_MIN_PV_W          = 24,
    PARAM_TACHO_EN              = 25,
    PARAM_TACHO_PPR             = 26,
    PARAM_VEDIRECT_EN           = 27,
    PARAM_OUT_MIN_BATT_DV       = 28,
    PARAM_HEATER_MIN_DUTY_PCT   = 29,
    PARAM_HEATER_FULL_BELOW_DC  = 30,
    PARAM_LOW_POWER_EN          = 31,
    PARAM_VANE_POWER_SAVE       = 32,
    PARAM_WIND2_MODE            = 33,   // PCB v2.0: 2nd anemometer on AUX
    PARAM_WIND2_DIR_OFFSET_DEG  = 34,
    PARAM_UV_EN                 = 35,   // PCB v2.0: raw UV sensor voltage, GPIO3
    PARAM_UV_AIN_SCALE_X1000    = 36,   // terminal V per ADC V, x1000
    PARAM_UV_INDEX_EN           = 37,   // calculated UV index publication
    PARAM_UV_CALIBRATED         = 38,   // explicit calibration-valid flag
    PARAM_UV_INDEX_SLOPE_X1000  = 39,   // UV index / V, x1000
    PARAM_UV_INDEX_OFFSET_X1000 = 40,   // UV index offset, x1000
    PARAM_MQTT_INTERVAL_SEC     = 41,   // 0 = same as the LoRaWAN interval, else 2..3600 s
    PARAM_LORA_WIND_AGG         = 42,   // 0 = interval, 1 = 10-min (WMO)
    PARAM_MQTT_WIND_AGG         = 43,   // 0 = interval, 1 = 10-min (WMO)
    PARAM_RF_EN                 = 44,   // 0 = off, 1 = 868 MHz wireless sensors (Bresser, IT+, Fine Offset) on the shared SX1262
    // value sources (sources.h): 0 auto, 1 board, 2-5 wireless sensor 1-4, 6 off
    PARAM_SRC_TEMP_HUM          = 45,
    PARAM_SRC_WIND              = 46,
    PARAM_SRC_RAIN              = 47,
    PARAM_SRC_UV                = 48,
    PARAM_SRC_LIGHT             = 49,
    PARAM_RF_PROTO              = 50,   // wireless protocols, bit mask 1..15: 1 Bresser, 2 IT+ 17.241k, 4 IT+ 9.579k, 8 Fine Offset;
                                        // 16 = Auto (search, stores the detected type 1 / 10 / 4); 17 = Davis (exclusive, LoRaWAN paused)
    PARAM_RAIN_PERSIST          = 51,   // 1 = save rain statistics (daily + week/month/year) to flash (hourly + before restarts), 0 = RTC RAM only
    PARAM_LOX_EN                = 52,   // 1 = Loxone UDP output on (host/port/prefix: web settings / portal)
    PARAM_LOX_INTERVAL_SEC      = 53,   // Loxone interval: 0 = every base cycle, else 2..3600 s
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
void cmdResetAuxPulses();
void cmdRestart();
void cmdSetDebugMode(bool enabled);
void cmdRequestConfigPortal();

// Wipes the entire "config" NVS namespace (cfgdata partition) and restarts
// into the first-boot config portal. Does not return.
void cmdFactoryReset();
