#include "lsensors.h"
#include "rf_names.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

int LogicalSensorTable::count() const
{
    int n = 0;
    while (n < LS_MAX && s[n].internalId[0]) n++;
    return n;
}

namespace {

// Every built-in placeholder name of the uploads, alarms and OLED screens
// (ph_live.cpp, oled_screens.cpp): a custom ID must neither equal one nor be the
// part before its "_" ("rain" + "_day"; "series" from the custom series' {series_<n>}).
const char* const BUILTIN[] = {
    "temp", "hum", "dew", "feels", "qnh", "press", "ptend3h", "wind", "gust", "dir",
    "rain_day", "rain_1h", "rain_24h", "rain_rate", "uv", "lux", "solar",
    "time_local", "time_utc", "epoch", "station", "lat", "lon",
    "clock", "date", "date_full", "ip", "u_temp", "u_wind", "u_rain", "u_rate", "u_press",
    "series_n",
};

bool isHex(char c) { return isxdigit((unsigned char)c) != 0; }

} // namespace

LsIdProblem lsCustomIdCheck(const char* customId)
{
    const size_t n = customId ? strlen(customId) : 0;
    if (n == 0) return LS_ID_EMPTY;
    if (n > LS_CUSTOM_ID_MAX) return LS_ID_TOO_LONG;
    for (size_t i = 0; i < n; i++) {
        const char c = customId[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') return LS_ID_CHARS;
    }
    if (n == LS_INTERNAL_ID_LEN && (customId[0] == 'L' || customId[0] == 'l')) {
        bool hex = true;
        for (size_t i = 1; i < n; i++) hex &= isHex(customId[i]);
        if (hex) return LS_ID_LOOKS_INTERNAL;
    }
    if (n > 2 && (customId[0] == 'r' || customId[0] == 'R') && (customId[1] == 'f' || customId[1] == 'F')) {
        bool hex = true;
        for (size_t i = 2; i < n; i++) hex &= isHex(customId[i]);
        if (hex) return LS_ID_LOOKS_LEGACY;
    }
    for (const char* b : BUILTIN) {
        if (!strcasecmp(b, customId)) return LS_ID_BUILTIN;
        const size_t bl = strlen(b);
        if (bl > n && b[n] == '_' && !strncasecmp(b, customId, n)) return LS_ID_BUILTIN_START;
    }
    return LS_ID_OK;
}

const char* lsCustomIdProblem(const char* customId)
{
    static const char* const TEXT[] = { nullptr, "empty", "too long (16 characters at most)", "only a-z, 0-9, _ and -",
        "looks like an internal ID (L + 6 hex digits)", "looks like an old wireless placeholder (rf + number)",
        "a built-in placeholder name", "the start of a built-in placeholder name" };
    return TEXT[lsCustomIdCheck(customId)];
}

int lsFindByInternalId(const LogicalSensorTable& t, const char* internalId)
{
    if (!internalId || !internalId[0]) return -1;
    for (int i = 0; i < LS_MAX && t.s[i].internalId[0]; i++) if (!strcmp(t.s[i].internalId, internalId)) return i;
    return -1;
}

int lsFindByCustomId(const LogicalSensorTable& t, const char* customId)
{
    if (!customId || !customId[0]) return -1;
    for (int i = 0; i < LS_MAX && t.s[i].internalId[0]; i++) if (!strcasecmp(t.s[i].customId, customId)) return i;
    return -1;
}

int lsFindByRadioId(const LogicalSensorTable& t, uint32_t radioId)
{
    if (!radioId) return -1;
    for (int i = 0; i < LS_MAX && t.s[i].internalId[0]; i++) if (t.s[i].radioId == radioId) return i;
    return -1;
}

void lsNewInternalId(const LogicalSensorTable& t, uint32_t (*rnd)(), char out[LS_INTERNAL_ID_LEN + 1])
{
    do snprintf(out, LS_INTERNAL_ID_LEN + 1, "L%06lX", (unsigned long)(rnd() & 0xFFFFFFUL));
    while (lsFindByInternalId(t, out) >= 0);
}

int lsAdd(LogicalSensorTable& t, uint32_t radioId, const char* displayName, uint32_t (*rnd)())
{
    const int n = t.count();
    if (n >= LS_MAX || !radioId || lsFindByRadioId(t, radioId) >= 0) return -1;
    LogicalSensor& e = t.s[n];
    e = LogicalSensor();
    char internalId[LS_INTERNAL_ID_LEN + 1];
    lsNewInternalId(t, rnd, internalId);   // drawn into a copy: e is part of t and must stay empty while it searches
    memcpy(e.internalId, internalId, sizeof internalId);
    char base[9];
    snprintf(base, sizeof base, "%08lX", (unsigned long)radioId);
    snprintf(e.customId, sizeof e.customId, "%s", base);
    for (int k = 2; lsFindByCustomId(t, e.customId) != n; k++) snprintf(e.customId, sizeof e.customId, "%s-%d", base, k);
    if (displayName) {
        snprintf(e.displayName, sizeof e.displayName, "%s", displayName);
        rfNameClean(e.displayName);
    }
    e.radioId = radioId;
    return n;
}

bool lsSetCustomId(LogicalSensorTable& t, int idx, const char* customId)
{
    if (idx < 0 || idx >= t.count() || lsCustomIdProblem(customId)) return false;
    const int other = lsFindByCustomId(t, customId);
    if (other >= 0 && other != idx) return false;
    snprintf(t.s[idx].customId, sizeof t.s[idx].customId, "%s", customId);
    return true;
}

bool lsRebindRadioId(LogicalSensorTable& t, int idx, uint32_t radioId)
{
    if (idx < 0 || idx >= t.count() || !radioId) return false;
    const int other = lsFindByRadioId(t, radioId);
    if (other >= 0 && other != idx) return false;
    t.s[idx].radioId = radioId;
    return true;
}

void lsRemove(LogicalSensorTable& t, int idx)
{
    const int n = t.count();
    if (idx < 0 || idx >= n) return;
    for (int i = idx; i + 1 < n; i++) t.s[i] = t.s[i + 1];
    t.s[n - 1] = LogicalSensor();
}

void lsMove(LogicalSensorTable& t, int idx, int delta)
{
    const int j = idx + delta, n = t.count();
    if (idx < 0 || idx >= n || j < 0 || j >= n) return;
    const LogicalSensor tmp = t.s[idx];
    t.s[idx] = t.s[j];
    t.s[j] = tmp;
}

LogicalSensorTable gLogicalSensors;

std::vector<uint32_t> lsRadioIds(const LogicalSensorTable& t)
{
    std::vector<uint32_t> ids;
    for (int i = 0; i < LS_MAX && t.s[i].internalId[0]; i++) if (t.s[i].radioId) ids.push_back(t.s[i].radioId);
    return ids;
}

size_t lsPack(const LogicalSensorTable& t, uint8_t* out, size_t len)
{
    const int n = t.count();
    const size_t need = 2 + LS_REC_SIZE * (size_t)n;
    if (len < need) return 0;
    memset(out, 0, need);
    out[0] = LS_BLOB_VERSION;
    out[1] = (uint8_t)n;
    uint8_t* p = out + 2;
    for (int i = 0; i < n; i++) {
        const LogicalSensor& e = t.s[i];
        memcpy(p, e.internalId, sizeof e.internalId);   p += sizeof e.internalId;
        memcpy(p, e.customId, sizeof e.customId);   p += sizeof e.customId;
        memcpy(p, e.displayName, sizeof e.displayName); p += sizeof e.displayName;
        for (int b = 0; b < 4; b++) *p++ = (uint8_t)(e.radioId >> (8 * b));
        *p++ = (uint8_t)e.minRssiDbm;
    }
    return need;
}

bool lsEqual(const LogicalSensorTable& a, const LogicalSensorTable& b)
{
    if (a.count() != b.count()) return false;
    for (int i = 0; i < a.count(); i++) {
        const LogicalSensor &x = a.s[i], &y = b.s[i];
        if (strcmp(x.internalId, y.internalId) || strcmp(x.customId, y.customId) || strcmp(x.displayName, y.displayName) || x.radioId != y.radioId || x.minRssiDbm != y.minRssiDbm)
            return false;
    }
    return true;
}

bool lsAppend(LogicalSensorTable& t, const LogicalSensor& e)
{
    const int n = t.count();
    if (n >= LS_MAX || !lsIsInternalId(e.internalId) || lsCustomIdProblem(e.customId) || lsFindByInternalId(t, e.internalId) >= 0 || lsFindByCustomId(t, e.customId) >= 0 ||
        (e.radioId && lsFindByRadioId(t, e.radioId) >= 0))
        return false;
    t.s[n] = e;
    rfNameClean(t.s[n].displayName);
    return true;
}

bool lsUnpack(LogicalSensorTable& t, const uint8_t* in, size_t len)
{
    t = LogicalSensorTable();
    if (!in || len < 2 || in[0] != LS_BLOB_VERSION || in[1] > LS_MAX || len != 2 + LS_REC_SIZE * in[1]) return false;
    const uint8_t* p = in + 2;
    for (int i = 0; i < in[1]; i++) {
        LogicalSensor e;
        memcpy(e.internalId, p, sizeof e.internalId);   p += sizeof e.internalId;
        memcpy(e.customId, p, sizeof e.customId);   p += sizeof e.customId;
        memcpy(e.displayName, p, sizeof e.displayName); p += sizeof e.displayName;
        e.internalId[sizeof e.internalId - 1] = e.customId[sizeof e.customId - 1] = e.displayName[sizeof e.displayName - 1] = '\0';   // never trust stored strings
        e.radioId = 0;
        for (int b = 0; b < 4; b++) e.radioId |= (uint32_t)*p++ << (8 * b);
        e.minRssiDbm = (int8_t)*p++;
        lsAppend(t, e);
    }
    return true;
}

// ---- placeholders ----

namespace {

// sensor part of a placeholder name: [s, e), field after it; false = no '_'
bool splitName(const char* name, const char*& sensorEnd, const char** field)
{
    const char* u = strrchr(name, '_');
    if (!u || u == name) return false;
    sensorEnd = u;
    *field = u + 1;
    return true;
}

// "rf" + digits / 8 hex: index of a ★ entry (slot = ★ number, radio ID = bound one)
int legacyIndex(const LogicalSensorTable& t, const char* s, size_t n)
{
    if (n < 3 || strncmp(s, "rf", 2)) return -1;
    bool digits = true, hex = true;
    for (size_t i = 2; i < n; i++) { digits &= s[i] >= '0' && s[i] <= '9'; hex &= isHex(s[i]); }
    if (n == 10 && hex) {
        char h[9]; memcpy(h, s + 2, 8); h[8] = '\0';
        return lsFindByRadioId(t, (uint32_t)strtoul(h, nullptr, 16));
    }
    if (digits && n <= 4) {
        const int slot = atoi(s + 2) - 1;
        return slot >= 0 && slot < t.count() ? slot : -1;
    }
    return -1;
}

typedef bool (*PhRewrite)(const LogicalSensorTable& t, const char* name, char* out, size_t len);

// copies in -> out, replacing the name of every "{name}" / "{name:sfx}" that rw() rewrites
bool rewrite(const LogicalSensorTable& t, const char* in, char* out, size_t len, PhRewrite rw)
{
    if (!len) return false;
    size_t n = 0;
    bool fit = true;
    auto put = [&](const char* s, size_t k) {
        for (size_t i = 0; i < k; i++) { if (n + 1 < len) out[n++] = s[i]; else fit = false; }
    };
    for (const char* s = in ? in : ""; *s; ) {
        const char* close = *s == '{' ? strchr(s, '}') : nullptr;
        if (!close || close - s > 40) { put(s, 1); s++; continue; }
        const char* colon = (const char*)memchr(s, ':', (size_t)(close - s));
        const char* nameEnd = colon ? colon : close;
        char name[41];
        const size_t k = (size_t)(nameEnd - s - 1);
        memcpy(name, s + 1, k);
        name[k] = '\0';
        char repl[48];
        if (rw(t, name, repl, sizeof repl)) {
            put("{", 1);
            put(repl, strlen(repl));
            put(nameEnd, (size_t)(close - nameEnd + 1));
        } else {
            put(s, (size_t)(close - s + 1));
        }
        s = close + 1;
    }
    out[n] = '\0';
    return fit;
}

bool toStored(const LogicalSensorTable& t, const char* name, char* out, size_t len)
{
    const char* field = nullptr;
    if (name[0] == '@') return false;                           // stored already
    const int i = lsOfPlaceholder(t, name, &field);
    if (i < 0) return false;
    snprintf(out, len, "@%s_%s", t.s[i].internalId, field);
    return true;
}

bool toCustomId(const LogicalSensorTable& t, const char* name, char* out, size_t len)
{
    const char* field = nullptr;
    if (name[0] != '@') return false;
    const int i = lsOfPlaceholder(t, name, &field);
    if (i < 0) return false;
    snprintf(out, len, "%s_%s", t.s[i].customId, field);
    return true;
}

} // namespace

int lsOfPlaceholder(const LogicalSensorTable& t, const char* name, const char** field)
{
    *field = nullptr;
    const char* end = nullptr;
    const char* f = nullptr;
    if (!name || !splitName(name, end, &f) || !*f) return -1;
    const size_t n = (size_t)(end - name);
    if (name[0] == '@') {
        if (n != LS_INTERNAL_ID_LEN + 1) return -1;
        char internalId[LS_INTERNAL_ID_LEN + 1];
        memcpy(internalId, name + 1, LS_INTERNAL_ID_LEN);
        internalId[LS_INTERNAL_ID_LEN] = '\0';
        const int i = lsFindByInternalId(t, internalId);
        if (i >= 0) *field = f;
        return i;
    }
    int i = legacyIndex(t, name, n);
    if (i < 0 && n <= LS_CUSTOM_ID_MAX) {
        char customId[LS_CUSTOM_ID_MAX + 1];
        memcpy(customId, name, n);
        customId[n] = '\0';
        i = lsFindByCustomId(t, customId);
    }
    if (i >= 0) *field = f;
    return i;
}

bool lsPlaceholdersToStored(const LogicalSensorTable& t, const char* in, char* out, size_t len) { return rewrite(t, in, out, len, toStored); }
bool lsPlaceholdersToCustomId(const LogicalSensorTable& t, const char* in, char* out, size_t len)    { return rewrite(t, in, out, len, toCustomId); }

// ---- locked copies (see lsLock() in lsensors.h) ----
bool lsCopyByRadioId(uint32_t radioId, LogicalSensor& out)
{
    LsGuard g;
    const int i = lsFindByRadioId(gLogicalSensors, radioId);
    out = i >= 0 ? gLogicalSensors.s[i] : LogicalSensor();
    return i >= 0;
}

bool lsCopyByInternalId(const char* internalId, LogicalSensor& out)
{
    LsGuard g;
    const int i = lsFindByInternalId(gLogicalSensors, internalId);
    out = i >= 0 ? gLogicalSensors.s[i] : LogicalSensor();
    return i >= 0;
}

int lsMineIds(uint32_t (&out)[LS_MAX])
{
    int n = 0;
    LsGuard g;
    for (int i = 0; i < LS_MAX && gLogicalSensors.s[i].internalId[0]; i++) if (gLogicalSensors.s[i].radioId) out[n++] = gLogicalSensors.s[i].radioId;
    return n;
}

std::vector<uint32_t> lsMineIds()
{
    uint32_t ids[LS_MAX];
    const int n = lsMineIds(ids);   // the vector is allocated after the lock
    return std::vector<uint32_t>(ids, ids + n);
}

std::vector<LogicalSensor> lsSnapshot()
{
    std::vector<LogicalSensor> v;
    v.reserve(LS_MAX);
    LsGuard g;
    for (int i = 0; i < LS_MAX && gLogicalSensors.s[i].internalId[0]; i++) v.push_back(gLogicalSensors.s[i]);
    return v;
}

int lsMineCount()
{
    LsGuard g;
    return gLogicalSensors.count();
}
