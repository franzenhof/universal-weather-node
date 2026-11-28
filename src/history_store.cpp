// Keeps the dashboard history (history.h) across restarts, in its own NVS
// partition "history" (1.5 MB = 384 pages of 4 KB, partitions.csv; up to 2.x
// it shared cfgdata with the settings): a blob is written only if at least
// RESERVE_BYTES stay free afterwards.
//   "fine":   24-h tier   (5-min values) 12.7 kB, + 18.6 kB with 32 custom series
//   "coarse": 35-day tier (1-h values)   37.0 kB, + 53.9 kB
//   "daily":  2-year tier (days)         32.2 kB, + 46.9 kB
// (NVS takes blobs up to ~500 kB, spread over pages.)
// Every 30 min each tier that has changed since its last save (fine: every
// time, hourly: once an hour, daily: once a day), and all changed ones
// before a planned restart; a power cut or a crash loses at most 30 min of
// 5-min values (the open hour is rebuilt from the saved 5-min values, the
// open day from the hourly values). Wear: at most 48 x 31.2 + 24 x 90.9 +
// 79.1 kB = 3.76 MB a day with 32 series (1.53 MB without) - NVS writes
// round-robin over all pages (384 x 4032 B of entries = 1.5 MB) and a
// rewritten blob leaves only erased entries behind, so each 4-KB page is
// erased about 2.4 times a day (once without series): ~100k cycles = 110+
// years. Live data: at most 201 kB = 50 pages;
// a rewrite needs the old copy
// until the new one is complete.
// Restored after the start as soon as the clock is synced (the blobs carry
// wall-clock times); nothing is saved before that, so a start without a
// clock never overwrites the saved history. Blobs of an older format
// (before 4.0.0) are not read and get overwritten.
#include "history.h"
#include "psram.h"
#include "serial_log.h"
#include "time_sync.h"
#include "storage_layout.h"
#include "storage_write.h"
#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <stdlib.h>

namespace {
constexpr const char* NS = "history";
constexpr const char* PART = "history";   // partition label (partitions.csv)
constexpr size_t RESERVE_BYTES = 16 * 1024;
constexpr unsigned long SAVE_EVERY_MS = 30UL * 60UL * 1000UL;
bool restored = false;
volatile bool clearReq = false;   // historyClearRequest() (web task) -> historyStoreCycle() (loop)
unsigned long lastSaveMs = 0;
uint32_t savedGen[HT_COUNT] = {};   // historyGeneration() at the last save
bool savedOnce[HT_COUNT] = {};
// A tier stored by a newer firmware (a downgrade): kept untouched in flash
// until the user deletes the chart history; this firmware's values of that
// tier stay in RAM (storage_layout.h).
bool newerTierKept[HT_COUNT] = {};
const char* const KEYS[HT_COUNT] = { "fine", "coarse", "daily" };

// The partition ready as NVS: data that is not NVS (a fresh chip, another
// firmware) is erased once - it only ever holds this history.
bool partReady()
{
    static bool ready = false;
    if (ready) return true;
    esp_err_t e = nvs_flash_init_partition(PART);
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        dbgSerial.println("History: partition not usable as NVS - erased.");
        if (nvs_flash_erase_partition(PART) == ESP_OK) e = nvs_flash_init_partition(PART);
    }
    ready = e == ESP_OK;
    if (!ready) dbgSerial.printf("History: partition \"%s\" not available (%d) - history not saved.\n", PART, (int)e);
    return ready;
}

size_t nvsFreeBytes()
{
    nvs_stats_t st;
    if (!partReady() || nvs_get_stats(PART, &st) != ESP_OK) return 0;
    return st.free_entries * 32;
}

// NVS stores a blob in 32-byte entries plus a header per chunk (one per page).
size_t nvsBytesFor(size_t n) { return (n + 31) / 32 * 32 + 4 * 32; }

// A tier unchanged since its last save is skipped (true).
bool save(HistoryTier tier)
{
    if (newerTierKept[tier]) return true;   // nothing to do: the newer firmware's tier stays
    const uint32_t gen = historyGeneration(tier);
    if (savedOnce[tier] && gen == savedGen[tier]) return true;
    const size_t cap = historyBlobMaxSize(tier);
    uint8_t* buf = (uint8_t*)bigAlloc(cap);   // up to 91 kB: PSRAM
    if (!buf) return false;
    bool ok = false;
    const size_t n = historyExport(tier, buf, cap);
    const char* key = KEYS[tier];
    if (n) {
        const size_t freeB = nvsFreeBytes();
        if (freeB < nvsBytesFor(n) + RESERVE_BYTES) {
            dbgSerial.printf("History: not saved (%s, %u B) - history partition nearly full (%u B free).\n",
                             key, (unsigned)n, (unsigned)freeB);
        } else {
            if (partReady()) {
                StorageWrite w(NS, PART);   // storage_write.h: refused once a reset sealed the storage
                ok = w.ok() && w.prefs().putBytes(key, buf, n) == n;
            }
            dbgSerial.printf("History: %s values %s (%u B).\n", key, ok ? "saved" : "NOT saved (NVS write failed)", (unsigned)n);
        }
    }
    free(buf);
    if (ok) { savedGen[tier] = gen; savedOnce[tier] = true; }
    return ok;
}

int restoreKey(Preferences& p, HistoryTier tier)
{
    const char* key = KEYS[tier];
    const size_t n = p.getBytesLength(key);
    // also longer than this tier's maximum: an older layout for the hook
    if (!storedBlobLengthReadable(n, STORED_HISTORY_TIER_READ_MAX)) return 0;
    uint8_t* buf = (uint8_t*)bigAlloc(n);
    if (!buf) return 0;
    int r = -1;
    if (p.getBytes(key, buf, n) == n) {
        // "UWH" + version: an older version goes through the migration hook (storage_layout.h)
        const StoredVersionAction action = historyBlobVersionAction(buf, n);
        if (action == STORED_NEWER) {
            newerTierKept[tier] = true;
            dbgSerial.printf("History: %s values stored by a newer firmware - kept untouched, not saved by this one "
                             "(Settings -> System -> delete the chart history to save again).\n", key);
            r = 0;
        } else if (action == STORED_MIGRATE) {
            const size_t cap = historyBlobMaxSize(tier);
            uint8_t* conv = (uint8_t*)bigAlloc(cap);
            const size_t m = conv ? storageMigrateBlob(SF_HISTORY_TIER, buf[3], buf, n, conv, cap) : 0;
            r = m ? historyImport(tier, conv, m) : -1;
            free(conv);
        } else {
            r = historyImport(tier, buf, n);
        }
    }
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
        if (partReady()) {
            StorageWrite w(NS, PART);
            if (w.ok()) w.prefs().clear();
        }
        for (bool& kept : newerTierKept) kept = false;   // the user deleted them: this firmware saves again
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
        if (!partReady() || !p.begin(NS, true, PART)) return;   // reading only
        const int d = restoreKey(p, HT_DAILY);
        const int c = restoreKey(p, HT_HOURLY);
        const int f = restoreKey(p, HT_FINE);
        p.end();
        if (c || f || d)
            dbgSerial.printf("History: restored %d daily, %d hourly and %d 5-min values%s.\n", d < 0 ? 0 : d, c < 0 ? 0 : c,
                             f < 0 ? 0 : f, (c < 0 || f < 0 || d < 0) ? " (a blob was not usable and was dropped)" : "");
        return;
    }
    if (millis() - lastSaveMs >= SAVE_EVERY_MS) {
        lastSaveMs = millis();
        for (int t = 0; t < HT_COUNT; t++) save((HistoryTier)t);
    }
}

void historyStoreFlush()
{
    if (!restored) return;
    for (int t = 0; t < HT_COUNT; t++) save((HistoryTier)t);
}
