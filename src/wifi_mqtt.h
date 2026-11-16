#pragma once
#include <Arduino.h>
#include "wind_window.h"

// Call once from setup(), regardless of wifiEn (checks the flag itself).
void wifiMqttSetup();

// Call on every loop() iteration. Non-blocking except for the (rate-limited,
// bounded-timeout) MQTT connect attempt itself; never blocks when WiFi/MQTT
// are disabled or unreachable — a cycle with no connection just does nothing.
void wifiMqttPoll();

bool wifiIsConnected();
bool mqttIsConnected();

// WiFi network change from the web settings page, with rollback: the new
// network is tried for WIFI_CHANGE_TEST_MS without touching the stored
// credentials (RAM-only driver config). Connected in time -> the new
// credentials are stored; otherwise the node goes back to the previous
// network. Called from the web task; pollWifi() (main loop) does the work.
constexpr unsigned long WIFI_CHANGE_TEST_MS = 60000;
// ssid 1-32 chars, pass empty (open network) or 8-63 chars. false = invalid.
// test = false: pre-provisioning - store right away and switch, no rollback
// (for a network that is not in range yet).
bool wifiRequestChange(const char* ssid, const char* pass, bool test = true);
bool wifiChangeBusy();             // requested or being tested
String wifiChangeResult();         // last outcome (plain text, escape for HTML), "" if none
String wifiCurrentPassword();      // stored password (to keep it when only the SSID changes)

// Publishes the current JSON telemetry snapshot to the data topic. No-op if
// wifiEn/mqttEn are disabled or MQTT isn't currently connected — the caller
// (main loop, once per uplink cycle) does not need to check state itself.
void mqttPublishTelemetry(const PathValues* pv = nullptr);   // pv: MQTT window values
