// Status LED (src/status_led.cpp): heartbeat timing, flashes for packets of
// "mine" / all sensors, packets while off are never shown later.
#include "status_led.h"
#include <cassert>
#include <cstdio>

int main()
{
    StatusLedState s;
    // off: dark whatever arrives
    assert(!statusLedStep(LED_OFF, 0, 1, 5, s));
    // heartbeat: 40 ms every 2 s, packets do not matter
    s = StatusLedState();
    assert(statusLedStep(LED_HEARTBEAT, 2000, 0, 0, s));
    assert(statusLedStep(LED_HEARTBEAT, 2039, 0, 3, s));
    assert(!statusLedStep(LED_HEARTBEAT, 2040, 0, 3, s));
    assert(!statusLedStep(LED_HEARTBEAT, 3999, 0, 3, s));
    assert(statusLedStep(LED_HEARTBEAT, 4000, 0, 3, s));
    // mine: only "mine" packets flash
    s = StatusLedState();
    assert(!statusLedStep(LED_RX_MINE, 100, 0, 0, s));
    assert(!statusLedStep(LED_RX_MINE, 110, 0, 1, s));    // a neighbour
    assert(statusLedStep(LED_RX_MINE, 120, 1, 2, s));     // mine
    assert(statusLedStep(LED_RX_MINE, 159, 1, 2, s));
    assert(!statusLedStep(LED_RX_MINE, 160, 1, 2, s));
    assert(!statusLedStep(LED_RX_MINE, 5000, 1, 2, s));   // nothing new: stays dark (no heartbeat)
    // any: every packet
    assert(statusLedStep(LED_RX_ANY, 5010, 1, 3, s));
    assert(!statusLedStep(LED_RX_ANY, 5050, 1, 3, s));
    // packets received while off are not flashed when switched on
    assert(!statusLedStep(LED_OFF, 6000, 4, 9, s));
    assert(!statusLedStep(LED_RX_ANY, 6010, 4, 9, s));
    // counter wrap
    s = StatusLedState(); s.seenAny = 0xFFFFFFFFu;
    assert(statusLedStep(LED_RX_ANY, 7000, 0, 0, s));
    puts("Firmware statusled regressions passed.");
    return 0;
}
