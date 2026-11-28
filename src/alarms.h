#pragma once
// Alarm rules and notifications (docs/alarms.md).
//
// ALARM_RULES fixed slots. A rule watches one value - a main value, a
// derived value, one of the built-in warnings or a value of a wireless
// sensor (by ID) - and raises when it is above / below the raise threshold
// for the minimum duration; it clears at the clear threshold (hysteresis).
// While raised, the notification repeats after the repeat interval (0 =
// once). Values that are not available (stale sensor, no source) never raise
// an alarm and never clear one.
//
// Pure part (rules, evaluation, templates, storage format, JSON) host-tested
// in test/firmware_alarms.cpp; alarms_run.cpp does the device side.
#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>
#include "placeholders.h"
#include "config.h"

constexpr int ALARM_RULES = 12;

enum AlarmSrc : uint8_t {
    AS_OFF = 0, AS_TEMP, AS_FEELS, AS_HUM, AS_DEW, AS_QNH, AS_PTEND, AS_WIND, AS_GUST,
    AS_RAINRATE, AS_RAINDAY, AS_RAIN1H, AS_UV, AS_FROST, AS_FOG, AS_SULTRY, AS_RF, AS_COUNT
};
// value of a wireless sensor (AS_RF)
enum AlarmRfField : uint8_t {
    AF_TEMP = 0, AF_HUM, AF_WIND, AF_GUST, AF_UV, AF_LUX, AF_LEAK, AF_LIGHTNING_KM, AF_MOISTURE,
    AF_CO2, AF_PM25, AF_SILENT_MIN, AF_BATTERY_LOW, AF_COUNT
};
// notification channels (bit numbers of AlarmRule::channels)
enum AlarmChannel : uint8_t { AC_NTFY = 0, AC_PUSHOVER, AC_TELEGRAM, AC_MQTT, AC_WEBHOOK, AC_COUNT };

struct AlarmRule {
    uint8_t  src = AS_OFF;          // AS_OFF = free slot
    uint8_t  enabled = 0;
    uint8_t  rfField = AF_TEMP;
    uint8_t  above = 1;             // 1: raise when value > raise, 0: when value < raise
    uint8_t  priority = 3;          // 1 (lowest) .. 5 (urgent)
    uint8_t  channels = 0;          // bit AC_*
    uint8_t  allClear = 0;          // 1 = notify when it clears
    uint8_t  reserved = 0;
    uint32_t rfId = 0;
    float    raise = 0.0f, clear = 0.0f;   // metric (°C, %, hPa, m/s, mm, mm/h, ...)
    uint16_t minDurationMin = 0;    // condition must hold this long before raising
    uint16_t repeatMin = 0;         // re-notify while raised (0 = once)
    char     name[24] = "";
    char     msg[72] = "";          // raise message template ("" = default text)
    char     clearMsg[48] = "";     // all-clear message template ("" = default text)
};

struct AlarmState {
    bool     active = false;
    uint32_t pendingSince = 0;      // condition true since (s), 0 = not pending
    uint32_t since = 0;             // raised at (s)
    uint32_t lastNotify = 0;        // last raise / repeat notification (s)
    bool     valid = false;         // value available at the last step
    float    value = 0.0f;          // last value (metric)
};

enum AlarmEvent : uint8_t { AE_NONE = 0, AE_RAISE, AE_REPEAT, AE_CLEAR };

// The alarm clock: seconds since boot from the 64-bit microsecond timer
// (esp_timer_get_time()). Never millis() / 1000: that wraps at 4294967 s
// (49.7 days), not at 2^32, so the unsigned deltas below would break there.
inline uint32_t alarmClockSecFrom(int64_t uptimeUs) { return (uint32_t)(uptimeUs / 1000000LL); }
uint32_t alarmClockSec();   // device: alarmClockSecFrom(esp_timer_get_time())

// One evaluation at time now (s, monotonic: alarmClockSec()). valid = value available.
AlarmEvent alarmStep(const AlarmRule& r, AlarmState& s, bool valid, float v, uint32_t now);

// Unit kind of a rule's value (placeholders.h): temperature, speed, ...
PhKind alarmKind(const AlarmRule& r);
// Key of the value for JSON / the settings ("temp", "rf.leak", ...)
const char* alarmSrcKey(uint8_t src);
const char* alarmRfFieldKey(uint8_t f);
const char* alarmChannelKey(uint8_t c);   // "ntfy", "pushover", "telegram", "mqtt", "webhook"

// Which measurements (AlarmRfField) a wireless sensor provides: bit f set =
// available. "Silent for" and "battery low" are always available.
uint16_t alarmCapMask(bool temp, bool hum, bool wind, bool uv, bool light, bool leak, bool lightning, bool moisture,
                      bool co2, bool pm25);
inline bool alarmFieldAvailable(uint8_t f, uint16_t mask) { return f < AF_COUNT && (mask >> f) & 1; }

// Ready-made rules: 0 frost, 1 fog / dew, 2 sultry, 3 storm (pressure
// falling >= 6 hPa / 3 h), 4 rain starts. false = no such template.
constexpr int ALARM_TEMPLATES = 5;
bool alarmTemplate(int tpl, AlarmRule& r);

// Display units (the unit setting) <-> metric, for the settings form / JSON
float alarmToDisplay(PhKind k, float metric, const Config& c);
float alarmFromDisplay(PhKind k, float display, const Config& c);
const char* alarmUnitLabel(PhKind k, uint8_t src, uint8_t rfField, const Config& c);

// NVS blob of all rules (version byte + rules + check)
constexpr size_t ALARM_BLOB_SIZE = 4 + sizeof(AlarmRule) * ALARM_RULES + 4;
void alarmsEncode(const AlarmRule* rules, uint8_t* out);
bool alarmsDecode(const uint8_t* in, size_t len, AlarmRule* rules);

// ---- notification channels (settings; own NVS blob, alarms_run.cpp) ----
struct AlarmChannels {
    char    ntfyUrl[64] = "https://ntfy.sh";
    char    ntfyTopic[64] = "";
    char    ntfyToken[64] = "";       // secret
    char    ntfyTags[32] = "warning";
    char    poToken[36] = "";         // secret (app token)
    char    poUser[36] = "";          // secret (user key)
    uint16_t poRetrySec = 60, poExpireSec = 3600;   // emergency priority (5)
    char    tgToken[64] = "";         // secret (bot token)
    char    tgChat[24] = "";
    uint8_t mqttEn = 0;               // events to <prefix>/alarm (the retained states are always published)
    uint8_t whPost = 0;
    char    whUrl[192] = "";          // secret (may carry a key)
    char    whBody[256] = "";
};

// GET /api/alarms (docs/api/schemas/alarms.schema.json): no secrets
void alarmsJson(JsonDocument& doc, const AlarmRule* rules, const AlarmState* states, const AlarmChannels& ch,
                const Config& c, uint32_t nowSec);

// ---- device part (alarms_run.cpp) ----
struct NotifyReq;
void alarmsSetup();                       // load rules / channels from NVS
void alarmsLock();                        // around reading / editing alarmsRules() / alarmsChannels()
void alarmsUnlock();
AlarmRule* alarmsRules();                 // ALARM_RULES entries
AlarmChannels& alarmsChannels();
void alarmsSave();                        // after an edit (under the lock)
void alarmsResetState(int rule);          // an edited rule starts over
void alarmsSnapshot(AlarmRule* rules, AlarmState* states);   // consistent copies
uint16_t alarmsEnabledMask();             // parameter 66: bit n = rule n+1 enabled
bool alarmsSetEnabledMask(uint16_t mask);
void alarmsPoll();                        // main loop: evaluates every 30 s
bool alarmsTest(uint8_t channel);         // queues a test message for one channel (AC_*)
int  alarmsActiveCount();
// active rules for /api/data "alarms": [{n, name, priority}]
void alarmsActiveJson(JsonArray a);
// upload task (wx_upload.cpp): sends the queued notifications one request at
// a time. http(req, reply, len) returns the HTTP status (< 0: client error).
typedef int (*AlarmHttpFn)(const NotifyReq& req, char* reply, size_t len);
void alarmsNotifyService(AlarmHttpFn http);
// MQTT task (wifi_mqtt.cpp): retained <prefix>/alarm/<n> ON / OFF per rule,
// events to <prefix>/alarm. all = publish every state (after a reconnect).
typedef bool (*AlarmPublishFn)(const char* topic, const char* payload, bool retain);
void alarmsMqttService(AlarmPublishFn pub, const char* prefix, bool all);
// true once after the rules were saved (Home Assistant discovery again)
bool alarmsConfigChanged();
// configured: ntfy, Pushover, Telegram, webhook (4 entries)
void alarmChannelsConfigured(bool* ch);
