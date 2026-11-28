#include "vsensor.h"
#include "FineOffsetEncoder.h"
#include "ItPlusEncoder.h"
#include "storage_layout.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static_assert(VS_MAX_FRAME_LEN >= FO_MAX_FRAME_LEN, "a frame must hold the longest Fine Offset frame");
static_assert(VS_MAX_FRAME_LEN >= ITPLUS_FRAME_LEN, "a frame must hold an IT+ frame");

namespace {
constexpr uint16_t B(int f) { return (uint16_t)(1u << f); }
constexpr uint16_t WH65_FIELDS = B(VF_TEMP) | B(VF_HUM) | B(VF_WIND) | B(VF_GUST) | B(VF_DIR) | B(VF_RAIN) | B(VF_UV) | B(VF_LUX);

const char* const FIELD_KEYS[VF_COUNT] = { "temp", "hum", "press", "wind", "gust", "dir", "rain", "uv", "lux" };

// Running rain totals wrap like the originals' counters (16 bit: WH40 0.1 mm, WH65 0.254 mm).
constexpr float WH40_RAIN_RANGE = 6553.6f;
constexpr float WH65_RAIN_RANGE = 0.254f * 65536.0f;

// IT+ table IDs (rf_sensors.h rfItPlusId(); test/firmware_vsensor.cpp compares)
constexpr uint32_t ITPLUS17_BASE = 0x17000000UL, ITPLUS9_BASE = 0x09000000UL, ITPLUS_HUM = 0x100;

bool isItPlus(uint8_t type) { return type == VS_ITPLUS17 || type == VS_ITPLUS9; }

float wrapRain(float mm, float range)
{
    if (!(mm > 0.0f)) return 0.0f;
    return fmodf(mm, range);
}

uint8_t pct(float h)
{
    long r = lroundf(h);
    return (uint8_t)(r < 0 ? 0 : r > 100 ? 100 : r);
}
} // namespace

// IT+ originals send every ~4 s; at 4 s one IT+ slot takes 8.6 s (17.241k)
// or 12.5 s (9.579k) of airtime per hour, so only one or two would fit the
// plan limit (VS_PLAN_MAX_MS_PER_H, 18 s). The IT+ intervals are the shortest
// at which 8 slots of the type fit (whole seconds): 15 s x 9 ms, 22 s x 13 ms
// (test/firmware_vsensor.cpp).
//                                   key     model       fields                                  mustMap        skipMissing   interval  len  kbps
const VsTypeInfo VS_TYPES[VS_TYPE_COUNT] = {
    { "off",   "",         0,                                    0,             0,            0,        0,   VS_BITRATE_KBPS },
    { "wh31",  "WH31",     B(VF_TEMP) | B(VF_HUM),               B(VF_TEMP),    B(VF_TEMP),   61000,    7,   VS_BITRATE_KBPS },
    { "wh40",  "WH40",     B(VF_RAIN),                           B(VF_RAIN),    B(VF_RAIN),   49000,    9,   VS_BITRATE_KBPS },
    { "wn32p", "WN32P",    B(VF_TEMP) | B(VF_HUM) | B(VF_PRESS), B(VF_TEMP),    B(VF_TEMP),   64000,    8,   VS_BITRATE_KBPS },
    { "wh65",  "WH65",     WH65_FIELDS,                          0,             B(VF_RAIN),   16000,    17,  VS_BITRATE_KBPS },
    { "tx29",  "IT+ TX29", B(VF_TEMP) | B(VF_HUM),               B(VF_TEMP),    B(VF_TEMP),   15000,    (uint8_t)ITPLUS_FRAME_LEN, VS_BITRATE_KBPS },
    { "tx35",  "IT+ TX35", B(VF_TEMP) | B(VF_HUM),               B(VF_TEMP),    B(VF_TEMP),   22000,    (uint8_t)ITPLUS_FRAME_LEN, VS_BITRATE_IT9_KBPS },
};

const char* vsFieldKey(uint8_t f) { return f < VF_COUNT ? FIELD_KEYS[f] : ""; }

int vsFieldByKey(const char* key)
{
    if (!key) return -1;
    for (int f = 0; f < VF_COUNT; f++) if (!strcmp(FIELD_KEYS[f], key)) return f;
    return -1;
}

int vsTypeByKey(const char* key)
{
    if (!key) return -1;
    for (int t = 0; t < VS_TYPE_COUNT; t++) if (!strcmp(VS_TYPES[t].key, key)) return t;
    return -1;
}

uint32_t vsDeriveId(const uint8_t mac[6], int slot, uint8_t type)
{
    uint32_t h = 2166136261u;   // FNV-1a over the MAC, the type and a tag
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
    for (const char* p = "uwn-vs"; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    h ^= type; h *= 16777619u;
    const uint32_t s = slot < 0 ? 0 : (uint32_t)slot;
    // the slot step keeps the IDs of all slots apart: 67 is prime to 254 and
    // 0x2F0F1 to 0xFFFFE (= 2 x 524287), so 67 * k mod 254 and
    // 0x2F0F1 * k mod 0xFFFFE are different and non-zero for k = 1..VS_SLOTS-1;
    // 37 is odd, so 37 * k mod 64 is non-zero for k = 1..63
    if (isItPlus(type)) return (h + s * 37u) % 64u;                  // 0..63
    if (type == VS_WH40) return 1 + (h + s * 0x2F0F1u) % 0xFFFFEu;   // 1..0xFFFFE
    return 1 + (h + s * 67u) % 254u;                                 // 1..254
}

uint32_t vsTableId(uint8_t type, uint32_t id, uint8_t channel, bool humidity)
{
    switch (type) {
        case VS_ITPLUS17:
        case VS_ITPLUS9:   return (type == VS_ITPLUS17 ? ITPLUS17_BASE : ITPLUS9_BASE) | (id & 0x3F) | (humidity ? ITPLUS_HUM : 0);
        case VS_WH31:  return (uint32_t)FO_FAMILY_WH31 << 24 | (uint32_t)channel << 8 | (id & 0xFF);
        case VS_WH40:  return (uint32_t)FO_FAMILY_WH40 << 24 | (id & 0xFFFFF);
        case VS_WN32P: return (uint32_t)FO_FAMILY_WH25 << 24 | (id & 0xFF);
        case VS_WH65:  return (uint32_t)FO_FAMILY_WH65 << 24 | (id & 0xFF);
        default:       return 0;
    }
}

uint32_t vsSlotTableId(const VsSlotCfg& c, int slot, const uint8_t mac[6])
{
    if (c.type == VS_OFF || c.type >= VS_TYPE_COUNT) return 0;
    return vsTableId(c.type, vsDeriveId(mac, slot, c.type), c.channel, c.src[VF_HUM][0] != 0);
}

bool vsIdCollides(const VsSlotCfg& c, int slot, const uint8_t mac[6], uint32_t receivedId)
{
    const uint32_t id = vsSlotTableId(c, slot, mac);
    if (!id) return false;
    return receivedId == id || (isItPlus(c.type) && receivedId == (id ^ ITPLUS_HUM));
}

bool vsResolve(const VsSlotCfg& c, VsLookup lookup, void* ctx, VsValues& out)
{
    out = VsValues();
    if (c.type == VS_OFF || c.type >= VS_TYPE_COUNT || !lookup) return false;
    const VsTypeInfo& t = VS_TYPES[c.type];
    uint16_t mapped = 0;
    for (int f = 0; f < VF_COUNT; f++) {
        if (!(t.fields & B(f)) || !c.src[f][0]) continue;
        mapped |= B(f);
        float v;
        if (lookup(c.src[f], v, ctx) && isfinite(v)) { out.has |= B(f); out.v[f] = v; }
    }
    if (t.skipMissing & mapped & (uint16_t)~out.has) return false;
    return out.has != 0;
}

VsFrame vsBuildFrame(const VsSlotCfg& c, int slot, const uint8_t mac[6], const VsValues& v, bool pairing)
{
    VsFrame fr;
    if (c.type == VS_OFF || c.type >= VS_TYPE_COUNT || slot < 0 || slot >= VS_SLOTS || !mac) return fr;
    const uint32_t id = vsDeriveId(mac, slot, c.type);   // (c): the only source of the ID
    auto has = [&](int f) { return (v.has & B(f)) != 0; };
    size_t n = 0;
    switch (c.type) {
        case VS_WH31:
            if (!has(VF_TEMP)) break;
            n = foEncodeWh31(fr.b, (uint8_t)id, c.channel, v.v[VF_TEMP], has(VF_HUM), has(VF_HUM) ? pct(v.v[VF_HUM]) : 0, true);
            break;
        case VS_WH40:
            if (!has(VF_RAIN)) break;
            n = foEncodeWh40(fr.b, id, wrapRain(v.v[VF_RAIN], WH40_RAIN_RANGE), true);
            break;
        case VS_WN32P:
            if (!has(VF_TEMP)) break;
            n = foEncodeWn32p(fr.b, (uint8_t)id, v.v[VF_TEMP], has(VF_HUM) ? pct(v.v[VF_HUM]) : 0,
                              has(VF_PRESS) ? v.v[VF_PRESS] : -1.0f, true);
            break;
        case VS_WH65: {
            FoWh65Values w;
            w.id = (uint8_t)id;
            w.batteryOk = true;
            if (has(VF_TEMP)) { w.hasTemp = true; w.tempC = v.v[VF_TEMP]; }
            if (has(VF_HUM))  { w.hasHum = true;  w.humidity = pct(v.v[VF_HUM]); }
            if (has(VF_WIND) && has(VF_DIR)) {
                w.hasWind = true;
                w.windAvgMs  = v.v[VF_WIND] < 0 ? 0 : v.v[VF_WIND];
                w.windGustMs = has(VF_GUST) && v.v[VF_GUST] >= 0 ? v.v[VF_GUST] : w.windAvgMs;
                long d = lroundf(v.v[VF_DIR]) % 360;
                w.windDirDeg = (uint16_t)(d < 0 ? d + 360 : d);
            }
            if (has(VF_RAIN)) { w.hasRain = true; w.rainTotalMm = wrapRain(v.v[VF_RAIN], WH65_RAIN_RANGE); }
            if (has(VF_UV)) {
                long u = lroundf(v.v[VF_UV]);
                w.hasUv = true; w.uvIndex = (uint8_t)(u < 0 ? 0 : u > 15 ? 15 : u);
            }
            if (has(VF_LUX)) { w.hasLight = true; w.lightLux = v.v[VF_LUX] < 0 ? 0 : v.v[VF_LUX]; }
            n = foEncodeWh65(fr.b, w);
            break;
        }
        case VS_ITPLUS17:
        case VS_ITPLUS9:
            if (!has(VF_TEMP)) break;
            n = itPlusEncode(fr.b, (uint8_t)id, pairing, v.v[VF_TEMP], has(VF_HUM), has(VF_HUM) ? pct(v.v[VF_HUM]) : 0);
            break;
        default: break;
    }
    if (n > 0 && n <= sizeof(fr.b)) {
        fr.n = (uint8_t)n;
        fr.sensorId = vsTableId(c.type, id, c.channel, has(VF_HUM));
        fr.rate = VS_TYPES[c.type].kbps;
    }
    return fr;
}

uint32_t vsAirtimeMs(size_t frameLen, float kbps)
{
    const uint32_t bits = VS_PREAMBLE_BITS + 16 + 8 * (uint32_t)frameLen;
    return (uint32_t)((bits * 1000.0f / kbps + 999.0f) / 1000.0f) + VS_TX_OVERHEAD_MS;   // rounded up
}

uint32_t vsTypeAirtimeMs(uint8_t type)
{
    if (type == VS_OFF || type >= VS_TYPE_COUNT) return 0;
    return vsAirtimeMs(VS_TYPES[type].frameLen, VS_TYPES[type].kbps);
}

void VsBandLedger::add(uint64_t nowMs, uint32_t airMs)
{
    lastHourMs(nowMs);   // moves to the current minute
    const uint32_t v = ms[minute % 60] + airMs;
    ms[minute % 60] = (uint16_t)(v > 0xFFFF ? 0xFFFF : v);
}

uint32_t VsBandLedger::lastHourMs(uint64_t nowMs)
{
    const uint64_t m = nowMs / 60000;
    if (m != minute) {
        const uint64_t gap = m > minute ? m - minute : 60;
        for (uint64_t k = 1; k <= gap && k <= 60; k++) ms[(minute + k) % 60] = 0;
        minute = m;
    }
    uint32_t sum = 0;
    for (int i = 0; i < 60; i++) sum += ms[i];
    return sum;
}

uint32_t vsPlannedMsPerHour(const VsSlotCfg cfg[VS_SLOTS])
{
    uint32_t total = 0;
    for (int i = 0; i < VS_SLOTS; i++) {
        const VsSlotCfg& c = cfg[i];
        if (!c.enabled || c.type == VS_OFF || c.type >= VS_TYPE_COUNT) continue;
        const VsTypeInfo& t = VS_TYPES[c.type];
        const uint32_t minInterval = t.intervalMs - (uint32_t)VS_JITTER_MS;
        const uint32_t perHour = (3600000u + minInterval - 1) / minInterval;
        total += perHour * vsTypeAirtimeMs(c.type);
    }
    return total;
}

uint8_t vsCheckSlot(const VsSlotCfg& c)
{
    if (c.type == VS_OFF || c.type >= VS_TYPE_COUNT) return VS_CHECK_OK;
    const VsTypeInfo& t = VS_TYPES[c.type];
    uint16_t mapped = 0;
    for (int f = 0; f < VF_COUNT; f++) if ((t.fields & B(f)) && c.src[f][0]) mapped |= B(f);
    if (!mapped || (t.mustMap & ~mapped)) return VS_CHECK_MUSTMAP;
    const bool w = mapped & B(VF_WIND), d = mapped & B(VF_DIR), g = mapped & B(VF_GUST);
    if ((w && !d) || ((d || g) && !w)) return VS_CHECK_WIND;
    if (c.type == VS_WH31 && (c.channel < 1 || c.channel > 8)) return VS_CHECK_CHANNEL;
    return VS_CHECK_OK;
}

bool vsCleanSrc(const char* in, char* out, size_t len)
{
    if (!out || !len) return false;
    out[0] = '\0';
    if (!in) return true;
    size_t n = 0;
    for (const char* p = in; *p; p++) {
        const char ch = *p;
        if (ch == '{' || ch == '}' || ch == ' ' || ch == '\t') continue;
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        ch == '_' || ch == '-' || ch == '@';
        if (!ok || n + 1 >= len) { out[0] = '\0'; return false; }
        out[n++] = ch;
    }
    out[n] = '\0';
    return true;
}

uint64_t vsNextDue(uint64_t dueMs, uint64_t nowMs, uint32_t intervalMs, int32_t jitterMs)
{
    if (jitterMs > VS_JITTER_MS) jitterMs = VS_JITTER_MS;
    if (jitterMs < -VS_JITTER_MS) jitterMs = -VS_JITTER_MS;
    // on the original's grid; after a long stall (blocking uplink, ...) from now
    uint64_t base = dueMs + intervalMs;
    if (base <= nowMs) base = nowMs + intervalMs;
    return (uint64_t)((int64_t)base + jitterMs);
}

bool vsDavisGapAllows(const VsDavisGap& g, int64_t nowUs, int sentSoFar, uint32_t airMs)
{
    if (!g.confirmed || sentSoFar < 0 || sentSoFar >= VS_DAVIS_MAX_FRAMES) return false;
    if (g.nextUs <= g.packetUs || nowUs < g.packetUs || nowUs - g.packetUs > (int64_t)VS_DAVIS_LATE_US) return false;
    const int64_t listenFrom = g.nextUs - (int64_t)g.guardUs - VS_DAVIS_PACKET_US;   // the next packet may start here
    const int64_t doneAt = nowUs + (int64_t)airMs * 1000 + VS_DAVIS_SWITCH_US + VS_DAVIS_MARGIN_US;
    return doneAt <= listenFrom;
}

int vsDavisGapFrames(const VsDavisGap& g, int64_t nowUs, const uint32_t airMs[], int n)
{
    int k = 0;
    for (int64_t t = nowUs; k < n && vsDavisGapAllows(g, t, k, airMs[k]); k++)
        t += (int64_t)airMs[k] * 1000 + VS_DAVIS_SWITCH_US;
    return k;
}

int vsDavisPick(const bool waiting[VS_SLOTS], const uint64_t queuedMs[VS_SLOTS])
{
    int best = -1;
    for (int i = 0; i < VS_SLOTS; i++)
        if (waiting[i] && (best < 0 || queuedMs[i] < queuedMs[best])) best = i;
    return best;
}

size_t vsPack(const VsSlotCfg cfg[VS_SLOTS], char* out, size_t len)
{
    if (!out || !len) return 0;
    size_t n = 0;
    out[0] = '\0';
    auto put = [&](const char* s) {
        for (; *s && n + 1 < len; s++) out[n++] = *s;
        out[n] = '\0';
    };
    for (int i = 0; i < VS_SLOTS; i++) {
        if (i) put("\n");
        const VsSlotCfg& c = cfg[i];
        if (c.type == VS_OFF || c.type >= VS_TYPE_COUNT) continue;
        char head[24];
        snprintf(head, sizeof(head), "%s|%u|%u|", VS_TYPES[c.type].key, c.enabled ? 1u : 0u, (unsigned)c.channel);
        put(head);
        bool first = true;
        for (int f = 0; f < VF_COUNT; f++) {
            if (!c.src[f][0]) continue;
            if (!first) put(",");
            first = false;
            put(FIELD_KEYS[f]); put("="); put(c.src[f]);
        }
    }
    return n;
}

void vsUnpack(const char* s, VsSlotCfg cfg[VS_SLOTS])
{
    for (int i = 0; i < VS_SLOTS; i++) cfg[i] = VsSlotCfg();
    if (!s) return;
    for (int i = 0; i < VS_SLOTS && *s; i++) {
        const char* e = strchr(s, '\n');
        const size_t ll = e ? (size_t)(e - s) : strlen(s);
        char line[VF_COUNT * (VS_SRC_LEN + 8) + 32];
        if (ll < sizeof(line)) {
            memcpy(line, s, ll);
            line[ll] = '\0';
            VsSlotCfg c;
            char* p1 = strchr(line, '|');
            char* p2 = p1 ? strchr(p1 + 1, '|') : nullptr;
            char* p3 = p2 ? strchr(p2 + 1, '|') : nullptr;
            if (p3) {
                *p1 = *p2 = *p3 = '\0';
                const int t = vsTypeByKey(line);
                if (t > VS_OFF) {
                    c.type = (uint8_t)t;
                    c.enabled = p1[1] == '1' ? 1 : 0;
                    const long chv = strtol(p2 + 1, nullptr, 10);
                    c.channel = (uint8_t)(chv >= 1 && chv <= 8 ? chv : 1);
                    bool ok = true;
                    for (char* tok = p3 + 1; ok && *tok;) {
                        char* comma = strchr(tok, ',');
                        if (comma) *comma = '\0';
                        char* eq = strchr(tok, '=');
                        if (eq) {
                            *eq = '\0';
                            const int f = vsFieldByKey(tok);
                            if (f >= 0 && !vsCleanSrc(eq + 1, c.src[f], VS_SRC_LEN)) ok = false;
                        }
                        if (!comma) break;
                        tok = comma + 1;
                    }
                    if (ok) cfg[i] = c;
                }
            }
        }
        if (!e) break;
        s = e + 1;
    }
}

size_t vsSettingsPack(const VsSettings& s, uint8_t* out, size_t len)
{
    if (!out || len < 3 + 1) return 0;
    out[0] = STORED_VIRTUAL_SENSORS_VERSION;
    out[1] = s.enabled ? 1 : 0;
    out[2] = (uint8_t)s.txPowerDbm;
    // the text with its terminator into the rest, then the terminator dropped
    const size_t n = vsPack(s.slot, (char*)out + 3, len - 3);
    return n + 1 < len - 3 ? 3 + n : 0;   // cut = 0: the caller's buffer is VS_BLOB_MAX
}

bool vsSettingsFromPayload(const uint8_t* p, size_t n, VsSettings& out)
{
    out = VsSettings();
    if (!p || n < 2 || n - 2 >= VS_PACKED_MAX) return false;
    char text[VS_PACKED_MAX];
    memcpy(text, p + 2, n - 2);
    text[n - 2] = '\0';
    out.enabled = p[0] ? 1 : 0;
    const int dbm = (int8_t)p[1];
    out.txPowerDbm = (int8_t)(dbm < VS_DBM_MIN ? VS_DBM_MIN : dbm > VS_DBM_MAX ? VS_DBM_MAX : dbm);
    vsUnpack(text, out.slot);
    return true;
}

VsSlotStatus      gVsStatus[VS_SLOTS];
volatile uint32_t gVsDavisGapMaxUs = 0;
volatile uint32_t gVsDavisGapFrames = 0;
VsBandLedger      gVsBand;
volatile uint32_t gVsBandLastHourMs = 0;
