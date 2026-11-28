#pragma once
// Names of wireless sensors ("Hochbeet"), by sensor ID: Config::rfNames as
// "ID:name|ID:name" (hex ID; '|' and control characters are not allowed in a
// name, at most RF_NAME_MAX bytes, cut at a UTF-8 character boundary).
// Host-tested (test/firmware_rfnames.cpp).
#include <stddef.h>
#include <stdint.h>

constexpr size_t RF_NAME_MAX = 24;

// false = no name for id (out = "")
bool rfNameLookup(const char* list, uint32_t id, char* out, size_t len);
// Sets (empty name: removes) the name of id; false = no room (list unchanged).
bool rfNameSet(char* list, size_t len, uint32_t id, const char* name);
// Cleans a name in place: no '|', no control characters, trimmed, <= RF_NAME_MAX bytes.
void rfNameClean(char* name);
// A name copied out of the tables (the ★ table may change on the web task
// while another task uses it - lsensors.h, "concurrent access").
struct RfNameBuf {
    char s[RF_NAME_MAX + 1];
    operator const char*() const { return s; }
};
// The name of id: its logical sensor's (★, lsensors.h), else the gCfg.rfNames
// entry ("" = none). Returned by value: keep it in a variable
// (`const RfNameBuf nm = rfNameOf(id);`), never in a `const char*`.
RfNameBuf rfNameOf(uint32_t id);

// How the outputs call a wireless sensor (A5c, docs/design/a5c-logical-sensors.md
// D7): a ★ sensor by its custom ID (MQTT topic, InfluxDB tag, /metrics label,
// Loxone, REST `sensor`), its name and internal ID; a received-only sensor by
// "rf" + its radio ID (as custom ID) and its rfNames entry.
// To ArduinoJson pass the fields as (const char*): a const char[N] is taken
// for a string literal there (kept by pointer, N - 1 bytes long).
struct RfLabel {
    char customId[17];             // ★: the custom ID; received-only: "rf" + radio ID (rf17000023)
    char displayName[RF_NAME_MAX + 1];
    char internalId[8];              // ★: the internal ID; "" = received-only
    char radio[9];            // the radio ID, 8 hex digits
    bool mine() const { return internalId[0] != '\0'; }
    // what a page / the OLED shows as its label: the name, else the custom ID
    const char* shown() const { return displayName[0] ? displayName : customId; }
};
RfLabel rfLabelOf(uint32_t id);
