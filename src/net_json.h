#pragma once
// Network sensors (net_sensor.h): the JSON part - field mapping, the
// "Universal Weather Node" preset, a REST reply. Host-tested
// (test/firmware_netsensor.cpp).
#include <ArduinoJson.h>
#include "net_sensor.h"

// A number at a dotted path ("temp", "main.temp", "list.0.main.temp": a
// number segment indexes an array). Numbers given as text ("21.5") count too.
bool netPath(JsonVariantConst root, const char* path, float& out);

// The keys a slot reads, as an ArduinoJson filter (only those are kept while
// parsing: another node's 5 KB <prefix>/data costs a few hundred bytes).
void netFilter(const NetSlotCfg& c, JsonDocument& filter);

// The slot's values from a parsed message / reply (key mapping, or the preset
// with the other node's units converted to metric).
void netExtract(const NetSlotCfg& c, JsonVariantConst root, VsValues& out);

// A whole JSON payload (MQTT): parsed with the filter and extracted.
// NET_ERR_NONE (values in out), NET_ERR_JSON or NET_ERR_NO_VALUES.
uint8_t netParseJson(const NetSlotCfg& c, const uint8_t* json, size_t len, VsValues& out);

// A REST reply: HTTP code, the result of parsing the body (only looked at for
// 200) and the parsed document. NET_ERR_NONE (values in out), NET_ERR_HTTP,
// NET_ERR_JSON or NET_ERR_NO_VALUES.
uint8_t netRestResult(const NetSlotCfg& c, int httpCode, DeserializationError err, JsonVariantConst root, VsValues& out);
