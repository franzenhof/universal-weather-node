#pragma once
// Per wireless sensor: rain today / last hour from the increments of the
// sensor's own rain counter, lightning strikes today / last hour and the last
// strike from the WH57's 8-bit strike counter. RAM only (lost at a restart:
// the counters themselves stay in the sensors). Up to RF_STATS_MAX sensors
// with rain or lightning. Host-tested (test/firmware_rfstats.cpp).
#include <stdint.h>

constexpr int RF_STATS_MAX = 12;

struct RfStat {
    uint32_t id = 0;                 // 0 = free
    uint32_t ymd = 0;                // local date today* belongs to (yyyymmdd), 0 = clock not known yet
    uint32_t bucket = 0;             // (time / 600) of the newest 10-minute bucket
    float    rainTodayMm = 0.0f;
    uint16_t rainB[6] = {};          // rain per 10 min, 0.01 mm (the last hour)
    uint16_t strikesToday = 0;
    uint8_t  strikeB[6] = {};        // strikes per 10 min
    bool     countValid = false;     // lastCount is a baseline
    uint8_t  lastCount = 0;
    uint32_t lastStrike = 0;         // UTC seconds of the last counted strike, 0 = none
};

// t: seconds (UTC when ymd != 0, else uptime); ymd: local date or 0 (unknown).
void  rfStatRain(RfStat& s, float mm, uint32_t t, uint32_t ymd);
// Counter of the sensor (8 bit, wraps): the first value is the baseline; a
// jump of more than 50 strikes in one packet is a counter reset (not counted).
// Returns the strikes counted.
int   rfStatLightning(RfStat& s, uint8_t count, uint32_t t, uint32_t ymd);
float rfStatRain1h(RfStat& s, uint32_t t);
int   rfStatStrikes1h(RfStat& s, uint32_t t);
// Rain rate (mm/h) of the current and the previous 10-minute bucket (10-20 min)
float rfStatRecentRate(RfStat& s, uint32_t t);
// The day / bucket roll-over without new data (for reading).
void  rfStatRoll(RfStat& s, uint32_t t, uint32_t ymd);

// ---- table (rf_sensors_data.cpp feeds it, the outputs read it) ----
// Written by the receiving (loop) task only, between rfStatsLock() and
// rfStatsUnlock() (a short critical section: no I/O, no allocation in between).
// Every other reader - JSON payload, web pages, also on the loop task - takes
// a copy with rfStatSnapshot(), so a reader never rolls the shared entry over
// (a day / bucket roll-over from the web task while the loop task adds rain
// would lose or double-count it).
void    rfStatsLock();
void    rfStatsUnlock();
RfStat* rfStatFor(uint32_t id, bool create);   // under the lock
void    rfStatsClear();                        // takes the lock itself
// frees the entries of sensors no longer in the table (used(id) false); under the lock
void    rfStatsPrune(bool (*used)(uint32_t id));
// A copy of the sensor's entry, rolled over to now (t: the statistics clock,
// for rfStatRain1h() / rfStatStrikes1h() on the copy). false = no entry.
bool    rfStatSnapshot(uint32_t id, RfStat& out, uint32_t& t);
// clock for the statistics (device: time_sync; weak default: unknown)
bool    rfStatsClock(uint32_t& t, uint32_t& ymd);
