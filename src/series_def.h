#pragma once
// Custom series ("eigene Messreihen", Settings -> Sensors): values that are
// not one of the main weather values - soil moisture, lightning, leaf
// wetness, PM2.5, CO2, a DS18B20 probe ... - get their own chart. Each has a
// free-text name and a source: a wireless sensor (ID) and one of its values,
// or a probe on the board (id 0). The aggregation into the history buckets is
// fixed per value: mean, lightning strikes = sum of the counter's increments
// per bucket, storm distance = minimum.
//
// This part has no firmware dependencies (host-tested,
// test/firmware_seriesdef.cpp): the value table, the stored form (one NVS
// string) and the int16 scaling of the history rings. The values per cycle:
// series.cpp; the rings: history.cpp.
#include <stddef.h>
#include <stdint.h>

constexpr int SERIES_MAX      = 8;    // slots; usable: seriesLimit() (series.h) - 4 without PSRAM
constexpr int SERIES_NAME_LEN = 24;   // bytes incl. terminator
constexpr int16_t SERIES_GAP  = INT16_MIN;   // "no value" in a ring entry

enum SeriesField : uint8_t {
    SF_TEMP = 0, SF_HUM, SF_MOIST, SF_LEAF, SF_STRIKES, SF_DIST, SF_PM25, SF_PM10, SF_CO2,
    SF_EC, SF_PRESS, SF_LUX, SF_UV, SF_BATT, SF_SUPERCAP,
    SF_DS1, SF_DS2, SF_DS3,   // board DS18B20 probes 1-3 (rfId 0)
    SF_COUNT
};
enum SeriesAgg : uint8_t { SA_MEAN = 0, SA_SUM, SA_MIN };

struct SeriesFieldInfo {
    const char* key;      // stored form, API ("moist", "strikes" ...)
    const char* unit;     // metric unit ("" = none); temperatures follow the device units in the API
    float       step;     // ring resolution: value / step as int16 (0.1 -> one decimal)
    uint8_t     decimals; // shown
    uint8_t     agg;      // SeriesAgg
    bool        isTemp;   // converted to °F with imperial units
    bool        board;    // a board probe, no wireless sensor
};
extern const SeriesFieldInfo SERIES_FIELDS[SF_COUNT];

struct SeriesDef {
    char     name[SERIES_NAME_LEN] = "";
    uint32_t rfId  = 0;          // wireless sensor ID; 0 for a board probe
    uint8_t  field = SF_COUNT;   // SF_COUNT = slot unused
    bool used() const { return field < SF_COUNT && name[0] && (rfId || SERIES_FIELDS[field].board); }
};

// Stored form: one line per slot "name|IDHEX|key", '\n' between; empty
// slots as empty lines. Unknown keys / damaged lines give an unused slot.
size_t seriesPack(const SeriesDef defs[SERIES_MAX], char* out, size_t len);
void   seriesUnpack(const char* s, SeriesDef defs[SERIES_MAX]);
// A form value as a name: no '|', no control characters, trimmed, cut to
// SERIES_NAME_LEN - 1 bytes without splitting a UTF-8 character.
void   seriesCleanName(const char* in, char* out, size_t len);
int    seriesFieldByKey(const char* key);   // -1 = unknown

// Identity of a slot's data (name, source, value): its stored history only
// belongs to a definition with the same key (history_store.cpp).
uint32_t seriesKey(const SeriesDef& d);

// value <-> ring entry
int16_t seriesEncode(uint8_t field, float v);
float   seriesDecode(uint8_t field, int16_t raw);   // raw != SERIES_GAP
