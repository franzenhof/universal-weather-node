#pragma once
// Custom series (series_def.h) on the device: the node's definitions, how
// many slots this board may use, and each slot's value for the finished
// measurement cycle.
#include "series_def.h"

// ---- the node's definitions (series_store.cpp, host-tested) ----
// SERIES_MAX slots, with the last cycle's values, in one PSRAM block
// (bigAlloc(), allocated on first use). Not part of Config: Config is copied
// whole (settings POST, portal, loadConfig()), and 32 definitions (~1.1 KB)
// would grow every copy and the internal RAM of gCfg. Persisted as the NVS
// string "series" in the settings (config.cpp: seriesLoad() at start,
// saveConfig() writes it). Writers: the settings POST (web task) only, slot
// by slot under SeriesGuard; readers on other tasks take a locked copy of the
// slot they need (seriesDefAt()) - a 34-byte struct copy is not atomic. The
// web task, the only writer, may read seriesDefs() directly.
SeriesDef* seriesDefs();               // SERIES_MAX entries; never nullptr (aborts without memory, like the history rings)
SeriesDef  seriesDefAt(int slot);      // a locked copy; an unused one for a slot out of range
// The definitions' lock (recursive): around every write of a slot, and held
// by the readers below. Never across NVS I/O or a network call.
void seriesLock();
void seriesUnlock();
class SeriesGuard {
public:
    SeriesGuard()  { seriesLock(); }
    ~SeriesGuard() { seriesUnlock(); }
    SeriesGuard(const SeriesGuard&) = delete;
    SeriesGuard& operator=(const SeriesGuard&) = delete;
};
// The stored form of all slots (seriesPack()) / all slots from it
// (seriesUnpack()), under the lock: for saveConfig() / seriesLoad().
size_t     seriesPackLocked(char* out, size_t len);
void       seriesUnpackLocked(const char* s);
void       seriesDefsChanged();        // after the settings changed them: HA discovery again (wifi_mqtt.cpp)
bool       seriesDefsChangedTake();    // true once after a change

// The finished cycle's value per slot (seriesCycle() on the device, set
// directly by the host tests): what /api/data, MQTT, InfluxDB, /metrics and
// the {series_<n>} placeholders show - the same value the history merges
// (lightning strikes: the increase since the previous cycle).
void seriesSetNow(const float v[SERIES_MAX], const bool ok[SERIES_MAX]);
bool seriesNow(int slot, float& v);          // false = slot unused or no value in the last cycle
const float* seriesNowValues();              // SERIES_MAX values / flags, for pushHistorySample()
const bool*  seriesNowOk();

// Placeholder {series_<n>} (ph_live.cpp, placeholders.h), n = slot 1-32 as
// on the settings page: "series" can be no custom ID (lsensors.cpp), so the
// name never collides with a wireless sensor's {<custom ID>_<field>}.
// Temperatures as PK_TEMP (unit suffixes c / f), pressure as PK_PRESS, the
// rest plain with the value's decimals.
struct PhValue;
int  seriesPlaceholderSlot(const char* name);   // "series_3" -> 2; -1 = not one
bool seriesPlaceholder(int slot, PhValue& out); // false = no value now

// ---- device (series.cpp) ----
// Usable slots: SERIES_MAX with a running PSRAM (Heltec V4), 4 on internal
// RAM - each series costs 3.6 KB of rings (history.cpp).
int  seriesLimit();

// Once per finished cycle, before the outputs: computes the value per slot
// (seriesNow()): none without a definition, a fresh value or (lightning) a
// previous count to compare with. Lightning strikes: the increase of the
// sensor's 8-bit counter since the previous cycle (wraps).
void seriesCycle();

// Current value of a definition (settings page): false = none right now.
bool seriesValueNow(const SeriesDef& d, float& v);
