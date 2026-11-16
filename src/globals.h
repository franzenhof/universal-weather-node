#pragma once

// Shared live state — sensor values are defined in sensors.cpp, the rest in
// main.cpp — used by the transport-agnostic modules (params, json_payload,
// wifi_mqtt, web_api) as well as by lora_payload.cpp and portal.cpp.

#include "config.h"

extern Config gCfg;
extern bool   portalRequestedViaDownlink;

extern float    windSpeedMs;
extern float    windGustMs;
extern float    rainMM;
extern float    rainRateMmH;
extern float    rainMMSinceStart;
extern uint16_t windDirection;
extern float    batteryVolt;
extern float    cpuTemperature;
extern const char* gResetReason;   // cause of the last restart, set once in setup()
extern unsigned long gBaseCycleMs; // current base cycle (shortest send interval), set by loop()

extern bool     bme280Present;
extern float    bme280TempC;
extern float    bme280PressAbs;
extern float    bme280PressSea;
extern float    bme280Humidity;

extern float    bus1Temps[];
extern int      bus1Count;

extern uint8_t  loopCount;
extern int      sendFailCount;

// millis() timestamp of the last completed measurement cycle (transmit +
// reset accumulators); used to compute time remaining until the next one.
extern unsigned long lastSendMs;

// PCNT control functions (configureWindPcnt, stopWindPcnt, configureRainPcnt,
// stopRainPcnt) are declared in sensors.h, the module that defines them —
// include that directly if you need them (e.g. params.cpp).
