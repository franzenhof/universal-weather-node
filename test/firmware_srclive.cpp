// Live source read (src/sources.cpp): srcLive() picks the same source as the
// cycle (srcUpdateCycle -> gSrc) but reflects the newest reading per packet,
// without touching gSrc. This is what the display reads for instantaneous values.
#include "globals.h"
#include "sensors.h"
#include "sources.h"
#include "rf_sensors.h"
#include "board_io.h"
#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    for (int i = 0; i < 6; i++) gCfg.srcSel[i] = SRC_AUTO;
    gCfg.calTempDc = 0; gCfg.calHumDpct = 0; gCfg.calPressDhpa = 0;
    bme280Present = true;
    bme280TempC = 20.0f; bme280Humidity = 50.0f;
    bme280PressAbs = 1000.0f; bme280PressSea = 1013.0f;   // auto -> the board feeds air + pressure

    srcUpdateCycle();
    assert(gSrc.tempValid  && std::fabs(gSrc.tempC - 20.0f) < 0.01f);
    assert(gSrc.humValid   && std::fabs(gSrc.humidity - 50.0f) < 0.01f);
    assert(gSrc.pressValid && std::fabs(gSrc.pressAbsHpa - 1000.0f) < 0.01f);

    // right after a cycle srcLive() == gSrc (same selection, same values)
    SrcValues live = srcLive();
    assert(live.tempValid && std::fabs(live.tempC - gSrc.tempC) < 0.001f);
    assert(std::fabs(live.pressAbsHpa - gSrc.pressAbsHpa) < 0.001f);
    assert(live.humValid == gSrc.humValid);

    // a newer board reading WITHOUT a cycle: srcLive() follows it, gSrc does not
    bme280TempC = 25.0f;
    bme280PressAbs = 1004.0f;
    live = srcLive();
    assert(std::fabs(live.tempC - 25.0f) < 0.01f);        // live = newest
    assert(std::fabs(live.pressAbsHpa - 1004.0f) < 0.01f);
    assert(std::fabs(gSrc.tempC - 20.0f) < 0.01f);        // the cycle snapshot is untouched
    assert(std::fabs(gSrc.pressAbsHpa - 1000.0f) < 0.01f);

    // the next cycle catches gSrc up to the current reading
    srcUpdateCycle();
    assert(std::fabs(gSrc.tempC - 25.0f) < 0.01f);
    assert(std::fabs(gSrc.pressAbsHpa - 1004.0f) < 0.01f);

    printf("Firmware srclive regressions passed.\n");
    return 0;
}
