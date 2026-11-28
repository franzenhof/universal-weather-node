#include "psram.h"
#include "psram_json.h"
#include "serial_log.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_rom_uart.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---- early console capture --------------------------------------------
// The PSRAM driver reports a failure at the start ("PSRAM ID read error ...")
// before setup(), through the ROM printf (and ESP_LOGx) to UART0. A V4 logs
// over its native USB, so that line was never visible. From the global
// constructors until psramLogStatus() the console output is also copied into
// a small buffer; psramLogStatus() repeats the PSRAM lines in our log.
// Everything is still sent to UART0 as before.
namespace {

constexpr size_t EARLY_BUF = 1024;
DRAM_ATTR char   earlyBuf[EARLY_BUF];
DRAM_ATTR volatile size_t earlyLen = 0;
DRAM_ATTR volatile bool   earlyOn  = false;
vprintf_like_t   prevVprintf = nullptr;

void IRAM_ATTR earlyAppend(char c)
{
    if (earlyOn && earlyLen < EARLY_BUF - 1) earlyBuf[earlyLen++] = c;
}

void IRAM_ATTR earlyPutc(char c)
{
    earlyAppend(c);
    esp_rom_uart_putc(c);
}

int earlyVprintf(const char* fmt, va_list ap)
{
    char line[160];
    va_list cp;
    va_copy(cp, ap);
    const int n = vsnprintf(line, sizeof(line), fmt, cp);
    va_end(cp);
    for (int i = 0; i < n && i < (int)sizeof(line) - 1; i++) earlyAppend(line[i]);
    return prevVprintf ? prevVprintf(fmt, ap) : vprintf(fmt, ap);
}

// Runs with the other global constructors - before initArduino() tries
// the PSRAM. The serial drivers later install their own putc (and stop
// this copy); psramLogStatus() stops it at the latest.
__attribute__((constructor)) void earlyCaptureStart()
{
    earlyOn = true;
    esp_rom_install_channel_putc(1, earlyPutc);
    prevVprintf = esp_log_set_vprintf(earlyVprintf);
}

bool lineIsAboutPsram(const char* s, size_t n)
{
    static const char* const KEYS[] = { "PSRAM", "psram", "SPI RAM", "SPI SRAM", "spiram" };
    for (const char* k : KEYS) {
        const size_t kl = strlen(k);
        for (size_t i = 0; i + kl <= n; i++)
            if (!memcmp(s + i, k, kl)) return true;
    }
    return false;
}

} // namespace

// ---- figures --------------------------------------------------------------

bool   psramActive()  { return psramFound(); }
size_t psramTotal()   { return psramActive() ? heap_caps_get_total_size(MALLOC_CAP_SPIRAM) : 0; }
size_t psramFree()    { return psramActive() ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0; }
size_t psramLargest() { return psramActive() ? heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) : 0; }

size_t internalFree()    { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
size_t internalMinFree() { return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
size_t internalLargest() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

void* bigAlloc(size_t bytes)
{
    void* p = psramActive() ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
    return p ? p : heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

namespace {
struct PsramFirstJsonAllocator : ArduinoJson::Allocator {
    static constexpr uint32_t PSRAM_CAPS    = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    static constexpr uint32_t INTERNAL_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    void* allocate(size_t n) override { return heap_caps_malloc_prefer(n, 2, PSRAM_CAPS, INTERNAL_CAPS); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc_prefer(p, n, 2, PSRAM_CAPS, INTERNAL_CAPS); }
};
} // namespace

ArduinoJson::Allocator* psramJsonAllocator()
{
    static PsramFirstJsonAllocator allocator;
    return &allocator;
}

size_t bigAllocLargest()
{
    const size_t i = internalLargest(), p = psramLargest();
    return p > i ? p : i;
}

// ---- log / status ---------------------------------------------------------

void psramLogStatus(int efuseCap)
{
    // stop the copy, the normal log output takes over
    earlyOn = false;
    if (prevVprintf) { esp_log_set_vprintf(prevVprintf); prevVprintf = nullptr; }

    const bool chipHasPsram = efuseCap > 0;
    if (psramActive()) {
        dbgSerial.printf("PSRAM: %u KiB, %u KiB free (large buffers, TLS and Wi-Fi use it)\n",
                         (unsigned)(psramTotal() / 1024), (unsigned)(psramFree() / 1024));
    } else if (chipHasPsram) {
        dbgSerial.printf("PSRAM: the chip has %s (eFuse), but it did not start - running without it\n",
                         efuseCap == 1 ? "8 MB" : "2 MB");
    } else {
        dbgSerial.println("PSRAM: none");
    }

    // the driver's own lines from the start - only where they explain something
    // (a V3 always has the "PSRAM ID read error" of the failed probe)
    if (!chipHasPsram || psramActive()) return;
    size_t start = 0;
    const size_t n = earlyLen;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || earlyBuf[i] == '\n' || earlyBuf[i] == '\r') {
            if (i > start && lineIsAboutPsram(earlyBuf + start, i - start))
                dbgSerial.printf("  start: %.*s\n", (int)(i - start), earlyBuf + start);
            start = i + 1;
        }
    }
}
