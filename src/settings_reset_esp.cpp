// The two resets, device part (settings_reset.h).
#include "settings_reset.h"
#include "globals.h"
#include "lsensors.h"
#include "series.h"
#include "vs_store.h"
#include "records.h"
#include "history.h"
#include "reset_history.h"
#include "system_restart.h"
#include "sensors.h"
#include "board_io.h"
#include "storage_write.h"
#include "serial_log.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_core_dump.h>
#include <nvs_flash.h>
#include <memory>
#include <new>

namespace {
portMUX_TYPE reqMux = portMUX_INITIALIZER_UNLOCKED;   // the request: web task, loop (downlink, MQTT)
uint8_t  requestedKind = 0;
uint32_t requestedAtMs = 0;
Config*  keptBuf = nullptr;   // "Reset settings": a default Config, allocated with the request

// After the seal: no flush (the writers are refused anyway), the reason for
// the reset history (RTC RAM), the log line out, restart.
void restartNow(const char* reason)
{
    resetHistoryNoteRestart(reason);
    dbgSerial.printf("Restart: %s\n", reason);
    delay(100);
    ESP.restart();
}

void clearNamespace(const char* ns)
{
    Preferences prefs;   // the reset's own write after the seal (storage_write.h)
    if (prefs.begin(ns, false, "cfgdata")) {
        prefs.clear();
        prefs.end();
    }
}

// kept: a default Config (storageResetRequest())
void resetSettings(std::unique_ptr<Config> kept)
{
    dbgSerial.println("Reset settings: defaults except network access, admin password, language and LoRaWAN keys.");
    // the measurement data stays: saved as before any planned restart, before the seal
    rainStatsFlush();
    recordsFlush();
    historyStoreFlush();
    settingsResetKeptInto(gCfg, *kept);
    // the tables saveConfigKeys() writes from RAM along with the keys: empty
    { LsGuard g; gLogicalSensors = LogicalSensorTable(); }
    seriesUnpackLocked("");
    vsSettingsWrite(VsSettings());
    // clear only once no write session is open any more: a session begun
    // before the seal would else write old keys into the cleared namespace.
    // NVS writes end; one that hangs is ended by the task watchdog (2 min),
    // before anything was cleared.
    while (!storageSeal()) dbgSerial.println("Reset settings: a write session is still open - waiting for it to close.");
    clearNamespace("alarms");    // rules and channels (tokens)
    clearNamespace("cupload");   // custom uploads (URLs may carry a key)
    Preferences prefs;
    bool ok = false;
    if (prefs.begin("config", false, "cfgdata")) {
        prefs.clear();   // every key, the HTTPS pair, ★ table, series, virtual / network sensors, HA list
        ok = saveConfigKeys(prefs, *kept);
        prefs.end();
    }
    if (!ok) dbgSerial.println("Reset settings: writing the kept settings failed - the node may open the setup portal.");
    restartNow("settings reset");
}

void eraseStep(const char* what, esp_err_t e)
{
    dbgSerial.printf("Erase everything: %s %s.\n", what, e == ESP_OK ? "erased" : esp_err_to_name(e));
}

void eraseEverything()
{
    dbgSerial.println("Erase everything: all settings, passwords, measurement data and diagnostics - the node starts as a new one.");
    if (!storageSeal()) dbgSerial.println("Erase everything: a write session was still open after 5 s - its handle dies with the erase.");
    // nvs_flash_erase_partition() closes every open handle of that partition
    // first (an open session's writes then fail); Preferences only re-formats
    // an erased partition empty, it brings no data back.
    eraseStep("settings partition cfgdata", nvs_flash_erase_partition("cfgdata"));
    eraseStep("history partition", nvs_flash_erase_partition("history"));
    eraseStep("core dump", esp_core_dump_image_erase());
    eraseStep("stock nvs partition (Wi-Fi, LoRaWAN session)", nvs_flash_erase());
    rainStatsForgetRtc();     // totals in RTC RAM survive a restart otherwise
    resetRainAccumulator();
    ioResetAuxPulseTotal();
    restartNow("everything erased");
}
} // namespace

bool storageResetRequest(StorageResetKind kind)
{
    Config* fresh = nullptr;
    if (kind == STORAGE_RESET_SETTINGS) {
        fresh = new (std::nothrow) Config();   // ~7 KB (PSRAM)
        if (!fresh) { dbgSerial.println("Reset settings: not enough memory - not started."); return false; }
    }
    portENTER_CRITICAL(&reqMux);
    if (kind >= requestedKind) {   // an "Erase everything" pending wins
        requestedKind = (uint8_t)kind;
        requestedAtMs = millis() + 1000;
    }
    if (fresh && !keptBuf) { keptBuf = fresh; fresh = nullptr; }
    portEXIT_CRITICAL(&reqMux);
    delete fresh;   // one is there already
    return true;
}

void storageResetPoll()
{
    portENTER_CRITICAL(&reqMux);
    const uint8_t kind = requestedKind;
    const bool due = kind && (int32_t)(millis() - requestedAtMs) >= 0;
    Config* kept = nullptr;
    if (due) { requestedKind = 0; kept = keptBuf; keptBuf = nullptr; }
    portEXIT_CRITICAL(&reqMux);
    if (!due) return;
    std::unique_ptr<Config> keptOwner(kept);   // set with every settings request
    if (kind == STORAGE_RESET_ERASE_EVERYTHING) eraseEverything();
    else resetSettings(std::move(keptOwner));
}
