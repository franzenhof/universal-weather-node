#include "rf_sensors.h"
#include "globals.h"
#include "lora_transport.h"
#include "serial_log.h"
#include "sources.h"
#include "rf_fsk17.h"
#include <TfaWeatherSensor.h>
#include <FineOffsetSensor.h>

#include <RadioLib.h>
#include <WeatherSensorCfg.h>
#include <WeatherSensor.h>
#include <vector>
#include <algorithm>

// The Heltec's transceiver (defined by heltec_unofficial.h in main.cpp; on HSPI).
extern SX1262 radio;

namespace {
WeatherSensor ws;
int           activeMode  = RF_RADIO_OFF;   // receiver set up (RF_RADIO_SX1262) or off
bool          initialized = false;
bool          receiving   = false;   // FSK reception running (cleared by LoRaWAN)
int16_t       lastError   = 0;
unsigned long lastStartMs = 0;
uint32_t      packetsTotal = 0;
char          appliedIds[sizeof(gCfg.rfIds)] = "";
std::vector<uint32_t> idFilter;      // parsed gCfg.rfIds (IT+ filter; Bresser filters in the library)
constexpr unsigned long RESTART_RETRY_MS = 10000;   // after a failed start
constexpr unsigned long NEW_BATTERY_TAKEOVER_MS = 60000;   // IT+ new-battery frame, see storeItPlus()

// Protocol time sharing: the phase listened to now (RF_PROTO_BRESSER,
// _ITPLUS17 = the 17.241 kbit/s phase, also for Fine Offset, or _ITPLUS9),
// since when, and the protocol set it was chosen from. One phase = listen
// continuously.
int           phase        = RF_PROTO_BRESSER;
unsigned long phaseStartMs = 0;
int           protoSet     = 0;      // gCfg.rfProto applied (RF_PROTO_*)
constexpr unsigned long DWELL_BRESSER_MS = 17000;   // > Bresser send interval (12-16 s)
constexpr unsigned long DWELL_ITPLUS_MS  = 5000;    // > IT+ send interval (about 4 s)
// 17.241 kbit/s phase with Fine Offset: > WH65 / WS68 send interval (16 s;
// WS80 4.75 s, WS90 8.8 s). WH31 / WH40 / WH51 / WH57 send every 49-79 s and
// are then heard only every few transmissions - alternating is not recommended.
constexpr unsigned long DWELL_FINEOFFSET_MS = 17000;

// IT+ and Fine Offset in one phase: IT+ sensors were measured at 868.22-868.30
// MHz (often 50-75 kHz low, hence 868.26 MHz for IT+ alone); Fine Offset EU
// sensors are nominally on 868.3 MHz (not measured here; tolerance assumed
// similar). 868.28 MHz with the next wider SX1262 filter (312 kHz, 868.12-
// 868.44 MHz) covers both incl. deviation, for about 1.2 dB less sensitivity
// than 234 kHz. Fine Offset alone: 868.3 MHz, 234 kHz (868.18-868.42 MHz).
constexpr float SHARED_FREQ_MHZ = 868.28f, SHARED_BW_KHZ = 312.0f;

TfaWeatherSensor tfa;
FineOffsetSensor fo;

void onLoraRadioTaken() { receiving = false; }

bool fineOffsetPhase() { return phase == RF_PROTO_ITPLUS17 && (protoSet & RF_PROTO_FINEOFFSET); }

int nextPhase(int set, int cur)
{
    for (int i = 0; i < 3; i++) {
        cur = cur == RF_PROTO_ITPLUS9 ? RF_PROTO_BRESSER : cur << 1;
        if (set & cur) return cur;
    }
    return RF_PROTO_BRESSER;
}

int16_t startPhase()
{
    if (phase == RF_PROTO_BRESSER) return ws.startReceive();   // re-registers the library's IRQ handler
    if (fineOffsetPhase()) {
        FineOffsetSensor::RxConfig c;            // fixed length FO_MAX_FRAME_LEN: also holds an IT+ frame
        if (protoSet & RF_PROTO_ITPLUS17) { c.frequencyMhz = SHARED_FREQ_MHZ; c.bandwidthKhz = SHARED_BW_KHZ; }
        return fo.startReceive(c);
    }
    return tfa.startReceive(phase == RF_PROTO_ITPLUS17 ? TfaWeatherSensor::IT_PLUS_17241 : TfaWeatherSensor::IT_PLUS_9579);
}

bool idAccepted(uint32_t id)
{
    return idFilter.empty() || std::find(idFilter.begin(), idFilter.end(), id) != idFilter.end();
}

void storeItPlus(const ItPlusFrame& f, float rssi)
{
    uint32_t id = (phase == RF_PROTO_ITPLUS17 ? RF_ID_ITPLUS17 : RF_ID_ITPLUS9) | f.sensorId;
    if (!idAccepted(id)) {
        if (f.newBattery)
            dbgSerial.printf("RF sensor 0x%08lX (IT+, new battery) not in the sensor ID list - ignored.\n", (unsigned long)id);
        return;
    }
    // A sensor with a new battery sends a new random ID: it may take over
    // the slot of an IT+ sensor of the same rate not heard for a minute
    // (normally the one whose battery was changed) instead of waiting for
    // the freshness window. With an ID list it must be entered there.
    unsigned long now = millis();
    int idx = rfSlotFor(id, now, f.newBattery ? NEW_BATTERY_TAKEOVER_MS : SRC_RF_FRESH_MS,
                        f.newBattery ? phase : 0);
    if (idx < 0) return;
    RfSensorReading& r = gRfSensors[idx];
    r.used      = true;
    r.id        = id;
    r.type      = SENSOR_TYPE_THERMO_HYGRO;
    r.decoder   = 0;
    r.proto     = phase;
    r.rssi      = rssi;
    r.batteryOk = !f.weakBattery;
    r.packets++;
    packetsTotal++;
    RfValues v;
    v.ok = RF_VAL_TEMP | (f.hasHumidity ? RF_VAL_HUM : 0);
    v.tempC = f.tempC;  v.humidity = f.humidity;
    unsigned long rainGapMs;
    rfStoreValues(r, v, now, rainGapMs);
    srcRfPacket(idx, false, 0.0f, false);
    dbgSerial.printf("RF sensor 0x%08lX (IT+): %.1f C%s, RSSI %.0f dBm%s%s\n", (unsigned long)id, f.tempC,
                     f.hasHumidity ? (String(", ") + f.humidity + " %").c_str() : "", rssi,
                     f.weakBattery ? ", battery low" : "", f.newBattery ? ", new battery" : "");
}

// Bresser sensor type numbers (r.type) for the Fine Offset families
uint8_t fineOffsetType(uint8_t family)
{
    switch (family) {
        case FO_FAMILY_WH31: case FO_FAMILY_WH31B: return SENSOR_TYPE_THERMO_HYGRO;
        case FO_FAMILY_WN34: case FO_FAMILY_WN38:  return SENSOR_TYPE_POOL_THERMO;
        case FO_FAMILY_WH51:                       return SENSOR_TYPE_SOIL;
        case FO_FAMILY_WH57:                       return SENSOR_TYPE_LIGHTNING;
        case FO_FAMILY_WH40: case FO_FAMILY_WN20:  return SENSOR_TYPE_RAIN;
        default:                                   return SENSOR_TYPE_WEATHER1;
    }
}

void storeFineOffset(const FoFrame& f, float rssi)
{
    uint32_t id = rfFineOffsetId(f);
    if (!idAccepted(id)) return;
    // The ID changes with a battery change (WH65, WH31, ...): the new one
    // takes a free slot or that of a sensor not heard for SRC_RF_FRESH_MS.
    unsigned long now = millis();
    int idx = rfSlotFor(id, now);
    if (idx < 0) return;
    RfSensorReading& r = gRfSensors[idx];
    r.used      = true;
    r.id        = id;
    r.type      = fineOffsetType(f.family);
    r.decoder   = 0;
    r.proto     = RF_PROTO_FINEOFFSET;
    r.model     = f.model;
    r.channel   = f.channel;
    r.rssi      = rssi;
    r.batteryOk = f.batteryOk;
    if (f.hasMoisture)  { r.hasMoisture = true;  r.moisturePct = f.moisture; }
    if (f.hasSupercap)  { r.hasSupercap = true;  r.supercapV = f.supercapV; }
    if (f.hasLightning) {
        r.hasLightning   = true;
        r.lightningCount = f.lightningCount;
        r.lightningKm    = f.stormDistanceKm == FO_LIGHTNING_NO_DISTANCE ? 0 : f.stormDistanceKm;
    }
    r.packets++;
    packetsTotal++;
    RfValues v = rfFineOffsetValues(f);
    unsigned long rainGapMs;
    float rainDelta = rfStoreValues(r, v, now, rainGapMs);
    srcRfPacket(idx, v.ok & RF_VAL_WIND, rainDelta, v.ok & RF_VAL_LIGHT, rainGapMs);
    dbgSerial.printf("RF sensor 0x%08lX (Fine Offset %s): RSSI %.0f dBm%s\n", (unsigned long)id, f.model, rssi,
                     f.batteryOk ? "" : ", battery low");
}

// The 17.241 / 9.579 kbit/s phases
void readFsk()
{
    float rssi;
    if (!fineOffsetPhase()) {
        ItPlusFrame f;
        if (tfa.getMessage(f, rssi)) storeItPlus(f, rssi);
        return;
    }
    uint8_t b[FO_MAX_FRAME_LEN];
    size_t len;
    if (!fo.getRaw(b, sizeof(b), len, rssi)) return;
    FoFrame f;
    ItPlusFrame it;
    switch (rfFsk17Decode(b, len, true, protoSet & RF_PROTO_ITPLUS17, f, it)) {
        case RF_FSK_FINEOFFSET: storeFineOffset(f, rssi); break;
        case RF_FSK_ITPLUS:     storeItPlus(it, rssi);    break;
        default:                break;
    }
}

// Setting switched on or off at run time.
void selectRadio(int mode)
{
    if (mode == RF_RADIO_SX1262) {
        WeatherSensor::setRadio(&radio);
        TfaWeatherSensor::setRadio(&radio);
        FineOffsetSensor::setRadio(&radio);
    }
    activeMode  = mode;
    receiving   = false;
    lastError   = 0;
    lastStartMs = 0;
    dbgSerial.printf("RF sensors: %s\n", mode == RF_RADIO_SX1262 ? "shared LoRa chip" : "off");
}

static_assert(RF_VAL_TEMP == MSG_OK_TEMP && RF_VAL_HUM == MSG_OK_HUMIDITY && RF_VAL_WIND == MSG_OK_WIND &&
              RF_VAL_RAIN == MSG_OK_RAIN && RF_VAL_UV == MSG_OK_UV && RF_VAL_LIGHT == MSG_OK_LIGHT,
              "RF_VAL_* must match the library's MSG_OK_*");

void copySlot(int slot)
{
    const WeatherSensor::sensor_t& s = ws.sensor[slot];
    // Keep our table keyed by sensor ID (the library slots are reused the same way).
    unsigned long now = millis();
    int idx = rfSlotFor(s.sensor_id, now);
    if (idx < 0) return;

    RfSensorReading& r = gRfSensors[idx];
    r.used      = true;
    r.id        = s.sensor_id;
    r.type      = s.s_type;
    r.decoder   = s.decoder;
    r.proto     = RF_PROTO_BRESSER;
    r.rssi      = s.rssi;
    r.batteryOk = s.battery_ok;
    r.lastMs    = now;
    r.staleMask &= (uint8_t)~RF_VAL_ANY;
    r.packets++;
    packetsTotal++;

    bool weather = s.s_type == SENSOR_TYPE_WEATHER0 || s.s_type == SENSOR_TYPE_WEATHER1 ||
                   s.s_type == SENSOR_TYPE_WEATHER3 || s.s_type == SENSOR_TYPE_WEATHER8 ||
                   s.s_type == SENSOR_TYPE_THERMO_HYGRO;
    if (!weather) return;
    const auto& w = s.w;
    // Only the values of *this* message (msg_ok, fork): a 6-in-1 sends two
    // alternating messages and its w.*_ok flags accumulate, which would
    // refresh e.g. rain with every temperature/wind message.
    RfValues v;
    v.ok          = s.msg_ok;
    v.tempC       = w.temp_c;
    v.humidity    = w.humidity;
    v.windAvgMs   = w.wind_avg_meter_sec;
    v.windGustMs  = w.wind_gust_meter_sec;
    v.windDirDeg  = w.wind_direction_deg;
    v.rainTotalMm = w.rain_mm;
    v.uvIndex     = w.uv;
    v.lightLux    = w.light_lux;
    unsigned long rainGapMs;
    float rainDelta = rfStoreValues(r, v, now, rainGapMs);

    srcRfPacket(idx, v.ok & RF_VAL_WIND, rainDelta, v.ok & RF_VAL_LIGHT, rainGapMs);

    dbgSerial.printf("RF sensor 0x%08lX type %u: RSSI %.0f dBm%s%s\n", (unsigned long)r.id, (unsigned)r.type, r.rssi,
                     r.batteryOk ? "" : ", battery low", s.startup ? ", startup" : "");
}

// Frees library slots whose sensor is gone from our table or not heard for
// SRC_RF_FRESH_MS (the library itself never frees a slot: with all slots
// taken, a new sensor - e.g. after a battery change - would never decode).
void freeStaleLibrarySlots()
{
    for (size_t i = 0; i < ws.sensor.size(); i++) {
        WeatherSensor::sensor_t& s = ws.sensor[i];
        if (!s.valid) continue;
        int idx = -1;
        for (int k = 0; k < RF_MAX_SENSORS; k++)
            if (gRfSensors[k].used && gRfSensors[k].proto == RF_PROTO_BRESSER && gRfSensors[k].id == s.sensor_id) idx = k;
        if (idx < 0 || (gRfSensors[idx].staleMask & RF_VAL_ANY)) s.valid = s.complete = false;
    }
}
} // namespace

void rfSensorsLoop()
{
    rfExpireValues(millis());
    int mode = gCfg.rfEn;
    if (mode != activeMode) selectRadio(mode);
    if (mode == RF_RADIO_OFF) return;
    if (!initialized) {
        gLoraRadioTakenHook = onLoraRadioTaken;
        lastError = ws.begin(RF_MAX_SENSORS, false);   // slots + first start; filters below
        receiving = lastError == RADIOLIB_ERR_NONE;
        lastStartMs = millis() | 1;
        initialized = true;
        appliedIds[0] = '\x01';           // force the filter update below
    }
    // ID filter from the settings (live; checked every loop, cheap)
    if (strncmp(appliedIds, gCfg.rfIds, sizeof(appliedIds)) != 0) {
        strlcpy(appliedIds, gCfg.rfIds, sizeof(appliedIds));
        idFilter = rfParseIds(appliedIds);
        ws.setSensorIds(idFilter, std::vector<uint32_t>());
        // Start over: sensors no longer accepted must not keep their slots
        // (both tables have only RF_MAX_SENSORS entries).
        rfSensorsClear();
        ws.clearSlots();
        dbgSerial.printf("RF sensors: accepting %s\n", appliedIds[0] ? appliedIds : "any sensor ID");
    }
    // Protocol set changed (settings) or time to switch to the next phase.
    int protos = gCfg.rfProto & RF_PROTO_ALL;
    if (!protos) protos = RF_PROTO_BRESSER;
    int set = rfPhaseSet(protos);
    if (protos != protoSet) {
        protoSet = protos;
        phase = nextPhase(set, RF_PROTO_ITPLUS9);    // first enabled, starting with Bresser
        phaseStartMs = millis();
        receiving = false;
    } else if ((set & (set - 1)) &&                 // more than one phase
               millis() - phaseStartMs >= (phase == RF_PROTO_BRESSER ? DWELL_BRESSER_MS :
                                           fineOffsetPhase()          ? DWELL_FINEOFFSET_MS : DWELL_ITPLUS_MS)) {
        phase = nextPhase(set, phase);
        phaseStartMs = millis();
        receiving = false;
    }
    // (Re)start after LoRaWAN used the transceiver, or retry after a failure.
    if (!receiving && (lastStartMs == 0 || lastError == 0 || millis() - lastStartMs >= RESTART_RETRY_MS)) {
        lastStartMs = millis();
        if (!lastStartMs) lastStartMs = 1;
        lastError = startPhase();
        receiving = lastError == RADIOLIB_ERR_NONE;
        if (!receiving)
            dbgSerial.printf("RF sensors: FSK receive start failed (%d), retry in %lu s.\n",
                             lastError, RESTART_RETRY_MS / 1000);
    }
    if (!receiving) return;

    if (phase != RF_PROTO_BRESSER) { readFsk(); return; }
    freeStaleLibrarySlots();
    ws.lastSlot = -1;
    if (ws.getMessage() == DECODE_OK && ws.lastSlot >= 0)
        copySlot(ws.lastSlot);
}

const char* rfSensorsListening()
{
    if (fineOffsetPhase())
        return (protoSet & RF_PROTO_ITPLUS17) ? "IT+ 17.241 kbit/s + Fine Offset" : "Fine Offset";
    return rfProtoName(phase);
}

bool     rfSensorsReceiving()   { return gCfg.rfEn && gCfg.rfEn == activeMode && receiving; }
int16_t  rfSensorsLastError()   { return lastError; }
uint32_t rfSensorsPacketsTotal() { return packetsTotal; }
