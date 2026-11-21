// Device part of ota_upload.h: Update, the partition table, heap, log
#include "ota_upload.h"
#include "serial_log.h"
#include <Arduino.h>
#include <Update.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include "ota_check.h"

namespace {
bool        espBegin()                            { return Update.begin(UPDATE_SIZE_UNKNOWN); }
bool        espWrite(const uint8_t* d, size_t n)  { return Update.write(const_cast<uint8_t*>(d), n) == n; }
bool        espEnd()                              { return Update.end(true); }
void        espAbort()                            { if (Update.isRunning()) Update.abort(); }
const char* espError()                            { return Update.errorString(); }
bool        espReadPt(uint8_t* b, size_t n)       { return esp_flash_read(nullptr, b, OTA_FACTORY_PT_OFFSET, n) == ESP_OK; }
size_t      espLargest()                          { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }
uint32_t    espNow()                              { return millis(); }
void        espLog(const char* tag, const char* m) { dbgSerial.printf("%s: %s\n", tag, m); }
const OtaUpdateOps ESP_OPS = { espBegin, espWrite, espEnd, espAbort, espError, espReadPt, espLargest, espNow, espLog };
}

const OtaUpdateOps& otaEspOps() { return ESP_OPS; }
