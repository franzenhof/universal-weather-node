#include "out_status.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

void outStatusOk(OutStatus& s, uint32_t nowMs)
{
    s.hasOk = true;
    s.okMs  = nowMs;
    s.fails = 0;
}

void outStatusFail(OutStatus& s, uint32_t nowMs, int32_t code, const char* text)
{
    s.hasErr = true;
    s.errMs  = nowMs;
    s.code   = code;
    if (s.fails < UINT16_MAX) s.fails++;
    // copy, collapsing control characters / line breaks of a server reply
    size_t n = 0;
    for (const char* p = text ? text : ""; *p && n + 1 < sizeof(s.err); p++)
        s.err[n++] = ((unsigned char)*p < 0x20) ? ' ' : *p;
    while (n && s.err[n - 1] == ' ') n--;
    s.err[n] = '\0';
}

void outStatusJson(JsonObject o, const OutStatus& s, uint32_t nowMs)
{
    if (s.hasOk) o["lastOkSecAgo"] = (nowMs - s.okMs) / 1000;
    if (s.hasErr) {
        o["lastErrorSecAgo"] = (nowMs - s.errMs) / 1000;
        o["lastError"]       = s.err;
        if (s.code) o["lastErrorCode"] = s.code;
    }
    o["failsInRow"] = s.fails;
}

const char* outStatusKey(uint8_t id)
{
    static const char* const keys[OUT_COUNT] = { "lorawan", "mqtt", "haDiscovery", "loxone",
        "awekas", "wunderground", "weathercloud", "wetternetzwerk", "windy", "opensensemap", "wow", "cwop",
        "ntfy", "pushover", "telegram", "webhook", "custom1", "custom2" };
    return id < OUT_COUNT ? keys[id] : "";
}

namespace {
OutStatus    gOut[OUT_COUNT];
portMUX_TYPE outMux = portMUX_INITIALIZER_UNLOCKED;
}

void outReport(uint8_t id, bool ok, int32_t code, const char* text)
{
    if (id >= OUT_COUNT) return;
    const uint32_t now = millis();
    portENTER_CRITICAL(&outMux);
    if (ok) outStatusOk(gOut[id], now);
    else    outStatusFail(gOut[id], now, code, text);
    portEXIT_CRITICAL(&outMux);
}

void outSnapshot(OutStatus out[OUT_COUNT])
{
    portENTER_CRITICAL(&outMux);
    memcpy(out, gOut, sizeof(gOut));
    portEXIT_CRITICAL(&outMux);
}
