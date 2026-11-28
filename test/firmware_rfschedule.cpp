// Receive scheduling (src/rf_schedule.cpp): interval learning and the phase a
// known sensor's packet is due in.
#include "rf_schedule.h"
#include <cassert>
#include <cstdio>

int main()
{
    const uint8_t BR = 1, P17 = 2;   // RF_PROTO_BRESSER / _ITPLUS17 (Fine Offset phase)

    // ---- learning ----
    // Fine Offset WH65 (nominal 16 s): first gap, a gap with one miss, outliers
    uint32_t p = rfLearnPeriod(0, 16000, 16020);
    assert(p == 16020);
    p = rfLearnPeriod(p, 16000, 32040);                 // one packet missed: still ~16 s
    assert(p >= 16015 && p <= 16025);
    assert(rfLearnPeriod(p, 16000, 1000) == p);         // repeat / too short: ignored
    assert(rfLearnPeriod(p, 16000, 2000000) == p);      // too long: ignored
    assert(rfLearnPeriod(p, 16000, 21000) == p);        // 1.3 intervals: an outlier
    // Bresser (no nominal): the first gap held two intervals, the next one corrects it
    p = rfLearnPeriod(0, 0, 24000);
    assert(p == 24000);
    p = rfLearnPeriod(p, 0, 12000);
    assert(p == 12000);
    p = rfLearnPeriod(p, 0, 36100);                     // two missed
    assert(p >= 12000 && p <= 12010);
    // WH41 on USB: 60 s instead of 10 min - no fixed interval (rfFixedIntervalMs() 0), learned freely
    assert(rfLearnPeriod(0, 0, 60000) == 60000);
    // a fixed interval: nothing below 0.75 x it is learned, not even as the first gap
    assert(rfLearnPeriod(0, 4000, 2370) == 0);
    assert(rfLearnPeriod(4070, 4000, 2370) == 4070);
    assert(rfLearnPeriod(4070, 4000, 2990) == 4070);
    assert(rfLearnPeriod(0, 4000, 3010) == 3010);       // just above the floor: still a plausible interval
    // Bresser (no fixed interval) keeps accepting a clearly shorter gap
    assert(rfLearnPeriod(4070, 0, 2370) == 2370);

    // ---- two IT+ transmitters on one ID (V4 log 2026-11-27, sensor 17000008):
    // A every ~4.07 s from 1.66 s, B every ~4.07 s from 8.08 s (offset ~2.4 s), B misses some.
    // Before: the 2.4 s gap was learned as "clearly shorter" and the period stayed at ~2 s.
    {
        const uint32_t a[] = { 1660, 5710, 9790, 13850, 17880, 21960, 26010, 30070, 34130, 38200, 42270, 46340, 50410, 54480, 58550 };
        const uint32_t b[] = { 8080, 16230, 20310, 24360, 36560, 40580, 48720, 52800 };
        const int countA = sizeof a / sizeof a[0], countB = sizeof b / sizeof b[0];
        uint32_t all[countA + countB];
        int n = 0, ia = 0, ib = 0;
        while (ia < countA || ib < countB)
            all[n++] = (ib >= countB || (ia < countA && a[ia] < b[ib])) ? a[ia++] : b[ib++];
        uint32_t learned = 0;
        int impossible = 0;
        for (int i = 1; i < n; i++) {
            learned = rfLearnPeriod(learned, 4000, all[i] - all[i - 1]);
            if (rfGapImpossible(all[i] - all[i - 1], 4000, true)) impossible++;
        }
        assert(learned >= 3900 && learned <= 4200);
        assert(impossible >= 5);
    }

    // ---- impossible gaps ----
    assert(!rfGapImpossible(2370, 0, true));             // no fixed interval: never
    assert(rfGapImpossible(2370, 4000, false));          // >= 2 s and < 3 s: a second transmitter
    assert(!rfGapImpossible(3000, 4000, true));          // 0.75 x: possible (jitter)
    assert(!rfGapImpossible(4070, 4000, true));
    assert(!rfGapImpossible(300, 4000, false));          // a repeat with the same values
    assert(rfGapImpossible(300, 4000, true));            // a "repeat" with other values

    // ---- due phase ----
    RfSchedEntry one[] = { { BR, 100000, 12000 } };
    assert(rfSchedDuePhase(one, 1, 105000) == 0);                       // between packets
    assert(rfSchedDuePhase(one, 1, 112000 - RF_SCHED_EARLY_MS) == BR);  // window opens
    assert(rfSchedDuePhase(one, 1, 112000 + RF_SCHED_LATE_MS) == BR);   // ... until just after
    assert(rfSchedDuePhase(one, 1, 112000 + RF_SCHED_LATE_MS + 1) == 0);
    assert(rfSchedDuePhase(one, 1, 124000) == BR);                      // missed one: next window
    assert(rfSchedDuePhase(one, 1, 100000 + 9 * 12000) == 0);           // 9 missed: lost, rotation only
    // millis() wrap
    RfSchedEntry wrap[] = { { P17, 0xFFFFF000u, 16000 } };
    assert(rfSchedDuePhase(wrap, 1, 0xFFFFF000u + 16000) == P17);

    // two sensors due at the same time: the one that missed more first
    RfSchedEntry both[] = { { BR, 100000, 12000 }, { P17, 88000, 12000 } };
    assert(rfSchedDuePhase(both, 2, 112000) == P17);    // Fine Offset missed one already
    both[1].lastMs = 112000;                            // ... and was received
    assert(rfSchedDuePhase(both, 2, 112500) == BR);     // now the Bresser window is the only one
    // no learned interval / no phase: ignored
    RfSchedEntry none[] = { { BR, 100000, 0 }, { 0, 100000, 12000 } };
    assert(rfSchedDuePhase(none, 2, 112000) == 0);

    // ---- simulation: the receiver hears a packet only in the matching phase;
    // the schedule takes over once the intervals are known (rotation 17 s
    // otherwise). Packets of a few ms, timing jitter up to +-40 ms.
    auto simulate = [&](uint32_t perB, uint32_t perF, uint32_t nomF, uint32_t startF, int& pctB, int& pctF) {
        const uint32_t per[2] = { perB, perF }, nom[2] = { 0, nomF };
        const uint8_t  ph[2]  = { BR, P17 };
        uint32_t next[2] = { 3000, startF }, last[2] = { 0, 0 }, learned[2] = { 0, 0 };
        int sent[2] = { 0, 0 }, heard[2] = { 0, 0 };
        uint8_t cur = BR; uint32_t curStart = 0, rnd = 12345;
        for (uint32_t t = 0; t < 7200000; t += 10) {
            RfSchedEntry e[2];
            int n = 0;
            for (int i = 0; i < 2; i++) if (learned[i]) e[n++] = { ph[i], last[i], learned[i] };
            int due = rfSchedDuePhase(e, n, t);
            if (due ? due != cur : t - curStart >= 17000) { cur = due ? (uint8_t)due : (cur == BR ? P17 : BR); curStart = t; }
            for (int i = 0; i < 2; i++) if (t >= next[i]) {
                sent[i]++;
                if (cur == ph[i]) {
                    heard[i]++;
                    if (last[i]) learned[i] = rfLearnPeriod(learned[i], nom[i], t - last[i]);
                    last[i] = t;
                }
                rnd = rnd * 1103515245u + 12345u;
                next[i] += per[i] - 40 + (rnd >> 16) % 81;
            }
        }
        pctB = heard[0] * 100 / sent[0]; pctF = heard[1] * 100 / sent[1];
        printf("schedule simulation %u / %u ms: Bresser %d %%, Fine Offset %d %% heard\n", perB, perF, pctB, pctF);
    };
    int b, f;
    simulate(12000, 16000, 16000, 7500, b, f);   // Bresser + WH65 (rotation alone: about half)
    assert(b >= 95 && f >= 95);
    simulate(12000, 4750, 4750, 7500, b, f);     // Bresser + WS80: the busiest case
    assert(b >= 90 && f >= 90);
    simulate(12000, 61000, 61000, 7500, b, f);   // Bresser + WH31
    assert(b >= 95 && f >= 95);

    // receive watchdog: restart after 10 min without a frame or a (re)start, also across the millis() wrap
    assert(!rfReceiverSilentTooLong(1000, 1000));
    assert(!rfReceiverSilentTooLong(1000 + RF_RECEIVE_WATCHDOG_MS - 1, 1000));
    assert(rfReceiverSilentTooLong(1000 + RF_RECEIVE_WATCHDOG_MS, 1000));
    assert(!rfReceiverSilentTooLong(5000, 0xFFFFF000u));   // 9 s across the wrap
    assert(rfReceiverSilentTooLong(RF_RECEIVE_WATCHDOG_MS, 0xFFFFF000u));
    return 0;
}
