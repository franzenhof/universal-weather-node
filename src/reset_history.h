#pragma once
// Reset history and crash report (B11): why the node restarted the last
// RESET_HISTORY_LEN times, how long it had run, when - and for a crash, where.
//
// How the facts are gathered (device part: reset_history_esp.cpp):
//  - the cause: esp_reset_reason() at the start;
//  - uptime and wall time before the reset: a heartbeat in RTC RAM
//    (RTC_NOINIT, survives every reset but a power loss), refreshed by the
//    main loop once a second - so after a watchdog reset it says when the
//    loop last ran; systemRestart() adds its reason ("settings saved");
//  - for a crash (panic, watchdog): the summary of the ESP-IDF core dump the
//    panic handler wrote to the "coredump" partition (task, PC, backtrace,
//    panic reason - an assert names file, line and function). The full dump
//    stays there until the next crash: admin download GET /api/diag/coredump,
//    decoded with the build's firmware.elf (docs/design/storage-layout.md).
// Kept in NVS (cfgdata, namespace "diag", blob "resets", version byte:
// storage_layout.h), one write per start. Read-only after setup(): the web
// task and /api/info read it without a lock.
//
// This part has no Arduino / ESP-IDF dependencies (host-tested:
// test/firmware_resethistory.cpp).
#include <stddef.h>
#include <stdint.h>

constexpr int RESET_HISTORY_LEN = 10;

// Causes as the node reports them (API keys: resetCauseKey()).
enum ResetCause : uint8_t {
    RC_UNKNOWN = 0, RC_POWER_ON, RC_EXTERNAL, RC_SOFTWARE, RC_PANIC, RC_INT_WATCHDOG, RC_TASK_WATCHDOG,
    RC_OTHER_WATCHDOG, RC_DEEP_SLEEP, RC_BROWNOUT, RC_USB, RC_JTAG, RC_POWER_GLITCH, RC_CPU_LOCKUP, RC_OTHER,
    RC_COUNT
};
const char* resetCauseKey(uint8_t cause);   // "powerOn", "software", "panic", "taskWatchdog", ... ("unknown")
bool        resetCauseIsCrash(uint8_t cause);   // panic, a watchdog, CPU lock-up: a core dump is expected

enum : uint8_t {
    RR_UPTIME_KNOWN = 1,   // uptimeSec is from the heartbeat
    RR_TIME_KNOWN   = 2,   // epoch is from the heartbeat (the clock was set)
    RR_CRASH        = 4,   // the history's lastCrash belongs to this reset
};

struct ResetRecord {
    uint8_t  cause = RC_UNKNOWN;
    uint8_t  flags = 0;
    uint32_t uptimeSec = 0;   // running time before the reset (last heartbeat)
    uint32_t epoch = 0;       // UTC of the last heartbeat before the reset (~ the reset)
    char     detail[48] = ""; // software restart: its reason; crash: "<task>: <panic reason>" (cut)
};

// From the core dump summary of the last crash.
struct CrashSummary {
    uint8_t  valid = 0;
    uint8_t  depth = 0;              // backtrace entries used
    uint8_t  backtraceCorrupted = 0;
    uint32_t epoch = 0;              // = its reset record's (0 = unknown)
    uint32_t pc = 0;                 // program counter of the crash
    uint32_t backtrace[8] = {};
    uint32_t exceptionCause = 0;     // Xtensa EXCCAUSE (xtensaExceptionName())
    uint32_t exceptionAddress = 0;   // EXCVADDR
    char     task[16] = "";
    char     reason[96] = "";        // the dump's panic details: abort() / assert text, watchdog tasks ("" = a CPU exception: see exceptionCause)
    char     firmware[32] = "";      // version that crashed
    char     elfSha[17] = "";        // leading hex digits of its ELF SHA-256 as the build keeps them (CONFIG_APP_RETRIEVE_LEN_ELF_SHA, 9): the firmware.elf to decode with
};

struct ResetHistory {
    uint8_t      count = 0;                  // records used, newest first
    uint32_t     starts = 0;                 // starts counted since the history began
    ResetRecord  rec[RESET_HISTORY_LEN];
    CrashSummary lastCrash;
};

// Adds this start's record at the front (the oldest one drops off) and the
// crash summary if the record is a crash's.
void resetHistoryPush(ResetHistory& h, const ResetRecord& r, const CrashSummary* crash);

// Heartbeat in RTC RAM: last sign of life before a reset.
constexpr uint32_t RESET_HEARTBEAT_MAGIC = 0x55574842;   // "UWHB"
struct ResetHeartbeat {
    uint32_t magic;
    uint32_t uptimeSec;
    uint32_t epoch;                // 0 = clock not set
    char     restartReason[40];    // systemRestart()'s reason, "" = none
    uint32_t check;                // over the fields above
};
void heartbeatStart(ResetHeartbeat& hb);                                  // this run: valid, empty
void heartbeatUpdate(ResetHeartbeat& hb, uint32_t uptimeSec, uint32_t epoch);
void heartbeatNoteRestart(ResetHeartbeat& hb, const char* reason);
bool heartbeatValid(const ResetHeartbeat& hb);                            // magic and check match

// This start's record: the cause, the heartbeat if valid (else nullptr), the
// crash summary if the core dump belongs to this reset (else nullptr).
ResetRecord resetRecordMake(uint8_t cause, const ResetHeartbeat* hb, const CrashSummary* crash);

// The dump's panic details in one short line: the task watchdog's caption
// becomes "task watchdog: loopTask (CPU 1), ..." (the task names must survive
// the cut to CrashSummary::reason), line breaks become spaces.
void crashReasonCompact(const char* in, char* out, size_t len);

// Whether a core dump belongs to the running firmware: `own` = our ELF SHA-256
// as hex text with ownDigits digits (what esp_app_get_elf_sha256() returned,
// without the terminator), `dumpSha` = the summary's field of dumpShaSize
// bytes (hex text, terminated only if it is shorter than the field). Compares
// exactly the digits both hold; no digits on either side = no match.
bool crashDumpShaMatches(const char* own, size_t ownDigits, const uint8_t* dumpSha, size_t dumpShaSize);

// Name of an Xtensa exception cause ("LoadProhibited"), nullptr = not one.
const char* xtensaExceptionName(uint32_t cause);

// NVS blob "resets" (storage_layout.h STORED_RESET_HISTORY_VERSION): version
// byte, then fixed little-endian fields (independent of struct padding).
constexpr size_t RESET_RECORD_BYTES = 1 + 1 + 4 + 4 + 48;
constexpr size_t CRASH_SUMMARY_BYTES = 3 + 4 * 4 + 4 * 8 + 16 + 96 + 32 + 17;
constexpr size_t RESET_HISTORY_BLOB = 1 + 1 + 4 + RESET_HISTORY_LEN * RESET_RECORD_BYTES + CRASH_SUMMARY_BYTES;
size_t resetHistoryPack(const ResetHistory& h, uint8_t* out, size_t len);   // RESET_HISTORY_BLOB, 0 = too small
// The payload after the version byte (storedBlobLoad()); false = wrong size (out = empty history).
bool   resetHistoryFromPayload(const uint8_t* p, size_t n, ResetHistory& out);

// ---- device part (reset_history_esp.cpp) ----
void resetHistoryBoot();                          // setup(), after cfgdataPrepare(): record this start
void resetHistoryLoop();                          // loop(): heartbeat once a second
void resetHistoryNoteRestart(const char* reason); // systemRestart()
const ResetHistory& resetHistory();               // read-only after resetHistoryBoot()
// The core dump in flash: address and size; false = none (or not valid).
// (raw flash image: header + ELF + checksum - esp-coredump --core-format raw)
bool resetHistoryCoreDump(size_t& addr, size_t& size);
bool resetHistoryCoreDumpRead(size_t addr, uint8_t* buf, size_t n);   // n bytes of flash from addr
// Deliberate crash for testing the diagnostics (admin, POST /api/diag/crash):
// the loop task crashes ~1 s later, so the answer still gets out.
enum CrashTestKind : uint8_t { CRASH_TEST_PANIC = 1, CRASH_TEST_WATCHDOG = 2 };
void crashTestRequest(CrashTestKind kind);
void crashTestPoll();                             // loop()
