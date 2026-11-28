// Reception watchdog (src/reception_window.cpp): rates, threshold, unknown
// states, restart after silence / re-bind, millis() wrap.
#include "reception_window.h"
#include "rf_schedule.h"
#include <cassert>
#include <cstdio>

namespace {

constexpr uint32_t MIN = 60000;

// Packets of a sensor with interval periodMs from startMs for durationMs;
// every keepEvery-th transmission is received (1 = all).
uint32_t feed(ReceptionWindow& w, uint32_t startMs, uint32_t periodMs, uint32_t durationMs, uint32_t keepEvery)
{
    uint32_t t = startMs, n = 0;
    for (uint32_t elapsed = 0; elapsed < durationMs; elapsed += periodMs, t += periodMs, n++)
        if (n % keepEvery == 0) receptionCountPacket(w, t);
    return t;   // time of the next transmission
}

void checkPerfectAndPoor(uint32_t startMs)
{
    const uint32_t period = 16000;
    // all packets for 70 min: 100 %, not poor
    ReceptionWindow w;
    uint32_t now = feed(w, startMs, period, 70 * MIN, 1);
    ReceptionVerdict v = receptionEvaluate(w, period, now);
    assert(v.known && v.percent >= 98 && !v.poor);

    // one in five (20 %) for 70 min: poor
    ReceptionWindow p;
    now = feed(p, startMs, period, 70 * MIN, 5);
    v = receptionEvaluate(p, period, now);
    assert(v.known && v.percent >= 18 && v.percent <= 22 && v.poor);

    // one in three (33 %): known, low, but above the threshold
    ReceptionWindow q;
    now = feed(q, startMs, period, 70 * MIN, 3);
    v = receptionEvaluate(q, period, now);
    assert(v.known && v.percent >= 31 && v.percent <= 36 && !v.poor);
}

// ---- ID collision helpers: packets as rfStoreValues() sees them ----
constexpr uint32_t IT_PLUS_MS = 4000;     // rfFixedIntervalMs() of an IT+ sensor
constexpr uint32_t REAL_IT_PLUS_MS = 4070;   // measured (V4 log 2026-11-27)

struct Receiver {
    ReceptionWindow w;
    uint32_t previousMs = 0;
    float previousTemp = 0.0f;
    bool any = false;
    uint32_t period = 0;
    void packet(uint32_t t, float temp)
    {
        const bool valuesDiffer = any && temp != previousTemp;
        const bool impossible = any && rfGapImpossible(t - previousMs, IT_PLUS_MS, valuesDiffer);
        if (any) period = rfLearnPeriod(period, IT_PLUS_MS, t - previousMs);
        receptionCountPacket(w, t, impossible);
        previousMs = t; previousTemp = temp; any = true;
    }
    ReceptionVerdict at(uint32_t t) const { return receptionEvaluate(w, period, t); }
};

// Two transmitters A and B with one ID, B offsetMs after A, from startMs for
// durationMs; B received only on 3 of 4 transmissions. Returns the time of
// A's next transmission.
uint32_t feedTwins(Receiver& rx, uint32_t startMs, uint32_t offsetMs, uint32_t durationMs, float tempA, float tempB)
{
    uint32_t n = 0, elapsed = 0;
    for (; elapsed < durationMs; elapsed += REAL_IT_PLUS_MS, n++) {
        const uint32_t t = startMs + elapsed;
        rx.packet(t, tempA);
        if (n % 4 != 3) rx.packet(t + offsetMs, tempB);
    }
    return startMs + elapsed;
}

void checkIdCollision()
{
    // the V4 log: A -3.4 °C every ~4.07 s, B -2.8 °C ~2.4 s later; flagged within a minute
    {
        Receiver rx;
        uint32_t now = feedTwins(rx, 1660, 2350, MIN, -3.4f, -2.8f);
        ReceptionVerdict v = rx.at(now);
        assert(v.idCollision && !v.known && !v.poor);
        assert(rx.period >= 3900 && rx.period <= 4200);   // the interval is not trapped at ~2 s
        now = feedTwins(rx, now, 2350, 70 * MIN, -3.4f, -2.8f);
        assert(rx.at(now).idCollision);
        // B's battery taken out: the warning holds until no impossible gap is left in the window
        Receiver& a = rx;
        uint32_t t = now;
        for (; t < now + 50 * MIN; t += REAL_IT_PLUS_MS) a.packet(t, -3.4f);
        assert(a.at(t).idCollision);
        for (; t < now + 75 * MIN; t += REAL_IT_PLUS_MS) a.packet(t, -3.4f);
        v = a.at(t);
        assert(!v.idCollision && v.known && v.percent >= 95);
    }
    // other offsets: B ~0.5 s after A (looks like a repeat, the values differ) and ~3.5 s (A 0.57 s after B)
    {
        Receiver close, late;
        const uint32_t nowClose = feedTwins(close, 1000, 500, 10 * MIN, 21.0f, 19.5f);
        const uint32_t nowLate  = feedTwins(late, 1000, 3500, 10 * MIN, 21.0f, 19.5f);
        assert(close.at(nowClose).idCollision && late.at(nowLate).idCollision);
    }
    // two transmitters with the same values close together: indistinguishable from repeats (and harmless)
    {
        Receiver same;
        const uint32_t now = feedTwins(same, 1000, 500, 10 * MIN, 21.0f, 21.0f);
        assert(!same.at(now).idCollision);
    }
    // a single sensor: jitter +-300 ms, ~30 % of the packets missed, a repeat now and then - never flagged
    {
        Receiver single;
        uint32_t t = 1000, rnd = 4711;
        for (int n = 0; n < 3 * 3600 * 1000 / (int)REAL_IT_PLUS_MS; n++) {
            rnd = rnd * 1103515245u + 12345u;
            const uint32_t r = rnd >> 16;
            if (r % 10 >= 3) {
                single.packet(t, 12.3f);
                if (r % 50 == 7) single.packet(t + 150, 12.3f);   // the same transmission decoded twice
            }
            t += REAL_IT_PLUS_MS - 300 + r % 601;
            assert(!single.at(t).idCollision);
        }
        const ReceptionVerdict v = single.at(t);
        assert(v.known && v.percent >= 60 && v.percent <= 80 && !v.poor);
    }
    // occasional noise: 4 corrupted packets with this ID in an hour stay below the thresholds
    {
        Receiver noisy;
        uint32_t t = 1000;
        for (int n = 0; n < 3600 * 1000 / (int)REAL_IT_PLUS_MS; n++, t += REAL_IT_PLUS_MS) {
            noisy.packet(t, 12.3f);
            if (n % 200 == 100) noisy.packet(t + 2500, 85.0f);
        }
        const ReceptionVerdict v = noisy.at(t);
        assert(!v.idCollision && v.known);
    }
    // the ratio: 5 impossible gaps among ~880 packets of an hour are noise, not a second transmitter
    {
        ReceptionWindow w;
        uint32_t t = 1000;
        for (int n = 0; n < 900; n++, t += REAL_IT_PLUS_MS) receptionCountPacket(w, t, n % 150 == 1);
        assert(!receptionEvaluate(w, REAL_IT_PLUS_MS, t).idCollision);
    }
}

} // namespace

int main()
{
    const uint32_t period = 16000;

    // ---- rates and threshold ----
    checkPerfectAndPoor(1000);

    // ---- no learned interval / too short an interval: unknown ----
    {
        ReceptionWindow w;
        const uint32_t now = feed(w, 1000, period, 70 * MIN, 1);
        assert(!receptionEvaluate(w, 0, now).known);
        assert(!receptionEvaluate(w, RECEPTION_MIN_PERIOD_MS - 1, now).known);
        assert(receptionEvaluate(w, period, now).known);
    }

    // ---- not enough data yet: unknown; a percentage after 10 min; poor only after a full hour ----
    {
        ReceptionWindow w;
        assert(!receptionEvaluate(w, period, 5000).known);          // never started
        uint32_t now = feed(w, 1000, period, 5 * MIN, 5);
        assert(!receptionEvaluate(w, period, now).known);           // 5 min of data
        now = feed(w, now, period, 25 * MIN, 5);
        ReceptionVerdict v = receptionEvaluate(w, period, now);
        assert(v.known && v.percent <= 25 && !v.poor);              // 30 min: low, not yet "poor"
        now = feed(w, now, period, 31 * MIN, 5);
        assert(receptionEvaluate(w, period, now).poor);             // a full hour
    }

    // ---- repeats of one transmission are counted once ----
    {
        ReceptionWindow w, r;
        uint32_t t = 1000;
        for (int n = 0; n < 70 * 60 / 16; n++, t += period) {
            receptionCountPacket(w, t);
            receptionCountPacket(r, t);
            receptionCountPacket(r, t + 300);   // Bresser-style repeat 0.3 s later
        }
        assert(receptionEvaluate(w, period, t).percent == receptionEvaluate(r, period, t).percent);
    }

    // ---- reception drops: the window follows within the hour ----
    {
        ReceptionWindow w;
        uint32_t now = feed(w, 1000, period, 70 * MIN, 1);
        assert(!receptionEvaluate(w, period, now).poor);
        now = feed(w, now, period, 30 * MIN, 20);                   // the node fell off the sill
        assert(!receptionEvaluate(w, period, now).poor);            // half the hour still good
        now = feed(w, now, period, 40 * MIN, 20);
        ReceptionVerdict v = receptionEvaluate(w, period, now);
        assert(v.known && v.percent <= 8 && v.poor);
        now = feed(w, now, period, 70 * MIN, 1);                    // back on the sill
        assert(!receptionEvaluate(w, period, now).poor);
    }

    // ---- time passes without a packet: read as empty buckets, no write needed ----
    {
        ReceptionWindow w;
        const uint32_t now = feed(w, 1000, period, 70 * MIN, 1);
        const ReceptionVerdict atOnce = receptionEvaluate(w, period, now);
        const ReceptionVerdict later  = receptionEvaluate(w, period, now + 40 * MIN);
        assert(atOnce.known && later.known && later.percent < atOnce.percent && later.percent <= 45);
    }

    // ---- silent: no packet for an hour = unknown (not poor), the next packet starts a new window ----
    {
        ReceptionWindow w;
        uint32_t now = feed(w, 1000, period, 70 * MIN, 1);
        now += RECEPTION_SILENCE_MS;
        assert(!receptionEvaluate(w, period, now).known);
        receptionStopIfSilent(w, now);
        assert(w.bucketsCovered == 0);
        now = feed(w, now, period, 20 * MIN, 1);                   // comes back: fresh window, not "poor"
        ReceptionVerdict v = receptionEvaluate(w, period, now);
        assert(v.known && v.percent >= 95 && !v.poor);
        // a running window is not stopped early
        receptionStopIfSilent(w, now);
        assert(w.bucketsCovered != 0);
    }

    // ---- re-bind / no longer ★: stopped, the next packet starts fresh ----
    {
        ReceptionWindow w;
        uint32_t now = feed(w, 1000, period, 70 * MIN, 5);
        assert(receptionEvaluate(w, period, now).poor);
        receptionStop(w);
        assert(!receptionEvaluate(w, period, now).known);
        now = feed(w, now, period, 20 * MIN, 1);
        ReceptionVerdict v = receptionEvaluate(w, period, now);
        assert(v.known && v.percent >= 95 && !v.poor);
    }

    // ---- millis() wrap: the same results across 0xFFFFFFFF ----
    checkPerfectAndPoor(0xFFFFFFFFu - 30 * MIN);
    {
        ReceptionWindow w;
        const uint32_t start = 0xFFFFFFFFu - 50 * MIN;
        uint32_t now = feed(w, start, period, 70 * MIN, 5);          // crosses the wrap
        assert(now < start && receptionEvaluate(w, period, now).poor);
        receptionStopIfSilent(w, now);                             // not silent despite the wrap
        assert(w.bucketsCovered != 0);
    }

    // ---- a 20-minute sensor: few expected packets ----
    {
        ReceptionWindow w;
        const uint32_t slow = 20 * MIN;
        receptionCountPacket(w, 1000);
        assert(!receptionEvaluate(w, slow, 1000 + 30 * MIN).known);   // < 3 expected
        receptionCountPacket(w, 1000 + 40 * MIN);
        const ReceptionVerdict v = receptionEvaluate(w, slow, 1000 + 61 * MIN);
        assert(v.known && !v.poor);                                 // 2 of ~3: fine
    }

    // ---- two transmitters on one radio ID ----
    checkIdCollision();

    printf("reception window: all tests passed\n");
    return 0;
}
