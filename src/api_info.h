#pragma once
// Third-party API contract (docs/api.md, docs/api/openapi.yaml): the
// apiVersion carried by /api/data (= MQTT data topic), /api/history and
// /api/info, and the /api/info document itself. Host-compiled
// (test/firmware_api.cpp): no WebServer/WiFi dependency here.
//
// Stability rule: fields are only ever ADDED under the same apiVersion.
// Renaming or removing a field, changing its type, unit or meaning needs
// API_VERSION 2 - and an update of docs/api/schemas/ (the contract test
// fails on any emitted key the schemas don't describe).
#include <ArduinoJson.h>
#include <stddef.h>

constexpr int API_VERSION = 1;

// /api/info: device, firmware, board, units, enabled outputs, active value
// groups and sources, cycle length, history ranges. Never secrets
// (passwords, keys, station IDs, broker address, coordinates).
void buildInfoJson(JsonDocument& doc);

// Stack of a FreeRTOS task never used so far (bytes), -1 = no such task
// ("loopTask", "webApi", "wxUpload"); /api/info system.stackFree, status page.
int hostOrDeviceStackFree(const char* task);

// The node's host name (without ".local"), the same for DHCP/Wi-Fi
// (WiFi.setHostname before every WiFi.begin, also in the setup portal) and
// mDNS: the device name in lower case, every run of characters outside
// [a-z0-9] replaced by one '-', no leading/trailing '-', at most
// HOSTNAME_MAX_LEN characters; without a (usable) name
// "weathernode-<last 6 hex digits of the MAC>".
// 31: the Arduino core keeps the DHCP hostname in a char[32].
constexpr size_t HOSTNAME_MAX_LEN = 31;
void deviceHostname(char* out, size_t len);
// The sanitising part alone ("" if nothing usable is left).
void hostnameFromName(const char* name, char* out, size_t len);
