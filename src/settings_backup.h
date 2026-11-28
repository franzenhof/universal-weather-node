#pragma once
// Settings backup / restore (docs/settings.md "Backup and restore"): the
// file format and the comparison with the current settings. The fields are
// the settings form's (name -> value as the form sends it); web_api.cpp
// records them by rendering the settings page into nothing and restores by
// sending them through the normal settings POST. Host-tested
// (test/firmware_backup.cpp).
#include <stddef.h>
#include <ArduinoJson.h>
#include "lsensors.h"

// 2: the logical sensor table ("sensors") and references by internal ID (A5c).
// A version-1 file restores everything else; its ★ list and its references by
// radio ID (value sources, series) are ignored - not converted. A higher
// version is refused.
constexpr int SETTINGS_BACKUP_VERSION = 2;

// Write-only fields (passwords, keys): only in a backup when asked for.
bool backupIsSecret(const char* name);

void backupHeader(JsonDocument& doc, const char* firmware, const char* host, const char* date, bool secrets);

enum BackupCheck { BK_OK = 0, BK_INVALID, BK_NEWER };
BackupCheck backupCheck(JsonDocument& in);

// A field's value as the form would send it: strings as they are, true /
// false as "1" / "0", numbers as text, null as "".
void backupValue(JsonVariantConst v, char* out, size_t len);

// Before a restore: drops the fields the current settings page does not
// have (other hardware, older firmware; secrets are kept), lists what would
// change in `changes` ({key, old, new}; secrets only {key, secret: true}) and
// returns how many. secretSame(key, value): true = the stored secret is the same.
int backupPrepare(JsonObject set, JsonObjectConst cur, JsonArray changes, bool (*secretSame)(const char*, const char*));

// The logical sensor table as the backup's "sensors" array:
// [{"internalId": "L3F9A2C", "customId": "garten", "displayName": "Garten Süd", "radioId": "17000008", "minRssiDbm": -95}]
// (minRssiDbm only when set).
void backupSensors(JsonArray a, const LogicalSensorTable& t);
// The other way: false = not an array; entries that do not fit (lsAppend())
// are left out and counted in *skipped.
bool backupReadSensors(JsonVariantConst a, LogicalSensorTable& t, int* skipped = nullptr);
// Version 1: drops the references by radio ID ("r<ID>" value sources, series
// slots with a wireless source) - not converted; those keep their current setting.
void backupDropV1Refs(JsonObject set);
