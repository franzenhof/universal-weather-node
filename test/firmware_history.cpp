// History lookups (src/history.cpp): rain windows never longer than
// nominal, trend samples not younger than nominal.
// history.cpp is compiled into this file: the host build has no FreeRTOS
// mutex functions, and runtime.cpp's "no history" stubs are renamed away.
#define historyPressureAgo hostStubPressureAgo
#define historyTempAgo     hostStubTempAgo
#define historyRainSum     hostStubRainSum
#include "host/runtime.cpp"
#undef historyPressureAgo
#undef historyTempAgo
#undef historyRainSum

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#ifndef portMAX_DELAY
#define portMAX_DELAY 0
#endif
#define xSemaphoreTake(m, t) ((void)(m), (void)(t), true)
#define xSemaphoreGive(m)    ((void)(m), true)
#include "../src/history.cpp"

#include "sources.h"
#include <cassert>
#include <cmath>

static void runFor(unsigned long sec, unsigned long stepSec, float rainPerStep)
{
    for (unsigned long t = 0; t < sec; t += stepSec) {
        delay(stepSec * 1000);
        rainMM = rainPerStep;
        pushHistorySample();
    }
}

int main()
{
    hostNow = 0;
    gCfg.rainEn = 1; gCfg.windEn = 0; gCfg.rfEn = 0;   // rain from the board gauge
    gSrc.tempValid = true; gSrc.tempC = 10.0f;
    bme280Present = true; bme280PressSea = 1013.0f;
    assert(srcActive(SG_RAIN));

    // 1-min cycles, 0.1 mm each: 1 h = 60 cycles = 6.0 mm at most
    runFor(58 * 60, 60, 0.1f);
    float t, p;
    assert(!historyTempAgo(3600, t));             // only 58 min of data: no "1 h" trend yet
    runFor(9 * 60, 60, 0.1f);                     // 67 min
    assert(historyTempAgo(3600, t) && t == 10.0f);
    assert(!historyPressureAgo(3 * 3600, p));
    runFor(170 * 60, 60, 0.1f);                   // ~3 h 57 min
    assert(historyPressureAgo(3 * 3600, p));
    float h1 = historyRainSum(3600), h24 = historyRainSum(24 * 3600);
    assert(h1 <= 6.0f + 1e-3f && h1 >= 5.4f - 1e-3f);   // at most 1 h, at most one 5-min bucket less
    float total = 0.1f * (58 + 9 + 170);
    assert(std::fabs(h24 - total) < 0.01f);       // shorter than 24 h running: everything since start

    // 10-min cycles: a bucket then spans two cycles (20 min), still not more than 1 h
    runFor(5 * 3600, 600, 1.0f);
    h1 = historyRainSum(3600);
    assert(h1 <= 6.0f + 1e-3f && h1 >= 3.0f);
    return 0;
}
