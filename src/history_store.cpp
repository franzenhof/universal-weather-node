// Keeps the dashboard history (history.h) across restarts, in the cfgdata
// NVS partition next to the settings - which always come first: a blob is
// written only if at least RESERVE_BYTES stay free afterwards.
//   "coarse": 7-day tier (1-h values, ~5.4 KB), every 24 h and before a
//             planned restart.
//   "fine":   24-h tier (5-min values, ~9.2 KB), only before a planned
//             restart, as a hand-over: removed again once restored.
// Restored after the start as soon as the clock is synced (the blobs carry
// wall-clock times); nothing is saved before that, so a start without a
// clock never overwrites the saved history.
#include "history.h"
#include "serial_log.h"
#include "time_sync.h"
#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>
#include <stdlib.h>

namespace {
constexpr const char* NS = "history";
constexpr size_t RESERVE_BYTES = 16 * 1024;
constexpr unsigned long SAVE_EVERY_MS = 24UL * 3600UL * 1000UL;
bool restored = false;
volatile bool clearReq = false;   // historyClearRequest() (web task) -> historyStoreCycle() (loop)
unsigned long lastSaveMs = 0;

size_t nvsFreeBytes()
{
    nvs_stats_t st;
    if (nvs_get_stats("cfgdata", &st) != ESP_OK) return 0;
    return st.free_entries * 32;
}

// NVS stores a blob in 32-byte entries plus a header per chunk (one per page).
size_t nvsBytesFor(size_t n) { return (n + 31) / 32 * 32 + 4 * 32; }

bool save(bool coarse)
{
    const size_t cap = historyBlobMaxSize(coarse);
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) return false;
    bool ok = false;
    const size_t n = historyExport(coarse, buf, cap);
    const char* key = coarse ? "coarse" : "fine";
    if (n) {
        const size_t freeB = nvsFreeBytes();
        if (freeB < nvsBytesFor(n) + RESERVE_BYTES) {
            dbgSerial.printf("History: not saved (%s, %u B) - settings storage nearly full (%u B free).\n",
                             key, (unsigned)n, (unsigned)freeB);
        } else {
            Preferences p;
            if (p.begin(NS, false, "cfgdata")) {
                ok = p.putBytes(key, buf, n) == n;
                p.end();
            }
            dbgSerial.printf("History: %s values %s (%u B).\n", key, ok ? "saved" : "NOT saved (NVS write failed)", (unsigned)n);
        }
    }
    free(buf);
    return ok;
}

int restoreKey(Preferences& p, bool coarse)
{
    const char* key = coarse ? "coarse" : "fine";
    const size_t n = p.getBytesLength(key);
    if (!n || n > historyBlobMaxSize(coarse)) return 0;
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) return 0;
    int r = p.getBytes(key, buf, n) == n ? historyImport(coarse, buf, n) : -1;
    free(buf);
    return r;
}
} // namespace

void historyClearRequest() { clearReq = true; }

void historyStoreCycle()
{
    if (clearReq) {
        clearReq = false;
        historyClear();
        Preferences p;
        if (p.begin(NS, false, "cfgdata")) { p.clear(); p.end(); }
        restored = true;           // nothing left to restore
        lastSaveMs = millis();
        dbgSerial.println("History: deleted (settings).");
        return;
    }
    if (!restored) {
        int64_t epoch;
        if (!epochNow(epoch)) return;   // windows need the wall clock
        restored = true;
        lastSaveMs = millis();
        Preferences p;
        if (!p.begin(NS, false, "cfgdata")) return;
        const int c = restoreKey(p, true);
        const int f = restoreKey(p, false);
        if (p.isKey("fine")) p.remove("fine");   // hand-over only
        p.end();
        if (c || f) dbgSerial.printf("History: restored %d hourly and %d 5-min values.\n", c < 0 ? 0 : c, f < 0 ? 0 : f);
        return;
    }
    if (millis() - lastSaveMs >= SAVE_EVERY_MS) {
        lastSaveMs = millis();
        save(true);
    }
}

void historyStoreFlush()
{
    if (!restored) return;
    save(true);
    save(false);
}
