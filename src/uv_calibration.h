#pragma once

#include <math.h>

// CayenneLPP analog input uses signed centi-units; use the same ceiling for
// binary telemetry and stored history so both protocols report consistently.
constexpr float UV_INDEX_REPORT_MAX = 327.67f;

inline bool uvCalibrationValid(bool calibrated, float slope, float offset)
{
    return calibrated && isfinite(slope) && slope > 0.0f && slope <= 30.0f &&
           isfinite(offset) && offset >= -10.0f && offset <= 10.0f;
}

inline float calculateUvIndex(float volts, float slope, float offset)
{
    if (!isfinite(volts) || volts < 0.0f || volts > 5.5f ||
        !isfinite(slope) || slope <= 0.0f || slope > 30.0f ||
        !isfinite(offset) || offset < -10.0f || offset > 10.0f)
        return NAN;
    float index = volts * slope + offset;
    if (!isfinite(index)) return NAN;
    if (index < 0.0f) return 0.0f;
    return index;
}
