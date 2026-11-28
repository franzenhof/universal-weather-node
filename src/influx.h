#pragma once
// InfluxDB / VictoriaMetrics push: build an InfluxDB line-protocol body for the
// current values (the same data as /metrics and /api/data) so the node can POST
// it to a time-series database, timestamp in the packet (olicat, forum #62).
// The body is host-compiled and tested (test/firmware_influx.cpp); the HTTP POST
// and scheduling live in wx_upload.cpp, which already has the TLS client.
//
// Line protocol, nanosecond timestamps (no precision= needed):
//   <measurement>,host=<name> temperature=12.5,humidity=61,... <ts_ns>
//   <measurement>_rf,id=900181A2,model=WS90 temperature=4.2,... <ts_ns>
// Only fields with a valid value are written; empty tags are left out.
#include <cstdint>
#include <string>

// Append the whole body (may be several lines) for the given Unix time. The
// Wi-Fi RSSI is passed in (system fields) so this stays free of WiFi includes;
// pass wifiConnected false to leave the RSSI field out. measurement: the
// configured name (a copy taken under wxUploadLock(); "" = "weather").
void buildInfluxLineProtocol(std::string& out, int64_t epochSec, int rssiDbm, bool wifiConnected, const char* measurement);

// The Authorization header for the configured token: taken as it is when it has
// a scheme ("Token xxx", "Bearer xxx", "Basic xxx" - it contains a space),
// a bare token gets "Token " in front (InfluxDB 2); empty = no header.
std::string influxAuthHeader(const char* token);
