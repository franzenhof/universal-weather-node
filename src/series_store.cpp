#include "series.h"
#include "psram.h"
#include "placeholders.h"
#include <mutex>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace {
struct Store {
    SeriesDef defs[SERIES_MAX];
    float     now[SERIES_MAX];
    bool      ok[SERIES_MAX];
};
Store* store = nullptr;
volatile bool changed = false;
std::recursive_mutex defsMutex;   // the definitions (SeriesGuard)

Store& get()
{
    if (!store) {
        void* p = bigAlloc(sizeof(Store));   // ~1.3 KB: PSRAM on the V4
        if (!p) abort();
        store = new (p) Store();
    }
    return *store;
}
} // namespace

SeriesDef* seriesDefs() { return get().defs; }

void seriesLock()   { defsMutex.lock(); }
void seriesUnlock() { defsMutex.unlock(); }

SeriesDef seriesDefAt(int slot)
{
    if (slot < 0 || slot >= SERIES_MAX) return SeriesDef();
    SeriesGuard g;
    return get().defs[slot];
}

size_t seriesPackLocked(char* out, size_t len)
{
    SeriesGuard g;
    return seriesPack(get().defs, out, len);
}

void seriesUnpackLocked(const char* s)
{
    SeriesGuard g;
    seriesUnpack(s, get().defs);
}

void seriesDefsChanged() { changed = true; }
bool seriesDefsChangedTake() { const bool c = changed; changed = false; return c; }

void seriesSetNow(const float v[SERIES_MAX], const bool ok[SERIES_MAX])
{
    Store& s = get();
    for (int i = 0; i < SERIES_MAX; i++) { s.now[i] = v[i]; s.ok[i] = ok[i]; }
}

bool seriesNow(int slot, float& v)
{
    if (slot < 0 || slot >= SERIES_MAX) return false;
    Store& s = get();
    if (!s.ok[slot] || !seriesDefAt(slot).used()) return false;
    v = s.now[slot];
    return true;
}

const float* seriesNowValues() { return get().now; }
const bool*  seriesNowOk()     { return get().ok; }

int seriesPlaceholderSlot(const char* n)
{
    if (!n || strncmp(n, "series_", 7) || n[7] < '1' || n[7] > '9') return -1;
    char* end;
    const long k = strtol(n + 7, &end, 10);
    return (*end || k < 1 || k > SERIES_MAX) ? -1 : (int)k - 1;
}

bool seriesPlaceholder(int slot, PhValue& o)
{
    float v;
    if (!seriesNow(slot, v)) return false;
    const uint8_t field = seriesDefAt(slot).field;
    if (field >= SF_COUNT) return false;   // changed to unused meanwhile
    const SeriesFieldInfo& fi = SERIES_FIELDS[field];
    o.kind = fi.isTemp ? PK_TEMP : field == SF_PRESS ? PK_PRESS : PK_PLAIN;
    o.v = v;
    o.decimals = o.kind == PK_PLAIN ? (int8_t)fi.decimals : (int8_t)-1;
    return true;
}
