#pragma once
// Host-test helpers for the logical sensor table (src/lsensors.h): set the ★
// sensors from an ID list (as the old "Mine" text list) and read them back.
#include "lsensors.h"
#include "rf_sensors.h"
#include <string>

inline void hostSetMine(const char* list)
{
    gLogicalSensors = LogicalSensorTable();
    for (uint32_t id : rfParseIds(list)) lsAdd(gLogicalSensors, id, nullptr, lsRandom);
}

inline std::string hostMine()
{
    char b[RF_MAX_SENSORS * 10 + 1];
    rfFormatIds(lsRadioIds(gLogicalSensors), b, sizeof b);
    return b;
}
