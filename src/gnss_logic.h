#pragma once

// GNSS rules without hardware (B9, host-tested in test/firmware_gnss.cpp):
// when a fix is good enough, when position / altitude go into the settings,
// how a sentence is timestamped, the baud search.

#include <math.h>
#include <stdint.h>

// Good enough to take position / altitude over: a real fix (not dead
// reckoning), at least 4 satellites (3D: altitude valid) and HDOP <= 2.
inline bool gnssFixGood(uint8_t quality, uint8_t sats, float hdop)
{
    return quality >= 1 && quality != 6 && sats >= 4 && hdop > 0.0f && hdop <= 2.0f;
}

// A longitude (or a difference of two) into -180..180 (the antimeridian).
inline double gnssWrapLon(double d)
{
    while (d > 180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return d;
}

// Distance (m) between two positions, equirectangular - exact enough for
// the 100-m rule.
inline double gnssDistanceM(double lat1, double lon1, double lat2, double lon2)
{
    const double k = 3.14159265358979323846 / 180.0;
    const double x = gnssWrapLon(lon2 - lon1) * k * cos((lat1 + lat2) * 0.5 * k);
    const double y = (lat2 - lat1) * k;
    return sqrt(x * x + y * y) * 6371000.0;
}

// The settings are written only on a real change (NVS wear; GNSS noise is a
// few metres): position > 100 m away (or none stored), altitude > 5 m.
constexpr double GNSS_POS_UPDATE_M = 100.0;
constexpr float  GNSS_ALT_UPDATE_M = 5.0f;
inline bool gnssPositionChanged(bool haveStored, double lat0, double lon0, double lat, double lon)
{
    return !haveStored || gnssDistanceM(lat0, lon0, lat, lon) > GNSS_POS_UPDATE_M;
}
inline bool gnssAltitudeChanged(int stored, float alt) { return fabsf(alt - (float)stored) > GNSS_ALT_UPDATE_M; }
// Altitude as the setting takes it (0-4000 m, like Settings -> Location).
inline int gnssAltitudeSetting(float alt)
{
    const long a = lroundf(alt);
    return a < 0 ? 0 : a > 4000 ? 4000 : (int)a;
}

// Mean over a window of good fixes (5 min at one fix per second) before the
// comparison: single fixes wander by metres, the altitude by more. The
// longitude is averaged as the offset from the window's first fix, so fixes
// on both sides of the antimeridian (+-180) do not average to 0.
struct GnssMean {
    static constexpr uint16_t WINDOW = 300;
    double   lon0 = 0, sLat = 0, sDLon = 0, sAlt = 0;
    uint16_t n = 0, nAlt = 0;
    // true when the window is full: the means are in mLat / mLon (and mAlt
    // when mAltOk), and the next window starts.
    bool add(double lat, double lon, bool altOk, float alt, double& mLat, double& mLon, bool& mAltOk, float& mAlt)
    {
        if (!n) lon0 = lon;
        sLat += lat; sDLon += gnssWrapLon(lon - lon0); n++;
        if (altOk) { sAlt += alt; nAlt++; }
        if (n < WINDOW) return false;
        mLat = sLat / n; mLon = gnssWrapLon(lon0 + sDLon / n);
        mAltOk = nAlt >= WINDOW / 2;
        mAlt = nAlt ? (float)(sAlt / nAlt) : 0.0f;
        reset();
        return true;
    }
    void reset() { *this = GnssMean(); }
};

// Which PPS edge a time sentence belongs to: a module sends one burst of
// sentences per second, after that second's edge. At 9600 baud the burst can
// last most of the second, so a late sentence (RMC, ZDA) may be read after
// the *next* edge. The edge count is noted when a burst starts - the first
// bytes after a poll that read none - and the latest edge belongs to the
// sentence only if no edge came since. A burst without an idle poll before
// it (the UART never went quiet) is unknown: no edge belongs to it.
struct GnssBurst {
    bool     idle = false, known = false;
    uint32_t edges = 0;
    // every poll: got = it read bytes; edgesBefore = PPS edge count read
    // before the first byte
    void poll(bool got, uint32_t edgesBefore)
    {
        if (!got) { idle = true; return; }
        if (idle) { known = true; edges = edgesBefore; }
        idle = false;
    }
    // the latest edge (edge count now) started this burst's second
    bool edgeOfBurst(uint32_t edgesNow) const { return known && edgesNow == edges; }
};

// Timestamp of a time sentence. With PPS: the edge of its burst (GnssBurst)
// is the start of its (whole) second, if the sentence came at most 900 ms
// after it - accurate to the interrupt latency (20 ms claimed). Otherwise:
// the time the line was read (modules send it 0.05-0.5 s into the second), 1 s.
constexpr uint32_t GNSS_PPS_WINDOW_MS = 900;
constexpr uint32_t GNSS_ACC_PPS_MS = 20, GNSS_ACC_NMEA_MS = 1000;
inline bool gnssTimeStamp(uint64_t lineMs, bool havePps, bool edgeOfBurst, uint64_t ppsMs, uint32_t msOfDay,
                          uint64_t& rxMs, uint32_t& accMs)
{
    if (havePps && edgeOfBurst && msOfDay % 1000 == 0 && lineMs >= ppsMs && lineMs - ppsMs <= GNSS_PPS_WINDOW_MS) {
        rxMs = ppsMs;
        accMs = GNSS_ACC_PPS_MS;
        return true;
    }
    rxMs = lineMs;
    accMs = GNSS_ACC_NMEA_MS;
    return false;
}

// A line read now has a usable arrival time only if the previous poll emptied
// the UART buffer and was at most 250 ms ago (the main loop polls every
// ~50 ms; after a stall - a LoRaWAN join, light sleep - the buffered lines
// are old: used for position / status, not for the clock).
constexpr uint32_t GNSS_TIMELY_GAP_MS = 250;
inline bool gnssLineTimely(bool drainedBefore, uint64_t gapMs) { return drainedBefore && gapMs <= GNSS_TIMELY_GAP_MS; }

// Baud: 0 = search 9600 -> 115200 -> 38400 (L76K: 9600, UC6580: 115200 by default).
inline bool gnssBaudValid(long b) { return b == 0 || b == 9600 || b == 38400 || b == 115200; }
inline uint32_t gnssNextBaud(uint32_t cur) { return cur == 9600 ? 115200 : cur == 115200 ? 38400 : 9600; }
constexpr uint32_t GNSS_BAUD_TRY_MS = 3000;     // per rate while searching
constexpr uint32_t GNSS_SILENT_MS = 30000;      // no valid sentence this long: search again (module re-plugged)
