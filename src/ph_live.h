#pragma once
// Placeholder values from the live measurements (placeholders.h) - for the
// custom uploads and the alarm webhook / messages. Names (metric unless a
// unit suffix is given, see placeholders.h; docs/weather-networks.md):
//   temp hum dew feels qnh press ptend3h wind gust dir rain_day rain_1h
//   rain_24h rain_rate uv lux solar time_utc time_local epoch station lat lon
//   rfN_temp rfN_hum rfN_wind rfN_gust rfN_dir rfN_rain rfN_uv rfN_lux
//   rfN_batt (N = wireless sensor number 1-32, or its ID: rf300007DD_temp)
//   series_N (custom series, slot 1-32: its value of the last cycle, series.h)
#include "placeholders.h"
#include "derived_now.h"
#include "wind_window.h"
#include "sources.h"

struct PhLive {
    DerivedNow d;
    SrcValues  live;        // the scalar sources (temp/hum/press/uv/light); = gSrc (cycle) unless liveScalars
    bool       windValid = false;
    float      windMs = 0, gustMs = 0;
    int        dirDeg = 0;
    char       timeUtc[24] = "", timeLocal[24] = "", epoch[16] = "";
};

// Snapshot now. wind10: the 10-min WMO wind for uploads (nullptr = the last
// cycle's wind). liveScalars true (the display) reads the newest per-packet
// source values (srcLive()); false (uploads, alarms, API) keeps the cycle
// snapshot gSrc — derived values (dew, feels, tendency, rain, records) always
// stay on the cycle.
void phLiveInit(PhLive& l, const WindStats* wind10, bool liveScalars = false);
bool phLiveLookup(const char* name, PhValue& out, void* ctx);   // ctx = PhLive*

// The names above, for the settings page's list of placeholders
// (GET /api/placeholders): main values, and the fields of a wireless sensor
// (rf<ID>_<field>; rf<N>_<field> works as well).
extern const char* const PH_LIVE_NAMES[];
extern const int         PH_LIVE_NAME_COUNT;
extern const char* const PH_RF_FIELDS[];
extern const int         PH_RF_FIELD_COUNT;
