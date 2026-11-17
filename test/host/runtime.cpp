#include "Arduino.h"
#include "globals.h"
#include "serial_log.h"
#include "LoRaWAN_ESP32.h"

unsigned long hostNow=100;
int hostPins[64]={},hostModes[64]={},hostAdc[64]={},hostPwm[8]={},hostAttached[64]={};
int16_t hostCounts[4]={};
void (*hostPcntIsr[4])(void*)={};
void* hostPcntArg[4]={};
bool  hostBmeOk=false;
float hostBmeT=20.0f,hostBmeP=101325.0f,hostBmeH=50.0f;
int hostProbeCount=0;
bool hostConversionComplete=false;
float hostProbeTemps[3]={10.5f,12.5f,14.5f};
HostESP ESP;
HostSerial Serial1;
SerialLog dbgSerial;
size_t SerialLog::write(uint8_t) { return 1; }
size_t SerialLog::write(const uint8_t*,size_t n) { return n; }
Config gCfg;
Config hostSavedConfig;
void saveConfig(const Config& c) { hostSavedConfig=c; }
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
void  wxDailyStats(WxDailyStats& out) { out = WxDailyStats{}; }
void  wxUpdateAvailable(char* out, size_t len) { if (len) out[0] = '\0'; }
