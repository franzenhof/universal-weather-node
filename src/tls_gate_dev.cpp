// Device part of tls_gate.h (heap figures, log)
#include "tls_gate.h"
#include "ota_upload.h"
#include "globals.h"
#include "serial_log.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

void tlsLogHeap(const char* what, const char* when)
{
    if (!gCfg.debugMode) return;
    dbgSerial.printf("TLS %s %s: free %u B, largest %u B\n", what, when, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

TlsDecision tlsMayStart(const char* what)
{
    const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const TlsDecision d = tlsDecide(freeHeap, largest, webHeavyActive(millis()) || otaUploadActive());   // also during a firmware upload
    static uint32_t lastLogMs = 0;   // a deferred request is retried every few seconds: log at most every 30 s
    if (d != TLS_GO && (!lastLogMs || millis() - lastLogMs > 30000)) {
        lastLogMs = millis();
        dbgSerial.printf("TLS %s deferred: %s (free %u B, largest %u B)\n", what,
                         d == TLS_WAIT_WEB ? "web page busy" : "low memory", (unsigned)freeHeap, (unsigned)largest);
    }
    return d;
}
