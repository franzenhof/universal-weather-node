#pragma once
// Custom series (series_def.h) on the device: how many slots this board may
// use, and each slot's value for the finished measurement cycle.
#include "series_def.h"

// Usable slots: SERIES_MAX with a running PSRAM (Heltec V4), 4 on internal
// RAM (V3) - each series costs ~0.9 KB of rings (history.cpp).
int  seriesLimit();

// The finished cycle's value per slot (gCfg.series): ok[i] = false without
// a definition, a fresh value or (lightning) a previous count to compare
// with. Lightning strikes: the increase of the sensor's 8-bit counter since
// the previous cycle (wraps), summed per bucket by the history.
void seriesCycleValues(float v[SERIES_MAX], bool ok[SERIES_MAX]);

// Current value of a definition (settings page): false = none right now.
bool seriesValueNow(const SeriesDef& d, float& v);
