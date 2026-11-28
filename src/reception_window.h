#pragma once
// Reception watchdog for ★ sensors (B10). Host-tested (test/firmware_reception.cpp).
//
// Every ★ sensor sends at a steady interval, which the receive scheduling
// learns (RfSensorReading::periodMs, rf_schedule.h). Packets received in the
// last hour against packets expected in that hour tells a reception problem
// (antenna detuned, node moved, a new wall) from a sensor that sends nothing
// at all - the latter stays "silent" / stale (sources.h, alarms "silent for").
//
// State: a ring of RECEPTION_BUCKETS 10-minute counters per sensor (20 bytes,
// part of RfSensorReading in internal RAM - fixed, no allocation in the radio
// path). The newest bucket is the one being filled; buckets the clock has
// moved past without a packet are accounted for at read time, so the reader
// never writes.
//
// Rules (receptionEvaluate()):
//  - unknown: no learned interval, the window not started (first packet after
//    a start, after a ★ / re-bind, after silence), less than
//    RECEPTION_MIN_SPAN_MS of data or fewer than RECEPTION_MIN_EXPECTED
//    expected packets, or an interval so short that a bucket counter could
//    saturate;
//  - percent = received * interval / covered time, at most 100;
//  - poor = percent < RECEPTION_POOR_PERCENT once the window covers a full
//    hour (RECEPTION_FULL_SPAN_MS). The sensor still sends: no packet for
//    RECEPTION_SILENCE_MS ends the window (silent, not poor), the next packet
//    starts a new one.
// No hysteresis: the hour-long window already smooths single misses.
//
// ID collision (two transmitters sending with one radio ID, typical for IT+
// with only 64 IDs): a sensor with a fixed send interval never sends two
// packets closer than RECEPTION_ID_COLLISION_* below allows (rfGapImpossible(),
// rf_schedule.h, decides per packet). The window keeps a second ring with the
// number of such impossible gaps per bucket. Suspected when, over the window,
// at least RECEPTION_ID_COLLISION_MIN_GAPS impossible gaps were seen AND they
// are at least RECEPTION_ID_COLLISION_MIN_PERCENT of the counted packets;
// cleared once the window holds no impossible gap any more (about an hour
// without one). While suspected the percentage is unknown: the counts of two
// transmitters say nothing about the reception of either.
//
// Concurrency: one writer task per sensor entry (the radio loop on core 1, the
// Davis receive task in Davis mode; receptionStopIfSilent() runs from
// rfExpireValues() on the radio loop like the staleMask expiry). Readers on
// other tasks (web pages, JSON, MQTT, alarms) read the plain 8/32-bit fields
// without a lock: a read during an update can be off by one bucket once -
// acceptable for a display value and for an alarm with a minimum duration.
//
// millis() wrap: only differences of uint32_t times are used, and the silence
// stop keeps lastCountMs younger than RECEPTION_SILENCE_MS while a window runs.
#include <stdint.h>

constexpr uint32_t RECEPTION_BUCKET_MS        = 600000;    // 10 min per counter
constexpr int      RECEPTION_BUCKETS          = 7;         // the current bucket + 6 full ones = 60-70 min
constexpr uint32_t RECEPTION_FULL_SPAN_MS     = (RECEPTION_BUCKETS - 1) * RECEPTION_BUCKET_MS;   // 1 h: "poor" only after this
constexpr uint32_t RECEPTION_MIN_SPAN_MS      = 600000;    // a percentage only after 10 min of data
constexpr uint32_t RECEPTION_MIN_EXPECTED     = 3;         // ... and at least this many expected packets
constexpr uint32_t RECEPTION_SILENCE_MS       = 3600000;   // no packet for 1 h: silent, the window ends
constexpr uint8_t  RECEPTION_POOR_PERCENT     = 30;        // below: "reception poor"
// A shorter interval could fill a counter (255) within one bucket: unknown then.
constexpr uint32_t RECEPTION_MIN_PERIOD_MS    = RECEPTION_BUCKET_MS / 255 + 1;
// ID collision thresholds. A single sensor produces no impossible gap at all
// (its gaps are its interval or multiples of it, jitter is far below 25 %;
// repeats of one transmission carry the same values and are not impossible),
// so the thresholds only guard against rare corrupted packets that pass the
// checksum with this ID: 5 such packets within an hour, and a fifth of all
// packets, is far beyond that. Two transmitters produce an impossible gap on
// about every other packet (whenever their offset leaves a gap below 0.75
// intervals), i.e. ~50 %: an IT+ pair is flagged within about a minute.
constexpr uint32_t RECEPTION_ID_COLLISION_MIN_GAPS    = 5;
constexpr uint32_t RECEPTION_ID_COLLISION_MIN_PERCENT = 20;

struct ReceptionWindow {
    uint32_t newestBucketStartMs = 0;              // millis() at which the newest bucket began
    uint32_t lastCountedMs = 0;                    // millis() of the last packet counted
    uint8_t  packetsPerBucket[RECEPTION_BUCKETS] = {};   // ring; saturates at 255
    uint8_t  impossibleGapsPerBucket[RECEPTION_BUCKETS] = {};   // same ring: ID collision evidence; saturates at 255
    uint8_t  newestBucket = 0;                     // index of the bucket being filled
    uint8_t  bucketsCovered = 0;                   // buckets since the window started, incl. the newest; 0 = not running
    bool     idCollisionLatched = false;           // thresholds reached; held until no impossible gap is left in the window
};
static_assert(sizeof(ReceptionWindow) <= 28, "per-sensor state stays tiny (internal RAM)");

struct ReceptionVerdict {
    bool    known = false;     // false: no percentage (see the rules above)
    uint8_t percent = 0;       // 0-100, valid when known
    bool    poor = false;      // known, a full hour covered and below RECEPTION_POOR_PERCENT
    bool    idCollision = false;   // two transmitters suspected on this radio ID (known is false then)
};

// A packet of the sensor arrived at nowMs. Repeats of one transmission
// (closer than RF_SCHED_MIN_GAP_MS to the last counted packet) are not counted.
// impossibleGap: this packet came impossibly soon after the previous one of the
// same ID (rfGapImpossible()) - evidence of a second transmitter; also recorded
// for a packet that is not counted as a repeat.
void receptionCountPacket(ReceptionWindow& w, uint32_t nowMs, bool impossibleGap = false);
// Ends the window when nothing was counted for RECEPTION_SILENCE_MS (call regularly).
void receptionStopIfSilent(ReceptionWindow& w, uint32_t nowMs);
// Ends the window now (sensor no longer ★, a re-bind moved the ★ elsewhere).
void receptionStop(ReceptionWindow& w);
// The verdict at nowMs for a sensor with the learned interval periodMs (0 = none).
ReceptionVerdict receptionEvaluate(const ReceptionWindow& w, uint32_t periodMs, uint32_t nowMs);
