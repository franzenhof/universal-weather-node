#include "rf_autosearch.h"

float autoRssiThreshold(bool v4, uint32_t elapsedMs)
{
    if (elapsedMs > AUTO_WEAK_AFTER_MS) elapsedMs = AUTO_WEAK_AFTER_MS;
    return AUTO_MIN_RSSI + (v4 ? AUTO_V4_RSSI_OFFSET : 0.0f) - AUTO_RAMP_DB * (float)elapsedMs / (float)AUTO_WEAK_AFTER_MS;
}

// The candidate with the strongest window mean that has enough packets in a
// still open window and reaches the threshold of this moment.
static const AutoCand* qualified(const AutoSearch& s, bool v4, uint32_t now)
{
    const float thr = autoRssiThreshold(v4, now - s.startMs);   // unsigned: survives the millis() wrap
    const AutoCand* best = nullptr;
    for (const AutoCand& x : s.c) {
        if (!x.id || x.inWindow < AUTO_MIN_PACKETS || now - x.windowStart > AUTO_WINDOW_MS) continue;
        if (x.meanRssi() >= thr && (!best || x.meanRssi() > best->meanRssi())) best = &x;
    }
    return best;
}

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
    return autoSearchTick(s, now, chosen, v4);
}

AutoResult autoSearchTick(AutoSearch& s, uint32_t now, const AutoCand** chosen, bool v4)
{
    if (!s.running) return AUTO_NONE;
    if (const AutoCand* q = qualified(s, v4, now)) {
        if (chosen) *chosen = q;
        return AUTO_LOCK;
    }
    if (now - s.startMs < AUTO_WEAK_AFTER_MS) return AUTO_NONE;
    const AutoCand* best = nullptr;
    for (const AutoCand& x : s.c) {
        if (!x.id || x.total < AUTO_MIN_PACKETS) continue;
        if (!best || x.total > best->total || (x.total == best->total && x.bestRssi > best->bestRssi)) best = &x;
    }
    if (!best) return AUTO_NONE;
    if (chosen) *chosen = best;
    return AUTO_LOCK_WEAK;
}
