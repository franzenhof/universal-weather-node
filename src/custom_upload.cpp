#include "custom_upload.h"
#include "serial_log.h"
#include "storage_layout.h"
#include "storage_write.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

namespace {
CustomUpload      ups[CUSTOM_UPLOADS];
SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
}

void customUploadSetup()
{
    Preferences prefs;
    if (!prefs.begin("cupload", true, "cfgdata")) return;
    // key "cfg": [version][CustomUpload x CUSTOM_UPLOADS] (storage_layout.h)
    CustomUpload tmp[CUSTOM_UPLOADS];
    constexpr size_t BLOB = 1 + sizeof(tmp);
    static_assert(BLOB <= STORED_BLOB_READ_MAX, "cupload beyond the loaders' read ceiling");
    // the stored blob (also longer than BLOB: an older layout for the hook) + the migration scratch, only at start
    const size_t n = prefs.isKey("cfg") ? prefs.getBytesLength("cfg") : 0;
    uint8_t* blob = storedBlobLengthReadable(n) ? (uint8_t*)malloc(n) : nullptr;
    uint8_t* scratch = blob ? (uint8_t*)malloc(BLOB) : nullptr;
    const uint8_t* p = scratch && prefs.getBytes("cfg", blob, n) == n
        ? storedBlobLoad(SF_CUSTOM_UPLOADS, blob, n, STORED_CUSTOM_UPLOADS_VERSION, sizeof(tmp), scratch, BLOB)
        : nullptr;
    const bool loaded = p != nullptr;
    if (loaded) memcpy(tmp, p, sizeof(tmp));
    free(scratch);
    free(blob);
    if (loaded) {
        for (CustomUpload& u : tmp) {
            u.url[sizeof(u.url) - 1] = u.body[sizeof(u.body) - 1] = '\0';
            if (u.intervalSec < 60 || u.intervalSec > 3600) u.intervalSec = 300;
        }
        xSemaphoreTake(mtx, portMAX_DELAY);
        memcpy(ups, tmp, sizeof(ups));
        xSemaphoreGive(mtx);
    }
    prefs.end();
}

void customUploadLock()   { xSemaphoreTake(mtx, portMAX_DELAY); }
void customUploadUnlock() { xSemaphoreGive(mtx); }
CustomUpload* customUploads() { return ups; }

void customUploadSave()
{
    StorageWrite w("cupload");   // storage_write.h: refused once a reset sealed the storage
    if (!w.ok()) return;
    Preferences& prefs = w.prefs();
    constexpr size_t BLOB = 1 + sizeof(ups);
    uint8_t* blob = (uint8_t*)malloc(BLOB);
    if (blob) {
        blob[0] = STORED_CUSTOM_UPLOADS_VERSION;
        memcpy(blob + 1, ups, sizeof(ups));
    }
    if (!blob || prefs.putBytes("cfg", blob, BLOB) != BLOB) dbgSerial.println("Custom upload: NVS write failed.");
    free(blob);
}

void customUploadsConfigured(bool* on)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < CUSTOM_UPLOADS; i++) on[i] = ups[i].url[0] != '\0';
    xSemaphoreGive(mtx);
}
