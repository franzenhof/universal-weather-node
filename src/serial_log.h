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
// Written from several tasks (the main loop, the web server's handlers,
// the upload task, ...) and read/reset by the /api/log handlers - so the
// ring is mutex-protected: a snapshot()/clear() racing a write() could hand
// back more than just a stale byte. Each write and each println(text) goes
// to the ring and the port as one step under the mutex: a line from another
// task never lands between a text and its line end, and the port shows the
// lines in the ring's order. Never called from an ISR (the button ISR only
// touches its own volatile variables, see main.cpp), so a plain mutex is safe.
class SerialLog : public Print
{
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    // text and line end in one step (Print's println is two writes)
    using Print::println;
    size_t println(const char* s);
    size_t println(const String& s);

    // Copies the buffered log (oldest byte first) into out as a NUL-terminated
    // string. Returns the number of bytes written, excluding the terminator.
    size_t snapshot(char* out, size_t outSize) const;

    void clear();

private:
    size_t writeLine(const uint8_t* a, size_t an, const uint8_t* b, size_t bn);
    SemaphoreHandle_t mutex_ = xSemaphoreCreateMutex();
};

extern SerialLog dbgSerial;

// Heltec V4: the log goes over the native USB (USB-Serial-JTAG, HWCDC) instead
// of UART0 - GPIO43/44 are 1-Wire and wind speed on the PCB (heltec_board.h
// boardUseUart0()). Call before the first log line; UART0 is then never started.
void serialLogUseUsb();
