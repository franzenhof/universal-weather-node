#include "serial_log.h"
#include <Arduino.h>

namespace {
constexpr size_t LOG_BUF_SIZE = 4096;   // the last ~50-80 lines (RAM: was 8 KB)
char   logBuf[LOG_BUF_SIZE];
size_t logHead = 0;     // next write position
bool   logFull = false; // true once the buffer has wrapped at least once
}

SerialLog dbgSerial;

#if ARDUINO_USB_MODE && !ARDUINO_USB_CDC_ON_BOOT
#include <HWCDC.h>
static Print* logOut = &Serial;   // UART0 (CP2102 on the V3) unless serialLogUseUsb()
void serialLogUseUsb()
{
    USBSerial.setTxTimeoutMs(0);   // no USB host: drop, never block the loop
    USBSerial.begin(115200);
    logOut = &USBSerial;
}
#else
static Print* logOut = &Serial;
void serialLogUseUsb() {}
#endif

size_t SerialLog::write(uint8_t c)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    logBuf[logHead] = (char)c;
    logHead = (logHead + 1) % LOG_BUF_SIZE;
    if (logHead == 0) logFull = true;
    xSemaphoreGive(mutex_);
    return logOut->write(c);
}

size_t SerialLog::write(const uint8_t* buffer, size_t size)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (size_t i = 0; i < size; i++) {
        logBuf[logHead] = (char)buffer[i];
        logHead = (logHead + 1) % LOG_BUF_SIZE;
        if (logHead == 0) logFull = true;
    }
    xSemaphoreGive(mutex_);
    return logOut->write(buffer, size);
}

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

void SerialLog::clear()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    logHead = 0;
    logFull = false;
    xSemaphoreGive(mutex_);
}
