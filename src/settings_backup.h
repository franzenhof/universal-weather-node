#pragma once
// Settings backup / restore (docs/settings.md "Backup and restore"): the
// file format and the comparison with the current settings. The fields are
// the settings form's (name -> value as the form sends it); web_api.cpp
// records them by rendering the settings page into nothing and restores by
// sending them through the normal settings POST. Host-tested
// (test/firmware_backup.cpp).
#include <stddef.h>
#include <ArduinoJson.h>

constexpr int SETTINGS_BACKUP_VERSION = 1;   // a backup with a higher major version is refused

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
