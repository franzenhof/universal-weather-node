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
//   Wireless n  only wireless sensor n (1-32): the n-th ★ sensor, then the
//             others in the order they were received (rfSensorIndexForSlot()).
//             Auto only looks at the first RF_SOURCE_SLOTS (4) of them, so a
//             neighbour's sensor further down never becomes a source by itself.
//   Off       not reported at all
// A wireless sensor counts while its last packet carrying the value is at most
// SRC_RF_FRESH_MS old (Bresser sensors send every 12-16 s).
//
// Wind from a wireless sensor uses the sensor's own mean/gust/direction per
// packet, aggregated per send path like the board's anemometer (interval or
// 10-min window). Rain: the shared running total rainMMSinceStart only takes
// increments from the selected source, so switching sources never jumps.
#include <stddef.h>
#include <stdint.h>
#include "wind_window.h"

// SG_PRESS (station pressure) came last so the stored settings src0..src4,
// parameters 45-49 and the LoRaWAN source bits 0-9 keep their meaning.
enum SrcGroup : uint8_t { SG_AIR = 0, SG_WIND, SG_RAIN, SG_UV, SG_LIGHT, SG_PRESS, SG_COUNT };
// Wireless n (1-32) = n + 1, Off = 34. Up to rc.5 Off was 6 (only Wireless
// 1-4 existed): a source set to Off has to be set again once (CHANGELOG rc.6).
constexpr int SRC_RF_SLOTS_MAX = 32;                                // Wireless 1..32
enum SrcSel   : uint8_t { SRC_AUTO = 0, SRC_BOARD = 1, SRC_RF1 = 2, SRC_OFF = SRC_RF1 + SRC_RF_SLOTS_MAX };
// Wireless slots Auto looks at (Wireless 1-4).
constexpr int RF_SOURCE_SLOTS = 4;
// 0-based wireless slot of a setting, -1 = not a wireless one
inline int  srcSelSlot(int sel)     { return sel >= SRC_RF1 && sel < SRC_OFF ? sel - SRC_RF1 : -1; }
inline int  srcSelForSlot(int slot) { return SRC_RF1 + slot; }
inline bool srcSelValid(long v)     { return v >= 0 && v <= SRC_OFF; }

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

// A wireless value source is bound to the chosen logical sensor (★, lsensors.h)
// by its internal ID (gCfg.sourceInternalId[g]): it stays on that sensor when the ★ order
// changes or its radio ID is re-bound, and delivers nothing (not another
// sensor's value) while that sensor is silent or no longer in the table.
// Only ★ sensors can be bound; "Wireless n" past them (a received-only sensor,
// set by number through a parameter) goes by its number.
// srcBindSel():   bind by number, after srcSel[g] was set (parameters)
// srcBoundInternalId():  the bound internal ID, "" = not a wireless source / by number
// srcFormValue(): the settings form's / backup's value - the internal ID
//                 ("L3F9A2C") for a bound source, else the srcSel number
// srcSetFormId(): takes such an internal ID; false = not one (a plain number
//                 goes through the parameter). An "r<radio ID>" value of a
//                 version-1 backup is swallowed and ignored (no conversion).
void        srcBindSel(SrcGroup g);
const char* srcBoundInternalId(SrcGroup g);
void        srcFormValue(SrcGroup g, char* out, size_t len);
bool        srcSetFormId(SrcGroup g, const char* v);

// gRfSensors[] index of wireless sensor slot 0..RF_MAX_SENSORS-1 (0-based:
// slot 0 = "Wireless 1", see above), -1 = none
int rfSensorIndexForSlot(int slot);
// Sensor of a placeholder name: "@L3F9A2C_temp" (stored form) or "<custom ID>_temp"
// of a logical sensor (lsensors.h), or the old "rf<N>_..." (number 1-32) /
// "rf<ID>_..." (exactly 8 hex digits, e.g. "rf300007DD_temp"): gRfSensors[]
// index, -1 = none (not received now); *field = the part after the '_'
// (nullptr = not a wireless sensor's placeholder).
int rfIndexOfPlaceholder(const char* name, const char** field);
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

// The per-cycle snapshot: compute the selected source values into gSrc, called
// once per base cycle (after the board averages are final). gSrc feeds uploads,
// alarms, history, LoRaWAN / MQTT / networks and the API.
void srcUpdateCycle();
extern volatile uint32_t gSrcCycleCount;   // counts srcUpdateCycle() (the web task pushes new values on a change)

// The same selection, computed on demand and side-effect-free, from the current
// (latest) board and wireless readings. Guaranteed to pick the same source as
// srcUpdateCycle() because they share srcCompute(); use it where the display
// should show the newest value per packet (live), not the cycle snapshot.
void srcCompute(SrcValues& v);   // the shared, pure selection
SrcValues srcLive();             // convenience: srcCompute() into a fresh SrcValues

// Board rain gauge increments (sensors.cpp).
void srcBoardRain(float mm);

// Wireless wind per send path (PATH_* in main.cpp, 0..SRC_PATHS-1).
constexpr int SRC_PATHS = 4;   // base, LoRaWAN, MQTT, Loxone
WindStats srcRfWind(int path, bool tenMin);
void      srcRfWindReset(int path);

// Parameter ID (params.h) of a group's source setting: 45-49, pressure 54.
int srcParamId(SrcGroup g);
