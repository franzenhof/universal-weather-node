#include "tls_gate.h"
#include <atomic>

TlsDecision tlsDecide(size_t freeHeap, size_t largest, bool webBusy)
{
    if (webBusy) return TLS_WAIT_WEB;
    if (largest < TLS_MIN_LARGEST || freeHeap < TLS_MIN_FREE) return TLS_WAIT_HEAP;
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
