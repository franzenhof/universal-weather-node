#pragma once
// Value sources: which sensor feeds each measured quantity into every output
// (LoRaWAN, JSON/MQTT, Home Assistant, weather networks, history, display).
//
// Per group the setting (gCfg.srcSel[]) is
//   Auto      board sensor first, then wireless sensors 1-4 (like a PC boot
//             order): the first one that currently delivers the value is used
//             (temperature / humidity: indoor sensors excluded, rfIsIndoor();
//             pressure: indoor sensors are fine - station pressure is the
//             same indoors)
//   Board     only the board's own sensor (BME280, anemometer, rain gauge, UV input)
//   Wireless n  only wireless sensor n (1-4): the n-th ID in "Wireless sensor
//             IDs", or - with an empty list - the n-th sensor received since
//             start. Up to RF_MAX_SENSORS (32) sensors are received and shown,
//             only the first RF_SOURCE_SLOTS (4) slots are value sources.
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

// SG_PRESS (station pressure) came last so the stored settings src0..src4,
// parameters 45-49 and the LoRaWAN source bits 0-9 keep their meaning.
enum SrcGroup : uint8_t { SG_AIR = 0, SG_WIND, SG_RAIN, SG_UV, SG_LIGHT, SG_PRESS, SG_COUNT };
enum SrcSel   : uint8_t { SRC_AUTO = 0, SRC_BOARD = 1, SRC_RF1 = 2, SRC_RF4 = 5, SRC_OFF = 6 };
// Wireless slots selectable as value sources (Wireless 1-4 = slots 1-4;
// stored settings and parameters 45-49 keep this meaning).
constexpr int RF_SOURCE_SLOTS = SRC_RF4 - SRC_RF1 + 1;

constexpr int SRC_NONE     = -2;   // srcResolve(): value not available
constexpr int SRC_IS_BOARD = -1;   // srcResolve(): board sensor; >= 0: gRfSensors[] index
constexpr unsigned long SRC_RF_FRESH_MS = 300000UL;

int  srcResolve(SrcGroup g);
struct RfSensorReading;
int  srcHierarchyRank(const RfSensorReading& r, SrcGroup g);   // Auto: Ecowitt's sensor order (lower = first)
inline bool srcActive(SrcGroup g)  { return srcResolve(g) != SRC_NONE; }
inline bool srcIsRadio(SrcGroup g) { return srcResolve(g) >= 0; }

// false = the value ("temp", "humidity", "wind", "rain", "uv", "light", "pressure") of
// gRfSensors[idx] was last received more than SRC_RF_FRESH_MS ago (stale).
bool srcRfValueFresh(int idx, const char* value);

// gRfSensors[] index of wireless sensor slot 0..RF_MAX_SENSORS-1 (0-based:
// slot 0 = "Wireless 1", see above), -1 = none
int rfSensorIndexForSlot(int slot);
// The other way round: 0-based slot of gRfSensors[idx] (its position in the
// ID list, else idx), -1 = unused entry / not in the list.
int rfSlotOfIndex(int idx);

// "board", "radio:39582376" or "none" (JSON); short labels for the web pages
const char* srcGroupKey(SrcGroup g);   // JSON key: tempHum, wind, rain, uv, light, pressure
void srcDescribe(SrcGroup g, char* out, int len);

// Payload: 2 bits per group (0 none, 1 board, 2 wireless) at bit 2*g
// (pressure: bits 10-11), bit 15 = a wireless sensor in use reports a low battery.
uint16_t srcBits();

// Called by rf_sensors.cpp for every decoded packet of gRfSensors[idx].
// rainDeltaMm: increase of the sensor's rain total since its previous packet.
// hasLight: the packet carries illuminance (sunshine duration, see derived.h).
// rainGapMs > 0: the previous rain value was that old (sensor not heard for
// longer than SRC_RF_FRESH_MS); the increase is then not counted at once but
// spread over the following packets at its average rate over the outage
// (at most over 1 h), so the rain rate shows no burst and no rain is lost.
void srcRfPacket(int idx, bool hasWind, float rainDeltaMm, bool hasLight = false, unsigned long rainGapMs = 0);

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
    // pressure: BME280 or a wireless sensor's station pressure; QNH with the
    // configured altitude like the BME280 path (seaLevelPressureHpa())
    bool  pressValid = false;
    float pressAbsHpa = 0.0f, pressSeaHpa = 0.0f;
    bool  rfBatteryLow = false;     // a wireless sensor in use reports a low battery
};
extern SrcValues gSrc;
void srcUpdateCycle();

// Board rain gauge increments (sensors.cpp).
void srcBoardRain(float mm);

// Wireless wind per send path (PATH_* in main.cpp, 0..SRC_PATHS-1).
constexpr int SRC_PATHS = 4;   // base, LoRaWAN, MQTT, Loxone
WindStats srcRfWind(int path, bool tenMin);
void      srcRfWindReset(int path);

// Parameter ID (params.h) of a group's source setting: 45-49, pressure 54.
int srcParamId(SrcGroup g);
