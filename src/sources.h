#pragma once
// Value sources: which sensor feeds each measured quantity into every output
// (LoRaWAN, JSON/MQTT, Home Assistant, weather networks, history, display).
//
// Per group the setting (gCfg.srcSel[]) is
//   Auto      board sensor first, then wireless sensors 1-4 (like a PC boot
//             order): the first one that currently delivers the value is used
//   Board     only the board's own sensor (BME280, anemometer, rain gauge, UV input)
//   Wireless n  only wireless sensor n: the n-th ID in "Wireless sensor IDs",
//             or - with an empty list - the n-th sensor received since start
//   Off       not reported at all
// A wireless sensor counts while its last packet carrying the value is at most
// SRC_RF_FRESH_MS old (Bresser sensors send every 12-16 s).
//
// Wind from a wireless sensor uses the sensor's own mean/gust/direction per
// packet, aggregated per send path like the board's anemometer (interval or
// 10-min window). Rain: the shared running total rainMMSinceStart only takes
// increments from the selected source, so switching sources never jumps.
#include <stdint.h>
#include "wind_window.h"

enum SrcGroup : uint8_t { SG_AIR = 0, SG_WIND, SG_RAIN, SG_UV, SG_LIGHT, SG_COUNT };
enum SrcSel   : uint8_t { SRC_AUTO = 0, SRC_BOARD = 1, SRC_RF1 = 2, SRC_RF4 = 5, SRC_OFF = 6 };

constexpr int SRC_NONE     = -2;   // srcResolve(): value not available
constexpr int SRC_IS_BOARD = -1;   // srcResolve(): board sensor; >= 0: gRfSensors[] index
constexpr unsigned long SRC_RF_FRESH_MS = 300000UL;

int  srcResolve(SrcGroup g);
inline bool srcActive(SrcGroup g)  { return srcResolve(g) != SRC_NONE; }
inline bool srcIsRadio(SrcGroup g) { return srcResolve(g) >= 0; }

// false = the value ("temp", "humidity", "wind", "rain", "uv", "light") of
// gRfSensors[idx] was last received more than SRC_RF_FRESH_MS ago (stale).
bool srcRfValueFresh(int idx, const char* value);

// gRfSensors[] index of wireless sensor slot 0..3 (see above), -1 = none
int rfSensorIndexForSlot(int slot);

// "board", "radio:39582376" or "none" (JSON); short labels for the web pages
const char* srcGroupKey(SrcGroup g);   // JSON key: tempHum, wind, rain, uv, light
void srcDescribe(SrcGroup g, char* out, int len);

// Payload: 2 bits per group (0 none, 1 board, 2 wireless) at bit 2*g,
// bit 15 = a wireless sensor in use reports a low battery.
uint16_t srcBits();

// Called by rf_sensors.cpp for every decoded packet of gRfSensors[idx].
// rainDeltaMm: increase of the sensor's rain total since its previous packet.
// hasLight: the packet carries illuminance (sunshine duration, see derived.h).
void srcRfPacket(int idx, bool hasWind, float rainDeltaMm, bool hasLight = false);

// Snapshot of the non-path values at the end of each base cycle
// (srcUpdateCycle(), after the board averages are final). Wind and rain
// are per path: pathValues() in main.cpp, srcActive(SG_WIND / SG_RAIN).
struct SrcValues {
    bool  tempValid = false, humValid = false;
    float tempC = 0.0f, humidity = 0.0f;
    bool  uvBoard = false;          // board UV input (gIo: volts, optional calibrated index)
    bool  uvRadio = false;          // wireless sensor UV index
    float uvIndexRadio = 0.0f;
    bool  lightValid = false;
    float lightLux = 0.0f;
    bool  rfBatteryLow = false;     // a wireless sensor in use reports a low battery
};
extern SrcValues gSrc;
void srcUpdateCycle();

// Board rain gauge increments (sensors.cpp).
void srcBoardRain(float mm);

// Wireless wind per send path (PATH_* in main.cpp, 0..SRC_PATHS-1).
constexpr int SRC_PATHS = 3;
WindStats srcRfWind(int path, bool tenMin);
void      srcRfWindReset(int path);
