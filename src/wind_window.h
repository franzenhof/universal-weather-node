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
    float push(uint32_t pulses, uint16_t elapsedMs, uint16_t dirDeg, bool hasDir, float msPerPps);
    // Statistics over the newest spanMs (or what is buffered, if less).
    WindStats stats(uint32_t spanMs, float msPerPps) const;
    size_t count() const { return n; }

private:
    // 32-bit pulses: a high-pulse anemometer can exceed 65535 in one sample.
    static constexpr uint16_t NO_DIR = 0xFFFF;
    struct Entry { uint32_t pulses; uint16_t ms; uint16_t dir; };   // dir NO_DIR = no vane reading
    const Entry& back(size_t i) const;   // i = 0: newest
    float gustEndingAt(size_t i, float msPerPps) const;
    Entry  buf[WIND_RING_SIZE];
    size_t head = 0;   // next write position
    size_t n    = 0;
};

class WindAcc {
public:
    void reset();
    void add(uint32_t pulses, uint16_t elapsedMs, uint16_t dirDeg, bool hasDir, float gustMs);
    WindStats result(float msPerPps) const;

private:
    uint32_t pulses = 0, ms = 0, dirCount = 0;
    float    sinSum = 0.0f, cosSum = 0.0f, gustMax = 0.0f;
    bool     any = false;
};

// 3-s gust alone, without the 10-min history (2nd anemometer, board_io.cpp):
// the same window as WindRing::push() over the last few samples. Header-only.
class WindGust {
public:
    void clear() { n = 0; }
    float push(uint32_t pulses, uint32_t elapsedMs, float msPerPps)
    {
        head = (head + 1) % N;
        p[head] = pulses; t[head] = elapsedMs;
        if (n < N) n++;
        uint32_t ps = 0, ms = 0;
        for (size_t k = 0; k < n; k++) {
            size_t i = (head + N - k) % N;
            ps += p[i]; ms += t[i];
            if (ms >= WIND_GUST_SPAN_MS * 9 / 10) break;
        }
        return ms ? (float)ps * 1000.0f / (float)ms * msPerPps : 0.0f;
    }

private:
    static constexpr size_t N = 8;   // samples are >= ~1 s, 3 s need 3 of them
    uint32_t p[N] = {}, t[N] = {};
    size_t   head = 0, n = 0;
};

// Spike guard for the board anemometer: a sub-sample above
// WIND_SPIKE_MIN_MS that is more than WIND_SPIKE_FACTOR times the strongest
// accepted sample of the last ~60 s is held until the next sample confirms it
// (at least half as fast) - a real gust front lasts several seconds, a burst
// on the pulse line (EMI, contact bounce) does not. Confirmed: both count
// (the gust appears one sample later); not confirmed: the held one is dropped.
// Off for now (WIND_SPIKE_GUARD_ON): could become a setting later.
constexpr bool     WIND_SPIKE_GUARD_ON = false;
constexpr float    WIND_SPIKE_MIN_MS = 20.0f;   // 72 km/h
constexpr float    WIND_SPIKE_FACTOR = 4.0f;
enum SpikeVerdict : uint8_t { SPIKE_TAKE, SPIKE_HOLD, SPIKE_TAKE_WITH_HELD };
struct WindSpikeGuard {
    SpikeVerdict check(float speedMs, uint32_t nowMs) {
        roll(nowMs);
        if (held) {
            held = false;
            if (speedMs >= heldMs * 0.5f) { note(heldMs); note(speedMs); return SPIKE_TAKE_WITH_HELD; }
        }
        if (speedMs > WIND_SPIKE_MIN_MS && speedMs > WIND_SPIKE_FACTOR * recentMax()) {
            held = true; heldMs = speedMs;
            return SPIKE_HOLD;
        }
        note(speedMs);
        return SPIKE_TAKE;
    }
private:
    static constexpr int      B = 4;
    static constexpr uint32_t BUCKET_MS = 15000;   // 4 x 15 s = the last minute
    float    mx[B] = {};
    uint32_t start = 0;
    bool     started = false, held = false;
    float    heldMs = 0.0f;
    void roll(uint32_t now) {
        if (!started) { started = true; start = now; return; }
        while (now - start >= BUCKET_MS) {
            for (int i = B - 1; i > 0; i--) mx[i] = mx[i - 1];
            mx[0] = 0.0f;
            start += BUCKET_MS;
            if (now - start > BUCKET_MS * B) { for (float& m : mx) m = 0.0f; start = now; }
        }
    }
    void  note(float s) { if (s > mx[0]) mx[0] = s; }
    float recentMax() const { float m = 0.0f; for (float v : mx) if (v > m) m = v; return m; }
};

// Rain of one path: difference of the running total since the path's last
// send. The total passed in must never go backwards - main.cpp uses
// rainWindowTotalMm() (sensors.h), which a reset of rainMMSinceStart
// (downlink 0x01 / MQTT) does not affect.
struct RainWindow {
    float    markMm   = 0.0f;   // running total at the last reset
    uint32_t startMs  = 0;
    void  reset(float totalMm, uint32_t nowMs) { markMm = totalMm; startMs = nowMs; }
    // Safety net only: a total below the mark counts from zero again.
    float amount(float totalMm) const { return totalMm >= markMm ? totalMm - markMm : totalMm; }
    float rateMmH(float totalMm, uint32_t nowMs) const {
        uint32_t dt = nowMs - startMs;
        return dt >= 1000 ? amount(totalMm) / ((float)dt / 3600000.0f) : 0.0f;
    }
};

// Rain rate over at least RAIN_RATE_MIN_MS, independent of how short the
// send windows are: with a 10-s cycle (fast MQTT) one 0.2-mm tip would
// otherwise read 72 mm/h. Keeps the running total every >= 30 s (16 entries,
// >= 8 min); the rate is the increase since the newest entry that is at
// least 5 min old. Shortly after the start (no entry old enough) the increase
// since the oldest entry is spread over 5 min - a low start, no spike.
constexpr uint32_t RAIN_RATE_MIN_MS = 300000;
struct RainRateRing {
    void push(float totalMm, uint32_t nowMs) {
        if (n && nowMs - ms[(head + N - 1) % N] < STEP_MS) return;
        tot[head] = totalMm; ms[head] = nowMs;
        head = (head + 1) % N;
        if (n < N) n++;
    }
    float rateMmH(float totalMm, uint32_t nowMs) const {
        if (!n) return 0.0f;
        size_t pick = (head + N - n) % N;              // oldest
        for (size_t k = 0; k < n; k++) {               // newest first
            size_t i = (head + N - 1 - k) % N;
            if (nowMs - ms[i] >= RAIN_RATE_MIN_MS) { pick = i; break; }
        }
        uint32_t dt = nowMs - ms[pick];
        if (dt < RAIN_RATE_MIN_MS) dt = RAIN_RATE_MIN_MS;
        float d = totalMm >= tot[pick] ? totalMm - tot[pick] : 0.0f;
        return d / ((float)dt / 3600000.0f);
    }
private:
    static constexpr size_t   N = 16;
    static constexpr uint32_t STEP_MS = 30000;
    float    tot[N] = {};
    uint32_t ms[N]  = {};
    size_t   head = 0, n = 0;
};

// The wind/rain values one path transmits (all in metric base units).
struct PathValues {
    bool     windValid   = false;   // at least one valid wind sample in this path's window
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
