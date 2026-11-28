// Reset history and crash report, device part (reset_history.h).
#include "reset_history.h"
#include "storage_layout.h"
#include "storage_write.h"
#include "psram.h"
#include "serial_log.h"
#include "time_sync.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_flash.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#if __has_include("generated_version.h")
#include "generated_version.h"
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// The crash report reads the core dump's summary: ESP-IDF core dump to the
// "coredump" partition in ELF format (on in the Arduino defaults).
#if !(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
#error "reset_history_esp.cpp needs CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH and CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF"
#endif

// Survives every reset except a power loss (then its check fails). No fixed
// address: a firmware with another RTC layout reads it as invalid once.
RTC_NOINIT_ATTR ResetHeartbeat rtcHeartbeat;

namespace {
constexpr const char* NS  = "diag";
constexpr const char* KEY = "resets";
ResetHistory*     history = nullptr;   // PSRAM; written in resetHistoryBoot() only
uint32_t          lastBeatMs = 0;
// The stored history was written by a newer firmware (a downgrade): it stays
// untouched in flash, this firmware keeps its own records in RAM only
// (storage_layout.h).
bool              newerHistoryKept = false;
volatile uint8_t  crashTestKind = 0;
volatile uint32_t crashTestAtMs = 0;

ResetHistory& historyStore()
{
    if (!history) {
        void* p = bigAlloc(sizeof(ResetHistory));   // ~1 KB
        if (!p) abort();
        history = new (p) ResetHistory();
    }
    return *history;
}

uint8_t causeFromIdf(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:    return RC_POWER_ON;
        case ESP_RST_EXT:        return RC_EXTERNAL;
        case ESP_RST_SW:         return RC_SOFTWARE;
        case ESP_RST_PANIC:      return RC_PANIC;
        case ESP_RST_INT_WDT:    return RC_INT_WATCHDOG;
        case ESP_RST_TASK_WDT:   return RC_TASK_WATCHDOG;
        case ESP_RST_WDT:        return RC_OTHER_WATCHDOG;
        case ESP_RST_DEEPSLEEP:  return RC_DEEP_SLEEP;
        case ESP_RST_BROWNOUT:   return RC_BROWNOUT;
        case ESP_RST_USB:        return RC_USB;
        case ESP_RST_JTAG:       return RC_JTAG;
        case ESP_RST_PWR_GLITCH: return RC_POWER_GLITCH;
        case ESP_RST_CPU_LOCKUP: return RC_CPU_LOCKUP;
        case ESP_RST_UNKNOWN:    return RC_UNKNOWN;
        default:                 return RC_OTHER;   // SDIO, eFuse
    }
}

// The summary of the core dump in flash - only if it is this firmware's
// (a dump that could not be written leaves an older one behind).
bool readCrashSummary(CrashSummary& c)
{
    if (esp_core_dump_image_check() != ESP_OK) return false;
    esp_core_dump_summary_t* s = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if (!s) return false;
    bool ok = esp_core_dump_get_summary(s) == ESP_OK;
    char sha[sizeof(c.elfSha)];
    const int shaBytes = esp_app_get_elf_sha256(sha, sizeof(sha));   // digits + terminator, 0 = none
    const size_t shaDigits = shaBytes > 0 ? (size_t)shaBytes - 1 : 0;
    if (ok && !crashDumpShaMatches(sha, shaDigits, s->app_elf_sha256, sizeof(s->app_elf_sha256))) {
        dbgSerial.println("Crash report: the core dump in flash is from another firmware - not used.");
        ok = false;
    }
    if (ok) {
        c = CrashSummary();
        c.valid = 1;
        c.pc = s->exc_pc;
        const uint32_t depth = s->exc_bt_info.depth;
        c.depth = (uint8_t)(depth < 8 ? depth : 8);
        for (int i = 0; i < c.depth; i++) c.backtrace[i] = s->exc_bt_info.bt[i];
        c.backtraceCorrupted = s->exc_bt_info.corrupted ? 1 : 0;
        c.exceptionCause = s->ex_info.exc_cause;
        c.exceptionAddress = s->ex_info.exc_vaddr;
        memcpy(c.task, s->exc_task, sizeof(c.task) - 1);
        c.task[sizeof(c.task) - 1] = '\0';
        char* details = (char*)malloc(512);   // the task watchdog's text is long: compacted before the cut
        if (details && esp_core_dump_get_panic_reason(details, 512) == ESP_OK) crashReasonCompact(details, c.reason, sizeof(c.reason));
        free(details);
        strlcpy(c.firmware, FIRMWARE_VERSION, sizeof(c.firmware));
        strlcpy(c.elfSha, sha, sizeof(c.elfSha));
    }
    free(s);
    return ok;
}

void load(ResetHistory& h)
{
    Preferences prefs;
    if (!prefs.begin(NS, true, "cfgdata")) return;   // never written
    static_assert(RESET_HISTORY_BLOB <= STORED_BLOB_READ_MAX, "reset history beyond the loaders' read ceiling");
    const size_t n = prefs.isKey(KEY) ? prefs.getBytesLength(KEY) : 0;
    // the stored blob (also longer than today's: an older layout for the hook) + the migration scratch, only at start
    uint8_t* b = storedBlobLengthReadable(n) ? (uint8_t*)malloc(n) : nullptr;
    uint8_t* scratch = b ? (uint8_t*)malloc(RESET_HISTORY_BLOB) : nullptr;
    if (scratch && prefs.getBytes(KEY, b, n) == n) {
        size_t len = 0;
        const uint8_t* p = storedBlobLoad(SF_RESET_HISTORY, b, n, STORED_RESET_HISTORY_VERSION, RESET_HISTORY_BLOB - 1,
                                          scratch, RESET_HISTORY_BLOB, &len);
        if (storedVersionAction(b[0], STORED_RESET_HISTORY_VERSION) == STORED_NEWER) {
            newerHistoryKept = true;
            dbgSerial.println("Reset history: stored by a newer firmware - kept untouched, this one's starts are not saved.");
        } else if (!p || !resetHistoryFromPayload(p, len, h)) {
            dbgSerial.println("Reset history: unknown layout - starting a new one.");
        }
    }
    free(scratch);
    free(b);
    prefs.end();
}

void save(const ResetHistory& h)
{
    if (newerHistoryKept) return;   // a newer firmware's history stays as it is
    uint8_t* b = (uint8_t*)malloc(RESET_HISTORY_BLOB);
    const size_t n = b ? resetHistoryPack(h, b, RESET_HISTORY_BLOB) : 0;
    bool ok = false;
    if (n) {
        StorageWrite w(NS);   // storage_write.h: refused once a reset sealed the storage
        ok = w.ok() && w.prefs().putBytes(KEY, b, n) == n;
    }
    free(b);
    if (!ok) dbgSerial.println("Reset history: NVS write failed.");
}
} // namespace

const ResetHistory& resetHistory() { return historyStore(); }

void resetHistoryBoot()
{
    ResetHistory& h = historyStore();
    load(h);
    const uint8_t cause = causeFromIdf(esp_reset_reason());
    const bool beatOk = heartbeatValid(rtcHeartbeat);
    CrashSummary crash;
    const bool crashOk = resetCauseIsCrash(cause) && readCrashSummary(crash);
    ResetRecord r = resetRecordMake(cause, beatOk ? &rtcHeartbeat : nullptr, crashOk ? &crash : nullptr);
    if (crashOk) crash.epoch = r.epoch;
    resetHistoryPush(h, r, crashOk ? &crash : nullptr);
    save(h);
    heartbeatStart(rtcHeartbeat);
    size_t dumpAddr, dumpSize;
    resetHistoryCoreDump(dumpAddr, dumpSize);   // checked here once, before the web tasks ask
    dbgSerial.printf("Reset history: start %lu, cause %s%s%s, ran %s%lu s before.\n", (unsigned long)h.starts,
                     resetCauseKey(cause), r.detail[0] ? " - " : "", r.detail, beatOk ? "" : "? (unknown) ",
                     (unsigned long)r.uptimeSec);
    if (crashOk) {
        dbgSerial.printf("Crash report: task %s, PC 0x%08lx, backtrace", crash.task, (unsigned long)crash.pc);
        for (int i = 0; i < crash.depth; i++) dbgSerial.printf(" 0x%08lx", (unsigned long)crash.backtrace[i]);
        dbgSerial.printf("%s (decode with firmware.elf, ELF SHA %s).\n", crash.backtraceCorrupted ? " (corrupted)" : "", crash.elfSha);
    }
}

namespace {
void beatNow()
{
    int64_t epoch = 0;
    if (!epochNow(epoch) || epoch < 0 || epoch > 0xFFFFFFFFLL) epoch = 0;
    heartbeatUpdate(rtcHeartbeat, (uint32_t)(esp_timer_get_time() / 1000000LL), (uint32_t)epoch);
}
} // namespace

void resetHistoryLoop()
{
    const uint32_t now = millis();
    if (now - lastBeatMs < 1000) return;
    lastBeatMs = now;
    beatNow();
}

void resetHistoryNoteRestart(const char* reason)
{
    beatNow();   // the uptime of this very moment
    heartbeatNoteRestart(rtcHeartbeat, reason);
}

bool resetHistoryCoreDump(size_t& addr, size_t& size)
{
    // checked once (a CRC over up to 128 KB of flash): the dump only changes with a crash, i.e. a restart
    static int8_t state = -1;   // -1 unknown, 0 none, 1 valid
    static size_t dumpAddr = 0, dumpSize = 0;
    if (state < 0)
        state = esp_core_dump_image_check() == ESP_OK && esp_core_dump_image_get(&dumpAddr, &dumpSize) == ESP_OK && dumpSize > 0;
    addr = dumpAddr;
    size = dumpSize;
    return state == 1;
}

bool resetHistoryCoreDumpRead(size_t addr, uint8_t* buf, size_t n)
{
    return esp_flash_read(nullptr, buf, addr, n) == ESP_OK;
}

void crashTestRequest(CrashTestKind kind)
{
    crashTestAtMs = millis() + 1000;
    crashTestKind = (uint8_t)kind;
}

void crashTestPoll()
{
    const uint8_t kind = crashTestKind;
    if (!kind || (int32_t)(millis() - crashTestAtMs) < 0) return;
    if (kind == CRASH_TEST_WATCHDOG) {
        dbgSerial.println("Crash test: the main loop stops here - the task watchdog resets the node in 120 s.");
        volatile uint32_t spin = 0;
        for (;;) { spin = spin + 1; }   // spins without feeding the watchdog
    }
    dbgSerial.println("Crash test: writing to address 0 (LoadStore / StoreProhibited panic).");
    delay(100);
    volatile uint32_t* volatile nowhere = nullptr;
    *nowhere = 0xDEAD;
}
