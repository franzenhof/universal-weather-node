#pragma once
// Prometheus text-format exposition for GET /metrics (docs/api.md): the same
// public data as /api/data, no login, in fixed SI base units (Celsius, m/s,
// hPa, mm, lux, seconds, bytes) regardless of the configured display units, so
// Prometheus / VictoriaMetrics / Grafana Alloy / Telegraf can scrape it
// directly. Host-compiled (test/firmware_metrics.cpp): the Wi-Fi RSSI is passed
// in rather than read here, so the core needs no WiFi dependency.
#include <string>

// Build the whole /metrics body. wifiConnected false leaves out the RSSI line.
void buildMetrics(std::string& out, int wifiRssiDbm, bool wifiConnected);
