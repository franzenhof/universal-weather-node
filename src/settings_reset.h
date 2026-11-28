#pragma once
// The two resets of Settings -> System (docs/settings.md#reset):
//
// "Reset settings" (also downlink 0xFF and MQTT "factoryReset"): every
//   setting back to its default except how the node is reached and who may
//   administer it (settingsResetKeptInto()); the Wi-Fi credentials (stock
//   "nvs"), the LoRaWAN session, the measurement data (records, rain
//   statistics, chart history), the reset history and the core dump stay.
//   Cleared: the namespaces "config", "alarms", "cupload" of cfgdata.
// "Erase everything" (hand the device on, web only): cfgdata, the history
//   partition, the core dump and the stock "nvs" partition (Wi-Fi, LoRaWAN
//   session and DevNonces) are erased, the RTC RAM totals forgotten; the
//   node starts like a new one, with the setup portal.
//
// Both seal the storage first (storage_write.h): nothing a running task
// saves can bring old data back between the clear / erase and the restart
// ("Reset settings" clears only once every open write session has closed;
// an erase invalidates their handles).
#include "config.h"

// The settings "Reset settings" keeps, copied from `current` into `fresh`
// (a default Config): provisioned, language, device name, Wi-Fi / web
// switches, static IP, own access point, admin password, LoRaWAN switch,
// keys and region. Everything else stays at its default. Host-tested.
void settingsResetKeptInto(const Config& current, Config& fresh);

enum StorageResetKind : uint8_t { STORAGE_RESET_SETTINGS = 1, STORAGE_RESET_ERASE_EVERYTHING = 2 };

// ---- device part (settings_reset_esp.cpp) ----
// Asks for a reset; done by storageResetPoll() in the loop about a second
// later (the web answer gets out first), ends in a restart. An "Erase
// everything" pending is not replaced by a later "Reset settings". false =
// not enough memory for "Reset settings" (its buffer is allocated here, so
// "started" is only answered when the reset can run) - logged, nothing
// requested.
bool storageResetRequest(StorageResetKind kind);
void storageResetPoll();   // loop()
