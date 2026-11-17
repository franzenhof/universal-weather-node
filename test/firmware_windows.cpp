// Wind ring buffer, 3-s gust, per-path windows and rain differences
// (src/wind_window.cpp). msPerPps = 1: 1 pulse per second = 1 m/s.
#include "wind_window.h"
#include <cassert>
#include <cmath>

static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

int main()
{
    // ---- 3-s gust from 1-s samples: a single 1-s spike is averaged ----
    WindRing r;
    float g = 0;
    const uint16_t p1[] = {2, 2, 2, 8, 2, 2};
    float maxGust = 0;
    for (uint16_t p : p1) { g = r.push(p, 1000, 90, true, 1.0f); if (g > maxGust) maxGust = g; }
    assert(near(maxGust, 4.0f));          // (2+2+8)/3, not the 8 m/s spike
    WindStats st = r.stats(WIND_SPAN_10MIN_MS, 1.0f);
    assert(st.valid && near(st.meanMs, 18.0f / 6) && near(st.gustMs, 4.0f));
    assert(st.dirValid && st.dirDeg == 90);

    // ---- 3-s samples: each sample is one gust window ----
    WindRing r3;
    r3.push(3, 3000, 0, true, 1.0f);
    g = r3.push(24, 3000, 0, true, 1.0f);
    assert(near(g, 8.0f));

    // ---- jitter: three samples a few ms short still form one 3-s window ----
    WindRing rj;
    rj.push(30, 990, 0, true, 1.0f);
    rj.push(3, 990, 0, true, 1.0f);
    rj.push(3, 990, 0, true, 1.0f);
    g = rj.push(3, 990, 0, true, 1.0f);   // window = last 3 samples only
    assert(near(g, 9.0f / 2.97f));

    // ---- calibration factor (Davis 1.00584 m/s per pulse/s) ----
    WindRing rc;
    g = rc.push(10, 1000, 0, true, 1.00584f);
    assert(near(g, 10.0584f));

    // ---- 10-min span: older samples and their gusts are excluded ----
    WindRing rs;
    for (int i = 0; i < 100; i++) rs.push(20, 1000, 180, true, 1.0f);   // strong, older than 10 min
    for (int i = 0; i < 600; i++) rs.push(5, 1000, 0, true, 1.0f);      // last 10 min
    st = rs.stats(WIND_SPAN_10MIN_MS, 1.0f);
    assert(near(st.meanMs, 5.0f) && near(st.gustMs, 5.0f) && st.dirDeg == 0);
    st = rs.stats(60000, 1.0f);
    assert(near(st.meanMs, 5.0f));
    assert(rs.count() <= WIND_RING_SIZE);

    // ---- direction: vector mean across north ----
    WindAcc a;
    a.add(5, 1000, 350, true, 5.0f);
    a.add(5, 1000, 10, true, 5.0f);
    st = a.result(1.0f);
    assert(st.valid && st.dirValid && (st.dirDeg == 0 || st.dirDeg == 359));
    assert(near(st.meanMs, 5.0f));

    // ---- per-path windows are independent; reset only affects one ----
    WindAcc lora, mqtt;
    for (int i = 0; i < 10; i++) {
        uint16_t p = (uint16_t)(i < 5 ? 2 : 6);
        lora.add(p, 1000, 0, true, (float)p);
        mqtt.add(p, 1000, 0, true, (float)p);
        if (i == 4) mqtt.reset();             // MQTT sent after 5 s
    }
    assert(near(lora.result(1.0f).meanMs, 4.0f) && near(lora.result(1.0f).gustMs, 6.0f));
    assert(near(mqtt.result(1.0f).meanMs, 6.0f));
    // the gust never reports less than the window mean
    WindAcc low;
    low.add(9, 1000, 0, true, 1.0f);
    assert(near(low.result(1.0f).gustMs, 9.0f));
    // empty window: not valid
    WindAcc none;
    assert(!none.result(1.0f).valid);

    // ---- rain: running total, difference per path, total reset ----
    RainWindow lr, mr;
    lr.reset(10.0f, 0);
    mr.reset(10.0f, 0);
    float total = 10.6f;                      // 3 tips of 0.2 mm
    assert(near(lr.amount(total), 0.6f) && near(mr.amount(total), 0.6f));
    mr.reset(total, 5000);                    // MQTT sent at 5 s
    total += 0.2f;
    assert(near(lr.amount(total), 0.8f) && near(mr.amount(total), 0.2f));
    assert(near(lr.rateMmH(total, 3600000), 0.8f));
    assert(lr.rateMmH(total, 500) == 0.0f);   // < 1 s: no rate
    assert(near(lr.amount(0.4f), 0.4f));      // safety net: a total below the mark counts from zero

    // ---- more than 65535 pulses in one sample (high-pulse anemometer) ----
    WindRing rb;
    g = rb.push(100000, 3000, 0, false, 0.001f);
    assert(near(g, 100000.0f / 3.0f * 0.001f));
    st = rb.stats(WIND_SPAN_10MIN_MS, 0.001f);
    assert(st.valid && !st.dirValid);       // no vane reading stored as "no direction"
    WindAcc big;
    big.add(100000, 2000, 0, false, 0.0f);
    assert(near(big.result(0.001f).meanMs, 50.0f));

    // ---- WindGust (2nd anemometer): same 3-s window as the ring ----
    WindGust wg;
    float wgMax = 0;
    for (uint16_t p : p1) { float x = wg.push(p, 1000, 1.0f); if (x > wgMax) wgMax = x; }
    assert(near(wgMax, 4.0f));
    wg.clear();
    assert(near(wg.push(24, 3000, 1.0f), 8.0f));
    assert(near(wg.push(3, 990, 1.0f), 27.0f / 3.99f));   // 3990 ms incl. the previous sample

    // ---- wind sample interval steps ----
    assert(windSampleSecNormalized(1) == 1 && windSampleSecNormalized(2) == 1);
    assert(windSampleSecNormalized(3) == 3 && windSampleSecNormalized(5) == 3 && windSampleSecNormalized(0) == 1);
    return 0;
}
