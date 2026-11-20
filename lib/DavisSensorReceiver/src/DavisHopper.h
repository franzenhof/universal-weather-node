#pragma once
// Frequency-hopping follower for one Davis transmitter - a pure state machine
// without RadioLib (host-testable). The caller owns the radio and the clock:
//
//   hopper.configure(cfg, nowUs);
//   loop:
//     if a packet arrived:  r = hopper.onPacket(bytes, rxDoneUs, model)
//     while ((ev = hopper.poll(nowUs)).kind != DAVIS_EV_NONE) log(ev);
//     tune the radio to hopper.tuneMHz() if that changed
//     sleep until hopper.nextDeadlineUs() or the next packet
//
// Times are microseconds of a monotonic 64-bit clock; packet times are the
// "RX done" moment (end of the packet), consistently.
//
// States:
//  ACQUIRE  parked on one channel until the transmitter comes by (it visits
//           every channel once per hop cycle = 5 packets on the EU band, at
//           most 5 x 3.0 s). Without a packet within that dwell the receiver
//           tries the next frequency offset of the scan (0, +step, -step,
//           +2 step, ... up to +-scanSpanKhz around the base offset) and,
//           after a full scan, the next channel.
//  TRACK    after a valid packet on channel n the receiver moves to channel
//           n+1 at once (nothing else to listen to) and expects the next
//           packet one period later. A packet of the tracked ID counts only
//           within -guard..+window of the prediction; none by then = a miss,
//           and the receiver moves on to the channel after. The period is
//           tracked with a simple PLL (the measured error, divided by the
//           slots since the last packet, corrects the estimate).
//  LOST     lostAfter misses in a row: acquisition again, the scan centred
//           on the last offset that worked.
//  RAW      raw capture: parked on one channel, no hopping, every packet
//           that passes the sync word is reported (diagnostics).
//
// Optional AFC (software, the SX1262 reports no frequency error in FSK
// mode): every 4th slot of TRACK listens 6 kHz above or below the current
// offset (alternating); when one side hears clearly more packets, the
// offset moves 1.5 kHz towards it. Probe slots never count as misses.
#include <stdint.h>
#include "DavisDecoder.h"

struct DavisBand {
    const float* mhz;        // channel frequencies in hop order
    uint8_t      count;
    const char*  name;
};
extern const DavisBand DAVIS_BAND_EU;           // EU 868 MHz, measured 2026 (kobuki/VPTools)
extern const DavisBand DAVIS_BAND_EU_CLASSIC;   // EU 868 MHz, the original DavisRFM69 table (2014)

enum DavisHopState : uint8_t { DAVIS_ST_ACQUIRE = 0, DAVIS_ST_TRACK, DAVIS_ST_LOST, DAVIS_ST_RAW };
const char* davisStateName(DavisHopState s);    // "ACQUIRE", "TRACK", "LOST", "RAW"

struct DavisHopperConfig {
    const DavisBand* band = &DAVIS_BAND_EU;
    uint8_t  txId = 0;               // 1-8; 0 = the first transmitter heard
    int32_t  offsetHz = 0;           // base frequency correction
    uint16_t guardMs = 40;           // a packet may come this early ...
    uint16_t windowMs = 25;          // ... or this late; later = miss
    uint8_t  lostAfter = 12;         // misses in a row -> LOST (re-acquire)
    uint16_t scanSpanKhz = 30;       // acquisition offset scan +-span; 0 = base offset only
    bool     afc = false;            // shoulder-probing AFC while tracking
    bool     raw = false;            // raw capture on rawChannel
    uint8_t  rawChannel = 0;
};

constexpr int32_t DAVIS_SCAN_STEP_HZ   = 6000;
constexpr int32_t DAVIS_AFC_PROBE_HZ   = 6000;
constexpr int32_t DAVIS_AFC_STEP_HZ    = 1500;
constexpr int32_t DAVIS_AFC_LIMIT_HZ   = 60000;   // |learned - base| at most
constexpr int64_t DAVIS_LATENCY_US     = 5000;    // slack for the caller's wake-up before a miss is declared
constexpr int64_t DAVIS_DWELL_EXTRA_US = 400000;  // acquisition dwell = 5 periods + this

enum DavisPktClass : uint8_t {
    DAVIS_PKT_ACCEPTED = 0,   // the tracked transmitter: values valid, timing re-anchored
    DAVIS_PKT_CRC_FAIL,       // passed the sync word, CRC wrong (noise, interference, off-frequency)
    DAVIS_PKT_OTHER_ID,       // valid packet of another transmitter ID
    DAVIS_PKT_REPEATER,       // valid only with the repeater bytes (relayed) - ignored
    DAVIS_PKT_BAD_TRAILER,    // CRC OK but bytes 8-9 not FF FF - ignored
    DAVIS_PKT_OUT_OF_WINDOW,  // tracked ID, but outside -guard..+window (a second station with the same ID?)
    DAVIS_PKT_RAW_OK,         // raw capture: valid packet (any ID)
};
const char* davisPktClassName(DavisPktClass c);

struct DavisRxResult {
    DavisPktClass cls = DAVIS_PKT_CRC_FAIL;
    DavisPacket   pkt;
    DavisHopState state = DAVIS_ST_ACQUIRE;   // after the packet
    uint8_t  channel = 0;                     // channel it was received on
    float    freqMHz = 0.0f;
    int32_t  offsetHz = 0;                    // offset the receiver was tuned with
    bool     hasTiming = false;               // TRACK: timingErrUs valid
    int32_t  timingErrUs = 0;                 // arrival - prediction
    bool     locked = false;                  // this packet started tracking
    bool     probe = false;                   // received in an AFC probe slot
};

enum DavisEventKind : uint8_t {
    DAVIS_EV_NONE = 0,
    DAVIS_EV_MISS,         // TRACK: no packet in the window (a = channel, b = misses in a row)
    DAVIS_EV_PROBE_MISS,   // TRACK: AFC probe slot without packet (not a miss)
    DAVIS_EV_LOST,         // TRACK -> LOST (a = channel parked on)
    DAVIS_EV_SCAN_STEP,    // ACQUIRE/LOST: next offset / channel (a = channel, offsetHz)
    DAVIS_EV_AFC_STEP,     // AFC moved the offset (offsetHz = new learned offset)
};
struct DavisEvent {
    DavisEventKind kind = DAVIS_EV_NONE;
    uint8_t  a = 0, b = 0;
    int32_t  offsetHz = 0;
    int64_t  atUs = 0;
};

class DavisHopper {
public:
    // Full (re)start with a new configuration.
    void configure(const DavisHopperConfig& c, int64_t nowUs);
    // Live change: restarts only if band, ID, offset base or raw mode change;
    // guard, window, lostAfter, scan span and AFC apply at once.
    void update(const DavisHopperConfig& c, int64_t nowUs);

    DavisRxResult onPacket(const uint8_t air[DAVIS_PACKET_LEN], int64_t rxUs, DavisModel model = DAVIS_VP2);
    DavisEvent    poll(int64_t nowUs);   // call until it returns DAVIS_EV_NONE

    // Where the receiver has to be tuned now.
    uint8_t  channel() const { return ch_; }
    int32_t  tuneOffsetHz() const { return tuneOffset_; }
    float    tuneMHz() const;
    float    channelMHz(uint8_t ch) const;
    int64_t  nextDeadlineUs() const;

    DavisHopState state() const { return state_; }
    uint8_t  lockedId() const { return lockedId_; }
    double   periodUs() const { return periodUs_; }       // estimate (nominal until tracked)
    int32_t  learnedOffsetHz() const { return learned_; }
    int64_t  lastPacketUs() const { return lastOkUs_; }    // 0 = none yet
    int64_t  nextExpectedUs() const { return nextUs_; }
    uint8_t  missStreak() const { return missStreak_; }
    const DavisHopperConfig& config() const { return cfg_; }

    // Counters since configure()
    uint32_t accepted = 0;      // packets of the tracked transmitter
    uint32_t crcErrors = 0;
    uint32_t otherPackets = 0;  // other ID, repeater, bad trailer, out of window
    uint32_t misses = 0;        // TRACK slots without packet (probe slots excluded)
    uint32_t trackSlots = 0;    // TRACK slots resolved (accepted + missed, probes excluded)
    uint32_t resyncs = 0;       // LOST -> TRACK
    uint32_t scanSteps = 0;

private:
    void     startAcquire(int64_t nowUs, uint8_t parkCh, int32_t center, bool lost);
    void     enterTrack(uint8_t rxCh, int64_t rxUs);
    void     nextSlot(bool afterPacket);
    int32_t  scanOffset(uint16_t idx) const;
    uint16_t scanCount() const;
    int64_t  dwellUs() const;
    void     push(DavisEventKind k, uint8_t a, uint8_t b, int32_t off, int64_t at);
    void     afcResult(bool ok);

    DavisHopperConfig cfg_;
    DavisHopState state_ = DAVIS_ST_ACQUIRE;
    uint8_t  ch_ = 0;
    int32_t  tuneOffset_ = 0;
    int32_t  learned_ = 0;        // offset of the tracked transmitter
    uint8_t  lockedId_ = 0;
    double   periodUs_ = 0.0;
    double   nominalUs_ = 0.0;
    // TRACK
    int64_t  anchorUs_ = 0;       // last accepted packet
    uint32_t k_ = 0;              // slots since the anchor (the expected one included)
    int64_t  nextUs_ = 0;         // predicted arrival of the slot listened to now
    uint8_t  missStreak_ = 0;
    uint32_t slotNo_ = 0;         // for the AFC probe pattern
    bool     probe_ = false;      // current slot is an AFC probe
    int8_t   probeSide_ = 1;
    uint8_t  probeN_[2] = {0, 0}, probeOk_[2] = {0, 0};   // [0] = +, [1] = -
    int64_t  lastOkUs_ = 0;
    // ACQUIRE / LOST
    int64_t  dwellStartUs_ = 0;
    uint16_t scanIdx_ = 0;
    int32_t  scanCenter_ = 0;
    bool     dwellExtended_ = false;
    // events
    DavisEvent ev_[6];
    uint8_t  evHead_ = 0, evCount_ = 0;
};
