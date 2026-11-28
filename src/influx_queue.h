#pragma once
// InfluxDB backfill (B4): a byte ring of the line-protocol bodies that are not
// yet accepted by the database, oldest first. Every push goes through it: the
// upload task appends the body of each interval and sends the oldest bodies in
// chunks (<= INFLUX_CHUNK_BYTES per request), so after an outage the missed
// points follow in their order with their original timestamps (the ns
// timestamp is in every line; InfluxDB v1/v2 and VictoriaMetrics accept late
// data). Full ring: the oldest push is dropped (counted).
//
// Why text and not compact records: the body is exactly what would have been
// sent (one builder, no second encoder for tags / field sets that change with
// the sensors), and at ~0.3-1 KB per push 512 KB of PSRAM hold ~8 h at the
// default 60 s interval. RAM only: a restart or power cut loses the backlog
// (no flash - one write per interval would wear it).
//
// Pure part (ring, chunking, reply handling, backoff) is host-tested in
// test/firmware_influxqueue.cpp; the buffer is handed in by the caller
// (device: PSRAM, wx_upload.cpp).
#include <cstddef>
#include <cstdint>
#include <string>

constexpr size_t   INFLUX_BACKLOG_BYTES = 512 * 1024;   // PSRAM ring (device)
constexpr size_t   INFLUX_CHUNK_BYTES   = 8 * 1024;     // per request (one larger push still goes alone)
constexpr uint32_t INFLUX_RETRY_MIN_MS  = 30000;        // first retry after a failure, doubled ...
constexpr uint32_t INFLUX_RETRY_MAX_MS  = 600000;       // ... up to 10 min

// What a reply means for the bodies that were sent:
//   OK    2xx: written - remove them
//   DROP  400 / 413 / 422 and other 4xx: the data itself was refused (bad
//         line, too large, partial write) - retrying cannot help, remove them
//   RETRY no reply (timeout, connection, DNS: code <= 0), 5xx, and the 4xx that
//         mean "not now / not configured right" (401, 403, 404 = no such bucket
//         / database, 408, 429): keep them, retry later with backoff - the data
//         is fine and gets written once the server or the setting is fixed
enum InfluxReply : uint8_t { IR_OK, IR_DROP, IR_RETRY };
InfluxReply influxClassify(int code);

// Delay before the next try after `failsInRow` failures (>= 1).
uint32_t influxRetryDelayMs(uint32_t failsInRow);

// What one send result changed, for the log lines.
struct InfluxEvent {
    InfluxReply action = IR_OK;
    bool     outageStarted = false;   // first failure: buffering starts
    bool     cleared = false;         // backlog sent completely after an outage
    uint32_t resent = 0;              // with `cleared`: pushes sent from the backlog
    uint32_t dropped = 0;             // with `cleared`: pushes lost meanwhile (full + refused)
};

class InfluxQueue {
public:
    // Use `buf` (cap bytes, owned by the caller) as the ring; empties it.
    void attach(uint8_t* buf, size_t cap);
    void detach() { attach(nullptr, 0); }
    bool attached() const { return buf_ != nullptr; }

    // Append one push (its body, epoch = its timestamp in s). Drops the oldest
    // pushes until it fits; a body larger than the ring is dropped itself
    // (false). Counted in droppedFull().
    bool push(uint32_t epochSec, const char* body, size_t len);

    // Append the oldest bodies to `out` while the total stays <= maxBytes (at
    // least one when not empty); returns how many. Nothing is removed.
    size_t peek(std::string& out, size_t maxBytes) const;

    // A send of the `n` pushes of the last peek() ended with HTTP `code`.
    InfluxEvent result(int code, size_t n, uint32_t nowMs);

    // The network is down (Wi-Fi): a push is buffered without a try. Returns
    // true when this starts an outage (for the log line).
    bool noteOffline();

    // Due for a send: something queued and no retry delay running.
    bool due(uint32_t nowMs) const;

    size_t   count() const { return count_; }       // pushes queued
    size_t   used() const { return used_; }         // bytes incl. 8-byte headers
    size_t   capacity() const { return cap_; }
    uint32_t oldestEpoch() const;                   // 0 when empty
    uint32_t droppedFull() const { return droppedFull_; }
    uint32_t droppedRejected() const { return droppedRejected_; }
    uint32_t failsInRow() const { return fails_; }
    bool     inOutage() const { return outage_; }

private:
    static constexpr size_t HDR = 8;   // uint32 epoch + uint32 length
    void readAt(size_t off, void* dst, size_t n) const;
    void writeAt(size_t off, const void* src, size_t n);
    uint32_t lenAt(size_t off) const;
    void dropOldest();

    uint8_t* buf_ = nullptr;
    size_t   cap_ = 0, head_ = 0, used_ = 0, count_ = 0;
    uint32_t droppedFull_ = 0, droppedRejected_ = 0;
    uint32_t fails_ = 0, nextTryMs_ = 0;
    bool     waiting_ = false;               // a retry delay runs until nextTryMs_
    bool     outage_ = false;
    uint32_t outageResent_ = 0, outageDropped0_ = 0;
};

// ---- device (wx_upload.cpp): a copy for the web task ----
struct InfluxBacklogStatus {
    bool     active = false;    // ring allocated
    uint32_t pushes = 0, bytes = 0, capacity = 0;
    uint32_t oldestEpoch = 0;   // 0 = empty
    uint32_t droppedFull = 0, droppedRejected = 0;
};
void influxBacklogStatus(InfluxBacklogStatus& out);
