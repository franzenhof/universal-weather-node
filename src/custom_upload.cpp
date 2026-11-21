#include "custom_upload.h"
#include "serial_log.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

namespace {
CustomUpload      ups[CUSTOM_UPLOADS];
SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
}

void customUploadSetup()
{
    Preferences prefs;
    if (!prefs.begin("cupload", true, "cfgdata")) return;
    CustomUpload tmp[CUSTOM_UPLOADS];
    if (prefs.getBytesLength("v1") == sizeof(tmp) && prefs.getBytes("v1", tmp, sizeof(tmp)) == sizeof(tmp)) {
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
    Preferences prefs;
    if (!prefs.begin("cupload", false, "cfgdata")) return;
    if (prefs.putBytes("v1", ups, sizeof(ups)) != sizeof(ups)) dbgSerial.println("Custom upload: NVS write failed.");
    prefs.end();
}

void customUploadsConfigured(bool* on)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < CUSTOM_UPLOADS; i++) on[i] = ups[i].url[0] != '\0';
    xSemaphoreGive(mtx);
}
