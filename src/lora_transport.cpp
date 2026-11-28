#include "lora_transport.h"
#include "out_status.h"
#include "globals.h"
#include "serial_log.h"
#include "time_sync.h"
#include <atomic>

// The Heltec's transceiver (heltec_board.cpp; Heltec V4 front end handled there).
#include "radio_board.h"
LoRaWANNode* loRaNode = nullptr;
void (*gLoraRadioTakenHook)() = nullptr;

void loraRadioTaken()
{
    if (gLoraRadioTakenHook) gLoraRadioTakenHook();
}
LoraDiag gLoraDiag;

namespace {
bool radioInitialized = false;
bool attempted = false;
std::atomic<bool> joined{false};
unsigned long lastAttemptMs = 0;
// Join retry backoff: 1, 2, 4, ... 64 min (capped at 60 min) between failed
// attempts, back to 1 min after a successful join. Every attempt costs ~1.5 s
// airtime at SF12 and one DevNonce (65536 per device): a fixed 1-min retry
// during a network outage would exceed the join duty cycle LoRaWAN asks for
// and use up the nonces within about 45 days.
constexpr unsigned long RETRY_MIN_MS = 60000;
constexpr unsigned long RETRY_MAX_MS = 3600000;
unsigned long retryMs = RETRY_MIN_MS;
void joinFailed()
{
    lastAttemptMs = millis();
    outReport(OUT_LORAWAN, false, 0, "join failed");
    dbgSerial.printf("LoRaWAN join failed; measurements/WiFi continue, retry in %lu min.\n", retryMs / 60000);
    retryMs = retryMs * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : retryMs * 2;
}
}

bool loraReady()
{
    return gCfg.loraEn && joined.load();
}

void loraPoll()
{
    if (!gCfg.loraEn || loraReady()) return;
    if (attempted && millis() - lastAttemptMs < retryMs) return;
    attempted = true;
    loraRadioTaken();   // join exchange: the transceiver leaves any other mode

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
    if (!loraReady()) {
        delete loRaNode;
        loRaNode = nullptr;
        joinFailed();
    } else {
        // RadioLib leaves the regional duty-cycle limit off by default (EU868:
        // 1 %); with it on, sendReceive() refuses an uplink that would exceed
        // it (RADIOLIB_ERR_UPLINK_UNAVAILABLE, see sendLoRa() in main.cpp).
        loRaNode->setDutyCycle(true);
        lastAttemptMs = millis();
        retryMs = RETRY_MIN_MS;
        loraTimeJoined();   // DeviceTimeReq with the first uplink (unless NTP ... is fresh)
        dbgSerial.println("LoRaWAN joined; uplinks resume at the next measurement cycle.");
    }
    loraSleep();
}

void loraReconnect()
{
    joined.store(false);
    gLoraDiag.rejoins++;
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
    loraRadioTaken();
    if (radioInitialized) radio.sleep();
}
