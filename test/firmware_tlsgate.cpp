// HTTPS memory gate (src/tls_gate.cpp): thresholds, the web "busy" mark with
// its expiry, and the web side's 503 decision.
#include "tls_gate.h"
#include <cassert>
#include <cstdio>

int main()
{
    // enough memory, web idle: go
    assert(tlsDecide(108 * 1024, 80 * 1024, 0, false) == TLS_GO);
    // a fragmented heap (largest block 30 KiB, plenty free) is fine: the record buffers are ~16.7 KB each
    assert(tlsDecide(100 * 1024, 30 * 1024, 0, false) == TLS_GO);
    assert(TLS_MIN_LARGEST == 24 * 1024 && TLS_MIN_FREE == 70 * 1024);
    // the field case: 105 KiB free but a settings page in progress -> wait for the web task
    assert(tlsDecide(105 * 1024, 80 * 1024, 0, true) == TLS_WAIT_WEB);
    // largest block or free heap below the limits -> wait for memory
    assert(tlsDecide(108 * 1024, TLS_MIN_LARGEST - 1, 0, false) == TLS_WAIT_HEAP);
    assert(tlsDecide(TLS_MIN_FREE - 1, 80 * 1024, 0, false) == TLS_WAIT_HEAP);
    assert(tlsDecide(TLS_MIN_FREE, TLS_MIN_LARGEST, 0, false) == TLS_GO);
    // PSRAM (V4): the record buffers go there - internal RAM needs less, the
    // internal largest block does not matter; a small PSRAM block does not count
    assert(tlsDecide(40 * 1024, 8 * 1024, 1800 * 1024, false) == TLS_GO);
    assert(tlsDecide(TLS_MIN_FREE_PSRAM - 1, 30 * 1024, 1800 * 1024, false) == TLS_WAIT_HEAP);
    assert(tlsDecide(TLS_MIN_FREE_PSRAM, 1024, 2 * TLS_MIN_LARGEST, false) == TLS_GO);
    assert(tlsDecide(40 * 1024, 8 * 1024, 2 * TLS_MIN_LARGEST - 1, false) == TLS_WAIT_HEAP);
    assert(tlsDecide(108 * 1024, 80 * 1024, 1800 * 1024, true) == TLS_WAIT_WEB);

    // busy mark: nested, released, and expiring if a handler never ended it
    assert(!webHeavyActive(1000));
    webHeavyBegin(1000);
    webHeavyBegin(1500);                    // restore -> settings save (nested)
    assert(webHeavyActive(2000));
    webHeavyEnd();
    assert(webHeavyActive(2500));
    webHeavyEnd();
    assert(!webHeavyActive(3000));
    webHeavyEnd();                          // an extra end never goes negative
    webHeavyBegin(10000);
    assert(webHeavyActive(10000 + WEB_BUSY_MAX_MS));
    assert(!webHeavyActive(10001 + WEB_BUSY_MAX_MS));   // stuck: expired, no deadlock
    assert(!webHeavyActive(10002 + WEB_BUSY_MAX_MS));

    // TLS running mark and the web side's decision
    assert(!tlsRunning());
    tlsBegin();
    assert(tlsRunning() && webShouldRefuse(tlsRunning(), WEB_LOW_HEAP - 1) && !webShouldRefuse(tlsRunning(), WEB_LOW_HEAP));
    tlsEnd();
    tlsEnd();
    assert(!tlsRunning() && !webShouldRefuse(tlsRunning(), 1000));
    puts("tlsgate ok");
    return 0;
}
