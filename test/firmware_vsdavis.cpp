// Virtual sensors in Davis mode (C5, vsensor.h VsDavisGap): the gap after a
// Davis packet - may a frame go now, how many fit, missed hop / unlocked =
// none, and the oldest-first pick that spreads 8 slots over the gaps while
// every slot keeps its interval on average.
#include "vsensor.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
constexpr int64_t PERIOD_ID1_US = 2562500;   // Davis ID 1: 41 / 16 s
constexpr uint32_t GUARD_US = 40000;         // DavisHopperConfig::guardMs default

VsDavisGap gapAt(int64_t packetUs, int64_t periodUs = PERIOD_ID1_US)
{
    VsDavisGap g;
    g.confirmed = true;
    g.packetUs = packetUs;
    g.nextUs = packetUs + periodUs;
    g.guardUs = GUARD_US;
    return g;
}

void testGapAllows()
{
    const int64_t t = 1000000000LL;
    const VsDavisGap g = gapAt(t);
    // right after the packet: the first and second frame fit, a third never
    assert(vsDavisGapAllows(g, t + 2000, 0, 13));
    assert(vsDavisGapAllows(g, t + 80000, 1, 13));
    assert(!vsDavisGapAllows(g, t + 80000, VS_DAVIS_MAX_FRAMES, 9));
    assert(!vsDavisGapAllows(g, t + 2000, -1, 9));
    // not confirmed (missed hop, searching, resync, raw capture): never
    VsDavisGap u = g;
    u.confirmed = false;
    assert(!vsDavisGapAllows(u, t + 2000, 0, 9));
    // a late wake-up does not use the gap
    assert(vsDavisGapAllows(g, t + VS_DAVIS_LATE_US, 0, 9));
    assert(!vsDavisGapAllows(g, t + VS_DAVIS_LATE_US + 1, 0, 9));
    // clock before the packet / prediction not after the packet: nothing
    assert(!vsDavisGapAllows(g, t - 1, 0, 9));
    VsDavisGap bad = g;
    bad.nextUs = bad.packetUs;
    assert(!vsDavisGapAllows(bad, t + 2000, 0, 9));
    // the exact edge: done + margin == the earliest start of the next packet
    VsDavisGap s = gapAt(t, 500000);
    const int64_t listenFrom = s.nextUs - GUARD_US - VS_DAVIS_PACKET_US;
    const int64_t latest = listenFrom - 13 * 1000 - VS_DAVIS_SWITCH_US - VS_DAVIS_MARGIN_US;
    assert(latest > t);
    assert(vsDavisGapAllows(s, latest, 0, 13));
    assert(!vsDavisGapAllows(s, latest + 1, 0, 13));
    // a wider guard window narrows the gap
    VsDavisGap w = gapAt(t, 500000);
    w.guardUs = 200000;
    assert(!vsDavisGapAllows(w, t + 2000, 0, 13));
    // a period too short for any frame (period - guard - packet < switch + margin + air)
    assert(!vsDavisGapAllows(gapAt(t, 350000), t + 1000, 0, 9));
}

void testGapFrames()
{
    const int64_t t = 5000000;
    const uint32_t air[4] = {13, 9, 13, 9};
    // every Davis period (2.5625 ... 3.0 s) takes the full two frames
    for (int id = 1; id <= 8; id++) {
        const int64_t period = 2562500 + (id - 1) * 62500;
        assert(vsDavisGapFrames(gapAt(t, period), t + 1000, air, 4) == VS_DAVIS_MAX_FRAMES);
    }
    assert(vsDavisGapFrames(gapAt(t), t + 1000, air, 1) == 1);
    assert(vsDavisGapFrames(gapAt(t), t + 1000, air, 0) == 0);
    VsDavisGap u = gapAt(t);
    u.confirmed = false;
    assert(vsDavisGapFrames(u, t + 1000, air, 4) == 0);
    // a short gap: only the first frame (the second would end inside the margin)
    const int64_t period = GUARD_US + VS_DAVIS_PACKET_US + VS_DAVIS_MARGIN_US + 13000 + VS_DAVIS_SWITCH_US + 1000 + 20000;
    assert(vsDavisGapFrames(gapAt(t, period), t + 1000, air, 4) == 1);
    // the budget per gap: two of the longest frames end far before the next packet
    const int64_t used = 2 * (14000 + (int64_t)VS_DAVIS_SWITCH_US);
    assert(used + VS_DAVIS_MARGIN_US + GUARD_US + VS_DAVIS_PACKET_US < PERIOD_ID1_US / 2);
}

void testPick()
{
    bool w[VS_SLOTS] = {};
    uint64_t q[VS_SLOTS] = {};
    assert(vsDavisPick(w, q) == -1);
    w[5] = true; q[5] = 300;
    w[2] = true; q[2] = 100;
    w[7] = true; q[7] = 100;
    assert(vsDavisPick(w, q) == 2);   // oldest; tie -> lower slot
    w[2] = false;
    assert(vsDavisPick(w, q) == 7);
    w[7] = false;
    assert(vsDavisPick(w, q) == 5);
}

// The whole path over hours: 8 slots on their interval grid (vsNextDue, the
// loop task) queue one frame each (a newer one replaces a waiting one =
// deferred), the Davis task takes up to two per gap after each received
// packet, oldest first; missed hops give no gap.
struct SimResult { uint32_t sent[VS_SLOTS]; uint32_t deferred[VS_SLOTS]; uint64_t maxWaitMs; uint64_t firstMs[VS_SLOTS], lastMs[VS_SLOTS]; };

SimResult simulate(const uint32_t intervalMs[VS_SLOTS], int64_t periodUs, int missEvery, uint64_t durMs, bool locked = true)
{
    SimResult r;
    memset(&r, 0, sizeof(r));
    uint64_t due[VS_SLOTS];
    bool waiting[VS_SLOTS] = {};
    uint64_t queued[VS_SLOTS] = {};
    uint32_t air[VS_SLOTS];
    for (int i = 0; i < VS_SLOTS; i++) { due[i] = 10000 + (uint64_t)i * 3000; air[i] = 13; }   // FIRST_TX_MS + slot stagger
    int64_t nextPacketUs = 1234567;
    int packetNo = 0;
    uint32_t jit = 12345;
    for (uint64_t now = 0; now < durMs; now++) {   // 1 ms steps: the loop task
        for (int i = 0; i < VS_SLOTS; i++) {
            if (now < due[i]) continue;
            jit = jit * 1103515245u + 12345u;
            const int32_t j = (int32_t)((jit >> 16) % (2 * VS_JITTER_MS + 1)) - VS_JITTER_MS;
            due[i] = vsNextDue(due[i], now, intervalMs[i], j);
            if (waiting[i]) r.deferred[i]++;
            waiting[i] = true;
            queued[i] = now;
        }
        const int64_t nowUs = (int64_t)now * 1000;
        if (nowUs >= nextPacketUs) {   // the Davis task: a packet (or a missed hop)
            const bool missed = missEvery && (++packetNo % missEvery) == 0;
            VsDavisGap g = gapAt(nextPacketUs, periodUs);
            g.confirmed = locked && !missed;
            int64_t t = nextPacketUs + 1500;   // read + decode
            for (int n = 0;; n++) {
                const int s = vsDavisPick(waiting, queued);
                if (s < 0 || !vsDavisGapAllows(g, t, n, air[s])) break;
                waiting[s] = false;
                r.sent[s]++;
                const uint64_t wait = now - queued[s];
                if (wait > r.maxWaitMs) r.maxWaitMs = wait;
                if (!r.firstMs[s]) r.firstMs[s] = now;
                r.lastMs[s] = now;
                t += air[s] * 1000 + VS_DAVIS_SWITCH_US;
            }
            nextPacketUs += periodUs;
        }
    }
    return r;
}

void testStaggering()
{
    const uint64_t HOUR = 3600000;
    // 8 x IT+ TX29 at 15 s (the shortest interval): 0.53 frames/s, the gaps
    // carry 0.78/s - nothing deferred, every slot on its interval
    uint32_t it[VS_SLOTS];
    for (int i = 0; i < VS_SLOTS; i++) it[i] = 15000;
    SimResult r = simulate(it, PERIOD_ID1_US, 0, HOUR);
    for (int i = 0; i < VS_SLOTS; i++) {
        assert(r.deferred[i] == 0);
        const double avg = (double)(r.lastMs[i] - r.firstMs[i]) / (r.sent[i] - 1);
        assert(avg > 14900 && avg < 15100);
    }
    assert(r.maxWaitMs < 4 * 2563);   // at most a few gaps
    // the slowest Davis transmitter (ID 8, 3.0 s) still carries them
    r = simulate(it, 3000000, 0, HOUR);
    for (int i = 0; i < VS_SLOTS; i++) assert(r.deferred[i] == 0);

    // missed hops (every 4th packet = 75 % sync): no frame in those gaps, the
    // rest still carries everything
    r = simulate(it, PERIOD_ID1_US, 4, HOUR);
    for (int i = 0; i < VS_SLOTS; i++) {
        assert(r.deferred[i] == 0);
        assert(r.sent[i] >= 238 && r.sent[i] <= 241);   // 3600 s / 15 s, minus the start
    }

    // mixed types (WH31 61 s, WH65 16 s, WN32P 63 s, IT+ 22 s), Davis ID 3, a third
    // of the hops missed: every slot on its grid
    const uint32_t mix[VS_SLOTS] = {61000, 61000, 16000, 16000, 63000, 22000, 22000, 61000};
    r = simulate(mix, 2687500, 3, HOUR);
    for (int i = 0; i < VS_SLOTS; i++) {
        assert(r.deferred[i] == 0);
        const double avg = (double)(r.lastMs[i] - r.firstMs[i]) / (r.sent[i] - 1);
        assert(avg > mix[i] * 0.99 && avg < mix[i] * 1.01);   // the wait shifts single frames, not the grid
    }

    // not locked (searching / LOST): nothing sent, each turn replaces the waiting frame
    r = simulate(it, PERIOD_ID1_US, 0, 120000, /*locked=*/false);
    for (int i = 0; i < VS_SLOTS; i++) { assert(r.sent[i] == 0); assert(r.deferred[i] >= 5); }

    // the transmitter gone (every hop missed): the same
    r = simulate(it, PERIOD_ID1_US, 1, 120000);
    for (int i = 0; i < VS_SLOTS; i++) assert(r.sent[i] == 0);
}
} // namespace

int main()
{
    testGapAllows();
    testGapFrames();
    testPick();
    testStaggering();
    std::puts("vs_davis ok");
    return 0;
}
