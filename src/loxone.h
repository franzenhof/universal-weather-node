#pragma once
// Loxone UDP output: after each Loxone send interval every current value goes
// to a Loxone Miniserver as its own UDP datagram "name=value" (ASCII, '.' as
// decimal separator), e.g. "temp=6.6", "windGust=26.5", "warnFrost=0". In
// Loxone Config: a Virtual UDP Input (sender = this node, the port below) with
// one Virtual UDP Input Command per value, command recognition "temp=\v".
//
// The names are the keys of /api/data (json_payload.cpp): the lines are built
// by walking the same JsonDocument buildJsonPayload() produces, so they always
// match the JSON / REST / MQTT names. Top-level numbers and booleans (0/1) are
// sent, plus flattened nested values (ds18b20_1.., warnFrost/warnFog/warnSultry,
// wind2Speed, heaterOn, fanRpm, victronPvW, ..., rf1_temp, rf1_battery, ...).
// Strings (units, text trends, ISO times) are not sent. A value that is not
// valid right now is not in the JSON and therefore not sent: Loxone keeps the
// last value it received.
#include <stddef.h>
#include <ArduinoJson.h>

constexpr int LOX_PREFIX_MAX   = 16;     // characters, [A-Za-z0-9_.-]
constexpr int LOX_PORT_DEFAULT = 7000;

// ---- pure helpers (loxone_lines.cpp, host-tested) ----

// Empty or only [A-Za-z0-9_.-], at most LOX_PREFIX_MAX characters.
bool loxPrefixValid(const char* s);
// In place: drops invalid characters, cuts to LOX_PREFIX_MAX.
void loxSanitizePrefix(char* s);
inline bool loxPortValid(long p)     { return p >= 1 && p <= 65535; }
inline bool loxIntervalValid(long s) { return s == 0 || (s >= 2 && s <= 3600); }

// Called once per value with one complete datagram "prefix+name=value".
typedef void (*LoxLineFn)(const char* line, size_t len, void* ctx);

// Walks doc (as built by buildJsonPayload()) and calls fn for every value
// that is sent. Returns the number of lines.
int loxoneLines(const JsonDocument& doc, const char* prefix, LoxLineFn fn, void* ctx);

// ---- network part (loxone.cpp, firmware only) ----
#ifdef ARDUINO
#include <IPAddress.h>
struct PathValues;

struct LoxoneStatus {
    bool          everSent     = false;
    unsigned long lastSendMs   = 0;
    int           lastValues   = 0;      // datagrams sent in the last set
    int           lastFailed   = 0;      // datagrams of the last set that failed
    uint32_t      sets         = 0;      // sets sent since start
    uint32_t      errors       = 0;      // failed datagrams / skipped sets since start
    const char*   lastError    = nullptr; // static text, nullptr = none
    unsigned long lastErrorMs  = 0;
    bool          ipValid      = false;
    IPAddress     ip;
};

void loxoneSetup();                         // setup(), before webApiSetup()
void loxonePoll();                          // every loop(): non-blocking DNS
void loxoneSendTelemetry(const PathValues* pv);   // one set (main loop, PATH_LOX)
const LoxoneStatus& loxoneStatus();
// loxHost / loxPrefix are written by the web task: hold this around writes.
void loxoneLock();
void loxoneUnlock();
#endif
