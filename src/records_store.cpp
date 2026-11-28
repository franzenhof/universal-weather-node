// Records: device part (see records.h).
#include "records.h"
#include "globals.h"
#include "sources.h"
#include "sensors.h"
#include "time_sync.h"
#include "wx_upload.h"
#include "serial_log.h"
#include "storage_layout.h"
#include "storage_write.h"
#include <Preferences.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>

namespace {
Records       rec;
portMUX_TYPE  mux = portMUX_INITIALIZER_UNLOCKED;
bool          dirty = false;
unsigned long lastSaveMs = 0;
volatile int  resetReq = 0;   // 1 = periods, 2 = all (from the web / downlink task)
// The stored records are a newer firmware's (a downgrade): kept untouched in
// flash until the user resets the records; this firmware's stay in RAM.
bool          newerRecordsKept = false;
constexpr unsigned long SAVE_EVERY_MS = 3600000UL;

void save()
{
    if (newerRecordsKept) { dirty = false; lastSaveMs = millis(); return; }
    uint8_t blob[RECORDS_BLOB_SIZE];
    portENTER_CRITICAL(&mux);
    recordsEncode(rec, blob);
    dirty = false;
    portEXIT_CRITICAL(&mux);
    StorageWrite w("records");   // storage_write.h: refused once a reset sealed the storage
    if (!w.ok()) return;
    if (w.prefs().putBytes("v1", blob, sizeof(blob)) != sizeof(blob)) dbgSerial.println("Records: NVS write failed.");
    lastSaveMs = millis();
}
} // namespace

void recordsSetup()
{
    Preferences prefs;
    if (!prefs.begin("records", true, "cfgdata")) return;   // never written
    const size_t n = prefs.getBytesLength("v1");
    uint8_t* blob = storedBlobLengthReadable(n) ? (uint8_t*)malloc(n) : nullptr;   // any length: a newer one too
    Records r;
    if (blob && prefs.getBytes("v1", blob, n) == n) {
        if (recordsDecode(blob, n, r)) {
            portENTER_CRITICAL(&mux);
            rec = r;
            portEXIT_CRITICAL(&mux);
        } else if (recordsBlobIsNewer(blob, n)) {
            newerRecordsKept = true;
            dbgSerial.println("Records: stored by a newer firmware - kept untouched, not saved by this one "
                              "(Settings -> System -> reset all records to save again).");
        }
    }
    free(blob);
    prefs.end();
    lastSaveMs = millis();
}

void recordsResetRequest(bool allTime) { resetReq = allTime ? 2 : 1; }

void recordsCycle()
{
    if (resetReq) {
        portENTER_CRITICAL(&mux);
        const bool allTime = resetReq == 2;
        recordsReset(rec, allTime);
        resetReq = 0;
        dirty = true;
        portEXIT_CRITICAL(&mux);
        if (allTime) newerRecordsKept = false;   // the user reset all of them: the newer firmware's are replaced from now on
        save();
    }
    struct tm lt;
    int64_t now;
    if (!localTimeNow(lt) || !epochNow(now)) return;   // records need the date
    const uint32_t t = (uint32_t)now;
    WxDailyStats ds;
    wxDailyStats(ds);
    portENTER_CRITICAL(&mux);
    const bool newDay = recordsRoll(rec, lt);
    if (gSrc.tempValid)  recordsUpdate(rec, RQ_TEMP, gSrc.tempC, t);
    if (gSrc.humValid)   recordsUpdate(rec, RQ_HUM, gSrc.humidity, t);
    if (gSrc.pressValid) recordsUpdate(rec, RQ_PRESS, gSrc.pressSeaHpa, t);
    if (srcActive(SG_WIND) && windCycleValid) recordsUpdate(rec, RQ_GUST, windGustMs, t);
    if (srcActive(SG_RAIN)) {
        recordsUpdate(rec, RQ_RAINRATE, rainRateMmH, t);
        if (ds.valid) recordsDailyRain(rec, ds.rainTodayMm);
    }
    dirty = true;
    portEXIT_CRITICAL(&mux);
    if (newDay || millis() - lastSaveMs >= SAVE_EVERY_MS) save();
}

void recordsFlush()
{
    if (dirty) save();
}

bool recordsToday(Records& out)
{
    struct tm lt;
    if (!localTimeNow(lt)) return false;
    recordsSnapshot(out);
    return recordsRoll(out, lt) ? false : out.ymd != 0;   // a stale "today" (no cycle since midnight) does not count
}

void recordsSnapshot(Records& out)
{
    portENTER_CRITICAL(&mux);
    out = rec;
    portEXIT_CRITICAL(&mux);
}
