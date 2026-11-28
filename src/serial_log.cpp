#include "serial_log.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

#include <esp_memory_utils.h>

namespace {
// The ring is allocated with the first line: the PSRAM is part of the heap
// from initArduino() on, before setup() logs anything, so normally the
// 64-KB ring in the PSRAM right away. Without PSRAM (or for a line before
// initArduino()) 4 KB of internal RAM; serialLogUsePsram() moves those.
constexpr size_t LOG_FALLBACK_SIZE = 4096;
char*  logBuf = nullptr;
size_t LOG_BUF_SIZE = 0;
size_t logHead = 0;     // next write position
bool   logFull = false; // true once the buffer has wrapped at least once

// With the mutex held. false = no memory at all: the line goes to the
// serial port only.
bool logEnsureBuffer()
{
    if (logBuf) return true;
    logBuf = (char*)heap_caps_malloc(LOG_PSRAM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (logBuf) { LOG_BUF_SIZE = LOG_PSRAM_SIZE; return true; }
    logBuf = (char*)heap_caps_malloc(LOG_FALLBACK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (logBuf) { LOG_BUF_SIZE = LOG_FALLBACK_SIZE; return true; }
    return false;
}
}

SerialLog dbgSerial;

#if ARDUINO_USB_MODE && !ARDUINO_USB_CDC_ON_BOOT
#include <HWCDC.h>
static Print* logOut = &Serial;   // UART0 (CP2102 on the V3) unless serialLogUseUsb()
// Arduino-ESP32 3.x declares no global HWCDC object without CDC_ON_BOOT
static HWCDC usbSerial;
void serialLogUseUsb()
{
    usbSerial.setTxTimeoutMs(0);   // no USB host: drop, never block the loop
    usbSerial.begin(115200);
    logOut = &usbSerial;
}
#else
static Print* logOut = &Serial;
void serialLogUseUsb() {}
#endif

// With the mutex held. The bytes go to the serial port only when there is
// no memory for the ring at all.
static void ringAppend(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n && logEnsureBuffer(); i++) {
        logBuf[logHead] = (char)p[i];
        logHead = (logHead + 1) % LOG_BUF_SIZE;
        if (logHead == 0) logFull = true;
    }
}

// a and b (may be empty) to the ring and the port as one step
size_t SerialLog::writeLine(const uint8_t* a, size_t an, const uint8_t* b, size_t bn)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ringAppend(a, an);
    ringAppend(b, bn);
    size_t n = logOut->write(a, an);   // never blocks (USB: TX timeout 0)
    if (bn) n += logOut->write(b, bn);
    xSemaphoreGive(mutex_);
    return n;
}

size_t SerialLog::write(uint8_t c) { return writeLine(&c, 1, nullptr, 0); }
size_t SerialLog::write(const uint8_t* buffer, size_t size) { return writeLine(buffer, size, nullptr, 0); }
size_t SerialLog::println(const char* s) { return writeLine((const uint8_t*)s, strlen(s), (const uint8_t*)"\r\n", 2); }
size_t SerialLog::println(const String& s) { return writeLine((const uint8_t*)s.c_str(), s.length(), (const uint8_t*)"\r\n", 2); }

size_t SerialLog::snapshot(char* out, size_t outSize) const
{
    if (outSize == 0) return 0;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    size_t available = logFull ? LOG_BUF_SIZE : logHead;
    size_t start      = logFull ? logHead : 0;        // oldest byte's position
    size_t toCopy      = min(available, outSize - 1);
    size_t skip        = available - toCopy;          // drop oldest bytes if out is smaller than the log

    for (size_t i = 0; i < toCopy; i++) {
        out[i] = logBuf[(start + skip + i) % LOG_BUF_SIZE];
    }
    xSemaphoreGive(mutex_);
    out[toCopy] = '\0';
    return toCopy;
}

size_t SerialLog::capacity() const { return LOG_BUF_SIZE; }

bool serialLogUsePsram(size_t bytes)
{
    xSemaphoreTake(dbgSerial.mutex(), portMAX_DELAY);
    const bool already = logBuf && esp_ptr_external_ram(logBuf) && LOG_BUF_SIZE >= bytes;
    xSemaphoreGive(dbgSerial.mutex());
    if (already) return true;
    char* big = (char*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!big) return false;
    const size_t n = dbgSerial.snapshot(big, bytes);   // the lines so far, oldest first
    xSemaphoreTake(dbgSerial.mutex(), portMAX_DELAY);
    free(logBuf);   // the internal 4-KB ring (or nullptr)
    logBuf = big;
    LOG_BUF_SIZE = bytes;
    logHead = n % bytes;
    logFull = false;
    xSemaphoreGive(dbgSerial.mutex());
    return true;
}

void SerialLog::clear()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    logHead = 0;
    logFull = false;
    xSemaphoreGive(mutex_);
}
