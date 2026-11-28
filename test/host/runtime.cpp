#include "Arduino.h"
#include "globals.h"
#include "serial_log.h"
#include "LoRaWAN_ESP32.h"
#include "heltec_board.h"
BoardInfo gBoard;   // no PCB unless a test sets gBoard.pcb

unsigned long hostNow=100;
int hostPins[64]={},hostModes[64]={},hostAdc[64]={},hostPwm[8]={},hostAttached[64]={};
// Pulse counters (src/pulse_counter.h): the test sets the accumulated count.
#include "pulse_counter.h"
uint32_t hostCounts[PULSE_COUNTER_COUNT]={};
bool     pulseCounterStart(PulseCounterId id, int, bool) { hostCounts[id]=0; return true; }
void     pulseCounterStop(PulseCounterId id) { hostCounts[id]=0; }
uint32_t pulseCounterRead(PulseCounterId id) { return hostCounts[id]; }
bool  hostBmeOk=false;
float hostBmeT=20.0f,hostBmeP=101325.0f,hostBmeH=50.0f;
int   hostBmeSda=-1,hostBmeAddr=-1;
int hostProbeCount=0;
bool hostConversionComplete=false;
float hostProbeTemps[3]={10.5f,12.5f,14.5f};
HostESP ESP;
#include "psram_host.cpp"   // PSRAM / internal RAM figures (src/psram.h)
#include "system_restart.h"   // the real one (rain flush) is in wx_upload.cpp, not built here
#include "settings_reset.h"
bool storageResetRequest(StorageResetKind) { return true; }   // the device part (settings_reset_esp.cpp) is not built here
void systemRestart(const char*) { ESP.restart(); }
HostSerial Serial1;
SerialLog dbgSerial;
size_t SerialLog::write(uint8_t) { return 1; }
size_t SerialLog::write(const uint8_t*,size_t n) { return n; }
size_t SerialLog::println(const char* s) { return strlen(s) + 2; }
size_t SerialLog::println(const String&) { return 0; }
// The regression tests were written for a node with the PCB (Davis wind and
// rain on, LoRaWAN on, no wireless sensors) - the defaults of a config stored
// by an older firmware (ConfigLegacyDefaults). New-node defaults are checked
// in firmware_rfauto.cpp.
static Config hostLegacyConfig()
{
    Config c;
    const ConfigLegacyDefaults l;
    c.loraEn = l.loraEn; c.wifiEn = l.wifiEn; c.webApiEn = l.webApiEn;
    c.rfEn = l.rfEn; c.rfProto = l.rfProto; c.windEn = l.windEn; c.rainEn = l.rainEn;
    return c;
}
Config gCfg = hostLegacyConfig();
Config hostSavedConfig;
void saveConfig(const Config& c) { hostSavedConfig=c; }
// Random source of new logical sensor IDs (lsensors.h): a deterministic sequence
#include "lsensors.h"
uint32_t hostLsRandom=0x3F9A2C;
uint32_t lsRandom() { return hostLsRandom += 0x10101; }
// The table's lock (lsensors.h): single-threaded here - a depth counter, so a
// test can check that every section was left (hostLsLockDepth == 0).
int hostLsLockDepth = 0;
void lsLock()   { hostLsLockDepth++; }
void lsUnlock() { if (--hostLsLockDepth < 0) abort(); }
bool portalRequestedViaDownlink=false;
uint8_t loopCount=42;
int sendFailCount=1;
unsigned long lastSendMs=0;
SX1262 radio;
HostPersistence persist;
int hostRadioResult=0,hostRadioBegins=0,hostJoinCalls=0,hostNodes=0,hostSleeps=0;
int hostSessionSaves=0;
bool hostProvisionOk=true,hostJoinOk=false;

// ---- Stubs for the derived values (history.cpp / wx_upload.cpp are not
// part of the host build): no history and no daily data yet. ----
#include "../../src/wx_upload.h"
bool  historyPressureAgo(uint32_t, float&) { return false; }
bool  historyTempAgo(uint32_t, float&) { return false; }
float historyRainSum(uint32_t) { return 0.0f; }
WxDailyStats hostDailyStats;   // default: clock not synced yet (no daily values)
void  wxDailyStats(WxDailyStats& out) { out = hostDailyStats; }
void  wxUpdateAvailable(char* out, size_t len) { if (len) out[0] = '\0'; }
