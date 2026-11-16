// Latest wireless-sensor values (filled by rf_sensors.cpp). Kept in its own
// file without RadioLib so the JSON/telemetry host tests can link it.
#include "rf_sensors.h"
#include "config.h"

RfSensorReading gRfSensors[RF_MAX_SENSORS];

#include <stdlib.h>

std::vector<uint32_t> rfParseIds(const char* s)
{
    std::vector<uint32_t> ids;
    const char* p = s;
    while (*p && ids.size() < RF_MAX_SENSORS) {
        while (*p == ' ' || *p == ',' || *p == ';') p++;
        if (!*p) break;
        char* end = nullptr;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        if (v) ids.push_back((uint32_t)v);
        p = end;
    }
    return ids;
}

const char* rfProtoName(int proto)
{
    switch (proto) {
        case RF_PROTO_ITPLUS17: return "IT+ 17.241 kbit/s";
        case RF_PROTO_ITPLUS9:  return "IT+ 9.579 kbit/s";
        default:                return "Bresser";
    }
}
