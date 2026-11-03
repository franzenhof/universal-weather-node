#pragma once

#include <LoRaWAN_ESP32.h>

extern LoRaWANNode* loRaNode;

// One bounded radio/join attempt; retries no more than once per minute.
// Call only between measurement cycles, never while a conversion is pending.
void loraPoll();
bool loraReady();
void loraReconnect();
void loraSleep();
