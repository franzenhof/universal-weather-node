// Records: device part (see records.h).
#include "records.h"
#include "globals.h"
#include "sources.h"
#include "sensors.h"
#include "time_sync.h"
#include "wx_upload.h"
#include "serial_log.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>

namespace {
Records       rec;
portMUX_TYPE  mux = portMUX_INITIALIZER_UNLOCKED;
bool          dirty = false;
unsigned long lastSaveMs = 0;
volatile int  resetReq = 0;   // 1 = periods, 2 = all (from the web / downlink task)
constexpr unsigned long SAVE_EVERY_MS = 3600000UL;

void save()
{
    uint8_t blob[RECORDS_BLOB_SIZE];
    portENTER_CRITICAL(&mux);
    recordsEncode(rec, blob);
    dirty = false;
    portEXIT_CRITICAL(&mux);
    Preferences prefs;
    if (!prefs.begin("records", false, "cfgdata")) return;
    if (prefs.putBytes("v1", blob, sizeof(blob)) != sizeof(blob)) dbgSerial.println("Records: NVS write failed.");
    prefs.end();
    lastSaveMs = millis();
}
} // namespace

void recordsSetup()
{
    Preferences prefs;
    if (!prefs.begin("records", true, "cfgdata")) return;   // never written
    uint8_t blob[RECORDS_BLOB_SIZE];
    size_t n = prefs.getBytesLength("v1");
    Records r;
    if (n == sizeof(blob) && prefs.getBytes("v1", blob, n) == n && recordsDecode(blob, n, r)) {
        portENTER_CRITICAL(&mux);
        rec = r;
        portEXIT_CRITICAL(&mux);
    }
    prefs.end();
    lastSaveMs = millis();
}

void recordsResetRequest(bool allTime) { resetReq = allTime ? 2 : 1; }

void recordsCycle()
{
    if (resetReq) {
        portENTER_CRITICAL(&mux);
        recordsReset(rec, resetReq == 2);
        resetReq = 0;
        dirty = true;
        portEXIT_CRITICAL(&mux);
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
