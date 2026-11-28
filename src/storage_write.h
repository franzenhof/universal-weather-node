#pragma once
// The one gate for every write of our own data to flash (cfgdata, history
// partition). The two resets (settings_reset.h) seal it before they clear or
// erase: from then on no task can open storage for writing, and the seal
// waits until the write sessions already open are closed - so a settings save
// or a store flush racing a reset can never bring old data back.
// Host-tested (test/firmware_settingsreset.cpp).
#include <Preferences.h>

// An open write session of one NVS namespace (RAII): ok() = open; closed
// (prefs.end()) by the destructor. Refused once storage is sealed.
class StorageWrite {
public:
    explicit StorageWrite(const char* ns, const char* partition = "cfgdata");
    ~StorageWrite();
    StorageWrite(const StorageWrite&) = delete;
    StorageWrite& operator=(const StorageWrite&) = delete;
    bool ok() const { return open_; }
    Preferences& prefs() { return prefs_; }
private:
    Preferences prefs_;
    bool open_ = false;
};

// Seals storage until the restart: new sessions are refused; waits up to
// waitMs for the open ones to close. false = some are still open: they may
// still write - "Reset settings" calls it again until true; an erase goes
// on (it invalidates their handles).
bool storageSeal(unsigned waitMs = 5000);
bool storageSealed();
