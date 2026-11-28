#include "rf_schedule.h"

namespace {
uint32_t shortestPossibleInterval(uint32_t fixedIntervalMs)
{
    return (uint32_t)((uint64_t)fixedIntervalMs * RF_SCHED_MIN_INTERVAL_NUM / RF_SCHED_MIN_INTERVAL_DEN);
}
} // namespace

uint32_t rfLearnPeriod(uint32_t periodMs, uint32_t fixedIntervalMs, uint32_t gapMs)
{
    if (gapMs < RF_SCHED_MIN_GAP_MS || gapMs > RF_SCHED_MAX_GAP_MS) return periodMs;
    const uint32_t ref = periodMs ? periodMs : fixedIntervalMs ? fixedIntervalMs : gapMs;
    uint32_t n = (gapMs + ref / 2) / ref;
    if (!n) n = 1;
    const uint32_t c = gapMs / n;
    if (fixedIntervalMs && c < shortestPossibleInterval(fixedIntervalMs)) return periodMs;   // a second transmitter or noise
    if (!periodMs) return c;
    if ((uint64_t)c * 10 < (uint64_t)periodMs * 9) return c;                 // clearly shorter: the guess was a multiple
    if ((uint64_t)c * 10 > (uint64_t)periodMs * 11) return periodMs;          // clearly longer: an outlier
    return (uint32_t)(((uint64_t)periodMs * 3 + c) / 4);                     // refine slowly
}

bool rfGapImpossible(uint32_t gapMs, uint32_t fixedIntervalMs, bool valuesDiffer)
{
    if (!fixedIntervalMs || gapMs >= shortestPossibleInterval(fixedIntervalMs)) return false;
    return gapMs >= RF_SCHED_MIN_GAP_MS || valuesDiffer;
}

int rfSchedDuePhase(const RfSchedEntry* e, int n, uint32_t nowMs)
{
    int best = 0;
    uint32_t bestMissed = 0;
    int32_t  bestIn = 0;
    for (int i = 0; i < n; i++) {
        const RfSchedEntry& s = e[i];
        if (!s.phase || !s.periodMs) continue;
        const uint32_t age = nowMs - s.lastMs;
        // the next expected packet whose window has not ended yet
        uint32_t k = age > RF_SCHED_LATE_MS ? (age - RF_SCHED_LATE_MS + s.periodMs - 1) / s.periodMs : 1;
        if (!k) k = 1;
        if (k > RF_SCHED_MAX_MISSED) continue;                                 // lost: left to the rotation
        const uint32_t due = k * s.periodMs;                                   // relative to lastMs
        if (age + RF_SCHED_EARLY_MS < due) continue;                           // not yet
        // the earliest packet first: once it arrived, that sensor's window
        // closes and the receiver moves on to the next one
        const int32_t in = (int32_t)(due - age);
        if (!best || in < bestIn || (in == bestIn && k > bestMissed)) { best = s.phase; bestIn = in; bestMissed = k; }
    }
    return best;
}

bool rfReceiverSilentTooLong(uint32_t nowMs, uint32_t lastLifeSignMs)
{
    return nowMs - lastLifeSignMs >= RF_RECEIVE_WATCHDOG_MS;
}
