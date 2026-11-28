// Reset history and crash report (src/reset_history.h): the record of a
// start from cause / heartbeat / core dump summary, the ring of 10, the NVS
// blob, and the compacted panic details.
#include "reset_history.h"
#include "storage_layout.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    // ---- causes ----
    assert(!strcmp(resetCauseKey(RC_TASK_WATCHDOG), "taskWatchdog") && !strcmp(resetCauseKey(RC_POWER_ON), "powerOn"));
    assert(!strcmp(resetCauseKey(200), "unknown"));
    assert(resetCauseIsCrash(RC_PANIC) && resetCauseIsCrash(RC_TASK_WATCHDOG) && resetCauseIsCrash(RC_INT_WATCHDOG));
    assert(!resetCauseIsCrash(RC_SOFTWARE) && !resetCauseIsCrash(RC_BROWNOUT) && !resetCauseIsCrash(RC_POWER_ON));

    // ---- heartbeat in RTC RAM ----
    ResetHeartbeat hb;
    memset(&hb, 0x5A, sizeof(hb));   // RTC RAM after a power loss: noise
    assert(!heartbeatValid(hb));
    heartbeatStart(hb);
    assert(heartbeatValid(hb) && hb.uptimeSec == 0 && !hb.restartReason[0]);
    heartbeatUpdate(hb, 3600, 1796000000u);
    heartbeatNoteRestart(hb, "settings saved");
    assert(heartbeatValid(hb) && hb.uptimeSec == 3600 && !strcmp(hb.restartReason, "settings saved"));
    heartbeatUpdate(hb, 3601, 1796000001u);   // a beat after the note keeps the reason
    assert(heartbeatValid(hb) && !strcmp(hb.restartReason, "settings saved"));
    {
        ResetHeartbeat bad = hb;
        bad.uptimeSec++;   // one field changed without the check
        assert(!heartbeatValid(bad));
        heartbeatNoteRestart(bad, "a reason much longer than the forty bytes of room in RTC RAM");
        assert(heartbeatValid(bad) && strlen(bad.restartReason) == sizeof(bad.restartReason) - 1);
    }

    // ---- the record of a start ----
    {
        ResetRecord r = resetRecordMake(RC_POWER_ON, nullptr, nullptr);   // power loss: heartbeat gone
        assert(r.cause == RC_POWER_ON && r.flags == 0 && r.uptimeSec == 0 && !r.detail[0]);
        r = resetRecordMake(RC_SOFTWARE, &hb, nullptr);
        assert(r.flags == (RR_UPTIME_KNOWN | RR_TIME_KNOWN) && r.uptimeSec == 3601 && r.epoch == 1796000001u);
        assert(!strcmp(r.detail, "settings saved"));
        ResetHeartbeat noClock;
        heartbeatStart(noClock);
        heartbeatUpdate(noClock, 50, 0);
        r = resetRecordMake(RC_EXTERNAL, &noClock, nullptr);   // clock never set; no reason taken for a button reset
        assert(r.flags == RR_UPTIME_KNOWN && r.uptimeSec == 50 && !r.detail[0]);
        r = resetRecordMake(77, nullptr, nullptr);
        assert(r.cause == RC_UNKNOWN);
    }
    CrashSummary crash;
    crash.valid = 1; crash.pc = 0x42013a2cu; crash.exceptionCause = 29; crash.depth = 2;
    snprintf(crash.task, sizeof(crash.task), "loopTask");
    {
        ResetRecord r = resetRecordMake(RC_PANIC, &hb, &crash);   // a CPU exception: no panic details, its name
        assert((r.flags & RR_CRASH) && !strcmp(r.detail, "loopTask: StoreProhibited"));
        CrashSummary a = crash;
        snprintf(a.reason, sizeof(a.reason), "assert failed: vsLoop vs_run.cpp:142 (slot < VS_SLOTS)");
        r = resetRecordMake(RC_PANIC, &hb, &a);
        assert(!strncmp(r.detail, "loopTask: assert failed: vsLoop vs_run.cpp:142", 46) && strlen(r.detail) == sizeof(r.detail) - 1);
        CrashSummary w = crash;
        snprintf(w.task, sizeof(w.task), "IDLE0");
        snprintf(w.reason, sizeof(w.reason), "task watchdog: loopTask (CPU 1)");
        r = resetRecordMake(RC_TASK_WATCHDOG, &hb, &w);   // a watchdog: the hung task, not the one interrupted
        assert((r.flags & RR_CRASH) && !strcmp(r.detail, "task watchdog: loopTask (CPU 1)"));
        r = resetRecordMake(RC_SOFTWARE, &hb, &crash);   // a dump does not make a software restart a crash
        assert(!(r.flags & RR_CRASH) && !strcmp(r.detail, "settings saved"));
        CrashSummary none;
        r = resetRecordMake(RC_PANIC, &hb, &none);       // crash without a valid dump
        assert(!(r.flags & RR_CRASH) && !r.detail[0]);
    }
    assert(!strcmp(xtensaExceptionName(28), "LoadProhibited") && !strcmp(xtensaExceptionName(0), "IllegalInstruction"));
    assert(!strcmp(xtensaExceptionName(33), "CoprocessorDisabled") && !xtensaExceptionName(7) && !xtensaExceptionName(64));

    // ---- the ring: newest first, 10 kept, the last crash stays ----
    ResetHistory h;
    resetHistoryPush(h, resetRecordMake(RC_PANIC, &hb, &crash), &crash);
    assert(h.count == 1 && h.starts == 1 && h.lastCrash.valid && h.lastCrash.pc == 0x42013a2cu);
    for (uint32_t i = 0; i < 11; i++) {
        ResetHeartbeat b;
        heartbeatStart(b);
        heartbeatUpdate(b, 100 + i, 0);
        resetHistoryPush(h, resetRecordMake(RC_SOFTWARE, &b, &crash), &crash);   // not a crash: lastCrash untouched
    }
    assert(h.count == RESET_HISTORY_LEN && h.starts == 12);
    assert(h.rec[0].uptimeSec == 110 && h.rec[9].uptimeSec == 101);   // the crash record dropped off ...
    assert(h.lastCrash.valid && !strcmp(h.lastCrash.task, "loopTask"));   // ... its summary stays

    // ---- NVS blob ----
    {
        h.lastCrash.backtrace[1] = 0x4200d7e1u;
        snprintf(h.lastCrash.reason, sizeof(h.lastCrash.reason), "task watchdog: loopTask (CPU 1)");
        snprintf(h.lastCrash.elfSha, sizeof(h.lastCrash.elfSha), "3f9a2c1be07d4410");
        uint8_t blob[RESET_HISTORY_BLOB], scratch[RESET_HISTORY_BLOB];
        assert(resetHistoryPack(h, blob, sizeof(blob) - 1) == 0);
        const size_t n = resetHistoryPack(h, blob, sizeof(blob));
        assert(n == RESET_HISTORY_BLOB && blob[0] == STORED_RESET_HISTORY_VERSION);
        size_t len = 0;
        const uint8_t* p = storedBlobLoad(SF_RESET_HISTORY, blob, n, STORED_RESET_HISTORY_VERSION, RESET_HISTORY_BLOB - 1,
                                          scratch, sizeof(scratch), &len);
        ResetHistory b;
        assert(p && resetHistoryFromPayload(p, len, b));
        assert(b.count == h.count && b.starts == h.starts);
        for (int i = 0; i < RESET_HISTORY_LEN; i++)
            assert(b.rec[i].cause == h.rec[i].cause && b.rec[i].flags == h.rec[i].flags && b.rec[i].uptimeSec == h.rec[i].uptimeSec &&
                   b.rec[i].epoch == h.rec[i].epoch && !strcmp(b.rec[i].detail, h.rec[i].detail));
        assert(b.lastCrash.valid && b.lastCrash.pc == h.lastCrash.pc && b.lastCrash.backtrace[1] == 0x4200d7e1u &&
               b.lastCrash.depth == 2 && b.lastCrash.exceptionCause == 29 && !strcmp(b.lastCrash.reason, h.lastCrash.reason) &&
               !strcmp(b.lastCrash.elfSha, "3f9a2c1be07d4410") && !strcmp(b.lastCrash.task, "loopTask"));
        assert(!resetHistoryFromPayload(p, len - 1, b) && b.count == 0);   // another layout: a new history
        blob[1 + 0] = 200;   // count out of range: clamped
        assert(resetHistoryFromPayload(blob + 1, n - 1, b) && b.count == RESET_HISTORY_LEN);
    }

    // ---- does the core dump belong to this firmware? ----
    {
        // IDF 5.5 with CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9: esp_app_get_elf_sha256()
        // gives 9 digits; the summary's 10-byte field holds the same 9 + '\0'.
        const char own[] = "3f9a2c1be";
        uint8_t dump[10];
        memcpy(dump, "3f9a2c1be", 10);
        assert(crashDumpShaMatches(own, 9, dump, sizeof(dump)));
        dump[8] = 'f';   // another build
        assert(!crashDumpShaMatches(own, 9, dump, sizeof(dump)));
        // a field without a terminator (filled to the brim): only its digits count,
        // the bytes after it in the struct are never read
        uint8_t full[12];
        memcpy(full, "3f9a2c1be0XX", 12);
        const char ownLong[] = "3f9a2c1be07d4410";
        assert(crashDumpShaMatches(ownLong, 16, full, 10));
        assert(!crashDumpShaMatches(ownLong, 16, full, 12));   // "XX" differs from "7d"
        // ours shorter than the dump's
        assert(crashDumpShaMatches(own, 9, (const uint8_t*)"3f9a2c1be07d4410", 17));
        // nothing to compare
        assert(!crashDumpShaMatches(own, 0, dump, sizeof(dump)));
        const uint8_t empty[10] = {};
        assert(!crashDumpShaMatches(own, 9, empty, sizeof(empty)));
        assert(!crashDumpShaMatches(nullptr, 9, dump, sizeof(dump)));
    }

    // ---- panic details in one line ----
    {
        char out[96];
        crashReasonCompact("Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:\n"
                           " - loopTask (CPU 1)\n - wxUpload (CPU 0)", out, sizeof(out));
        assert(!strcmp(out, "task watchdog: loopTask (CPU 1), wxUpload (CPU 0)"));
        crashReasonCompact("assert failed: foo x.cpp:12\nmore", out, sizeof(out));
        assert(!strcmp(out, "assert failed: foo x.cpp:12 more"));
        char small[10];
        crashReasonCompact("abort() was called at PC 0x42001234", small, sizeof(small));
        assert(!strcmp(small, "abort() w"));
        crashReasonCompact(nullptr, small, sizeof(small));
        assert(!small[0]);
    }
    std::puts("reset history ok");
    return 0;
}
