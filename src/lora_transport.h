#pragma once

#include <LoRaWAN_ESP32.h>

extern LoRaWANNode* loRaNode;

// One bounded radio/join attempt; failed joins back off from 1 to 60 min.
// Call only between measurement cycles, never while a conversion is pending.
void loraPoll();
bool loraReady();
void loraReconnect();
void loraSleep();

// The SX1262 is shared with the wireless-sensor receiver (rf_sensors.cpp).
// Every LoRaWAN use of the transceiver calls loraRadioTaken() first; the hook
// (set by the receiver) marks its FSK reception as stopped, so it restarts it
// once LoRaWAN is done.
extern void (*gLoraRadioTakenHook)();
void loraRadioTaken();

// Read-only LoRaWAN diagnostics for the status page. Written by the main loop
// (sendLoRa(), loraReconnect()), read by the web task: plain scalars, a torn
// read only shows a value from one cycle earlier.
struct LoraDiag {
    bool          haveUplink = false;
    uint8_t       dataRate = 0;          // DR of the last uplink
    float         freqMHz = 0.0f;        // channel of the last uplink
    int16_t       txPowerDbm = 0;
    uint32_t      fCntUp = 0;
    uint32_t      devAddr = 0;
    bool          haveDownlink = false;  // since boot
    unsigned long lastDownlinkMs = 0;
    float         lastDlRssi = 0.0f;
    float         lastDlSnr = 0.0f;
    uint32_t      uplinksWithoutDownlink = 0;
    uint32_t      rejoinThreshold = 0;   // current silent-session limit
    uint16_t      rejoins = 0;           // session drops since boot (send failures or silence)
    unsigned long intervalMs = 0;        // effective uplink interval (stretched for 1 % airtime)
};
extern LoraDiag gLoraDiag;
