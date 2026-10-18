#pragma once

#include <ArduinoJson.h>

// Two-tier in-RAM ring buffer of bucketed measurement values, feeding the
// /chart web page and /api/history endpoint. RAM-only, not persisted across
// a restart: a *timestamped* buffer keyed on uptime would become
// discontinuous once the uptime resets to 0 on reboot. Long-term history
// remains Grafana's job via MQTT/ChirpStack; this is a short local window
// for the device's own web UI.
//
// Classic RRDtool-style tiering rather than one long flat buffer: a "fine"
// tier keeps short buckets for a detailed recent view, a "coarse" tier keeps
// long buckets for a full week at a fraction of the RAM a flat buffer of
// the same duration/resolution would need. Each pushHistorySample() call
// merges the just-finished uplink cycle's values into *both* tiers' current
// bucket (running average, except wind gust = running max and rain amount =
// running sum — see history.cpp); a bucket is finalized into the ring array
// once its time window elapses, independent of uplinkIntervalSec.
constexpr size_t HISTORY_FINE_CAPACITY        = 288;  // 288 x 5 min   = 24h
constexpr unsigned long HISTORY_FINE_BUCKET_SEC   = 300;
constexpr size_t HISTORY_COARSE_CAPACITY      = 168;  // 168 x 1h      = 7 days
constexpr unsigned long HISTORY_COARSE_BUCKET_SEC = 3600;

// Call once per completed measurement cycle (same point mqttPublishTelemetry()
// consumes the finished cycle's values from) — reads already-computed
// globals, no new sensor access.
void pushHistorySample();

// Serializes one tier's ring buffer (oldest first) as parallel arrays plus
// one set of sibling *Unit fields, converting from stored base units via
// units.h at serialize time — same convention as buildJsonPayload().
// coarse=false -> fine/24h tier, coarse=true -> coarse/7-day tier.
void buildHistoryJson(JsonDocument& doc, bool coarse);
