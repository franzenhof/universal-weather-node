#pragma once
// Logical sensors (A5c, docs/design/a5c-logical-sensors.md). Naming: "ls" in a
// function name = logical sensor; spelled-out suffixes (ByInternalId, ByCustomId,
// ByRadioId). Every ★ wireless sensor gets three identities -
//   internal ID  "L3F9A2C" - random, never changes, used for everything stored
//                inside the node; shaped unlike any custom ID, so a mix-up shows
//   custom ID    "17000008" / "garten" - proposed: the radio ID at take-over;
//                the user may change it; what other systems and placeholders see
//   display name "Garten Süd" - free text, display only
// plus the current radio ID (re-bound after a battery change) and an optional
// minimum signal (dBm). The ★ order is the table order. No migration from the old
// ★ list (until 4.x is released, configs of dev builds are not carried over).
// Host-tested (test/firmware_lsensors.cpp); no Arduino / NVS here.
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include "storage_layout.h"

constexpr int    LS_MAX      = 32;
constexpr size_t LS_INTERNAL_ID_LEN  = 7;    // "L" + 6 hex
constexpr size_t LS_CUSTOM_ID_MAX  = 16;
constexpr size_t LS_DISPLAY_NAME_MAX = 24;   // = RF_NAME_MAX

struct LogicalSensor {
    char     internalId[LS_INTERNAL_ID_LEN + 1] = "";   // "" = free entry
    char     customId[LS_CUSTOM_ID_MAX + 1] = "";
    char     displayName[LS_DISPLAY_NAME_MAX + 1] = "";
    uint32_t radioId             = 0;    // current binding, 0 = none
    int8_t   minRssiDbm             = 0;    // dBm, 0 = none
};

struct LogicalSensorTable {
    LogicalSensor s[LS_MAX];
    int count() const;                   // used entries (they are kept packed at the front)
};

// Why a custom ID is refused (nullptr = valid). Rules: 1-16 characters of
// a-z A-Z 0-9 _ -; not the shape of an internal ID ("L" + 6 hex); not a
// built-in placeholder name or the start of one ("rain" would hide
// {rain_day}); not a legacy wireless placeholder ("rf3", "rf17000008").
enum LsIdProblem : uint8_t { LS_ID_OK = 0, LS_ID_EMPTY, LS_ID_TOO_LONG, LS_ID_CHARS, LS_ID_LOOKS_INTERNAL,
                             LS_ID_LOOKS_LEGACY, LS_ID_BUILTIN, LS_ID_BUILTIN_START };
LsIdProblem lsCustomIdCheck(const char* customId);   // the reason as a code (the web page translates it)
const char* lsCustomIdProblem(const char* customId);

// Index of the entry, -1 = none. Custom IDs compare case-insensitively.
int lsFindByInternalId(const LogicalSensorTable& t, const char* internalId);
int lsFindByCustomId(const LogicalSensorTable& t, const char* customId);
int lsFindByRadioId(const LogicalSensorTable& t, uint32_t radioId);

// A new internal ID: "L" + 6 hex from rnd(), redrawn until unused.
void lsNewInternalId(const LogicalSensorTable& t, uint32_t (*rnd)(), char out[LS_INTERNAL_ID_LEN + 1]);

// Appends a logical sensor for radioId (custom ID = its 8-digit hex ID, made
// unique with "-2", "-3" ... if taken). Returns the index, -1 = table full
// or radioId already bound.
int lsAdd(LogicalSensorTable& t, uint32_t radioId, const char* displayName, uint32_t (*rnd)());

// Changes a custom ID; false = invalid (lsCustomIdProblem) or used by another entry.
bool lsSetCustomId(LogicalSensorTable& t, int idx, const char* customId);
// Re-binds the radio ID (battery change); false = bound to another entry.
bool lsRebindRadioId(LogicalSensorTable& t, int idx, uint32_t radioId);
// Removes an entry and closes the gap (order kept).
void lsRemove(LogicalSensorTable& t, int idx);
// Moves an entry by delta (-1 up, +1 down) in the ★ order.
void lsMove(LogicalSensorTable& t, int idx, int delta);

// true = shaped like an internal ID ("L" + 6 hex digits, upper case as drawn).
// Inline: also used by modules that do not link the table (series_def.cpp).
inline bool lsIsInternalId(const char* s)
{
    if (!s || s[0] != 'L') return false;
    for (size_t i = 1; i <= LS_INTERNAL_ID_LEN; i++) {
        const char c = s[i];
        if (i == LS_INTERNAL_ID_LEN) return c == '\0';
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return false;
}
// The bound radio IDs in ★ order (entries without a radio ID are skipped).
std::vector<uint32_t> lsRadioIds(const LogicalSensorTable& t);

// ---- the node's table (A5c step 1) ----
// Persisted as the NVS blob "lsens" next to the Config custom IDs (config.cpp:
// lsLoad() at start, saveConfig() writes it too). Not part of Config: Config
// is copied whole (settings POST `next`, portal, loadConfig()) and none of
// those copies touch the ★ list.
extern LogicalSensorTable gLogicalSensors;

// ---- concurrent access (A5c step 2b) ----
// Writers: only the web server's request task (core 0) changes gLogicalSensors - settings
// POST (★ / un-★, order, custom ID, name, minimum signal, re-bind), /api/rf/sensor,
// restore; lsLoad() runs at boot before any other task. Readers: the main loop
// / radio (core 1: rfStateOf, slots, sources, placeholders, OLED), the upload
// and MQTT paths (outputs) and the web task itself.
// Scheme: one FreeRTOS *recursive* mutex (config.cpp), held only around short
// in-memory sections - never across network, NVS or display I/O and never
// while taking another lock:
//  - every writer holds it for the whole change (an lsRemove() shift, a custom ID
//    copy: microseconds), so readers never see a half-shifted table or a torn
//    custom ID;
//  - readers on other tasks take it for find + copy of the few fields they
//    need (lsCopyByRadioId / lsCopyByInternalId / lsMineIds / lsSnapshot, rfLabelOf,
//    rfNameOf) and work on the copy afterwards. A reader in the radio path
//    therefore waits at most one writer section, never for a page or a publish;
//  - readers on the web task (the writer's own task) may read gLogicalSensors directly:
//    no write can run between their lines.
// Recursive, so a writer may call helpers that lock again (rfSetState() ->
// rfNameOf()). No other lock is ever taken while it is held, so it cannot be
// part of a deadlock cycle. The host tests are single-threaded: the lock is a
// depth counter there (test/host/runtime.cpp).
void lsLock();
void lsUnlock();
class LsGuard {
public:
    LsGuard()  { lsLock(); }
    ~LsGuard() { lsUnlock(); }
    LsGuard(const LsGuard&) = delete;
    LsGuard& operator=(const LsGuard&) = delete;
};
// Locked copies for readers outside the web task. false = no such entry (out cleared).
bool lsCopyByRadioId(uint32_t radioId, LogicalSensor& out);
bool lsCopyByInternalId(const char* internalId, LogicalSensor& out);
std::vector<uint32_t>      lsMineIds();    // lsRadioIds(gLogicalSensors)
int lsMineIds(uint32_t (&out)[LS_MAX]);     // the same without a heap allocation; returns the count
std::vector<LogicalSensor> lsSnapshot();   // the used entries in ★ order
int                        lsMineCount();  // gLogicalSensors.count()
// Random source for new internal IDs: esp_random() on the device (config.cpp),
// a counter on the host (test/host/runtime.cpp).
uint32_t lsRandom();

// NVS blob: byte 0 = layout version (LS_BLOB_VERSION), byte 1 = entries,
// then per entry internalId[8] customId[17] displayName[25] radioId (4, little endian) minRssiDbm (1).
constexpr uint8_t LS_BLOB_VERSION = STORED_LOGICAL_SENSORS_VERSION;   // storage_layout.h
constexpr size_t  LS_REC_SIZE  = (LS_INTERNAL_ID_LEN + 1) + (LS_CUSTOM_ID_MAX + 1) + (LS_DISPLAY_NAME_MAX + 1) + 4 + 1;
constexpr size_t  LS_BLOB_MAX  = 2 + LS_REC_SIZE * LS_MAX;
size_t lsPack(const LogicalSensorTable& t, uint8_t* out, size_t len);            // bytes written, 0 = buffer too small
// false = not a table of this layout (t then empty). Entries with a damaged
// internal ID, an invalid custom ID or a duplicate are dropped.
bool   lsUnpack(LogicalSensorTable& t, const uint8_t* in, size_t len);
// Same entries in the same order.
bool   lsEqual(const LogicalSensorTable& a, const LogicalSensorTable& b);
// Appends an entry as it is (backup restore); false = refused: table full,
// internalId not an internal ID, custom ID invalid, or internalId / custom ID / radio ID taken.
bool   lsAppend(LogicalSensorTable& t, const LogicalSensor& e);

// ---- placeholders (D4) ----
// Stored form "{@L3F9A2C_temp}", typed / shown form "{<custom ID>_temp}"; old
// forms "{rf17000008_temp}" (radio ID) and "{rf3_temp}" (slot = ★ number)
// still resolve (sources.cpp rfIndexOfPlaceholder()).
// Entry of a placeholder name ("@L3F9A2C_temp", "garten_temp", "rf17000008_temp",
// "rf3_temp"; the part before the last '_' names the sensor): index, -1 =
// not a logical sensor's; *field = the part after the last '_' (nullptr = no '_').
int  lsOfPlaceholder(const LogicalSensorTable& t, const char* name, const char** field);
// Rewrites every wireless placeholder of a ★ sensor in `in` to the stored
// form (custom ID, radio ID and slot forms). false = the result did not fit (out cut).
bool lsPlaceholdersToStored(const LogicalSensorTable& t, const char* in, char* out, size_t len);
// The other way: stored form -> custom ID form, for showing a template in a form.
// Unknown internal IDs stay as they are. false = cut.
bool lsPlaceholdersToCustomId(const LogicalSensorTable& t, const char* in, char* out, size_t len);
