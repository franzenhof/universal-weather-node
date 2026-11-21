#pragma once
// Wireless sensor type "Auto (search)": when a sensor type is locked and
// saved. Host-tested (test/firmware_rfautosearch.cpp).
//
// A type is locked only after AUTO_MIN_PACKETS valid packets of the same
// sensor ID within AUTO_WINDOW_MS (IT+ sends every ~4 s, Bresser 12-16 s,
// Fine Offset up to ~80 s: 3 packets fit 10 min for every family) with a mean
// RSSI of at least AUTO_MIN_RSSI - a neighbour's sensor at the edge of the
// range (a field node locked onto one at -114 dBm with 30 of ~1800 packets)
// no longer ends the search. The Heltec V4's LNA reads ~17-23 dB higher:
// there the threshold is AUTO_V4_RSSI_OFFSET higher. Weaker sensors stay in
// the table; if after AUTO_WEAK_AFTER_MS only weak ones were heard, the best
// (most packets, then strongest) is locked anyway and marked "weak signal".
#include <stdint.h>

constexpr int      AUTO_CAND_MAX        = 8;
constexpr int      AUTO_MIN_PACKETS     = 3;
constexpr uint32_t AUTO_WINDOW_MS       = 600000UL;    // 10 min
constexpr float    AUTO_MIN_RSSI        = -105.0f;     // dBm (V3)
constexpr float    AUTO_V4_RSSI_OFFSET  = 20.0f;       // V4 front-end gain
constexpr uint32_t AUTO_WEAK_AFTER_MS   = 1800000UL;   // 30 min

struct AutoCand {
    uint32_t id = 0;            // 0 = free
    uint8_t  proto = 0;         // RF_PROTO_* the packet came with
    uint8_t  inWindow = 0;      // packets since windowStart
    uint16_t total = 0;         // packets since the search started
    uint32_t windowStart = 0, lastMs = 0;
    float    rssiSum = 0.0f;    // of the packets in the window
    float    bestRssi = -200.0f;
    float meanRssi() const { return inWindow ? rssiSum / inWindow : -200.0f; }
};

struct AutoSearch {
    bool     running = false;
    uint32_t startMs = 0;
    AutoCand c[AUTO_CAND_MAX];
};

enum AutoResult : uint8_t { AUTO_NONE = 0, AUTO_LOCK, AUTO_LOCK_WEAK };

float autoRssiThreshold(bool v4);
void  autoSearchStart(AutoSearch& s, uint32_t now);
// One valid packet; AUTO_LOCK + *chosen when this sensor fulfils the rule.
AutoResult autoSearchPacket(AutoSearch& s, uint32_t id, uint8_t proto, float rssi, bool v4, uint32_t now,
                            const AutoCand** chosen);
// Without new packets: the weak fallback after AUTO_WEAK_AFTER_MS.
AutoResult autoSearchTick(AutoSearch& s, uint32_t now, const AutoCand** chosen);
