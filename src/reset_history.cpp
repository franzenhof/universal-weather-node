#include "reset_history.h"
#include "storage_layout.h"
#include <stdio.h>
#include <string.h>

namespace {
const char* const CAUSE_KEYS[RC_COUNT] = {
    "unknown", "powerOn", "external", "software", "panic", "interruptWatchdog", "taskWatchdog",
    "watchdog", "deepSleep", "brownout", "usb", "jtag", "powerGlitch", "cpuLockup", "other",
};

uint32_t fnv1a(const uint8_t* p, size_t n, uint32_t h = 2166136261u)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

uint32_t heartbeatCheck(const ResetHeartbeat& hb)
{
    uint32_t h = fnv1a((const uint8_t*)&hb.magic, 4);
    h = fnv1a((const uint8_t*)&hb.uptimeSec, 4, h);
    h = fnv1a((const uint8_t*)&hb.epoch, 4, h);
    return fnv1a((const uint8_t*)hb.restartReason, sizeof(hb.restartReason), h);
}

void copyText(char* dst, size_t cap, const char* src)
{
    if (!cap) return;
    size_t n = 0;
    if (src) for (; src[n] && n + 1 < cap; n++) dst[n] = src[n];
    dst[n] = '\0';
}

// ---- little-endian writer / reader with explicit offsets ----
struct Out {
    uint8_t* p; size_t n;
    explicit Out(uint8_t* b) : p(b), n(0) {}
    void u8(uint8_t v) { p[n++] = v; }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) p[n++] = (uint8_t)(v >> (8 * i)); }
    void text(const char* s, size_t cap) { memset(p + n, 0, cap); copyText((char*)p + n, cap, s); n += cap; }
};
struct In {
    const uint8_t* p; size_t n;
    explicit In(const uint8_t* b) : p(b), n(0) {}
    uint8_t u8() { return p[n++]; }
    uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)p[n++] << (8 * i); return v; }
    void text(char* s, size_t cap) { memcpy(s, p + n, cap); s[cap - 1] = '\0'; n += cap; }
};
} // namespace

const char* resetCauseKey(uint8_t cause) { return cause < RC_COUNT ? CAUSE_KEYS[cause] : CAUSE_KEYS[RC_UNKNOWN]; }

bool resetCauseIsCrash(uint8_t cause)
{
    return cause == RC_PANIC || cause == RC_INT_WATCHDOG || cause == RC_TASK_WATCHDOG || cause == RC_OTHER_WATCHDOG ||
           cause == RC_CPU_LOCKUP;
}

void resetHistoryPush(ResetHistory& h, const ResetRecord& r, const CrashSummary* crash)
{
    for (int i = RESET_HISTORY_LEN - 1; i > 0; i--) h.rec[i] = h.rec[i - 1];
    h.rec[0] = r;
    if (h.count < RESET_HISTORY_LEN) h.count++;
    h.starts++;
    if (crash && crash->valid && (r.flags & RR_CRASH)) h.lastCrash = *crash;   // else the older crash stays the last one
}

void heartbeatStart(ResetHeartbeat& hb)
{
    memset(&hb, 0, sizeof(hb));
    hb.magic = RESET_HEARTBEAT_MAGIC;
    hb.check = heartbeatCheck(hb);
}

void heartbeatUpdate(ResetHeartbeat& hb, uint32_t uptimeSec, uint32_t epoch)
{
    hb.uptimeSec = uptimeSec;
    hb.epoch = epoch;
    hb.check = heartbeatCheck(hb);
}

void heartbeatNoteRestart(ResetHeartbeat& hb, const char* reason)
{
    memset(hb.restartReason, 0, sizeof(hb.restartReason));
    copyText(hb.restartReason, sizeof(hb.restartReason), reason);
    hb.check = heartbeatCheck(hb);
}

bool heartbeatValid(const ResetHeartbeat& hb)
{
    return hb.magic == RESET_HEARTBEAT_MAGIC && hb.check == heartbeatCheck(hb) &&
           memchr(hb.restartReason, '\0', sizeof(hb.restartReason)) != nullptr;
}

ResetRecord resetRecordMake(uint8_t cause, const ResetHeartbeat* hb, const CrashSummary* crash)
{
    ResetRecord r;
    r.cause = cause < RC_COUNT ? cause : (uint8_t)RC_UNKNOWN;
    if (hb) {
        r.flags |= RR_UPTIME_KNOWN;
        r.uptimeSec = hb->uptimeSec;
        if (hb->epoch) { r.flags |= RR_TIME_KNOWN; r.epoch = hb->epoch; }
        if (cause == RC_SOFTWARE) copyText(r.detail, sizeof(r.detail), hb->restartReason);
    }
    if (crash && crash->valid && resetCauseIsCrash(cause)) {
        r.flags |= RR_CRASH;
        const char* why = crash->reason[0] ? crash->reason : xtensaExceptionName(crash->exceptionCause);
        // a watchdog's details name the hung tasks; the dump's task is just the one that was running then
        const bool watchdog = cause == RC_TASK_WATCHDOG || cause == RC_INT_WATCHDOG || cause == RC_OTHER_WATCHDOG;
        if (watchdog && crash->reason[0]) copyText(r.detail, sizeof(r.detail), crash->reason);
        else snprintf(r.detail, sizeof(r.detail), "%s%s%s", crash->task, why ? ": " : "", why ? why : "");
    }
    return r;
}

const char* xtensaExceptionName(uint32_t cause)
{
    switch (cause) {
        case 0:  return "IllegalInstruction";
        case 1:  return "Syscall";
        case 2:  return "InstructionFetchError";
        case 3:  return "LoadStoreError";
        case 4:  return "Level1Interrupt";
        case 5:  return "Alloca";
        case 6:  return "IntegerDivideByZero";
        case 8:  return "Privileged";
        case 9:  return "LoadStoreAlignment";
        case 12: return "InstrPIFDataError";
        case 13: return "LoadStorePIFDataError";
        case 14: return "InstrPIFAddrError";
        case 15: return "LoadStorePIFAddrError";
        case 16: return "InstTLBMiss";
        case 17: return "InstTLBMultiHit";
        case 18: return "InstFetchPrivilege";
        case 20: return "InstFetchProhibited";
        case 24: return "LoadStoreTLBMiss";
        case 25: return "LoadStoreTLBMultiHit";
        case 26: return "LoadStorePrivilege";
        case 28: return "LoadProhibited";
        case 29: return "StoreProhibited";
        default: return cause >= 32 && cause <= 39 ? "CoprocessorDisabled" : nullptr;
    }
}

size_t resetHistoryPack(const ResetHistory& h, uint8_t* out, size_t len)
{
    if (!out || len < RESET_HISTORY_BLOB) return 0;
    Out o(out);
    o.u8(STORED_RESET_HISTORY_VERSION);
    o.u8(h.count > RESET_HISTORY_LEN ? RESET_HISTORY_LEN : h.count);
    o.u32(h.starts);
    for (const ResetRecord& r : h.rec) {
        o.u8(r.cause); o.u8(r.flags); o.u32(r.uptimeSec); o.u32(r.epoch); o.text(r.detail, sizeof(r.detail));
    }
    const CrashSummary& c = h.lastCrash;
    o.u8(c.valid); o.u8(c.depth); o.u8(c.backtraceCorrupted);
    o.u32(c.epoch); o.u32(c.pc); o.u32(c.exceptionCause); o.u32(c.exceptionAddress);
    for (uint32_t a : c.backtrace) o.u32(a);
    o.text(c.task, sizeof(c.task)); o.text(c.reason, sizeof(c.reason));
    o.text(c.firmware, sizeof(c.firmware)); o.text(c.elfSha, sizeof(c.elfSha));
    return o.n;
}

bool resetHistoryFromPayload(const uint8_t* p, size_t n, ResetHistory& out)
{
    out = ResetHistory();
    if (!p || n != RESET_HISTORY_BLOB - 1) return false;
    In in(p);
    ResetHistory h;
    h.count = in.u8();
    if (h.count > RESET_HISTORY_LEN) h.count = RESET_HISTORY_LEN;
    h.starts = in.u32();
    for (ResetRecord& r : h.rec) {
        r.cause = in.u8(); r.flags = in.u8(); r.uptimeSec = in.u32(); r.epoch = in.u32(); in.text(r.detail, sizeof(r.detail));
        if (r.cause >= RC_COUNT) r.cause = RC_UNKNOWN;
    }
    CrashSummary& c = h.lastCrash;
    c.valid = in.u8(); c.depth = in.u8(); c.backtraceCorrupted = in.u8();
    if (c.depth > 8) c.depth = 8;
    c.epoch = in.u32(); c.pc = in.u32(); c.exceptionCause = in.u32(); c.exceptionAddress = in.u32();
    for (uint32_t& a : c.backtrace) a = in.u32();
    in.text(c.task, sizeof(c.task)); in.text(c.reason, sizeof(c.reason));
    in.text(c.firmware, sizeof(c.firmware)); in.text(c.elfSha, sizeof(c.elfSha));
    out = h;
    return true;
}

bool crashDumpShaMatches(const char* own, size_t ownDigits, const uint8_t* dumpSha, size_t dumpShaSize)
{
    if (!own || !dumpSha) return false;
    size_t dumpDigits = 0;
    while (dumpDigits < dumpShaSize && dumpSha[dumpDigits]) dumpDigits++;
    const size_t compared = ownDigits < dumpDigits ? ownDigits : dumpDigits;
    return compared > 0 && memcmp(own, dumpSha, compared) == 0;
}

void crashReasonCompact(const char* in, char* out, size_t len)
{
    if (!out || !len) return;
    out[0] = '\0';
    if (!in) return;
    static const char WDT[] = "Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:";
    size_t n = 0;
    auto put = [&](const char* s, size_t k) { for (size_t i = 0; i < k && s[i] && n + 1 < len; i++) out[n++] = s[i]; out[n] = '\0'; };
    const char* p = in;
    bool list = false;
    if (!strncmp(p, WDT, sizeof(WDT) - 1)) { put("task watchdog: ", 15); p += sizeof(WDT) - 1; list = true; }
    bool first = true;
    while (*p) {
        if (p[0] == '\n') {   // "\n - name (CPU 1)": one item of the list, ", " between
            p++;
            if (list && p[0] == ' ' && p[1] == '-' && p[2] == ' ') p += 3;
            if (!first || !list) put(list ? ", " : " ", 2);
            continue;
        }
        first = false;
        put(p, 1);
        p++;
    }
}
