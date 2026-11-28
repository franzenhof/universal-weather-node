#pragma once
// Custom series ("eigene Messreihen", Settings -> Sensors): values that are
// not one of the main weather values - soil moisture, lightning, leaf
// wetness, PM2.5, CO2, a DS18B20 probe ... - get their own chart. Each has a
// free-text name and a source: a logical sensor (★, lsensors.h, by its
// internal ID) and one of its values, or a probe on the board (no ID). The
// aggregation into the history buckets (all three tiers and the downsampled
// points of /api/history alike) has a default per value - mean; lightning
// strikes = sum of the counter's increments per bucket; storm distance =
// minimum - and may be set per series: mean, minimum, maximum or sum.
//
// This part has no firmware dependencies (host-tested,
// test/firmware_seriesdef.cpp): the value table, the stored form (one NVS
// string) and the int16 scaling of the history rings. The values per cycle:
// series.cpp; the rings: history.cpp.
#include <stddef.h>
#include <stdint.h>

constexpr int SERIES_MAX      = 32;   // slots (Heltec V4, rings and definitions in the PSRAM)
constexpr int SERIES_NAME_LEN = 24;   // bytes incl. terminator
constexpr int16_t SERIES_GAP  = INT16_MIN;   // "no value" in a ring entry

enum SeriesField : uint8_t {
    SF_TEMP = 0, SF_HUM, SF_MOIST, SF_LEAF, SF_STRIKES, SF_DIST, SF_PM25, SF_PM10, SF_CO2,
    SF_EC, SF_PRESS, SF_LUX, SF_UV, SF_BATT, SF_SUPERCAP,
    SF_DS1, SF_DS2, SF_DS3,   // board DS18B20 probes 1-3 (no sensorInternalId)
    SF_LDIST,                 // LDS01 laser distance (air gap / level), mm
    SF_COUNT
};
enum SeriesAgg : uint8_t { SA_MEAN = 0, SA_SUM, SA_MIN, SA_MAX, SA_COUNT, SA_DEFAULT = 0xFF };
extern const char* const SERIES_AGG_KEYS[SA_COUNT];   // stored form, API: "mean", "sum", "min", "max"
int seriesAggByKey(const char* key);                  // -1 = unknown

struct SeriesFieldInfo {
    const char* key;      // stored form, API ("moist", "strikes" ...)
    const char* unit;     // metric unit ("" = none); temperatures follow the device units in the API
    float       step;     // ring resolution: value / step as int16 (0.1 -> one decimal)
    uint8_t     decimals; // shown
    uint8_t     agg;      // SeriesAgg: the default (a series may override it)
    bool        isTemp;   // converted to °F with imperial units
    bool        board;    // a board probe, no wireless sensor
    const char* haClass;  // Home Assistant device_class (nullptr = none)
};
extern const SeriesFieldInfo SERIES_FIELDS[SF_COUNT];

struct SeriesDef {
    char     name[SERIES_NAME_LEN] = "";
    char     sensorInternalId[8] = "";      // internal ID of the logical sensor ("L3F9A2C"); "" for a board probe
    uint8_t  field = SF_COUNT;   // SF_COUNT = slot unused
    uint8_t  agg = SA_DEFAULT;   // SeriesAgg; SA_DEFAULT = the value's own (SERIES_FIELDS[].agg)
    bool used() const { return field < SF_COUNT && name[0] && (sensorInternalId[0] ? !SERIES_FIELDS[field].board : SERIES_FIELDS[field].board); }
};

// The aggregation in effect (SA_MEAN for an unused slot).
uint8_t seriesAgg(const SeriesDef& d);
// Sets the aggregation; the value's default is stored as SA_DEFAULT (it then
// follows a change of the value).
void    seriesSetAgg(SeriesDef& d, uint8_t agg);

// Stored form: one line per slot "name|L3F9A2C|key" (a board probe: "name|B|key"),
// plus "|max" etc. when the aggregation is not the value's default;
// '\n' between; empty slots as empty lines. Unknown keys / damaged lines give
// an unused slot; a wireless line of an older firmware (radio ID, before A5c)
// too - no conversion.
constexpr size_t SERIES_PACKED_MAX = SERIES_MAX * (SERIES_NAME_LEN + 24);   // the longest stored form + terminator
size_t seriesPack(const SeriesDef defs[SERIES_MAX], char* out, size_t len);
void   seriesUnpack(const char* s, SeriesDef defs[SERIES_MAX]);
// A form value as a name: no '|', no control characters, trimmed, cut to
// SERIES_NAME_LEN - 1 bytes without splitting a UTF-8 character.
void   seriesCleanName(const char* in, char* out, size_t len);
int    seriesFieldByKey(const char* key);   // -1 = unknown

// Identity of a slot's data (name, source, value, an aggregation other than
// the default): its stored history only belongs to a definition with the
// same key (history_store.cpp).
uint32_t seriesKey(const SeriesDef& d);

// Home Assistant discovery of a defined slot (wifi_mqtt.cpp): object id /
// unique_id suffix "series_<slot>" (stable per slot), a value template that
// picks the slot out of the data topic's "series" array (json_payload.cpp;
// None while it is missing), unit (temperatures: °C / °F with fahrenheit) and
// device class.
struct SeriesHaEntity {
    char        key[12];
    char        tmpl[160];
    const char* unit;          // "" = none
    const char* deviceClass;   // nullptr = none
};
void seriesHaEntity(int slot, const SeriesDef& d, bool fahrenheit, SeriesHaEntity& out);

// value <-> ring entry
int16_t seriesEncode(uint8_t field, float v);
float   seriesDecode(uint8_t field, int16_t raw);   // raw != SERIES_GAP
