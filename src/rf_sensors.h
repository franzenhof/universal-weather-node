#pragma once
// Wireless weather sensors on 868.3 MHz FSK, received with the Heltec's SX1262
// between LoRaWAN uplinks: Bresser 5/6/7-in-1, lightning, leakage, ... and
// LaCrosse / TFA Dostmann IT+ temperature/humidity sensors (itplus.h).
// gCfg.rfProto selects the protocols; with more than one the receiver
// alternates (Bresser 17 s, each IT+ rate 5 s - longer than the sensors'
// send intervals of 12-16 s / about 4 s, so every sensor is heard each round).
//
// The transceiver is shared: LoRaWAN (lora_transport.cpp / sendLoRa()) calls
// loraRadioTaken() before every use, which stops the FSK reception here;
// rfSensorsLoop() restarts it afterwards. A sensor packet that arrives during
// an uplink and its RX windows (~0.1-3 s) is lost - the sensors repeat every
// 12-16 s, and only the latest values are kept.
//
// Decoding: lib/BresserWeatherSensorReceiver (fork of matthias-bs/
// BresserWeatherSensorReceiver, MIT, see its FORK.md); IT+ in itplus.cpp.
// IT+ sensors have only a 6-bit ID: they appear as 170000xx (17.241 kbit/s)
// and 090000xx (9.579 kbit/s), xx = ID (+0x40 for a second probe channel).
#include <stdint.h>
#include <vector>
#include "sources.h"         // SRC_RF_FRESH_MS

constexpr int RF_MAX_SENSORS = 4;

// Value bits (RfValues::ok, RfSensorReading::staleMask); the same values as
// the Bresser library's MSG_OK_* (checked in rf_sensors.cpp).
constexpr uint8_t RF_VAL_TEMP = 0x01, RF_VAL_HUM = 0x02, RF_VAL_WIND = 0x04, RF_VAL_RAIN = 0x08,
                  RF_VAL_UV = 0x10, RF_VAL_LIGHT = 0x20,
                  RF_VAL_ANY = 0x80;   // staleMask only: lastMs (no packet at all)

struct RfSensorReading {
    bool          used = false;
    uint32_t      id = 0;
    uint8_t       type = 0;          // Bresser sensor type (1 = weather station, 2 = thermo-hygro, ...)
    uint8_t       decoder = 0;       // 5/6/7-in-1, lightning, leakage (library DECODER_*)
    uint8_t       proto = 1;         // RF_PROTO_* (config.h) it was received with
    float         rssi = 0.0f;
    bool          batteryOk = true;
    unsigned long lastMs = 0;        // millis() of the last packet (any content)
    // millis() of the last packet that actually carried each value: a sensor
    // can keep sending (e.g. wind) while one of its measurements fails, so the
    // freshness of every value is judged on its own (sources.cpp)
    unsigned long tempMs = 0, humMs = 0, windMs = 0, rainMs = 0, uvMs = 0, lightMs = 0;
    // RF_VAL_* whose timestamp was found older than SRC_RF_FRESH_MS
    // (rfExpireValues()): keeps them stale when millis() wraps after 49.7 days
    // and an old timestamp would look recent again. Cleared by the next packet
    // carrying the value.
    uint8_t       staleMask = 0;
    uint32_t      packets = 0;       // received since start
    // weather values (only the *_ok groups the sensor sends)
    bool  hasTemp = false, hasHumidity = false, hasWind = false, hasRain = false, hasUv = false, hasLight = false;
    float tempC = 0.0f;
    uint8_t humidity = 0;
    float windAvgMs = 0.0f, windGustMs = 0.0f, windDirDeg = 0.0f;
    float rainTotalMm = 0.0f;        // the sensor's own running total (resets on battery change)
    float uvIndex = 0.0f;
    float lightLux = 0.0f;
};

extern RfSensorReading gRfSensors[RF_MAX_SENSORS];

// "39582376, 1234" -> IDs (hex; max RF_MAX_SENSORS; empty = accept any)
std::vector<uint32_t> rfParseIds(const char* s);

// ---- Table handling (rf_sensors_data.cpp, no RadioLib: host-tested) ----

// The values of one decoded packet; only those flagged in ok are stored.
struct RfValues {
    uint8_t ok = 0;                  // RF_VAL_* this packet carries
    float   tempC = 0.0f;
    uint8_t humidity = 0;
    float   windAvgMs = 0.0f, windGustMs = 0.0f, windDirDeg = 0.0f;
    float   rainTotalMm = 0.0f, uvIndex = 0.0f, lightLux = 0.0f;
};

// Stores v in r with the packet time now (per value, see tempMs ...) and
// sets lastMs. Returns the rain increase since the sensor's previous rain
// value (0 for the first one after a node start: rain while the node was
// down is not known). rainGapMs: 0 normally; the age of the previous rain
// value (> SRC_RF_FRESH_MS) when the sensor was not heard for longer than
// the freshness window, so the increase covers that whole outage.
float rfStoreValues(RfSensorReading& r, const RfValues& v, unsigned long now, unsigned long& rainGapMs);

// Table index for a sensor ID: its own slot, else a free one, else the slot
// of the sensor heard longest ago provided that was more than takeOverMs ago
// (the slot is then cleared for the new sensor), else -1 (table full).
// onlyProto != 0: take over only a sensor received with that RF_PROTO_*.
int rfSlotFor(uint32_t id, unsigned long now, unsigned long takeOverMs = SRC_RF_FRESH_MS, int onlyProto = 0);

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
uint32_t rfSensorsPacketsTotal();
const char* rfSensorsListening();      // protocol currently listened to, e.g. "IT+ 17.241 kbit/s"
const char* rfProtoName(int proto);    // "Bresser", "IT+ 17.241 kbit/s", "IT+ 9.579 kbit/s"

constexpr uint32_t RF_ID_ITPLUS17 = 0x17000000UL, RF_ID_ITPLUS9 = 0x09000000UL;
