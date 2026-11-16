#pragma once
// Wind sampling ring buffer and per-path aggregation windows.
//
// Plain C++ (no Arduino dependencies) so it is host-testable
// (test/firmware_windows.cpp).
//
// The wind sensor is a pulse counter: every wind sample is "pulses counted
// over elapsedMs" plus one vane reading. Means are pulse-weighted
// (sum of pulses / sum of time), so samples of slightly different length
// (loop jitter, a sample delayed by a send) are weighted correctly.
//
//  * Gust: highest 3-second mean (WMO-No. 8). With 1 s sampling that is the
//    pulses of the last 3 samples, with 3 s sampling one sample.
//  * Each transmission path (LoRaWAN, MQTT, the base cycle) owns a WindAcc
//    that is reset when that path sends: "interval" values.
//  * The ring keeps the last 10 minutes for the WMO-style values
//    (10-min mean, highest 3-s gust and vector-mean direction of the last
//    10 minutes): weather networks, and LoRaWAN/MQTT in "10 min" mode.
#include <stdint.h>
#include <stddef.h>

constexpr uint32_t WIND_SPAN_10MIN_MS = 600000;
constexpr uint32_t WIND_GUST_SPAN_MS  = 3000;
// 10 min at 1 s sampling plus margin for samples shorter than 1 s
constexpr size_t   WIND_RING_SIZE     = 660;

struct WindStats {
    bool     valid    = false;   // at least one sample
    float    meanMs   = 0.0f;
    float    gustMs   = 0.0f;
    bool     dirValid = false;
    uint16_t dirDeg   = 0;
};

class WindRing {
public:
    void clear();
    // Adds one sample and returns the 3-s gust (m/s) ending at it.
    // dirDeg is ignored when hasDir is false.
    float push(uint16_t pulses, uint16_t elapsedMs, uint16_t dirDeg, bool hasDir, float msPerPps);
    // Statistics over the newest spanMs (or what is buffered, if less).
    WindStats stats(uint32_t spanMs, float msPerPps) const;
    size_t count() const { return n; }

private:
    struct Entry { uint16_t pulses; uint16_t ms; uint16_t dir; bool hasDir; };
    const Entry& back(size_t i) const;   // i = 0: newest
    float gustEndingAt(size_t i, float msPerPps) const;
    Entry  buf[WIND_RING_SIZE];
    size_t head = 0;   // next write position
    size_t n    = 0;
};

class WindAcc {
public:
    void reset();
    void add(uint16_t pulses, uint16_t elapsedMs, uint16_t dirDeg, bool hasDir, float gustMs);
    WindStats result(float msPerPps) const;

private:
    uint32_t pulses = 0, ms = 0, dirCount = 0;
    float    sinSum = 0.0f, cosSum = 0.0f, gustMax = 0.0f;
    bool     any = false;
};

// Rain of one path: difference of the running total since the path's last send.
struct RainWindow {
    float    markMm   = 0.0f;   // running total at the last reset
    uint32_t startMs  = 0;
    void  reset(float totalMm, uint32_t nowMs) { markMm = totalMm; startMs = nowMs; }
    // A reset of the running total (downlink 0x01) counts from zero again.
    float amount(float totalMm) const { return totalMm >= markMm ? totalMm - markMm : totalMm; }
    float rateMmH(float totalMm, uint32_t nowMs) const {
        uint32_t dt = nowMs - startMs;
        return dt >= 1000 ? amount(totalMm) / ((float)dt / 3600000.0f) : 0.0f;
    }
};

// The wind/rain values one path transmits (all in metric base units).
struct PathValues {
    float    windSpeedMs = 0.0f;
    float    windGustMs  = 0.0f;
    uint16_t windDirDeg  = 0;
    float    rainMm      = 0.0f;
    float    rainRateMmH = 0.0f;
};

// Wind aggregation of a path (config values loraWindAgg / mqttWindAgg).
enum WindAggMode : int { WIND_AGG_INTERVAL = 0, WIND_AGG_10MIN = 1 };

// Wind sample interval (config sampleSec): only 1 s and 3 s are offered;
// other stored or requested values map to a step (2 -> 1 s, 4 and more -> 3 s).
inline int windSampleSecNormalized(int sec) { return sec >= 3 ? 3 : 1; }
