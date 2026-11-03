#include "lora_transport.h"
#include "globals.h"
#include "serial_log.h"
#include <atomic>

// Defined by heltec_unofficial.h in main.cpp (that header owns definitions).
extern SX1262 radio;
LoRaWANNode* loRaNode = nullptr;

namespace {
bool radioInitialized = false;
bool attempted = false;
std::atomic<bool> joined{false};
unsigned long lastAttemptMs = 0;
constexpr unsigned long RETRY_MS = 60000;
}

bool loraReady()
{
    return gCfg.loraEn && joined.load();
}

void loraPoll()
{
    if (!gCfg.loraEn || loraReady()) return;
    if (attempted && millis() - lastAttemptMs < RETRY_MS) return;
    attempted = true;

    if (!radioInitialized) {
        int16_t state = radio.begin();
        radioInitialized = state == RADIOLIB_ERR_NONE;
        if (!radioInitialized) {
            dbgSerial.printf("Radio init failed (%d); measurements/WiFi continue, retry in 60 s.\n", state);
            lastAttemptMs = millis();
            return;
        }
    }
    // Do not let manage() fall back to its unbounded serial provisioning dialog.
    if (!persist.provision(gCfg.region, 0x00, gCfg.joinEui, gCfg.devEui, gCfg.appKey, gCfg.appKey)) {
        dbgSerial.println("LoRaWAN provisioning failed; measurements/WiFi continue, retry in 60 s.");
        lastAttemptMs = millis();
        loraSleep();
        return;
    }
    loRaNode = persist.manage(&radio);
    joined.store(loRaNode && loRaNode->isActivated());
    lastAttemptMs = millis();
    if (!loraReady()) {
        delete loRaNode;
        loRaNode = nullptr;
        dbgSerial.println("LoRaWAN join failed; measurements/WiFi continue, retry in 60 s.");
    } else {
        dbgSerial.println("LoRaWAN joined; uplinks resume at the next measurement cycle.");
    }
    loraSleep();
}

void loraReconnect()
{
    joined.store(false);
    if (loRaNode) {
        loRaNode->clearSession();
        if (!persist.saveSession(loRaNode))
            dbgSerial.println("LoRaWAN: failed to persist cleared session/nonces.");
    }
    delete loRaNode;
    loRaNode = nullptr;
    attempted = true;
    lastAttemptMs = millis();
    dbgSerial.println("LoRaWAN reconnect scheduled in 60 s; measurements/WiFi continue.");
}

void loraSleep()
{
    if (radioInitialized) radio.sleep();
}
