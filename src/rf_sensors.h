#pragma once
// Wireless weather sensors on 868.3 MHz FSK, received with the Heltec's SX1262
// between LoRaWAN uplinks: Bresser 5/6/7-in-1, lightning, leakage, ...,
// LaCrosse / TFA Dostmann IT+ temperature/humidity sensors and Fine Offset
// (Ecowitt / Froggit / Ambient Weather) sensors.
// gCfg.rfProto selects the protocols; with more than one data rate the
// receiver alternates (Bresser 17 s, each IT+ data rate 5 s, the 17.241
// kbit/s phase with Fine Offset 17 s - longer than the send intervals of
// 12-16 s / about 4 s / 16 s of Bresser, IT+ and the Fine Offset weather
// stations, so those are heard each round). IT+ 17.241 kbit/s and Fine Offset share one
// phase: same bit rate and sync word, one receive configuration decodes both
// (the frame is tried as Fine Offset first - family code + CRC + sum - then
// as IT+), so adding Fine Offset to IT+ 17.241 costs no receive time.
//
// The transceiver is shared: LoRaWAN (lora_transport.cpp / sendLoRa()) calls
// loraRadioTaken() before every use, which stops the FSK reception here;
// rfSensorsLoop() restarts it afterwards. A sensor packet that arrives during
// an uplink and its RX windows (~0.1-3 s) is lost - the sensors repeat every
// 4-16 s, and only the latest values are kept.
//
// Decoding: lib/BresserWeatherSensorReceiver (fork of matthias-bs/
// BresserWeatherSensorReceiver, MIT, see its FORK.md), lib/TfaWeatherSensorReceiver
// (IT+) and lib/FineOffsetSensorReceiver (own, MIT).
// IDs: IT+ sensors have only a 6-bit ID: they appear as 170000xx (17.241
// kbit/s) and 090000xx (9.579 kbit/s), xx = ID (+0x40 for a second probe
// channel). Fine Offset sensors appear as FFiiiiii: FF = family code (= model
// number: 24 WH65/WS69, 30 WH31, 40 WH40, 51 WH51, 57 WH57, 80 WS80, 90 WS90,
// 45 WH45, 55 WH55, A2 WH52, ...; 25 WN32P/WH25 and 32 WN32/WH32, which send no
// family code), iiiiii = the sensor ID as the Ecowitt app / rtl_433 show it;
// WH31: 3000CCii with CC = channel 1-8 and ii = ID.
#include <stdint.h>
#include <vector>
#include "sources.h"         // SRC_RF_FRESH_MS
#include "reception_window.h"   // ReceptionWindow (B10)

// Wireless sensors received at the same time (table gRfSensors[], also the
// maximum of the ID list). Slot n (1..RF_MAX_SENSORS) = the n-th ID of
// "Wireless sensor IDs", or - with an empty list - table index n-1 (the order
// the sensors were first received). Only slots 1..RF_SOURCE_SLOTS (sources.h)
// can be selected as value sources ("Wireless 1-4"); all slots appear in the
// JSON (rfSensors[].slot), MQTT, Home Assistant, Loxone and the web pages.
// A fixed static table (no dynamic allocation: no heap fragmentation on a
// node that runs for months); about 170 bytes per entry.
constexpr int RF_MAX_SENSORS = 32;
static_assert(RF_MAX_SENSORS >= RF_SOURCE_SLOTS, "the value-source slots must fit into the table");
static_assert(RF_MAX_SENSORS <= SRC_RF_SLOTS_MAX, "every slot must be selectable as a value source (sources.h)");

// Value bits (RfValues::ok, RfSensorReading::staleMask); the same values as
// the Bresser library's MSG_OK_* (checked in rf_sensors.cpp).
constexpr uint8_t RF_VAL_TEMP = 0x01, RF_VAL_HUM = 0x02, RF_VAL_WIND = 0x04, RF_VAL_RAIN = 0x08,
                  RF_VAL_UV = 0x10, RF_VAL_LIGHT = 0x20,
                  RF_VAL_PRESS = 0x40,   // station pressure (Fine Offset WS69, WN32P / WH25); not a Bresser MSG_OK_* bit
                  RF_VAL_ANY = 0x80;   // staleMask only: lastMs (no packet at all)

struct RfSensorReading {
    bool          used = false;
    uint32_t      id = 0;
    uint8_t       type = 0;          // Bresser sensor type (1 = weather station, 2 = thermo-hygro, ...)
    uint8_t       decoder = 0;       // 5/6/7-in-1, lightning, leakage (library DECODER_*)
    uint8_t       proto = 1;         // RF_PROTO_* (config.h) it was received with
    float         rssi = 0.0f;
    bool          batteryOk = true;
    uint16_t      batteryMv = 0;     // Fine Offset sensors that send their battery voltage (WH51, WN34, WH40 ...), 0 = none
    unsigned long lastMs = 0;        // millis() of the last packet (any content)
    unsigned long firstMs = 0;       // millis() when the slot was taken (first packet of this ID since start); 0 = unknown
    // millis() of the last packet that actually carried each value: a sensor
    // can keep sending (e.g. wind) while one of its measurements fails, so the
    // freshness of every value is judged on its own (sources.cpp)
    unsigned long tempMs = 0, humMs = 0, windMs = 0, rainMs = 0, uvMs = 0, lightMs = 0, pressMs = 0;
    // RF_VAL_* whose timestamp was found older than SRC_RF_FRESH_MS
    // (rfExpireValues()): keeps them stale when millis() wraps after 49.7 days
    // and an old timestamp would look recent again. Cleared by the next packet
    // carrying the value.
    uint8_t       staleMask = 0;
    uint32_t      packets = 0;       // received since start
    // receive scheduling (rf_schedule.h): last packet seen by rfStoreValues()
    // and the send interval learned from the gaps (0 = not known yet)
    unsigned long schedMs = 0;
    uint32_t      periodMs = 0;
    // reception watchdog (reception_window.h): packets of the last hour, only
    // while the sensor is ★ (stopped otherwise, so a ★ / re-bind starts fresh);
    // read with rfReceptionOf()
    ReceptionWindow reception;
    // weather values (only the *_ok groups the sensor sends)
    bool  hasTemp = false, hasHumidity = false, hasWind = false, hasRain = false, hasUv = false, hasLight = false;
    float tempC = 0.0f;
    uint8_t humidity = 0;
    float windAvgMs = 0.0f, windGustMs = 0.0f, windDirDeg = 0.0f;
    float rainTotalMm = 0.0f;        // the sensor's own running total (resets on battery change)
    // phantom-rain filter (rfStoreValues()): a total that dropped or jumped,
    // held until the next packet confirms it
    bool  rainTotalHeld = false;
    float heldRainTotalMm = 0.0f;
    float uvIndex = 0.0f;
    float lightLux = 0.0f;
    // Fine Offset only (rfSensors JSON, status page; no value-source groups)
    const char* model = nullptr;     // "WS90", "WH31", "Vantage Pro2", ... (static string); nullptr = none
    uint8_t channel = 0;             // WH31: 1-8; Davis: transmitter ID 1-8; 0 = none
    bool    hasMoisture = false;     // WH51 soil moisture
    uint8_t moisturePct = 0;         // as the sensor reports it, or from the two-point calibration (calibration.h)
    uint16_t moistureAd = 0xFFFF;    // WH51: raw AD (9 bit), 0xFFFF = none
    bool    hasSolar = false;        // Davis: solar radiation (lightLux holds the lux approximation)
    float   solarWm2 = 0.0f;
    bool    hasSupercap = false;     // WS85 / WS90 / Davis Vue super capacitor (solar-charged)
    float   supercapV = 0.0f;
    bool    hasLightning = false;    // WH57
    uint8_t lightningCount = 0;      // strikes counted by the sensor (8 bit, wraps)
    uint8_t lightningKm = 0;         // distance to the storm front; 0 = none detected
    // Fine Offset, per sensor only (no value-source groups); latest value
    bool    hasPressure = false;     // WN32P / WH25 / WH32B (station pressure, hPa)
    float   pressureHpa = 0.0f;
    bool    hasPm25 = false, hasPm10 = false, hasPm1 = false, hasPm4 = false;   // ug/m3
    bool    pm10Estimated = false;   // WH41 / WH0290 / WH43: PM10 derived from PM2.5 by the sensor
    float   pm25 = 0.0f, pm10 = 0.0f, pm1 = 0.0f, pm4 = 0.0f;
    bool    hasCo2 = false;          // WH45 / WH46
    uint16_t co2Ppm = 0;
    bool    extPower = false;        // air quality sensor powered via USB
    bool    hasLeak = false, leak = false, leakHighSensitivity = false;   // WH55: leak = water detected
    uint16_t leakRaw = 0;
    bool    hasLeafWetness = false;  // WN35
    uint8_t leafWetnessPct = 0;
    bool    hasConductivity = false; // WH52 soil EC
    float   conductivityUsCm = 0.0f;
    bool    hasDistance = false;     // LDS01 laser distance (air gap), mm
    uint16_t distanceMm = 0;
    bool    hasHeatCount = false;    // LDS01 heater-on count (low byte, wraps)
    uint8_t heatCount = 0;
};

extern RfSensorReading gRfSensors[RF_MAX_SENSORS];

// "39582376, 1234" -> IDs (hex; at most RF_MAX_SENSORS, more are ignored;
// empty = accept any)
std::vector<uint32_t> rfParseIds(const char* s);

// Ecowitt-style ID of a Fine Offset sensor as the Ecowitt app / console shows
// it ("181A2" for our 900181A2; WH31 / WH25 / WH32: the 8-bit ID): false =
// not a Fine Offset ID (family byte unknown).
bool rfEcowittId(uint32_t id, uint32_t& shortId);
// A short ID typed into the Hidden list ("181A2") is replaced by the
// full ID of the Fine Offset sensor it belongs to once that sensor is heard.
// true = the list changed (gCfg.rfIgnore, in RAM; saved with the next save).
bool rfNormalizeShortId(uint32_t fullId);

// ---- Table handling (rf_sensors_data.cpp, no RadioLib: host-tested) ----

// The values of one decoded packet; only those flagged in ok are stored.
struct RfValues {
    uint8_t ok = 0;                  // RF_VAL_* this packet carries
    float   tempC = 0.0f;
    uint8_t humidity = 0;
    float   windAvgMs = 0.0f, windGustMs = 0.0f, windDirDeg = 0.0f;
    float   rainTotalMm = 0.0f, uvIndex = 0.0f, lightLux = 0.0f;
    float   pressureHpa = 0.0f;      // station (absolute) pressure
};

// Stores v in r with the packet time now (per value, see tempMs ...) and
// sets lastMs. Returns the rain increase since the sensor's previous rain
// value (0 for the first one after a node start: rain while the node was
// down is not known). rainGapMs: 0 normally; the age of the previous rain
// value (> rfFreshMs()) when the sensor was not heard for longer than its
// freshness window, so the increase covers that whole outage.
// Radio rain totals (Fine Offset, Bresser) pass a phantom-rain filter, the
// counterpart of DavisRainFilter: a total below the previous one (counter
// restart or a corrupt packet whose checksum happened to pass) or above it by
// more than plausible rain is held (increase 0) until the next packet. If that
// one continues from the held total, the step is real - a restart counts the
// new counter's total, a jump counts while it fits a cloudburst rate, else the
// counter is re-based; if it continues from the old total, the held packet was
// corrupt and is dropped. Davis (filtered by DavisRainFilter) and network
// sources are taken as they come.
float rfStoreValues(RfSensorReading& r, const RfValues& v, unsigned long now, unsigned long& rainGapMs);
// Counts rfStoreValues() calls (any task; the web task pushes new values to /api/events subscribers).
extern volatile uint32_t gRfStoreCount;

// Table index for a sensor ID: its own slot, else a free one, else the slot
// of the sensor heard longest ago provided that was more than takeOverMs ago
// (the slot is then cleared for the new sensor), else -1 (table full).
// onlyProto != 0: take over only a sensor received with that RF_PROTO_*.
// mine: a ★ sensor (gLogicalSensors) - if the table is full of others, it takes
// the entry of the non-"Mine" sensor heard longest ago.
int rfSlotFor(uint32_t id, unsigned long now, unsigned long takeOverMs = SRC_RF_FRESH_MS, int onlyProto = 0,
              bool mine = false);

// ---- Sensor states (settings: star / circle / cross) ----
// MINE     a logical sensor (gLogicalSensors, lsensors.h): numbered first (wireless 1..n in table order; 1-4 =
//          value sources), Home Assistant device
// RECEIVED any other sensor: stored and output, numbered after the "Mine"
//          ones in reception order; not accepted while rfOnlyMine and a ★ sensor exists
// HIDDEN   in gCfg.rfIgnore: never stored, no number, not output anywhere
enum RfSensorState : uint8_t { RF_STATE_RECEIVED = 0, RF_STATE_MINE = 1, RF_STATE_HIDDEN = 2 };
RfSensorState rfStateOf(uint32_t id);
bool rfOnlyMineActive();                 // rfOnlyMine and at least one ★ sensor
bool rfIdAccepted(uint32_t id);          // may enter the table
// Moves id into the state (out of the other): ★ = a new logical sensor
// (lsAdd(), takes over the rfNames / rfMinRssi entry), un-★ = lsRemove() (name
// and minimum go back to rfNames / rfMinRssi); false = the table / hidden
// list is full, nothing changed.
bool rfSetState(uint32_t id, RfSensorState st);
// Moves a ★ sensor one place up (dir < 0) or down in the table; false = not possible.
bool rfMoveMine(uint32_t id, int dir);
// "XXXXXXXX, YYYYYYYY"
void rfFormatIds(const std::vector<uint32_t>& ids, char* out, size_t len);
// Clears the table entries a changed list no longer accepts (the others keep
// their data; a "Mine" change only renumbers).
void rfRemoveRejected();

// Hidden sensors still in range (status page "n hidden sensors in range",
// settings "Hidden sensors in range"): noted when a frame of a hidden ID is
// dropped. Not part of the sensor table.
// outOfRange: latched by rfExpireValues() once lastMs is older than
// RF_HIDDEN_RANGE_MS (like staleMask: an old time stamp looks recent again
// after the millis() wrap), cleared by the next frame.
struct RfHiddenSeen {
    uint32_t id = 0; const char* model = nullptr; float rssi = 0; unsigned long lastMs = 0; uint32_t packets = 0;
    bool outOfRange = false;
};
constexpr int RF_HIDDEN_SEEN = 16;
constexpr unsigned long RF_HIDDEN_RANGE_MS = 600000UL;   // "in range": heard within 10 min
extern RfHiddenSeen gRfHidden[RF_HIDDEN_SEEN];
void rfNoteHidden(uint32_t id, const char* model, float rssi, unsigned long now);
bool rfHiddenSeenInRange(const RfHiddenSeen& h, unsigned long now);   // a hidden ID heard within RF_HIDDEN_RANGE_MS
int  rfHiddenInRange(unsigned long now);

// Indoor sensors (Fine Offset WH45 / WH46 air quality, WN32P / WH25 indoor
// thermo-hygro-barometer): never picked by the *Auto* value source for
// temperature / humidity - without an ID list one could otherwise land in
// wireless 1-4 and feed the outdoor temperature. "Wireless n" may still use them.
bool rfIsIndoor(const RfSensorReading& r);
// Nominal send interval (ms) of a sensor (Ecowitt wiki by Gyvate, section
// "transmission intervals"; Fine Offset family by ID, 0 = unknown) and its
// freshness window: max(SRC_RF_FRESH_MS, 2.5 x the interval) - the 10-minute
// air quality sensors (WH41 / WH43 / WH45 / WH46 on battery) stay fresh
// between their packets, everything else keeps the 5 minutes. Network
// sensors: the window set for their slot (net_sensor.h, default 10 min).
// IT+ (both data rates): about 4 s (docs/wireless-sensors.md).
unsigned long rfNominalIntervalMs(const RfSensorReading& r);
// The nominal interval when the sensor always sends at it, 0 otherwise (unknown,
// or it depends on the power supply: the air quality sensors send every 10 min
// on battery, every 60 s on USB). Interval learning never goes below 0.75 x it,
// and a shorter gap counts as evidence of a second transmitter on this ID
// (rf_schedule.h rfGapImpossible(), reception_window.h).
unsigned long rfFixedIntervalMs(const RfSensorReading& r);
unsigned long rfFreshMs(const RfSensorReading& r);
// A packet of this sensor arrived within rfFreshMs() (and it has not expired
// across a millis() wrap): its values are current. Outputs that stamp values
// with "now" (InfluxDB push, /metrics) leave the values of a silent sensor out.
bool rfSensorFresh(const RfSensorReading& r);
// Reception of a ★ radio sensor over the last hour (reception_window.h):
// unknown for network sensors and while the window has too little data. A
// sensor that is not ★ has no window (stopped with its next packet): callers
// show it for ★ sensors only. Any task (racy read of small counters, display value).
// idCollision: two transmitters suspected on this radio ID (percentage unknown then).
ReceptionVerdict rfReceptionOf(const RfSensorReading& r);
// Battery change (A5c): the same kind of sensor = same protocol and, for IT+
// the same humidity variant (RF_ID_ITPLUS_HUM), Fine Offset the same model,
// Bresser the same type. Other protocols (Davis): the protocol.
bool rfSameKind(const RfSensorReading& a, const RfSensorReading& b);
// "Replaces?" offer: the one received sensor (not in the table, not hidden,
// same kind as `star`, first heard after `star` had been silent for its
// freshness window) among all[0..n) while `star` is silent now. -1 = none or
// more than one (then only the manual select is offered).
int rfReplaceCandidate(const RfSensorReading* all, int n, const RfSensorReading& star, unsigned long now);
// Table index of the received IT+ twin of id (same IT+ ID, the other kind -
// with / without humidity; see rfItPlusTwin()), -1 = none.
int rfTwinIndex(uint32_t id);


// Forget all sensors (ID filter changed).
void rfSensorsClear();

// Marks values older than SRC_RF_FRESH_MS in staleMask (see there); call
// regularly (rfSensorsLoop(), every loop iteration).
void rfExpireValues(unsigned long now);

// Timestamp of one RF_VAL_* value of r (RF_VAL_ANY: lastMs).
unsigned long rfValueMs(const RfSensorReading& r, uint8_t bit);

// Call every loop() iteration: (re)starts the FSK reception when LoRaWAN is
// done with the transceiver and decodes received packets. No-op while
// rfEn is off.
void rfSensorsLoop();

// Receiver state for the status page.
bool    rfSensorsReceiving();
int16_t rfSensorsLastError();
int     rfSensorsScheduled();   // sensors followed by the receive schedule (rf_schedule.h), 0 = rotation only
uint32_t rfSensorsPacketsTotal();
uint32_t rfSensorsWatchdogRestarts();   // receiver restarts by the receive watchdog (rf_schedule.h) since start
// A followed sensor's packet is due about now (rf_schedule.h window): the
// virtual sensors (vs_run.h) hold their frame back. false while rfEn is off.
bool    rfSensorsPacketDue();

// The last radio-clock time a Fine Offset sensor sent (WH31 with a DCF77 /
// WWVB receiver, family 0x52): shown on the status page; not used as the
// node's clock (yet) - and barely could be: it sends the time only after a
// battery change and then about once a day at night (~02:00 local, forum
// SonneKF 2026-11-24). false = none received since the start.
struct RfRadioClock {
    uint8_t  stationId = 0;      // the sensor's 8-bit ID (as in its WH31 frames)
    uint16_t year = 0;
    uint8_t  month = 0, day = 0, hour = 0, minute = 0, second = 0;
    unsigned long rxMs = 0;      // millis() of the reception
};
bool rfRadioClock(RfRadioClock& out);
const char* rfSensorsListening();      // protocol currently listened to, e.g. "IT+ 17.241 kbit/s"
const char* rfProtoName(int proto);    // "Bresser", "IT+ 17.241 kbit/s", "IT+ 9.579 kbit/s", "Fine Offset", "Davis"

// Receive phases for a protocol set (RF_PROTO_* bit mask): Fine Offset is
// received in the IT+ 17.241 kbit/s phase, so its bit maps to RF_PROTO_ITPLUS17.
int rfPhaseSet(int protoSet);

// ---- Receive scheduling and the "Auto (search)" type (host-tested) ----

// Protocol set (RF_PROTO_* bit mask) received for a gCfg.rfProto value:
// RF_PROTO_AUTO searches all families (RF_PROTO_ALL, never Davis), an invalid
// value = Bresser; RF_PROTO_DAVIS = 0 (no phases: davis_rx.cpp owns the radio).
int rfProtoSetFor(int cfgProto);

// The phase after `cur` within a phase set (rfPhaseSet()), in the order
// Bresser -> 17.241 kbit/s (IT+ 17 + Fine Offset) -> IT+ 9.579 kbit/s.
int rfNextPhase(int phaseSet, int cur);

// How long a phase is listened to before switching (only with more than one
// phase): longer than the send interval of the sensors received in it -
// Bresser 17 s (sends every 12-16 s), 17.241 kbit/s with Fine Offset 17 s
// (WH65 16 s, WS90 8.8 s, WS80 4.75 s), IT+ alone 5 s (about every 4 s).
constexpr unsigned long RF_DWELL_BRESSER_MS = 17000, RF_DWELL_ITPLUS_MS = 5000, RF_DWELL_FINEOFFSET_MS = 17000;
unsigned long rfPhaseDwellMs(int phase, bool fineOffsetPhase);

// Auto search: the sensor type stored once the first valid packet arrived,
// by the protocol it was decoded as (RfSensorReading::proto): Bresser -> 1,
// IT+ 17.241 kbit/s or Fine Offset -> 10 (they share the 17.241 kbit/s
// phase), IT+ 9.579 kbit/s -> 4; 0 = unknown.
int rfAutoDetectedProto(int receivedProto);

// Locks the auto search on a packet? Only while searching (cfgProto ==
// RF_PROTO_AUTO), for a CRC-checked packet whose ID is accepted by the
// sensor ID list (empty list = any ID).
bool rfAutoShouldLock(int cfgProto, bool idAccepted);

// Auto search: the type it stored since the start while that is still the
// configured type (status page "detected: ..."), else 0.
int rfSensorsDetectedProto();
// Auto search progress (rf_autosearch.h) and whether the type was locked by the weak-signal fallback
struct AutoSearch;
const AutoSearch& rfAutoSearchState();
bool rfAutoLockedWeak();

// Table-only protocols (RfSensorReading::proto, never a gCfg.rfProto value):
// network sensors (net_sensor.h), IDs E1000001 .. E1000004, values from an
// MQTT topic or a polled URL. rfProtoName(): "MQTT" / "REST".
constexpr uint8_t RF_PROTO_MQTT = 32, RF_PROTO_REST = 33;
inline bool rfIsNetProto(int p) { return p == RF_PROTO_MQTT || p == RF_PROTO_REST; }
// The sensor table is in use: 868 MHz reception on, or a network sensor
// configured. The outputs of the table (JSON rfSensors, value sources, HA,
// InfluxDB, /metrics, ...) check this instead of gCfg.rfEn.
bool rfTableOn();

// Davis transmitters appear as DA0000ii, ii = transmitter ID 1-8.
constexpr uint32_t RF_ID_DAVIS = 0xDA000000UL;
// Davis solar radiation -> illuminance for the light value source (sunlight,
// about 126.7 lx per W/m2; an approximation, the native value is solarWm2).
constexpr float RF_DAVIS_LUX_PER_WM2 = 126.7f;
constexpr uint32_t RF_ID_ITPLUS17 = 0x17000000UL, RF_ID_ITPLUS9 = 0x09000000UL;
// IT+ IDs are only 6 bits (+64 for a second probe) and random after every
// battery change, so two sensors in range may share one. A sensor with a
// humidity sensor (the frame says so: humidity byte != 0x6A) gets this bit in
// its node ID, a temperature-only one not: two such twins stay two table rows,
// and neither one's values end up under the other. Two twins of the same kind
// cannot be told apart.
constexpr uint32_t RF_ID_ITPLUS_HUM = 0x00000100UL;
inline uint32_t rfItPlusId(bool rate17, uint8_t sensorId, bool hasHumidity)
{
    return (rate17 ? RF_ID_ITPLUS17 : RF_ID_ITPLUS9) | sensorId | (hasHumidity ? RF_ID_ITPLUS_HUM : 0);
}
// The other IT+ variant of id (with / without humidity), 0 if id is no IT+ ID.
inline uint32_t rfItPlusTwin(uint32_t id)
{
    const uint32_t base = id & 0xFF000000UL;
    if ((base != RF_ID_ITPLUS17 && base != RF_ID_ITPLUS9) || (id & 0x00FFFE00UL)) return 0;
    return id ^ RF_ID_ITPLUS_HUM;
}
