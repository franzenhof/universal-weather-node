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
    // candidate list full across the millis() wrap: the one heard longest ago (before the wrap) makes room
    const uint32_t MIN = 60000UL, START = 0xFFFFFFFFUL - 5 * MIN;   // wraps 5 min into the search
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
