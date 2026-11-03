#include "Arduino.h"
#include "globals.h"
#include "serial_log.h"
#include "LoRaWAN_ESP32.h"

unsigned long hostNow=100;
int hostPins[64]={},hostModes[64]={},hostAdc[64]={},hostPwm[8]={},hostAttached[64]={};
int16_t hostCounts[4]={};
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
