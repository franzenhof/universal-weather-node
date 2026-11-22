#include "tls_gate.h"
#include <atomic>

TlsDecision tlsDecide(size_t internalFree, size_t internalLargest, size_t psramLargest, bool webBusy)
{
    if (webBusy) return TLS_WAIT_WEB;
    if (psramLargest >= 2 * TLS_MIN_LARGEST)   // record buffers fit in the PSRAM
        return internalFree >= TLS_MIN_FREE_PSRAM ? TLS_GO : TLS_WAIT_HEAP;
    if (internalLargest < TLS_MIN_LARGEST || internalFree < TLS_MIN_FREE) return TLS_WAIT_HEAP;
    return TLS_GO;
}

bool webShouldRefuse(bool tls, size_t freeHeap) { return tls && freeHeap < WEB_LOW_HEAP; }

namespace {
std::atomic<int>      heavyCount{0};
std::atomic<uint32_t> heavySince{0};
std::atomic<int>      tlsCount{0};
}

void webHeavyBegin(uint32_t now)
{
    if (heavyCount.fetch_add(1) == 0) heavySince.store(now ? now : 1);
}
void webHeavyEnd()
{
    if (heavyCount.fetch_sub(1) <= 1) { heavyCount.store(0); heavySince.store(0); }
}
bool webHeavyActive(uint32_t now)
{
    const uint32_t since = heavySince.load();
    if (!since || heavyCount.load() <= 0) return false;
    if (now - since > WEB_BUSY_MAX_MS) { heavyCount.store(0); heavySince.store(0); return false; }   // stuck: expire
    return true;
}
void tlsBegin() { tlsCount.fetch_add(1); }
void tlsEnd()   { if (tlsCount.fetch_sub(1) <= 1) tlsCount.store(0); }
bool tlsRunning() { return tlsCount.load() > 0; }
