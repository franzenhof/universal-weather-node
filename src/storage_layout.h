#pragma once
// The storage layout of firmware 4.x, frozen with 4.0.0 (B11). The whole
// picture - partitions, namespaces, keys, formats - is in
// docs/design/storage-layout.md; this header is the one list of the format
// versions the code writes, and the hook point for later migrations.
//
// Rule after the release: any change to a stored format bumps its version
// here and teaches storageMigrateBlob() / storageMigrateConfig()
// (storage_migrate.cpp) to convert the previous version. A loader that meets
// an older version calls the hook; one that meets a newer version (a
// downgrade) leaves the data alone and starts from defaults without
// overwriting it: the settings blobs until the user saves settings, the
// stores written on their own (reset history, chart history tiers, records)
// until the user resets that store - meanwhile this firmware keeps their
// values in RAM only. The rain statistics are the exception (see
// docs/design/storage-layout.md#migrations).
//
// No Arduino / NVS here (host-tested: test/firmware_storage.cpp).
#include <stddef.h>
#include <stdint.h>

// ---- version of each own stored format (cfgdata partition unless noted) ----
// "config" namespace: one NVS key per setting; key "cfgver" holds this.
constexpr uint8_t STORED_CONFIG_VERSION          = 1;
// "config"/"lsens": logical sensor table (lsensors.h), byte 0
constexpr uint8_t STORED_LOGICAL_SENSORS_VERSION = 1;
// "config"/"netsens": network sensor slots (net_sensor.h), byte 0
constexpr uint8_t STORED_NETWORK_SENSORS_VERSION = 1;
// "config"/"vsens": virtual sensors (vsensor.h), byte 0
constexpr uint8_t STORED_VIRTUAL_SENSORS_VERSION = 1;
// "config"/"series": custom chart series (series_def.h), byte 0
constexpr uint8_t STORED_SERIES_VERSION          = 1;
// "alarms"/"chan": notification channels (alarms.h), byte 0. The rules
// blob "alarms"/"rules" carries its version in the magic ("ALR2" = 2).
constexpr uint8_t STORED_ALARM_CHANNELS_VERSION  = 1;
// "cupload"/"cfg": custom HTTP uploads (custom_upload.h), byte 0
constexpr uint8_t STORED_CUSTOM_UPLOADS_VERSION  = 1;
// "diag"/"resets": reset history and the last crash (reset_history.h), byte 0
constexpr uint8_t STORED_RESET_HISTORY_VERSION   = 1;
// "config"/"https": the HTTPS certificate and private key (https_cert.h), byte 0
constexpr uint8_t STORED_HTTPS_VERSION           = 1;
// "history" partition, keys "fine" / "coarse" / "daily": "UWH" + this byte
constexpr uint8_t STORED_HISTORY_TIER_VERSION    = 4;
// Formats that already carried a version before the freeze (unchanged):
//   "rainstat"/"periods"  u16 version 2 (rain_stats.h RAIN_BLOB_VERSION)
//   "records"/"v1"        magic "REC1" (records.cpp)
//   "alarms"/"rules"      magic "ALR2" (alarms.cpp)
//   settings backup file  "version": 2 (settings_backup.h SETTINGS_BACKUP_VERSION)

// How much of a stored blob a loader reads, whatever its own format's current
// maximum is: an older version may be larger than the current layout (a
// format that shrank) and must still reach the migration hook. The loader
// checks what it takes - a current blob against its exact / maximum size,
// the hook's result like a current blob. Far above every cfgdata blob of 4.x
// (the largest: "config"/"https", HTTPS_BLOB_MAX = 12 KB; static_assert at
// each loader).
constexpr size_t STORED_BLOB_READ_MAX = 32 * 1024;
// The same for a chart history tier (own partition; 4.0.0: up to ~91 KB).
constexpr size_t STORED_HISTORY_TIER_READ_MAX = 256 * 1024;
inline bool storedBlobLengthReadable(size_t storedLen, size_t readMax = STORED_BLOB_READ_MAX)
{
    return storedLen > 0 && storedLen <= readMax;
}

// What a loader does with the version byte it found.
enum StoredVersionAction : uint8_t {
    STORED_TAKE = 0,   // the current version: read it
    STORED_MIGRATE,    // older: storageMigrateBlob() / storageMigrateConfig() first
    STORED_NEWER,      // written by a newer firmware (downgrade): not read, not overwritten
};
inline StoredVersionAction storedVersionAction(uint8_t stored, uint8_t current)
{
    return stored == current ? STORED_TAKE : stored < current ? STORED_MIGRATE : STORED_NEWER;
}

// Formats the blob hook knows (one per versioned blob above).
enum StoredFormat : uint8_t {
    SF_LOGICAL_SENSORS = 0, SF_NETWORK_SENSORS, SF_VIRTUAL_SENSORS, SF_SERIES,
    SF_ALARM_CHANNELS, SF_CUSTOM_UPLOADS, SF_RESET_HISTORY, SF_HISTORY_TIER, SF_HTTPS, SF_FORMAT_COUNT
};

// The migration hook for blobs: converts `in` (a blob of format f with the
// older version `from`, version byte included) into the current layout in
// `out` (outLen bytes of room) and returns its length, 0 = no conversion
// known - the loader then starts from defaults. 4.0.0 is the first layout,
// so nothing older exists yet and this returns 0 for every input.
size_t storageMigrateBlob(StoredFormat f, uint8_t from, const uint8_t* in, size_t len, uint8_t* out, size_t outLen);

// Blobs of the form [version byte][payload]: the payload if `in` has the
// current version and exactly 1 + payloadLen bytes (payloadLen 0 = any
// length), else nullptr. *action says why (STORED_TAKE on success).
const uint8_t* storedBlobPayload(const uint8_t* in, size_t len, uint8_t current, size_t payloadLen,
                                 StoredVersionAction* action = nullptr);

// What every loader of a [version][payload] blob calls: the payload of the
// current version, or - for an older version - the result of
// storageMigrateBlob() (written to `scratch`, scratchLen bytes; checked like
// a current blob). nullptr = not usable: start from defaults (a newer
// version is never migrated). *gotLen = the payload's length.
const uint8_t* storedBlobLoad(StoredFormat f, const uint8_t* in, size_t len, uint8_t current, size_t payloadLen,
                              uint8_t* scratch, size_t scratchLen, size_t* gotLen = nullptr);

// The migration hook for the "config" namespace (renamed, split or re-scaled
// keys): called by loadConfigInto() before any key is read when "cfgver" is
// older than STORED_CONFIG_VERSION. A config without "cfgver" is a new node
// or a factory reset: nothing to migrate. 4.0.0: no older key set exists.
void storageMigrateConfig(uint8_t from);
