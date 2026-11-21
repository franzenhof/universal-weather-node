#pragma once
// Placeholder values from the live measurements (placeholders.h) - for the
// custom uploads and the alarm webhook / messages. Names (metric unless a
// unit suffix is given, see placeholders.h; docs/weather-networks.md):
//   temp hum dew feels qnh press ptend3h wind gust dir rain_day rain_1h
//   rain_24h rain_rate uv lux solar time_utc time_local epoch station lat lon
//   rfN_temp rfN_hum rfN_wind rfN_gust rfN_dir rfN_rain rfN_uv rfN_lux
//   rfN_batt (N = wireless sensor number 1-32)
#include "placeholders.h"
#include "derived_now.h"
#include "wind_window.h"

struct PhLive {
    DerivedNow d;
    bool       windValid = false;
    float      windMs = 0, gustMs = 0;
    int        dirDeg = 0;
    char       timeUtc[24] = "", timeLocal[24] = "", epoch[16] = "";
};

// Snapshot now. wind10: the 10-min WMO wind for uploads (nullptr = the last cycle's wind).
void phLiveInit(PhLive& l, const WindStats* wind10);
bool phLiveLookup(const char* name, PhValue& out, void* ctx);   // ctx = PhLive*
