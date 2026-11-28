// Auto (search) lock rules (src/rf_autosearch.cpp): packet count within the
// window, RSSI threshold incl. the V4 offset, weak-signal fallback.
#include "rf_autosearch.h"
#include <cassert>
#include <cstdio>

int main()
{
    const uint8_t ITP = 2, FO = 8, BR = 1;   // RF_PROTO_* values only as tags here
    AutoSearch s;
    const AutoCand* c = nullptr;
    autoSearchStart(s, 0);
    // a good sensor: locked at the third packet within 10 min
    assert(autoSearchPacket(s, 0x900181A2, FO, -80, false, 1000, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 0x900181A2, FO, -82, false, 80000, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 0x900181A2, FO, -81, false, 160000, &c) == AUTO_LOCK && c->id == 0x900181A2 && c->proto == FO);

    // the field case: a neighbour's IT+ at -114 dBm never ends the search by itself
    autoSearchStart(s, 0);
    for (uint32_t t = 4000; t < 600000; t += 240000) assert(autoSearchPacket(s, 0x17000008, ITP, -114, false, t, &c) == AUTO_NONE);
    // ...while a good Bresser sensor heard meanwhile is taken
    assert(autoSearchPacket(s, 0x39582376, BR, -95, false, 700000, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 0x39582376, BR, -96, false, 712000, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 0x39582376, BR, -94, false, 724000, &c) == AUTO_LOCK && c->id == 0x39582376);

    // three packets spread over more than 10 min: no lock (the window restarts)
    autoSearchStart(s, 0);
    assert(autoSearchPacket(s, 1, ITP, -90, false, 1000, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 1, ITP, -90, false, 300000, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 1, ITP, -90, false, 700000, &c) == AUTO_NONE);   // window over: count 1 again
    assert(autoSearchPacket(s, 1, ITP, -90, false, 704000, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 1, ITP, -90, false, 708000, &c) == AUTO_LOCK);

    // RSSI threshold: -105 dBm on the V3, 20 dB higher on the V4 (LNA)
    assert(autoRssiThreshold(false) == -105.0f && autoRssiThreshold(true) == -85.0f);
    autoSearchStart(s, 0);
    for (int k = 0; k < 2; k++) autoSearchPacket(s, 2, ITP, -100, true, 1000 + k * 4000, &c);
    assert(autoSearchPacket(s, 2, ITP, -100, true, 9000, &c) == AUTO_NONE);      // V4: -100 is weak
    autoSearchStart(s, 0);
    for (int k = 0; k < 2; k++) autoSearchPacket(s, 2, ITP, -100, false, 1000 + k * 4000, &c);
    assert(autoSearchPacket(s, 2, ITP, -100, false, 9000, &c) == AUTO_LOCK);     // V3: fine

    // weak fallback: after 30 min the candidate with the most packets (then the strongest)
    autoSearchStart(s, 0);
    for (int k = 0; k < 5; k++) autoSearchPacket(s, 0x17000008, ITP, -114, false, 10000 + k * 60000, &c);
    for (int k = 0; k < 3; k++) autoSearchPacket(s, 0x5700E21A, FO, -110, false, 20000 + k * 80000, &c);
    autoSearchPacket(s, 0x12, BR, -108, false, 30000, &c);                       // 1 packet only: never chosen
    assert(autoSearchTick(s, AUTO_WEAK_AFTER_MS - 1, &c) == AUTO_NONE);
    assert(autoSearchTick(s, AUTO_WEAK_AFTER_MS, &c) == AUTO_LOCK_WEAK && c->id == 0x17000008 && c->total == 5);
    // nothing with at least 3 packets: keeps searching
    autoSearchStart(s, 0);
    autoSearchPacket(s, 0x12, BR, -108, false, 30000, &c);
    assert(autoSearchTick(s, 2 * AUTO_WEAK_AFTER_MS, &c) == AUTO_NONE);
    // not running (fixed type): never
    AutoSearch idle;
    assert(autoSearchTick(idle, 10 * AUTO_WEAK_AFTER_MS, &c) == AUTO_NONE);
    // more candidates than places: the one heard longest ago is replaced
    autoSearchStart(s, 0);
    for (uint32_t id = 1; id <= AUTO_CAND_MAX + 1; id++) autoSearchPacket(s, id, BR, -120, false, id * 1000, &c);
    int n = 0;
    bool firstGone = true;
    for (const AutoCand& x : s.c) { if (x.id) n++; if (x.id == 1) firstGone = false; }
    assert(n == AUTO_CAND_MAX && firstGone);

    // --- ramp: thr(t) = base - 15 dB * t / 30 min (V3 base -105) ---
    const uint32_t MIN = 60000UL;
    assert(autoRssiThreshold(false, 0) == -105.0f && autoRssiThreshold(true, 0) == -85.0f);
    assert(autoRssiThreshold(false, 10 * MIN) == -110.0f && autoRssiThreshold(false, 30 * MIN) == -120.0f);
    assert(autoRssiThreshold(true, 30 * MIN) == -100.0f && autoRssiThreshold(false, 90 * MIN) == -120.0f);   // ramp ends at 30 min
    // strong sensor at t = 0 locks at once (third packet)
    autoSearchStart(s, 0);
    autoSearchPacket(s, 5, ITP, -90, false, 100, &c);
    autoSearchPacket(s, 5, ITP, -90, false, 4100, &c);
    assert(autoSearchPacket(s, 5, ITP, -90, false, 8100, &c) == AUTO_LOCK && c->id == 5);
    // -110 dBm: not at t ~ 0, locked once the threshold reaches -110 (10 min); the tick finds it without a packet
    autoSearchStart(s, 0);
    for (int k = 0; k < 3; k++) assert(autoSearchPacket(s, 6, ITP, -110, false, 1000 + k * 4000, &c) == AUTO_NONE);
    assert(autoSearchTick(s, 5 * MIN, &c) == AUTO_NONE);
    assert(autoSearchTick(s, 10 * MIN - 1000, &c) == AUTO_NONE);   // thr -109.99
    assert(autoSearchTick(s, 10 * MIN, &c) == AUTO_LOCK && c->id == 6);   // thr -110: reached without a new packet
    assert(autoSearchTick(s, 10 * MIN + 2000, &c) == AUTO_NONE);          // window of the packets (opened at 1 s) is over
    // late packets of the same sensor: 9:50 / 9:54 / 9:58 / 10:02 -> the third one is just too weak, the fourth locks
    autoSearchStart(s, 0);
    assert(autoSearchPacket(s, 6, ITP, -110, false, 9 * MIN + 50000, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 6, ITP, -110, false, 9 * MIN + 54000, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 6, ITP, -110, false, 9 * MIN + 58000, &c) == AUTO_NONE);   // thr -109.99 > -110
    assert(autoSearchPacket(s, 6, ITP, -110, false, 10 * MIN + 2000, &c) == AUTO_LOCK && c->id == 6);
    // -110 dBm sent regularly from the start: window of the first 3 packets ends at 10 min, lock at the first packet where thr <= -110
    autoSearchStart(s, 0);
    AutoResult r = AUTO_NONE;
    uint32_t lockAt = 0;
    for (uint32_t t = 1000; t < 30 * MIN && r == AUTO_NONE; t += 20000) { r = autoSearchPacket(s, 7, ITP, -110, false, t, &c); lockAt = t; }
    assert(r == AUTO_LOCK && lockAt >= 10 * MIN && lockAt < 10 * MIN + 60000);
    // -118 dBm: only after ~26 min (thr -118 at 26:00)
    autoSearchStart(s, 0);
    r = AUTO_NONE; lockAt = 0;
    for (uint32_t t = 1000; t < 30 * MIN && r == AUTO_NONE; t += 20000) { r = autoSearchPacket(s, 8, ITP, -118, false, t, &c); lockAt = t; }
    assert(r == AUTO_LOCK && lockAt >= 26 * MIN && lockAt < 26 * MIN + 30000);
    // -125 dBm: never by the ramp, only by the weak fallback at 30 min
    autoSearchStart(s, 0);
    r = AUTO_NONE;
    for (uint32_t t = 1000; t < 30 * MIN; t += 20000) assert(autoSearchPacket(s, 9, ITP, -125, false, t, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 9, ITP, -125, false, 30 * MIN + 1000, &c) == AUTO_LOCK_WEAK && c->id == 9);
    // 2 packets never lock, however late and strong
    autoSearchStart(s, 0);
    assert(autoSearchPacket(s, 10, ITP, -60, false, 25 * MIN, &c) == AUTO_NONE);
    assert(autoSearchPacket(s, 10, ITP, -60, false, 25 * MIN + 4000, &c) == AUTO_NONE);
    assert(autoSearchTick(s, 29 * MIN, &c) == AUTO_NONE);
    // V4: everything 20 dB higher: -90 locks at 10 min (thr -90), -105 at ~26 min... not before
    autoSearchStart(s, 0);
    r = AUTO_NONE; lockAt = 0;
    for (uint32_t t = 1000; t < 30 * MIN && r == AUTO_NONE; t += 20000) { r = autoSearchPacket(s, 11, ITP, -90, true, t, &c); lockAt = t; }
    assert(r == AUTO_LOCK && lockAt >= 10 * MIN && lockAt < 10 * MIN + 60000);
    autoSearchStart(s, 0);
    r = AUTO_NONE; lockAt = 0;
    for (uint32_t t = 1000; t < 30 * MIN && r == AUTO_NONE; t += 20000) { r = autoSearchPacket(s, 12, ITP, -98, true, t, &c); lockAt = t; }
    assert(r == AUTO_LOCK && lockAt >= 26 * MIN && lockAt < 26 * MIN + 30000);
    // two qualifying candidates: the stronger window mean wins, not the one that sent last
    autoSearchStart(s, 0);
    for (int k = 0; k < 3; k++) autoSearchPacket(s, 31, BR, -112.5f, false, 14 * MIN + k * 1000, &c);   // thr at 14:00 is -111.7: no
    for (int k = 0; k < 3; k++) autoSearchPacket(s, 30, FO, -113, false, 14 * MIN + 4000 + k * 1000, &c);
    assert(autoSearchTick(s, 14 * MIN + 8000, &c) == AUTO_NONE);
    assert(autoSearchTick(s, 17 * MIN, &c) == AUTO_LOCK && c->id == 31);   // thr -113.5: both qualify, -112.5 beats -113
    // the same via a packet: 30 (weaker) sends the packet that makes both qualify, 31 is chosen
    autoSearchStart(s, 0);
    for (int k = 0; k < 3; k++) autoSearchPacket(s, 31, BR, -112.5f, false, 14 * MIN + k * 1000, &c);
    for (int k = 0; k < 2; k++) autoSearchPacket(s, 30, FO, -113, false, 16 * MIN + k * 1000, &c);
    assert(autoSearchPacket(s, 30, FO, -113, false, 17 * MIN, &c) == AUTO_LOCK && c->id == 31);
    // millis() wrap during the search: start shortly before 2^32, -118 dBm packets continue across the wrap
    const uint32_t START = 0xFFFFFFFFUL - 5 * MIN;   // wraps 5 min into the search
    autoSearchStart(s, START);
    r = AUTO_NONE; lockAt = 0;
    for (uint32_t k = 0; k < 90 && r == AUTO_NONE; k++) { r = autoSearchPacket(s, 40, ITP, -118, false, START + 1000 + k * 20000, &c); lockAt = 1000 + k * 20000; }
    assert(r == AUTO_LOCK && lockAt >= 26 * MIN && lockAt < 26 * MIN + 30000);
    // weak fallback across the wrap as well
    autoSearchStart(s, START);
    for (uint32_t k = 0; k < 3; k++) autoSearchPacket(s, 41, ITP, -125, false, START + 1000 + k * 4000, &c);
    assert(autoSearchTick(s, START + AUTO_WEAK_AFTER_MS - 1, &c) == AUTO_NONE);
    assert(autoSearchTick(s, START + AUTO_WEAK_AFTER_MS, &c) == AUTO_LOCK_WEAK && c->id == 41);
    // candidate list full across the wrap: the one heard longest ago (before the wrap) makes room
    autoSearchStart(s, START);
    autoSearchPacket(s, 100, ITP, -130, false, START + 1000, &c);
    for (uint32_t k = 1; k < AUTO_CAND_MAX; k++) autoSearchPacket(s, 100 + k, ITP, -130, false, START + 6 * MIN + k * 1000, &c);
    autoSearchPacket(s, 200, ITP, -130, false, START + 7 * MIN, &c);
    {
        bool has100 = false, has101 = false, has200 = false;
        for (const AutoCand& x : s.c) { has100 |= x.id == 100; has101 |= x.id == 101; has200 |= x.id == 200; }
        assert(!has100 && has101 && has200);
    }
    puts("rfautosearch ok");
    return 0;
}
