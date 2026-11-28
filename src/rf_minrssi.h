#pragma once
// Minimum signal strength per wireless sensor (expert setting): Config::rfMinRssi
// as "ID:dBm|ID:dBm" (hex ID, integer dBm, e.g. "900181A2:-95"). A packet of that
// ID weaker than the minimum is dropped (a neighbour's twin with the same ID).
// Host-tested (test/firmware_rfminrssi.cpp).
#include <stddef.h>
#include <stdint.h>

constexpr int RF_MINRSSI_LO = -140;   // valid range, dBm
constexpr int RF_MINRSSI_HI = -20;

// The minimum of id in dBm; 0 = none set (also for an invalid entry).
int rfMinRssiLookup(const char* list, uint32_t id);
// Sets the minimum of id (0: removes it); false = outside RF_MINRSSI_LO..HI or no room (list unchanged).
bool rfMinRssiSet(char* list, size_t len, uint32_t id, int dbm);
// Parses "-95" (spaces allowed, optional "dBm"): false = not a number or out of range; "" gives 0 = remove.
bool rfMinRssiParse(const char* text, int& dbm);
// The gCfg.rfMinRssi entry of id (0 = none)
int rfMinRssiOf(uint32_t id);
