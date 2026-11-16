#pragma once
// Snapshot of the derived values (derived.h) from the current measurements,
// the in-RAM history (trends, rolling rain sums) and the daily bookkeeping
// (wx_upload.cpp) - shared by JSON/MQTT, Home Assistant, the status page,
// the dashboard and the weather-network uploads. Base units throughout.
#include "derived.h"
#include "wx_upload.h"

struct DerivedNow {
    bool  dewValid = false;       float dewPointC = 0;
    bool  feelsValid = false;     float feelsLikeC = 0;
    bool  windChill = false;      // feelsLike is the wind chill
    bool  heatIndex = false;      // feelsLike is the heat index
    bool  cloudValid = false;     float cloudBaseM = 0;
    const char* windDirText = nullptr;
    bool  pressTrendValid = false; float pressDelta3hHpa = 0;
    PressureTendency tendency = PT_STEADY;
    const char* weatherTrend = nullptr;
    bool  tempTrendValid = false; float tempDelta1hC = 0;
    bool  rainValid = false;      float rain1hMm = 0, rain24hMm = 0;
    bool  sunValid = false;       // a light source is selected (sunshine duration)
    bool  warnValid = false;      uint8_t warnings = 0;
    WxDailyStats daily;
};

// windMs/dirDeg: the wind values of the caller's path (or the base cycle).
void derivedNow(DerivedNow& d, float windMs, int dirDeg);
