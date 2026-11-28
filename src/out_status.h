#pragma once
// Upload / send status per output: last success, last error (time, code,
// short text) and consecutive failures - for the status page (Connections,
// Weather networks) and /api/info outputs.status. Fixed size, no secrets:
// the texts are local reasons or trimmed server replies, never URLs.
// Pure part host-tested (test/firmware_outstatus.cpp).
#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>

enum OutId : uint8_t {
    OUT_LORAWAN = 0, OUT_MQTT, OUT_HA, OUT_LOXONE,
    OUT_WX_FIRST,                          // the weather networks in wx_upload.cpp order (8)
    OUT_NOTIFY_FIRST = OUT_WX_FIRST + 8,   // alarm channels ntfy, Pushover, Telegram, webhook (alarms.h AC_* without MQTT)
    OUT_CUSTOM_FIRST = OUT_NOTIFY_FIRST + 4,   // custom uploads 1-2
    OUT_INFLUX = OUT_CUSTOM_FIRST + 2,     // InfluxDB / VictoriaMetrics push
    OUT_COUNT
};

struct OutStatus {
    bool     hasOk = false, hasErr = false;
    uint32_t okMs = 0, errMs = 0;    // millis() of the last success / error
    int32_t  code = 0;               // last error: HTTP status, client / radio / MQTT code (0 = none)
    uint16_t fails = 0;              // consecutive failures (0 after a success)
    char     err[40] = "";           // last error text (trimmed)
};

void outStatusOk(OutStatus& s, uint32_t nowMs);
void outStatusFail(OutStatus& s, uint32_t nowMs, int32_t code, const char* text);
// {"lastOkSecAgo", "lastErrorSecAgo", "lastError", "lastErrorCode", "failsInRow"}
void outStatusJson(JsonObject o, const OutStatus& s, uint32_t nowMs);
const char* outStatusKey(uint8_t id);   // "lorawan", "mqtt", "haDiscovery", "loxone", "awekas", ..., "ntfy", ..., "custom1", "custom2", "influxdb"

// ---- device (thread-safe; any task) ----
void outReport(uint8_t id, bool ok, int32_t code = 0, const char* text = nullptr);
void outSnapshot(OutStatus out[OUT_COUNT]);
// Web server task only (it runs one handler at a time): a fresh snapshot in
// one shared buffer instead of a ~1.1-KB static copy per page / API call.
// Valid until the next call (a later call only makes it newer).
const OutStatus* outSnapshotForWebTask();
