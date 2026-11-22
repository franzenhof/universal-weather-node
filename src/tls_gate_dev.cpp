// Device part of tls_gate.h (heap figures, log)
#include "tls_gate.h"
#include "ota_upload.h"
#include "globals.h"
#include "serial_log.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "psram.h"

void tlsLogHeap(const char* what, const char* when)
{
    if (!gCfg.debugMode) return;
    dbgSerial.printf("TLS %s %s: free %u B, largest %u B (internal RAM), PSRAM free %u B\n", what, when,
                     (unsigned)internalFree(), (unsigned)internalLargest(), (unsigned)psramFree());
}

TlsDecision tlsMayStart(const char* what)
{
    const size_t freeHeap = internalFree();
    const size_t largest  = internalLargest();
    const TlsDecision d = tlsDecide(freeHeap, largest, psramLargest(), webHeavyActive(millis()) || otaUploadActive());   // also during a firmware upload
    static uint32_t lastLogMs = 0;   // a deferred request is retried every few seconds: log at most every 30 s
    if (d != TLS_GO && (!lastLogMs || millis() - lastLogMs > 30000)) {
        lastLogMs = millis();
        dbgSerial.printf("TLS %s deferred: %s (free %u B, largest %u B)\n", what,
                         d == TLS_WAIT_WEB ? "web page busy" : "low memory", (unsigned)freeHeap, (unsigned)largest);
    }
    return d;
}
