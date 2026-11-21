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
    // WH41 on USB: 60 s instead of the nominal 10 min
    assert(rfLearnPeriod(0, 600000, 60000) == 60000);

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
    return 0;
}
