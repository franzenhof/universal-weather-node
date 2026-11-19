#pragma once
#include <Arduino.h>
constexpr int RADIOLIB_ERR_NONE=0;
extern int hostRadioResult,hostRadioBegins,hostJoinCalls,hostNodes,hostSleeps;
extern bool hostProvisionOk,hostJoinOk;
extern int hostSessionSaves;
struct SX1262 {
    int begin() { ++hostRadioBegins; return hostRadioResult; }
    void sleep() { ++hostSleeps; }
};
extern SX1262 radio;
// radio_board.h: the firmware's BoardSX1262 is this fake on the host
#define HOST_FAKE_RADIO
using BoardSX1262 = SX1262;
struct LoRaWANNode {
    bool active;
    LoRaWANNode():active(hostJoinOk) { ++hostNodes; }
    ~LoRaWANNode() { --hostNodes; }
    bool isActivated() { return active; }
    void clearSession() { active=false; }
    bool dutyCycle=false;
    void setDutyCycle(bool enable=true, unsigned long=0) { dutyCycle=enable; }
};
struct HostPersistence {
    bool provision(const char*,int,uint64_t,uint64_t,const uint8_t*,const uint8_t*) { return hostProvisionOk; }
    LoRaWANNode* manage(SX1262*) { ++hostJoinCalls; return new LoRaWANNode; }
    bool saveSession(LoRaWANNode* node) {
        if (node->isActivated()) return false;
        ++hostSessionSaves;
        return true;
    }
};
extern HostPersistence persist;
