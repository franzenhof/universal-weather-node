#include "rf_autosearch.h"

float autoRssiThreshold(bool v4) { return AUTO_MIN_RSSI + (v4 ? AUTO_V4_RSSI_OFFSET : 0.0f); }

void autoSearchStart(AutoSearch& s, uint32_t now)
{
    s = AutoSearch();
    s.running = true;
    s.startMs = now;
}

AutoResult autoSearchPacket(AutoSearch& s, uint32_t id, uint8_t proto, float rssi, bool v4, uint32_t now,
                            const AutoCand** chosen)
{
    if (!s.running) autoSearchStart(s, now);
    AutoCand* c = nullptr;
    for (AutoCand& x : s.c) if (x.id == id) c = &x;
    if (!c) {   // a free entry, else the one heard longest ago
        for (AutoCand& x : s.c) if (!x.id) { c = &x; break; }
        if (!c) { c = &s.c[0]; for (AutoCand& x : s.c) if (now - x.lastMs > now - c->lastMs) c = &x; }   // age: wrap-safe
        *c = AutoCand();
        c->id = id;
    }
    if (!c->inWindow || now - c->windowStart > AUTO_WINDOW_MS) {   // window over: count again from this packet
        c->inWindow = 0;
        c->rssiSum = 0.0f;
        c->windowStart = now;
    }
    c->proto = proto;
    c->inWindow++;
    c->total++;
    c->rssiSum += rssi;
    c->lastMs = now;
    if (rssi > c->bestRssi) c->bestRssi = rssi;
    if (c->inWindow >= AUTO_MIN_PACKETS && c->meanRssi() >= autoRssiThreshold(v4)) {
        if (chosen) *chosen = c;
        return AUTO_LOCK;
    }
    return autoSearchTick(s, now, chosen);
}

AutoResult autoSearchTick(AutoSearch& s, uint32_t now, const AutoCand** chosen)
{
    if (!s.running || now - s.startMs < AUTO_WEAK_AFTER_MS) return AUTO_NONE;
    const AutoCand* best = nullptr;
    for (const AutoCand& x : s.c) {
        if (!x.id || x.total < AUTO_MIN_PACKETS) continue;
        if (!best || x.total > best->total || (x.total == best->total && x.bestRssi > best->bestRssi)) best = &x;
    }
    if (!best) return AUTO_NONE;
    if (chosen) *chosen = best;
    return AUTO_LOCK_WEAK;
}
