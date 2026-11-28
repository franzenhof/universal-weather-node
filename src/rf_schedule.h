#pragma once
// Receive scheduling across protocol phases (Bresser, 17.241 kbit/s with
// Fine Offset, IT+ 9.579 kbit/s). Host-tested (test/firmware_rfschedule.cpp).
//
// The SX1262 listens to one data rate at a time. With sensors of more than
// one family, plain rotation (rfPhaseDwellMs()) misses about every other
// packet. Each known sensor sends at a steady interval, so its next packet
// can be predicted from the last one: shortly before it is due the receiver
// switches to that sensor's phase and stays until just after. Between such
// windows the rotation runs as before, which still finds new sensors and
// recaptures lost ones.
//
// Pure functions, no state of their own: the learned interval lives in
// RfSensorReading::periodMs, the decision is taken from the sensor table.
#include <stdint.h>

constexpr uint32_t RF_SCHED_EARLY_MS   = 1200;     // listen this long before a packet is due
constexpr uint32_t RF_SCHED_LATE_MS    = 1200;     // ... and this long after
constexpr uint32_t RF_SCHED_MIN_GAP_MS = 2000;     // shorter gaps: repeats, not the interval
constexpr uint32_t RF_SCHED_MAX_GAP_MS = 1200000;  // 20 min
constexpr uint32_t RF_SCHED_MAX_MISSED = 8;        // intervals without a packet: no longer predicted

// A sensor with a fixed send interval never sends two transmissions closer
// than this share of it (jitter is a few per cent): a shorter gap is a second
// transmitter with the same ID, or noise - never the interval.
constexpr uint32_t RF_SCHED_MIN_INTERVAL_NUM = 3, RF_SCHED_MIN_INTERVAL_DEN = 4;   // 0.75 x the fixed interval

// Interval learned from the gap between two packets of one sensor.
// periodMs: learned so far (0 = none), fixedIntervalMs: the type's fixed send
// interval (rfFixedIntervalMs(); 0 = unknown, e.g. Bresser). A gap that holds
// several intervals (missed packets) is divided by their number; a clearly
// shorter interval replaces the guess (the first gap of a sensor without a
// fixed interval may hold two). With a fixed interval nothing below 0.75 x it
// is ever learned (two transmitters on one ID would otherwise trap the period
// at a fraction of the real one).
uint32_t rfLearnPeriod(uint32_t periodMs, uint32_t fixedIntervalMs, uint32_t gapMs);

// A packet gapMs after the previous one of the same ID that one sensor with
// this fixed interval cannot have sent: shorter than 0.75 x the interval and
// either at least RF_SCHED_MIN_GAP_MS (not a repeat) or a closer "repeat"
// whose values differ from the previous packet (a real repeat carries the same
// values). Evidence of an ID collision (reception_window.h). Always false
// without a fixed interval.
bool rfGapImpossible(uint32_t gapMs, uint32_t fixedIntervalMs, bool valuesDiffer);

// One tracked sensor: its receive phase (RF_PROTO_BRESSER / _ITPLUS17 /
// _ITPLUS9), last packet and learned interval.
struct RfSchedEntry {
    uint8_t  phase;
    uint32_t lastMs;
    uint32_t periodMs;
};

// The phase a sensor's packet is due in right now, 0 = none (free time).
// Several due at once: the earliest expected packet first (after it arrived
// that sensor's window closes and the next one is served); at the same
// expected time the sensor that missed more intervals.
int rfSchedDuePhase(const RfSchedEntry* e, int n, uint32_t nowMs);

// Receive watchdog (rf_sensors.cpp): the receivers re-arm reception after each
// frame without checking the result, so one lost restart (SPI / BUSY timeout)
// would leave a receiver that is never switched (one phase, no LoRaWAN, no
// virtual sensors) deaf until a reboot. A receiver that has neither delivered
// a frame of any kind nor been (re)started for RF_RECEIVE_WATCHDOG_MS is
// restarted. Wrap-safe (unsigned difference).
constexpr uint32_t RF_RECEIVE_WATCHDOG_MS = 600000;   // 10 min
bool rfReceiverSilentTooLong(uint32_t nowMs, uint32_t lastLifeSignMs);
