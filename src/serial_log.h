#pragma once

#include <Print.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Tiny ring-buffer "tee": every write is mirrored to the real hardware
// Serial (so the USB/UART monitor keeps working exactly as before) and also
// appended to a fixed-size ring buffer, so the web UI can show a rolling
// view of what this device's own code has logged. Library-internal prints
// (WiFiManager, RadioLib, ...) aren't included here — they call the real
// Serial object directly, not through this wrapper. Our own code uses
// dbgSerial instead of Serial for exactly this reason.
//
// Written from both the main loop task (virtually all of our own logging)
// and the web-server task on core 0 (webApiTask's own start/stop messages),
// and read/reset from the web task's /api/log/raw and /api/log/clear
// handlers — mutex-protected internally for exactly the same reason as
// history.cpp's ring buffer: a snapshot()/clear() racing a concurrent
// write() could hand back more than just a stale byte, since logHead/logFull
// are read mid-update. Never called from an ISR (the button ISR only
// touches its own volatile variables, see main.cpp), so a plain
// (non-FromISR) mutex is safe to use here.
class SerialLog : public Print
{
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buffer, size_t size) override;

    // Copies the buffered log (oldest byte first) into out as a NUL-terminated
    // string. Returns the number of bytes written, excluding the terminator.
    size_t snapshot(char* out, size_t outSize) const;

    void clear();

private:
    SemaphoreHandle_t mutex_ = xSemaphoreCreateMutex();
};

extern SerialLog dbgSerial;
