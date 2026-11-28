#pragma once
// Virtual sensors (TODO item 9, docs/virtual-sensors.md): the node transmits
// its own values as Fine Offset (Ecowitt) sensor frames on 868.3 MHz, so a
// gateway or console shows e.g. a wired Davis or a BME280 as a WH65 / WN32P,
// or as LaCrosse / TFA IT+ frames (TX29 / TX35 style) for an IT+ station.
//
// The red line, held by the structure of this module (test/firmware_vsensor.cpp
// checks it with static_asserts):
//  (a) only frames built from the node's own values by the typed encoders
//      (FineOffsetEncoder.h, ItPlusEncoder.h): a VsFrame can only be made by vsBuildFrame(),
//      from a slot's configuration and a VsValues of numbers, and the radio
//      entry point vsRadioSend() takes nothing but a VsFrame. There is no
//      function, setting, API, MQTT command or downlink that takes bytes to
//      send.
//  (b) no actuator / gateway / command frame builders exist in the repo.
//  (c) the ID is never configured: VsSlotCfg has no ID field; vsBuildFrame()
//      takes it from vsDeriveId() (the node's MAC + slot), so it can never be
//      a foreign sensor's ID, and nothing re-sends a received frame.
//  (d) off by default (VsSettings::enabled = 0, every slot off).
//
// This part has no firmware dependencies (host-tested): types, the ID, the
// field mapping, the frame, scheduling, the airtime ledger, the stored form.
// The settings live in their own store (vs_store.h, PSRAM, NVS blob "vsens"),
// not in Config; the device side (value lookup, radio, loop) is vs_run.cpp.
#include <stddef.h>
#include <stdint.h>

// 8 like the WH31 channels an Ecowitt gateway / console knows.
constexpr int VS_SLOTS = 8;
constexpr size_t VS_MAX_FRAME_LEN = 32;   // >= FO_MAX_FRAME_LEN (checked in vsensor.cpp)

// VS_ITPLUS17 / VS_ITPLUS9: LaCrosse / TFA IT+ temperature (+ humidity) frame
// at 17.241 kbit/s (TX29-IT style) / 9.579 kbit/s (TX35-IT style).
enum VsType : uint8_t { VS_OFF = 0, VS_WH31, VS_WH40, VS_WN32P, VS_WH65, VS_ITPLUS17, VS_ITPLUS9, VS_TYPE_COUNT };
enum VsField : uint8_t { VF_TEMP = 0, VF_HUM, VF_PRESS, VF_WIND, VF_GUST, VF_DIR, VF_RAIN, VF_UV, VF_LUX, VF_COUNT };

// A value name (placeholder name, ph_live.h, without braces, in the stored
// form: "temp", "dew", "@L3F9A2C_temp", "rf300007DD_hum", "ds1", "rain_total").
constexpr size_t VS_SRC_LEN = 16;

// Radio parameters: what the Fine Offset receiver in this repo expects
// (FineOffsetSensor::RxConfig / startReceive()): 868.3 MHz, 17.241 kbit/s
// NRZ FSK, no whitening, no hardware CRC, sync word 2D D4, fixed length.
// Deviation: the 50 kHz the receiver config carries for TX (not measured
// on an original sensor - checked over the air). Preamble 48 bits (AA..),
// like the longer preambles of the originals (32-48 bits).
// IT+ uses the same PHY (TfaWeatherSensor::startReceive(): sync 2D D4, 5-byte
// frames) at 17.241 or 9.579 kbit/s (VsTypeInfo::kbps); deviation and
// preamble taken over from the Fine Offset types - not measured on an
// original IT+ sensor (the receiver here listens with 234 kHz bandwidth).
constexpr float    VS_FREQ_MHZ      = 868.3f;
constexpr float    VS_BITRATE_KBPS  = 17.241f;   // Fine Offset, IT+ TX29 style
constexpr float    VS_BITRATE_IT9_KBPS = 9.579f; // IT+ TX35 style
constexpr float    VS_DEV_KHZ       = 50.0f;
constexpr uint16_t VS_PREAMBLE_BITS = 48;
constexpr int      VS_DBM_MIN = 0, VS_DBM_MAX = 14, VS_DBM_DEFAULT = 10;   // at the antenna; EU 868.0-868.6 MHz: 25 mW ERP

// 868.0-868.6 MHz (ETSI EN 300 220, band g1): 1 % duty cycle = 36 s per hour,
// shared with the LoRaWAN uplinks on 868.1/.3/.5 MHz (which RadioLib limits
// to 1 % by themselves). The virtual sensors only use what is left of the
// node's band airtime in the last hour, and a configuration may plan at
// most half of the budget.
constexpr uint32_t VS_BAND_BUDGET_MS   = 36000;
constexpr uint32_t VS_PLAN_MAX_MS_PER_H = VS_BAND_BUDGET_MS / 2;
constexpr uint32_t VS_TX_OVERHEAD_MS   = 2;      // PA ramp, mode switch (counted as airtime, conservative)
constexpr int32_t  VS_JITTER_MS        = 250;    // +- per interval, like the originals' drift
constexpr uint32_t VS_MAX_DEFER_MS     = 3000;   // wait at most this long for a received sensor's packet window

struct VsTypeInfo {
    const char* key;          // stored form, API ("wh31", ...)
    const char* model;        // shown ("WH31", ...)
    uint16_t    fields;       // VF_* bits the frame carries
    uint16_t    mustMap;      // a slot without a source for these is refused
    uint16_t    skipMissing;  // mapped but unavailable now: the frame is skipped (not sent with a "none" code)
    uint32_t    intervalMs;   // the original's send interval
    uint8_t     frameLen;     // bytes after the sync word
    float       kbps;         // bit rate
};
extern const VsTypeInfo VS_TYPES[VS_TYPE_COUNT];
const char* vsFieldKey(uint8_t f);     // "temp", "hum", ... (form field / stored form)
int vsFieldByKey(const char* key);     // -1 = unknown
int vsTypeByKey(const char* key);      // -1 = unknown

// One slot's configuration. Deliberately no ID field: the ID is derived (c).
struct VsSlotCfg {
    uint8_t type = VS_OFF;
    uint8_t enabled = 0;
    uint8_t channel = 1;                    // WH31: 1..8
    char    src[VF_COUNT][VS_SRC_LEN] = {}; // "" = not sent
};

// All virtual sensor settings: the switch, the transmit power and the slots
// (vs_store.h keeps the node's copy).
struct VsSettings {
    uint8_t   enabled    = 0;                // the feature as a whole; off by default (d)
    int8_t    txPowerDbm = VS_DBM_DEFAULT;   // at the antenna, VS_DBM_MIN..VS_DBM_MAX
    VsSlotCfg slot[VS_SLOTS];
};

// The fixed ID of a slot, derived from the node's MAC (6 bytes) and the slot:
// 8 bits (1..254) for WH31 / WN32P / WH65, 20 bits (1..0xFFFFE) for WH40,
// 6 bits (0..63) for IT+. Different slots of one node always get different IDs.
uint32_t vsDeriveId(const uint8_t mac[6], int slot, uint8_t type);

// The receiver's table ID of that sensor (rf_fsk17.h rfFineOffsetId(),
// rf_sensors.h rfItPlusId()): family code << 24 | id (WH31: channel << 8 | id;
// WN32P: 25 << 24 | id); IT+: 17000000 / 09000000 | id, + 0x100 with humidity.
uint32_t vsTableId(uint8_t type, uint32_t id, uint8_t channel, bool humidity = false);
// The table ID a slot is shown with (IT+: with humidity when one is mapped).
uint32_t vsSlotTableId(const VsSlotCfg& c, int slot, const uint8_t mac[6]);
// A received sensor's table ID collides with the slot: the same ID, for IT+
// also the other humidity variant (a station tells IT+ sensors apart by the
// 6-bit ID only).
bool vsIdCollides(const VsSlotCfg& c, int slot, const uint8_t mac[6], uint32_t receivedId);

// IT+ pairing: the "new battery" flag is set for this long after a slot was
// enabled or changed (or the node started) - an IT+ station learns a sensor's
// ID from such frames during its sensor search.
constexpr uint32_t VS_ITPLUS_PAIR_MS = 5 * 60 * 1000;
inline bool vsPairing(uint64_t nowMs, uint64_t armedMs)
{
    return nowMs >= armedMs && nowMs - armedMs < VS_ITPLUS_PAIR_MS;
}

// The values of one frame, metric (°C, %, hPa, m/s, °, mm running total, UV index, lux).
struct VsValues {
    uint16_t has = 0;          // VF_* bits
    float    v[VF_COUNT] = {};
};

// name -> metric value; false = unknown / not available now
typedef bool (*VsLookup)(const char* name, float& out, void* ctx);

// Fills out from the slot's sources. false = the frame must be skipped now
// (a skipMissing value is unavailable, or no value at all).
bool vsResolve(const VsSlotCfg& c, VsLookup lookup, void* ctx, VsValues& out);

// A frame ready for the radio. Only vsBuildFrame() can make one with content.
class VsFrame {
public:
    const uint8_t* data() const { return b; }
    size_t         size() const { return n; }
    uint32_t       id() const { return sensorId; }
    float          kbps() const { return rate; }   // bit rate of the type
private:
    VsFrame() = default;
    uint8_t  b[VS_MAX_FRAME_LEN] = {};
    uint8_t  n = 0;
    uint32_t sensorId = 0;
    float    rate = VS_BITRATE_KBPS;
    friend VsFrame vsBuildFrame(const VsSlotCfg& c, int slot, const uint8_t mac[6], const VsValues& v, bool pairing);
};

// Encodes the slot's frame (typed encoder, ID from vsDeriveId()); size() == 0
// if the type is off or the values do not make a frame. pairing: IT+ sets
// the new-battery flag (vsPairing()); ignored by the other types.
VsFrame vsBuildFrame(const VsSlotCfg& c, int slot, const uint8_t mac[6], const VsValues& v, bool pairing);

// The radio entry point (vs_run.cpp): sends one frame, true = sent. Takes
// nothing but a VsFrame - there is no raw-byte variant.
bool vsRadioSend(const VsFrame& f);

// Airtime of one frame (preamble + sync + payload at kbps + overhead), ms.
uint32_t vsAirtimeMs(size_t frameLen, float kbps = VS_BITRATE_KBPS);
uint32_t vsTypeAirtimeMs(uint8_t type);   // of the type's frame, 0 = off

// The node's airtime in the 868.0-868.6 MHz band, per minute over the last
// hour (virtual sensors + LoRaWAN uplinks). nowMs: a 64-bit monotonic clock.
struct VsBandLedger {
    uint16_t ms[60] = {};
    uint64_t minute = 0;    // minute of ms[minute % 60]
    void     add(uint64_t nowMs, uint32_t airMs);
    uint32_t lastHourMs(uint64_t nowMs);
};
// Room for a frame of airMs now?
inline bool vsBudgetAllows(VsBandLedger& l, uint64_t nowMs, uint32_t airMs)
{
    return l.lastHourMs(nowMs) + airMs <= VS_BAND_BUDGET_MS;
}

// Planned airtime per hour of the enabled slots (shortest interval with jitter).
uint32_t vsPlannedMsPerHour(const VsSlotCfg cfg[VS_SLOTS]);

// Check of one slot for the settings: 0 = ok, else VS_CHECK_*.
enum VsCheck : uint8_t { VS_CHECK_OK = 0, VS_CHECK_MUSTMAP, VS_CHECK_WIND, VS_CHECK_CHANNEL };
uint8_t vsCheckSlot(const VsSlotCfg& c);

// A typed value name -> stored form: braces and spaces removed; only
// a-z A-Z 0-9 _ - @ allowed. false = invalid or too long.
bool vsCleanSrc(const char* in, char* out, size_t len);

// Scheduling: the next due time after a send / skip at nowMs.
uint64_t vsNextDue(uint64_t dueMs, uint64_t nowMs, uint32_t intervalMs, int32_t jitterMs);
// Hold the send back while a received sensor's packet is due, at most VS_MAX_DEFER_MS.
inline bool vsHoldForRx(uint64_t nowMs, uint64_t dueMs, bool rxDue)
{
    return rxDue && nowMs >= dueMs && nowMs - dueMs < VS_MAX_DEFER_MS;
}

// ---- Davis mode (C5): frames in the gap after a Davis packet ----
// In Davis mode the Davis receive task owns the radio and follows the ISS's
// hop sequence: one 10-byte packet every 2.5625 s (ID 1) ... 3.0 s (ID 8).
// No general radio scheduler: right after an accepted packet whose arrival
// confirmed the hop prediction, the Davis task sends up to
// VS_DAVIS_MAX_FRAMES queued frames and is back in Davis reception on the
// next hop channel long before the next packet can come. A missed hop or a
// prediction that is not locked (search, resync, raw capture) = no frame.
constexpr int      VS_DAVIS_MAX_FRAMES = 2;        // per gap
// Per frame, on top of its airtime: FSK TX setup (beginFSK resets and
// calibrates the chip), TX done, and the Davis receive setup after it
// (beginFSK again). Assumed (not measured on the chip yet; the status shows
// the longest gap use measured).
constexpr uint32_t VS_DAVIS_SWITCH_US  = 60000;
constexpr uint32_t VS_DAVIS_MARGIN_US  = 300000;   // the radio is back this long before the next packet's window
constexpr uint32_t VS_DAVIS_PACKET_US  = 7000;     // a Davis packet on air: (32 + 16 + 80 bits) / 19.2 kbit/s = 6.7 ms
constexpr uint32_t VS_DAVIS_LATE_US    = 500000;   // only right after the packet (a late wake-up uses no gap)

struct VsDavisGap {
    bool     confirmed = false;   // accepted packet while tracking, its arrival matched the prediction
    int64_t  packetUs = 0;        // RX done of that packet (monotonic us)
    int64_t  nextUs = 0;          // predicted RX done of the next packet
    uint32_t guardUs = 0;         // the next packet may come this early (DavisHopperConfig::guardMs)
};
// May the sentSoFar-th frame of this gap (airtime airMs) be sent at nowUs?
// Checked before every frame with the real clock, so a slower radio than
// assumed only ends the gap early.
bool vsDavisGapAllows(const VsDavisGap& g, int64_t nowUs, int sentSoFar, uint32_t airMs);
// Planning: how many of the frames (airtimes in send order) fit, each taking
// airtime + VS_DAVIS_SWITCH_US from nowUs on.
int  vsDavisGapFrames(const VsDavisGap& g, int64_t nowUs, const uint32_t airMs[], int n);
// Which waiting frame goes first: the one queued longest ago (ties: the lower
// slot), -1 = none waiting. Spreads the slots over the gaps; every slot keeps
// its own interval grid (vsNextDue()), the wait only shifts single frames.
int  vsDavisPick(const bool waiting[VS_SLOTS], const uint64_t queuedMs[VS_SLOTS]);

// Status per slot (status page, /api/info).
struct VsSlotStatus {
    uint32_t lastTxMs = 0;       // millis() of the last frame sent (valid while sent > 0)
    uint32_t sent = 0;
    uint32_t skipBudget = 0;     // airtime budget used up
    uint32_t skipBusy = 0;       // firmware upload / TX error
    uint32_t skipMissing = 0;    // a value missing
    uint32_t deferDavis = 0;     // Davis mode: no usable gap before the slot's next frame (missed hop, not locked)
    int16_t  lastError = 0;      // RadioLib code of the last failed TX
};

// The slots as text: one line per slot
// "type|enabled|channel|field=name,field=name"; '\n' between, empty slot = empty line.
size_t vsPack(const VsSlotCfg cfg[VS_SLOTS], char* out, size_t len);
void   vsUnpack(const char* s, VsSlotCfg cfg[VS_SLOTS]);
constexpr size_t VS_PACKED_MAX = VS_SLOTS * (VF_COUNT * (VS_SRC_LEN + 8) + 24);   // the longest text + terminator

// NVS blob "vsens" (storage_layout.h STORED_VIRTUAL_SENSORS_VERSION):
// [version][enabled][txPowerDbm as int8][the slots' text, no terminator].
constexpr size_t VS_BLOB_MAX = 3 + VS_PACKED_MAX;
size_t vsSettingsPack(const VsSettings& s, uint8_t* out, size_t len);   // bytes, 0 = buffer too small
// The payload after the version byte (storedBlobLoad()); false = too short (out = defaults).
// The power is clamped to VS_DBM_MIN..VS_DBM_MAX.
bool   vsSettingsFromPayload(const uint8_t* p, size_t n, VsSettings& out);

// Shared state, written by the loop task (vs_run.cpp), read by the web task
// (status page, /api/info): counters per slot, and the band airtime of the
// last hour as of the last loop (the ledger itself belongs to the loop task).
extern VsSlotStatus      gVsStatus[VS_SLOTS];
extern VsBandLedger      gVsBand;
extern volatile uint32_t gVsBandLastHourMs;
// Davis mode: the longest gap use measured (us, first frame's TX setup to
// Davis reception running again), and the frames sent in gaps since start.
extern volatile uint32_t gVsDavisGapMaxUs;
extern volatile uint32_t gVsDavisGapFrames;
