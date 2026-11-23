// Radio trace (see rf_trace.h).
#include "rf_trace.h"
#include "globals.h"
#include "rf_sensors.h"
#include "sources.h"
#include "heltec_board.h"
#include "time_sync.h"
#include <FineOffsetDecoder.h>
#include <ItPlusDecoder.h>

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(__has_include)
#if __has_include("generated_version.h")
#include "generated_version.h"
#endif
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

#if defined(ESP_PLATFORM)
#include "psram.h"
// The capture is one block of > 4 KB: from the PSRAM if the board has one
// (psram.h), else from internal RAM, which then must keep the reserve.
static size_t deviceFreeHeap()     { return psramActive() ? psramFree() : internalFree(); }
static size_t deviceLargestBlock() { return psramActive() ? psramLargest() : internalLargest(); }
#else
static size_t deviceFreeHeap()     { return 200 * 1024; }
static size_t deviceLargestBlock() { return 100 * 1024; }
#endif
size_t (*rfTraceFreeHeap)()         = deviceFreeHeap;
size_t (*rfTraceLargestFreeBlock)() = deviceLargestBlock;

namespace {

struct IdStat {
    uint32_t    id;
    uint8_t     phase;
    const char* model;
    uint32_t    count[RF_TR_RESULTS];
    float       rssiMin, rssiMax, rssiLast;
    uint32_t    tx;                  // OK transmissions, repeats (RF_TRACE_REPEAT_MS) not counted
    uint32_t    firstOkMs, lastOkMs;
    uint32_t    nominalMs;           // send interval, 0 = unknown
};

// Everything of one capture, one heap block (freed as a whole).
struct Trace {
    uint32_t startMs = 0, stopMs = 0, durationMs = 0;
    int64_t  startEpoch = 0;          // 0 = clock not synced at the start
    bool     running = false, autoStarted = false;
    uint8_t  filter = RF_TRACE_ALL;
    uint32_t filterId = 0;
    uint32_t seen = 0, recorded = 0;
    uint32_t written = 0;             // bytes ever appended to the ring
    uint32_t counts[RF_TP_COUNT][RF_TR_RESULTS] = {};
    IdStat   ids[RF_TRACE_MAX_IDS];
    int      nIds = 0;
    uint32_t idsDropped = 0;
    uint32_t reads = 0, readDelaySum = 0, readDelayMax = 0, lateReads = 0;   // RfTracePacket::readDelayMs
    char     stopReason[40] = "";
    char     ring[RF_TRACE_SIZE];
};

Trace*       trace = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

const char* const PHASES[RF_TP_COUNT] = { "BRESSER", "IT+17", "IT+9", "FSK17", "DAVIS" };
const char* const RESULTS[RF_TR_RESULTS] = { "OK", "IGNORED", "FAILED", "UNKNOWN", "EVENT" };

bool wantsLocked(const Trace& t, RfTraceResult r, bool hasId, uint32_t id)
{
    if (!t.running) return false;
    switch (t.filter) {
        case RF_TRACE_PROBLEMS: return r == RF_TR_FAILED || r == RF_TR_UNKNOWN;
        case RF_TRACE_ONE_ID:   return hasId && id == t.filterId;
        default:                return true;
    }
}

void countLocked(Trace& t, uint8_t phase, RfTraceResult r, bool hasId, uint32_t id, const char* model, float rssi, bool hasRssi,
                 uint32_t nominalMs = 0)
{
    if (r == RF_TR_EVENT) return;
    t.seen++;
    if (phase < RF_TP_COUNT && r < RF_TR_RESULTS) t.counts[phase][r]++;
    if (!hasId) return;
    IdStat* s = nullptr;
    for (int i = 0; i < t.nIds; i++) if (t.ids[i].id == id && t.ids[i].phase == phase) { s = &t.ids[i]; break; }
    if (!s) {
        if (t.nIds >= RF_TRACE_MAX_IDS) { t.idsDropped++; return; }
        s = &t.ids[t.nIds++];
        *s = IdStat{ id, phase, model, {}, rssi, rssi, rssi, 0, 0, 0, 0 };
    }
    if (model && !s->model) s->model = model;
    if (r < RF_TR_RESULTS) s->count[r]++;
    if (nominalMs) s->nominalMs = nominalMs;
    if (r == RF_TR_OK) {
        const uint32_t now = millis();
        if (!s->tx) { s->firstOkMs = now; s->tx = 1; }
        else if (now - s->lastOkMs >= RF_TRACE_REPEAT_MS) s->tx++;
        s->lastOkMs = now;
    }
    if (hasRssi) {
        if (rssi < s->rssiMin) s->rssiMin = rssi;
        if (rssi > s->rssiMax) s->rssiMax = rssi;
        s->rssiLast = rssi;
    }
}

void appendLocked(Trace& t, const char* s, size_t n)
{
    for (size_t i = 0; i < n; i++) t.ring[(t.written + i) % RF_TRACE_SIZE] = s[i];
    t.written += n;
    t.recorded++;
}

// "  12.345 s [10:15:03]" - uptime and, with a synced clock, local time
int timeStamp(char* out, size_t len)
{
    uint32_t ms = millis();
    int n = snprintf(out, len, "%9lu.%03lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    struct tm tm;
    if (n > 0 && (size_t)n < len && localTimeNow(tm))
        n += snprintf(out + n, len - n, " %02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return n;
}

void hex(char* out, size_t len, const uint8_t* b, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 3 <= len; i++) k += snprintf(out + k, len - k, "%02X", b[i]);
    if (len) out[k < len ? k : len - 1] = '\0';
}

// Writer helpers for the file.
struct Out {
    RfTraceWriter w;
    void*         ctx;
    void str(const char* s) { w(s, strlen(s), ctx); }
    void fmt(const char* f, ...) __attribute__((format(printf, 2, 3)))
    {
        char b[256];
        va_list ap;
        va_start(ap, f);
        int n = vsnprintf(b, sizeof(b), f, ap);
        va_end(ap);
        if (n > 0) w(b, (size_t)n < sizeof(b) ? (size_t)n : sizeof(b) - 1, ctx);
    }
};

void addf(char* out, size_t len, size_t& n, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
void addf(char* out, size_t len, size_t& n, const char* fmt, ...)
{
    if (n + 1 >= len) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out + n, len - n, fmt, ap);
    va_end(ap);
    if (k > 0) n += (size_t)k < len - n ? (size_t)k : len - n - 1;
}

const char* protoText(int p)
{
    switch (p) {
        case RF_PROTO_AUTO:  return "Auto (search)";
        case RF_PROTO_DAVIS: return "Davis (exclusive)";
        default:             return nullptr;
    }
}

} // namespace

const char* rfTracePhaseName(uint8_t phase)   { return phase < RF_TP_COUNT ? PHASES[phase] : "?"; }
const char* rfTraceResultName(uint8_t result) { return result < RF_TR_RESULTS ? RESULTS[result] : "?"; }

bool rfTraceStart(uint32_t durationMs, RfTraceFilter filter, uint32_t id, bool autoStarted, char* err, size_t errLen)
{
    if (err && errLen) err[0] = '\0';
    if (filter > RF_TRACE_ONE_ID || (filter == RF_TRACE_ONE_ID && !id) || durationMs > RF_TRACE_MAX_MIN * 60000UL) {
        if (err) snprintf(err, errLen, "invalid capture settings");
        return false;
    }
    rfTraceClear();   // the previous capture's memory first
    size_t freeHeap = rfTraceFreeHeap(), largest = rfTraceLargestFreeBlock();
    if (largest < sizeof(Trace) || freeHeap < sizeof(Trace) + RF_TRACE_HEAP_RESERVE) {
        if (err) snprintf(err, errLen, "not enough free memory (%u KiB free, largest block %u KiB, need %u + %u KiB)",
                          (unsigned)(freeHeap / 1024), (unsigned)(largest / 1024),
                          (unsigned)(sizeof(Trace) / 1024), (unsigned)(RF_TRACE_HEAP_RESERVE / 1024));
        return false;
    }
    Trace* t = new (std::nothrow) Trace();
    if (!t) {
        if (err) snprintf(err, errLen, "memory allocation failed");
        return false;
    }
    t->startMs = millis();
    t->durationMs = durationMs;
    t->filter = filter;
    t->filterId = filter == RF_TRACE_ONE_ID ? id : 0;
    t->autoStarted = autoStarted;
    int64_t e;
    if (epochNow(e)) t->startEpoch = e;
    t->running = true;
    portENTER_CRITICAL(&mux);
    trace = t;
    portEXIT_CRITICAL(&mux);
    return true;
}

void rfTraceStop(const char* reason)
{
    portENTER_CRITICAL(&mux);
    if (trace && trace->running) {
        trace->running = false;
        trace->stopMs = millis();
        strlcpy(trace->stopReason, reason ? reason : "stopped", sizeof(trace->stopReason));
    }
    portEXIT_CRITICAL(&mux);
}

void rfTraceClear()
{
    portENTER_CRITICAL(&mux);
    Trace* t = trace;
    trace = nullptr;
    portEXIT_CRITICAL(&mux);
    delete t;
}

void rfTracePoll()
{
    bool due = false;
    portENTER_CRITICAL(&mux);
    due = trace && trace->running && trace->durationMs && millis() - trace->startMs >= trace->durationMs;
    portEXIT_CRITICAL(&mux);
    if (due) rfTraceStop("time is up");
}

bool rfTraceRunning()
{
    portENTER_CRITICAL(&mux);
    bool r = trace && trace->running;
    portEXIT_CRITICAL(&mux);
    return r;
}

RfTraceStatus rfTraceStatusNow()
{
    RfTraceStatus s;
    portENTER_CRITICAL(&mux);
    if (trace) {
        const Trace& t = *trace;
        s.allocated   = true;
        s.running     = t.running;
        s.autoStarted = t.autoStarted;
        s.filter      = t.filter;
        s.filterId    = t.filterId;
        s.durationMs  = t.durationMs;
        s.elapsedMs   = (t.running ? millis() : t.stopMs) - t.startMs;
        s.leftMs      = t.running && t.durationMs > s.elapsedMs ? t.durationMs - s.elapsedMs : 0;
        s.seen        = t.seen;
        s.recorded    = t.recorded;
        s.usedBytes   = t.written < RF_TRACE_SIZE ? t.written : RF_TRACE_SIZE;
        s.overwrittenBytes = t.written > RF_TRACE_SIZE ? t.written - RF_TRACE_SIZE : 0;
        memcpy(s.stopReason, t.stopReason, sizeof(s.stopReason));
    }
    portEXIT_CRITICAL(&mux);
    return s;
}

bool rfTraceWants(RfTraceResult r, bool hasId, uint32_t id)
{
    portENTER_CRITICAL(&mux);
    bool w = trace && wantsLocked(*trace, r, hasId, id);
    portEXIT_CRITICAL(&mux);
    return w;
}

void rfTracePacket(const RfTracePacket& p)
{
    if (!rfTraceRunning()) return;
    // formatted outside the lock
    char line[384], ts[32], raw[2 * RF_TRACE_FRAME_LEN + 1];
    timeStamp(ts, sizeof(ts));
    hex(raw, sizeof(raw), p.raw, p.rawLen < RF_TRACE_FRAME_LEN ? p.rawLen : RF_TRACE_FRAME_LEN);
    int n = snprintf(line, sizeof(line), "%s %-7s %4.0f dBm", ts, rfTracePhaseName(p.phase), p.rssi);
    if (p.hasFreqErr) n += snprintf(line + n, sizeof(line) - n, " fe %+.1fk", p.freqErrKhz);
    n += snprintf(line + n, sizeof(line) - n, " %-7s", rfTraceResultName(p.result));
    if (p.protocol && p.protocol[0]) n += snprintf(line + n, sizeof(line) - n, " %s", p.protocol);
    if (p.model && p.model[0])       n += snprintf(line + n, sizeof(line) - n, " %s", p.model);
    if (p.hasId)                     n += snprintf(line + n, sizeof(line) - n, " ID %08lX", (unsigned long)p.id);
    if (p.check && p.check[0])       n += snprintf(line + n, sizeof(line) - n, " (%s)", p.check);
    if (p.values && p.values[0])     n += snprintf(line + n, sizeof(line) - n, " %s", p.values);
    if (n < (int)sizeof(line)) n += snprintf(line + n, sizeof(line) - n, " | %uB %s\n", (unsigned)p.rawLen, raw);
    if (n >= (int)sizeof(line)) { n = sizeof(line) - 1; line[n - 1] = '\n'; }

    portENTER_CRITICAL(&mux);
    if (trace && trace->running) {
        countLocked(*trace, p.phase, p.result, p.hasId, p.id, p.model, p.rssi, true, p.nominalMs);
        if (p.hasReadDelay) {
            trace->reads++;
            trace->readDelaySum += p.readDelayMs;
            if (p.readDelayMs > trace->readDelayMax) trace->readDelayMax = p.readDelayMs;
            if (p.readDelayMs > RF_TRACE_LATE_READ_MS) trace->lateReads++;
        }
        if (wantsLocked(*trace, p.result, p.hasId, p.id)) appendLocked(*trace, line, (size_t)n);
    }
    portEXIT_CRITICAL(&mux);
}

void rfTraceText(uint8_t phase, const char* text, RfTraceResult r, bool hasId, uint32_t id)
{
    if (!text || !rfTraceRunning()) return;
    char line[340], ts[32];
    timeStamp(ts, sizeof(ts));
    int n = snprintf(line, sizeof(line), "%s %-7s %s", ts, rfTracePhaseName(phase), text);
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
    if (n && line[n - 1] != '\n') {
        if (n == (int)sizeof(line) - 1) n--;
        line[n++] = '\n';
        line[n] = '\0';
    }
    portENTER_CRITICAL(&mux);
    if (trace && trace->running) {
        countLocked(*trace, phase, r, hasId, id, nullptr, 0.0f, false);
        if (wantsLocked(*trace, r, hasId, id)) appendLocked(*trace, line, (size_t)n);
    }
    portEXIT_CRITICAL(&mux);
}

void rfTraceClassifyItPlus(const uint8_t* b, RfTracePacket& p, char* check, size_t checkLen)
{
    if ((b[0] >> 4) == 9) {
        bool crc = itPlusCrc8(b, 4) == b[4];
        snprintf(check, checkLen, crc ? "CRC ok, values invalid" : "CRC fail");
        p.result = RF_TR_FAILED;
        p.protocol = "IT+";
        p.id = (p.phase == RF_TP_ITPLUS9 ? RF_ID_ITPLUS9 : RF_ID_ITPLUS17) | (uint32_t)(((b[0] & 0x0F) << 2) | (b[1] >> 6));
        p.hasId = crc;   // without a valid CRC the ID bits are not trustworthy
    } else {
        snprintf(check, checkLen, "no known format (model nibble %X, IT+ needs 9)", b[0] >> 4);
        p.result = RF_TR_UNKNOWN;
        p.protocol = "";
    }
    p.check = check;
}

void rfTraceClassifyFsk17(const uint8_t* b, size_t len, bool fo, bool itPlus, RfTracePacket& p, char* check, size_t checkLen)
{
    p.check = check;
    check[0] = '\0';
    size_t L = fo && len ? foFrameLength(b[0]) : 0;
    if (L) {
        p.protocol = "Fine Offset";
        p.result = RF_TR_FAILED;
        if (len < L) {
            snprintf(check, checkLen, "family 0x%02X, frame too short (%u of %u bytes)", b[0], (unsigned)len, (unsigned)L);
        } else {
            bool crc = foCrc8(b, L - 2) == b[L - 2], sum = foSum8(b, L - 1) == b[L - 1];
            if (crc && sum) snprintf(check, checkLen, "family 0x%02X, CRC ok, sum ok, values rejected", b[0]);
            else            snprintf(check, checkLen, "family 0x%02X, %u bytes, CRC %s, sum %s", b[0], (unsigned)L,
                                     crc ? "ok" : "fail", sum ? "ok" : "fail");
        }
        if (itPlus && (b[0] >> 4) == 9 && itPlusCrc8(b, 4) == b[4]) {
            size_t k = strlen(check);
            snprintf(check + k, checkLen - k, "; as IT+: CRC ok, values invalid");
        }
        return;
    }
    if (fo && len >= FO_WH25_FRAME_LEN && ((b[0] >> 4) == 0xE || (b[0] >> 4) == 0xD)) {
        bool sum = foSum8(b, 6) == b[6];
        p.protocol = "Fine Offset";
        p.model = (b[0] >> 4) == 0xE ? "WH25/WN32P" : "WH32/WN32";
        p.result = RF_TR_FAILED;
        snprintf(check, checkLen, sum ? "WH25/WH32 frame, sum ok, values rejected" : "WH25/WH32 frame?, sum fail");
        return;
    }
    if (itPlus && len >= ITPLUS_FRAME_LEN && (b[0] >> 4) == 9) {
        rfTraceClassifyItPlus(b, p, check, checkLen);
        return;
    }
    p.result = RF_TR_UNKNOWN;
    p.protocol = "";
    snprintf(check, checkLen, "no known format (first byte 0x%02X)", len ? b[0] : 0);
}

void rfTraceFoValues(const FoFrame& f, char* out, size_t len)
{
    if (!len) return;
    out[0] = '\0';
    size_t n = 0;
    if (f.hasClock)     addf(out, len, n, "radio clock %04u-%02u-%02u %02u:%02u:%02u ", f.clockYear, f.clockMonth, f.clockDay,
                             f.clockHour, f.clockMinute, f.clockSecond);
    if (f.channel)      addf(out, len, n, "ch%u ", f.channel);
    if (f.hasTemp)      addf(out, len, n, "T %.1fC ", f.tempC);
    if (f.hasHumidity)  addf(out, len, n, "H %u%% ", f.humidity);
    if (f.hasWind)      addf(out, len, n, "W %.1f/%.1f m/s %u deg ", f.windAvgMs, f.windGustMs, f.windDirDeg);
    if (f.hasRain)      addf(out, len, n, "R %.1fmm ", f.rainMm);
    if (f.hasUv)        addf(out, len, n, "UV %.1f ", f.uvIndex);
    if (f.hasLight)     addf(out, len, n, "L %.0flx ", f.lightLux);
    if (f.hasPressure)  addf(out, len, n, "P %.1fhPa ", f.pressureHpa);
    if (f.hasMoisture)  addf(out, len, n, "soil %u%% ", f.moisture);
    if (f.hasLightning) addf(out, len, n, "strikes %u dist %u ", f.lightningCount, f.stormDistanceKm);
    if (f.hasPm25)      addf(out, len, n, "PM2.5 %.0f ", f.pm25);
    if (f.hasPm10)      addf(out, len, n, "PM10 %.0f ", f.pm10);
    if (f.hasCo2)       addf(out, len, n, "CO2 %u ", f.co2Ppm);
    if (f.hasLeak)      addf(out, len, n, "leak %s ", f.leak ? "YES" : "no");
    if (f.hasLeafWetness)  addf(out, len, n, "leaf %u%% ", f.leafWetnessPct);
    if (f.hasConductivity) addf(out, len, n, "EC %.0fuS/cm ", f.conductivityUsCm);
    if (f.hasSupercap)  addf(out, len, n, "cap %.2fV ", f.supercapV);
    if (f.batteryMv)    addf(out, len, n, "bat %umV ", f.batteryMv);
    if (!f.batteryKnown) { if (!f.hasClock) addf(out, len, n, "values not decoded yet (raw bytes below)"); }   // a radio-clock frame carries only the time
    else                 addf(out, len, n, "%s", f.batteryOk ? "bat ok" : "BAT LOW");
}

void rfTraceItPlusValues(const ItPlusFrame& f, char* out, size_t len)
{
    if (f.hasHumidity) snprintf(out, len, "T %.1fC H %u%%%s%s", f.tempC, f.humidity, f.newBattery ? " new-bat" : "",
                                f.weakBattery ? " BAT LOW" : " bat ok");
    else               snprintf(out, len, "T %.1fC%s%s", f.tempC, f.newBattery ? " new-bat" : "",
                                f.weakBattery ? " BAT LOW" : " bat ok");
}

void rfTraceFileName(char* out, size_t len, const char* hostname)
{
    int64_t start = 0;
    uint32_t startMs = millis();
    portENTER_CRITICAL(&mux);
    if (trace) { start = trace->startEpoch; startMs = trace->startMs; }
    portEXIT_CRITICAL(&mux);
    const char* h = hostname && hostname[0] ? hostname : "node";
    if (start) {
        time_t tt = (time_t)start;
        struct tm tm;
        localtime_r(&tt, &tm);
        snprintf(out, len, "uwn-rftrace-%s-%04d%02d%02d-%02d%02d.txt", h, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min);
    } else {
        snprintf(out, len, "uwn-rftrace-%s-up%lus.txt", h, (unsigned long)(startMs / 1000));
    }
}

void rfTraceWriteFile(RfTraceWriter w, void* ctx, const char* hostname, const char* receiverInfo)
{
    Out o{ w, ctx };
    const RfTraceStatus st = rfTraceStatusNow();
    o.str("# Universal Weather Node - radio trace (diagnostic file)\n");
    o.str("# PRIVACY: this file contains the IDs and readings of ALL wireless sensors in range,\n"
          "# including your neighbours'. It contains no passwords, keys or network credentials.\n#\n");
    o.fmt("# Firmware:        %s\n", FIRMWARE_VERSION);
    o.fmt("# Host name:       %s\n", hostname && hostname[0] ? hostname : "-");
    o.fmt("# Board:           Heltec WiFi LoRa 32 %s, front end %s, %lu MB flash\n", boardName(gBoard.kind),
          femName(gBoard.fem), (unsigned long)(gBoard.flashBytes >> 20));
    o.fmt("# PCB:             %s\n", boardPcbAllowed() ? (gCfg.pcbVersion == 20 ? "v2.0" : gCfg.pcbVersion == 12 ? "Rev 1.2" : "Rev 1.3")
                                                          : "none (Heltec V4)");
    uint32_t ms = millis();
    o.fmt("# Uptime:          %lu s\n", (unsigned long)(ms / 1000));
    int64_t now;
    if (epochNow(now)) {
        time_t tt = (time_t)now;
        struct tm tm;
        gmtime_r(&tt, &tm);
        o.fmt("# Time (UTC):      %04d-%02d-%02d %02d:%02d:%02d (line times below are local)\n", tm.tm_year + 1900,
              tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        o.str("# Time:            clock not synced (line times are uptime only)\n");
    }
#if defined(ESP_PLATFORM)
    o.fmt("# Free heap:       %u KiB (largest block %u KiB, internal RAM)\n", (unsigned)(internalFree() / 1024),
          (unsigned)(internalLargest() / 1024));
    if (psramActive())
        o.fmt("# PSRAM:           %u KiB, %u KiB free\n", (unsigned)(psramTotal() / 1024), (unsigned)(psramFree() / 1024));
    else
        o.fmt("# PSRAM:           %s\n", gBoard.psramCap > 0 ? "on the chip, not started" : "none");
#else
    o.fmt("# Free heap:       %u KiB (largest block %u KiB)\n", (unsigned)(rfTraceFreeHeap() / 1024),
          (unsigned)(rfTraceLargestFreeBlock() / 1024));
#endif
    o.str("#\n# Receive settings\n");
    const char* pt = protoText(gCfg.rfProto);
    if (pt) o.fmt("#   wireless:      %s, sensor type %s\n", gCfg.rfEn ? "on" : "off", pt);
    else    o.fmt("#   wireless:      %s, sensor type bit mask %d (1 Bresser, 2 IT+ 17.241k, 4 IT+ 9.579k, 8 Fine Offset)\n",
                  gCfg.rfEn ? "on" : "off", gCfg.rfProto);
    o.fmt("#   Mine:          %s\n", gCfg.rfIds[0] ? gCfg.rfIds : "(none)");
    o.fmt("#   hidden:        %s\n", gCfg.rfIgnore[0] ? gCfg.rfIgnore : "(none)");
    o.fmt("#   only mine:     %s\n", rfOnlyMineActive() ? "on (other sensors are dropped: IGNORED)" : "off");
    o.fmt("#   LoRaWAN:       %s (shares the radio: no reception during uplinks)\n", gCfg.loraEn ? "on" : "off");
    o.fmt("#   sources:       air %u, wind %u, rain %u, UV %u, light %u, pressure %u (0 auto, 1 board, 2-5 wireless 1-4, 6 off)\n",
          gCfg.srcSel[0], gCfg.srcSel[1], gCfg.srcSel[2], gCfg.srcSel[3], gCfg.srcSel[4], gCfg.srcSel[5]);
    o.fmt("#   units:         %s, wind %s\n", gCfg.unitSystem == 1 ? "imperial" : "metric", gCfg.windUnitMs ? "m/s" : "km/h");
    if (gCfg.rfProto == RF_PROTO_DAVIS)
        o.fmt("#   Davis:         ID %d, offset %d Hz, window %d / guard %d ms, scan %d kHz, AFC %d, table %d, bw %d, "
              "bucket %d, model %d, sniffer %d, raw %d ch%d\n", gCfg.davisTxId, gCfg.davisOffsetHz, gCfg.davisWindowMs,
              gCfg.davisGuardMs, gCfg.davisScanKhz, gCfg.davisAfc, gCfg.davisTable, gCfg.davisRxBw, gCfg.davisBucket,
              gCfg.davisModel, gCfg.davisSniffer, gCfg.davisRaw, gCfg.davisRawCh);
    if (receiverInfo && receiverInfo[0]) o.fmt("#   receiver:      %s\n", receiverInfo);

    o.str("#\n# Sensor table (slot, ID, protocol, model, type, channel, packets, RSSI, age, battery)\n");
    int used = 0;
    for (int i = 0; i < RF_MAX_SENSORS; i++) {
        const RfSensorReading& r = gRfSensors[i];
        if (!r.used) continue;
        used++;
        int slot = rfSlotOfIndex(i);
        o.fmt("#   %2d  %08lX  %-18s %-12s type %2u ch %u  %6lu pkts  %4.0f dBm  %5lu s  %s\n", slot + 1,
              (unsigned long)r.id, rfProtoName(r.proto), r.model ? r.model : "-", r.type, r.channel,
              (unsigned long)r.packets, r.rssi, (unsigned long)((ms - r.lastMs) / 1000), r.batteryOk ? "battery ok" : "BATTERY LOW");
    }
    if (!used) o.str("#   (none)\n");

    o.str("#\n# Capture\n");
    if (!st.allocated) { o.str("#   no capture in memory - start one on the status page\n"); return; }
    static const char* const filters[] = { "all packets", "only failed or unknown packets", "one sensor ID" };
    o.fmt("#   filter:        %s", filters[st.filter <= RF_TRACE_ONE_ID ? st.filter : 0]);
    if (st.filter == RF_TRACE_ONE_ID) o.fmt(" %08lX", (unsigned long)st.filterId);
    o.str("\n");
    if (st.durationMs) o.fmt("#   duration:      %lu min%s\n", (unsigned long)(st.durationMs / 60000), st.autoStarted ? " (Davis mode)" : "");
    else               o.fmt("#   duration:      until stopped%s\n", st.autoStarted ? " (started by Davis mode)" : "");
    o.fmt("#   state:         %s after %lu s%s%s\n", st.running ? "RUNNING" : "finished", (unsigned long)(st.elapsedMs / 1000),
          st.running ? "" : ": ", st.running ? "" : st.stopReason);
    o.fmt("#   frames seen:   %lu, lines recorded %lu, buffer %lu of %u bytes, %lu older bytes overwritten\n",
          (unsigned long)st.seen, (unsigned long)st.recorded, (unsigned long)st.usedBytes, (unsigned)RF_TRACE_SIZE,
          (unsigned long)st.overwrittenBytes);

    // statistics (whole capture, also what the filter or the ring dropped)
    struct Snap { uint32_t counts[RF_TP_COUNT][RF_TR_RESULTS]; IdStat ids[RF_TRACE_MAX_IDS]; int nIds; uint32_t dropped;
                  uint32_t reads, readDelaySum, readDelayMax, lateReads; };
    Snap* s = new (std::nothrow) Snap;
    if (s) {
        portENTER_CRITICAL(&mux);
        if (trace) {
            memcpy(s->counts, trace->counts, sizeof(s->counts));
            memcpy(s->ids, trace->ids, sizeof(s->ids));
            s->nIds = trace->nIds;
            s->dropped = trace->idsDropped;
            s->reads = trace->reads; s->readDelaySum = trace->readDelaySum;
            s->readDelayMax = trace->readDelayMax; s->lateReads = trace->lateReads;
        } else {
            memset(s, 0, sizeof(*s));
        }
        portEXIT_CRITICAL(&mux);
        o.str("#\n# Frames per phase:      OK  IGNORED   FAILED  UNKNOWN\n");
        for (int ph = 0; ph < RF_TP_COUNT; ph++) {
            const uint32_t* c = s->counts[ph];
            if (!(c[0] | c[1] | c[2] | c[3])) continue;
            o.fmt("#   %-8s %10lu %8lu %8lu %8lu\n", PHASES[ph], (unsigned long)c[0], (unsigned long)c[1],
                  (unsigned long)c[2], (unsigned long)c[3]);
        }
        if (s->reads)
            o.fmt("#\n# Read delay (packet to firmware, Fine Offset / IT+): avg %lu ms, max %lu ms, %lu of %lu reads later than %lu ms\n"
                  "#   (a packet arriving before the read replaces the unread one)\n",
                  (unsigned long)(s->readDelaySum / s->reads), (unsigned long)s->readDelayMax, (unsigned long)s->lateReads,
                  (unsigned long)s->reads, (unsigned long)RF_TRACE_LATE_READ_MS);
        o.str("#\n# Frames per ID (phase, model, OK / ignored / failed, RSSI min..max last; received of the transmissions\n"
              "#   expected from the send interval between the first and the last one, the sensor's repeats not counted)\n");
        for (int i = 0; i < s->nIds; i++) {
            const IdStat& d = s->ids[i];
            char rate[48] = "";
            if (d.nominalMs && d.tx >= 2) {
                const uint32_t expected = (uint32_t)(((uint64_t)(d.lastOkMs - d.firstOkMs) + d.nominalMs / 2) / d.nominalMs) + 1;
                if (expected >= 3)
                    snprintf(rate, sizeof(rate), "   %lu of %lu sent (%lu %%)", (unsigned long)d.tx, (unsigned long)expected,
                             (unsigned long)(d.tx >= expected ? 100 : d.tx * 100 / expected));
            }
            o.fmt("#   %08lX %-7s %-12s %6lu / %lu / %lu   %.0f..%.0f %.0f dBm%s\n", (unsigned long)d.id, PHASES[d.phase],
                  d.model ? d.model : "-", (unsigned long)d.count[RF_TR_OK], (unsigned long)d.count[RF_TR_IGNORED],
                  (unsigned long)d.count[RF_TR_FAILED], d.rssiMin, d.rssiMax, d.rssiLast, rate);
        }
        if (!s->nIds) o.str("#   (none)\n");
        if (s->dropped) o.fmt("#   ... %lu frames of further IDs not listed\n", (unsigned long)s->dropped);
        delete s;
    }
    o.str("#\n# Lines: uptime s [local time] phase RSSI result protocol model ID (check) values | bytes hex\n#\n");

    // the ring, oldest first; read in pieces (the capture may still be running)
    uint32_t pos = 0;
    bool started = false;
    char buf[512];
    for (;;) {
        size_t n = 0;
        bool align = false;   // the piece starts inside a line (ring wrapped): drop up to its end
        portENTER_CRITICAL(&mux);
        if (trace) {
            uint32_t oldest = trace->written > RF_TRACE_SIZE ? trace->written - RF_TRACE_SIZE : 0;
            if (pos < oldest) { pos = oldest; align = true; }
            uint32_t avail = trace->written - pos;
            n = avail < sizeof(buf) ? avail : sizeof(buf);
            for (size_t i = 0; i < n; i++) buf[i] = trace->ring[(pos + i) % RF_TRACE_SIZE];
            pos += n;
        }
        portEXIT_CRITICAL(&mux);
        if (!n) break;
        size_t from = 0;
        if (align) {
            if (started) o.str("\n# ... lines overwritten while downloading ...\n");
            while (from < n && buf[from] != '\n') from++;
            if (from < n) from++;
        }
        started = true;
        if (from < n) w(buf + from, n - from, ctx);
    }
    o.str(st.running ? "# (capture still running)\n" : "# end of capture\n");
}
