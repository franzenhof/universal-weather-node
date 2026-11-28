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

// Interval learned from the gap between two packets of one sensor.
// periodMs: learned so far (0 = none), nominalMs: from the type (0 = unknown,
// e.g. Bresser). A gap that holds several intervals (missed packets) is
// divided by their number; a clearly shorter interval replaces the guess (the
// first gap of a sensor without a nominal interval may hold two).
uint32_t rfLearnPeriod(uint32_t periodMs, uint32_t nominalMs, uint32_t gapMs);

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
// would leave a receiver that is never switched (one phase, no LoRaWAN) deaf
// until a reboot. A receiver that has neither delivered a frame of any kind
// nor been (re)started for RF_RECEIVE_WATCHDOG_MS is restarted. Wrap-safe (unsigned difference).
constexpr uint32_t RF_RECEIVE_WATCHDOG_MS = 600000;   // 10 min
bool rfReceiverSilentTooLong(uint32_t nowMs, uint32_t lastLifeSignMs);
