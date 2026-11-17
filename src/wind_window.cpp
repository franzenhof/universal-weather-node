#include "wind_window.h"
#include <math.h>

namespace {
constexpr float DEG2RAD = 0.017453292519943295f;
constexpr float RAD2DEG = 57.29577951308232f;

uint16_t vectorMean(float s, float c)
{
    int deg = (int)lroundf(atan2f(s, c) * RAD2DEG);
    return (uint16_t)((deg % 360 + 360) % 360);
}

float speedOf(uint32_t pulses, uint32_t ms, float msPerPps)
{
    return ms ? (float)pulses * 1000.0f / (float)ms * msPerPps : 0.0f;
}
} // namespace

// ---------------------------------------------------------------- WindRing
void WindRing::clear() { head = 0; n = 0; }

const WindRing::Entry& WindRing::back(size_t i) const
{
    return buf[(head + WIND_RING_SIZE - 1 - i) % WIND_RING_SIZE];
}

// 3-s mean ending at back(i): walk back until ~3 s are covered. 90 % is
// enough, so three 1-s samples that were a few ms short still form one
// window instead of pulling in a fourth sample.
float WindRing::gustEndingAt(size_t i, float msPerPps) const
{
    uint32_t p = 0, ms = 0;
    for (size_t k = i; k < n; k++) {
        const Entry& e = back(k);
        p  += e.pulses;
        ms += e.ms;
        if (ms >= WIND_GUST_SPAN_MS * 9 / 10) break;
    }
    return speedOf(p, ms, msPerPps);
}

float WindRing::push(uint32_t pulses, uint16_t elapsedMs, uint16_t dirDeg, bool hasDir, float msPerPps)
{
    buf[head] = { pulses, elapsedMs, hasDir ? (uint16_t)(dirDeg % 360) : NO_DIR };
    head = (head + 1) % WIND_RING_SIZE;
    if (n < WIND_RING_SIZE) n++;
    return gustEndingAt(0, msPerPps);
}

WindStats WindRing::stats(uint32_t spanMs, float msPerPps) const
{
    WindStats r;
    uint32_t p = 0, ms = 0, dirs = 0;
    float s = 0.0f, c = 0.0f, gust = 0.0f;
    for (size_t i = 0; i < n && ms < spanMs; i++) {
        const Entry& e = back(i);
        // Only gust windows that lie completely inside the span: the window
        // ending at entry i reaches 3 s back from the end of that entry.
        uint32_t endAge = ms;   // time from now back to the end of entry i
        p  += e.pulses;
        ms += e.ms;
        if (e.dir != NO_DIR) { s += sinf(e.dir * DEG2RAD); c += cosf(e.dir * DEG2RAD); dirs++; }
        if (endAge + WIND_GUST_SPAN_MS * 9 / 10 <= spanMs || i == 0) {
            float g = gustEndingAt(i, msPerPps);
            if (g > gust) gust = g;
        }
    }
    if (ms == 0) return r;
    r.valid  = true;
    r.meanMs = speedOf(p, ms, msPerPps);
    r.gustMs = gust > r.meanMs ? gust : r.meanMs;
    if (dirs) { r.dirValid = true; r.dirDeg = vectorMean(s, c); }
    return r;
}

// ----------------------------------------------------------------- WindAcc
void WindAcc::reset() { *this = WindAcc(); }

void WindAcc::add(uint32_t p, uint16_t elapsedMs, uint16_t dirDeg, bool hasDir, float gustMs)
{
    pulses += p;
    ms     += elapsedMs;
    if (hasDir) { sinSum += sinf(dirDeg * DEG2RAD); cosSum += cosf(dirDeg * DEG2RAD); dirCount++; }
    if (!any || gustMs > gustMax) gustMax = gustMs;
    any = true;
}

WindStats WindAcc::result(float msPerPps) const
{
    WindStats r;
    if (!any || ms == 0) return r;
    r.valid  = true;
    r.meanMs = speedOf(pulses, ms, msPerPps);
    // The 3-s gust can start before this window (it looks back over the
    // ring), but it never reports less than the window's own mean.
    r.gustMs = gustMax > r.meanMs ? gustMax : r.meanMs;
    if (dirCount) { r.dirValid = true; r.dirDeg = vectorMean(sinSum, cosSum); }
    return r;
}
