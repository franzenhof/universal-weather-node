#pragma once
#include "wind_window.h"

#include <ArduinoJson.h>

// Builds the shared telemetry snapshot into `doc`, mirroring the same
// values/conditions as the CayenneLPP uplink (buildLppPacket() in main.cpp).
// Consumed by both the MQTT publish and the REST API (/api/data).
// pv: wind/rain of the sending path (MQTT window); nullptr = base cycle values.
void buildJsonPayload(JsonDocument& doc, const PathValues* pv = nullptr);

// One wireless sensor gRfSensors[idx] (must be used) as the rfSensors[] item
// of buildJsonPayload(); also the payload of the MQTT topic <prefix>/rf/<ID>.
void addRfSensorJson(JsonObject o, int idx);
