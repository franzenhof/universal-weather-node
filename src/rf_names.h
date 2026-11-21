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
// The gCfg.rfNames entry of id (static buffer, "" = none)
const char* rfNameOf(uint32_t id);
