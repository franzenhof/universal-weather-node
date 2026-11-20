#include "rf_sensors.h"
#include "globals.h"
#include "lora_transport.h"
#include "serial_log.h"
#include "sources.h"
#include "rf_fsk17.h"
#include "davis_rx.h"
#include "rf_trace.h"
#include <TfaWeatherSensor.h>
#include <FineOffsetSensor.h>

#include <RadioLib.h>
#include <WeatherSensorCfg.h>
#include <WeatherSensor.h>
#include <vector>
#include <algorithm>

// The Heltec's transceiver (heltec_board.cpp; Heltec V4 front end handled there).
#include "radio_board.h"

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
int           protoSet     = 0;      // protocol set applied (RF_PROTO_*, rfProtoSetFor(gCfg.rfProto))
int           appliedCfg   = 0;      // gCfg.rfProto it was derived from (RF_PROTO_AUTO: searching)
int           detectedProto = 0;     // auto search: type stored since the start (0 = none)
// Dwell times: rfPhaseDwellMs() (rf_sensors_data.cpp). WH31 / WH40 / WH51 /
// WH57 send every 49-79 s and are then heard only every few transmissions
// while alternating - alternating is not recommended for them.

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

// Auto search: the first valid packet (CRC-checked by the decoders, from a
// listed ID - the callers check the ID list first) fixes the sensor type.
// Stored at once (only the "rfproto" key: one NVS write); rfSensorsLoop()
// then switches from the search rotation to that type's phase.
void autoLock(int receivedProto)
{
    if (!rfAutoShouldLock(gCfg.rfProto, true)) return;
    int type = rfAutoDetectedProto(receivedProto);
    if (!type) return;
    gCfg.rfProto  = type;
    detectedProto = type;
    saveRfProto(type);
    dbgSerial.printf("RF sensors: auto search found %s - sensor type %d saved.\n", rfProtoName(receivedProto), type);
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

// Returns how the frame was taken (radio trace): OK, or IGNORED (ID list, table full).
RfTraceResult storeItPlus(const ItPlusFrame& f, float rssi)
{
    uint32_t id = (phase == RF_PROTO_ITPLUS17 ? RF_ID_ITPLUS17 : RF_ID_ITPLUS9) | f.sensorId;
    if (!idAccepted(id)) {
        if (f.newBattery)
            dbgSerial.printf("RF sensor 0x%08lX (IT+, new battery) not in the sensor ID list - ignored.\n", (unsigned long)id);
        return RF_TR_IGNORED;
    }
    // A sensor with a new battery sends a new random ID: it may take over
    // the slot of an IT+ sensor of the same rate not heard for a minute
    // (normally the one whose battery was changed) instead of waiting for
    // the freshness window. With an ID list it must be entered there.
    unsigned long now = millis();
    int idx = rfSlotFor(id, now, f.newBattery ? NEW_BATTERY_TAKEOVER_MS : SRC_RF_FRESH_MS,
                        f.newBattery ? phase : 0);
    if (idx < 0) return RF_TR_IGNORED;
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
    autoLock(r.proto);
    return RF_TR_OK;
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
        case FO_FAMILY_WH25: case FO_FAMILY_WH32:  return SENSOR_TYPE_THERMO_HYGRO;
        case FO_FAMILY_WH52: case FO_FAMILY_WN35:  return SENSOR_TYPE_SOIL;   // WN35 leaf wetness: told apart by model
        case FO_FAMILY_WH55:                       return SENSOR_TYPE_LEAKAGE;
        case FO_FAMILY_WH41: case FO_FAMILY_WH0290: case FO_FAMILY_WH43:
        case FO_FAMILY_WH45: case FO_FAMILY_WH46:  return SENSOR_TYPE_AIR_PM;
        default:                                   return SENSOR_TYPE_WEATHER1;
    }
}

RfTraceResult storeFineOffset(const FoFrame& f, float rssi)
{
    uint32_t id = rfFineOffsetId(f);
    if (!idAccepted(id)) return RF_TR_IGNORED;
    // The ID changes with a battery change (WH65, WH31, ...): the new one
    // takes a free slot or that of a sensor not heard for SRC_RF_FRESH_MS.
    unsigned long now = millis();
    int idx = rfSlotFor(id, now);
    if (idx < 0) return RF_TR_IGNORED;
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
    rfFineOffsetExtras(r, f);
    r.packets++;
    packetsTotal++;
    RfValues v = rfFineOffsetValues(f);
    unsigned long rainGapMs;
    float rainDelta = rfStoreValues(r, v, now, rainGapMs);
    srcRfPacket(idx, v.ok & RF_VAL_WIND, rainDelta, v.ok & RF_VAL_LIGHT, rainGapMs);
    dbgSerial.printf("RF sensor 0x%08lX (Fine Offset %s): RSSI %.0f dBm%s\n", (unsigned long)id, f.model, rssi,
                     f.batteryOk ? "" : ", battery low");
    autoLock(RF_PROTO_FINEOFFSET);
    return RF_TR_OK;
}

uint8_t tracePhase()
{
    if (phase == RF_PROTO_BRESSER) return RF_TP_BRESSER;
    if (fineOffsetPhase())         return RF_TP_FSK17;
    return phase == RF_PROTO_ITPLUS9 ? RF_TP_ITPLUS9 : RF_TP_ITPLUS17;
}

const char* ignoredText(RfTraceResult r) { return r == RF_TR_IGNORED ? "not in the ID list or table full" : "ok"; }

// The 17.241 / 9.579 kbit/s phases. Every frame goes to the radio trace
// (while one runs) - also those no decoder accepts.
void readFsk()
{
    float rssi;
    char check[96] = "", vals[160] = "";
    RfTracePacket tp;
    tp.phase = tracePhase();
    if (!fineOffsetPhase()) {
        uint8_t b[ITPLUS_FRAME_LEN];
        if (!tfa.getRaw(b, rssi)) return;
        ItPlusFrame f;
        tp.rssi = rssi; tp.raw = b; tp.rawLen = sizeof(b);
        if (itPlusDecode(b, f)) {
            tp.result = storeItPlus(f, rssi);
            if (!rfTraceRunning()) return;
            tp.protocol = "IT+"; tp.check = tp.result == RF_TR_OK ? "CRC ok" : ignoredText(tp.result);
            tp.hasId = true; tp.id = (phase == RF_PROTO_ITPLUS17 ? RF_ID_ITPLUS17 : RF_ID_ITPLUS9) | f.sensorId;
            rfTraceItPlusValues(f, vals, sizeof(vals)); tp.values = vals;
        } else {
            if (!rfTraceRunning()) return;
            rfTraceClassifyItPlus(b, tp, check, sizeof(check));
        }
        rfTracePacket(tp);
        return;
    }
    uint8_t b[FO_MAX_FRAME_LEN];
    size_t len;
    if (!fo.getRaw(b, sizeof(b), len, rssi)) return;
    FoFrame f;
    ItPlusFrame it;
    tp.rssi = rssi; tp.raw = b; tp.rawLen = len;
    switch (rfFsk17Decode(b, len, true, protoSet & RF_PROTO_ITPLUS17, f, it)) {
        case RF_FSK_FINEOFFSET: {
            tp.result = storeFineOffset(f, rssi);
            if (!rfTraceRunning()) return;
            size_t fl = f.family == FO_FAMILY_WH25 || f.family == FO_FAMILY_WH32 ? FO_WH25_FRAME_LEN : foFrameLength(f.family);
            if (fl && fl < len) tp.rawLen = fl;   // the frame; the rest of the fixed-length read is noise
            tp.protocol = "Fine Offset"; tp.model = f.model;
            tp.check = tp.result == RF_TR_OK ? "CRC ok, sum ok" : ignoredText(tp.result);
            tp.hasId = true; tp.id = rfFineOffsetId(f);
            rfTraceFoValues(f, vals, sizeof(vals)); tp.values = vals;
            break;
        }
        case RF_FSK_ITPLUS:
            tp.result = storeItPlus(it, rssi);
            if (!rfTraceRunning()) return;
            tp.rawLen = ITPLUS_FRAME_LEN;
            tp.protocol = "IT+"; tp.check = tp.result == RF_TR_OK ? "CRC ok" : ignoredText(tp.result);
            tp.hasId = true; tp.id = RF_ID_ITPLUS17 | it.sensorId;
            rfTraceItPlusValues(it, vals, sizeof(vals)); tp.values = vals;
            break;
        default:
            if (!rfTraceRunning()) return;
            rfTraceClassifyFsk17(b, len, true, protoSet & RF_PROTO_ITPLUS17, tp, check, sizeof(check));
            break;
    }
    rfTracePacket(tp);
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

// false = not taken (node table full).
bool copySlot(int slot)
{
    const WeatherSensor::sensor_t& s = ws.sensor[slot];
    // Keep our table keyed by sensor ID (the library slots are reused the same way).
    unsigned long now = millis();
    int idx = rfSlotFor(s.sensor_id, now);
    if (idx < 0) return false;

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

    // Any decoded Bresser packet (digest-checked; the library applies the ID
    // list) ends an auto search - also non-weather types (lightning, ...).
    autoLock(RF_PROTO_BRESSER);

    bool weather = s.s_type == SENSOR_TYPE_WEATHER0 || s.s_type == SENSOR_TYPE_WEATHER1 ||
                   s.s_type == SENSOR_TYPE_WEATHER3 || s.s_type == SENSOR_TYPE_WEATHER8 ||
                   s.s_type == SENSOR_TYPE_THERMO_HYGRO;
    if (!weather) return true;
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
    return true;
}

// Radio trace of the Bresser phase: the library hands every frame it read
// to bresserRaw() (during ws.getMessage()), traceBresser() writes it after
// the frame was decoded and taken.
uint8_t      bRaw[MSG_BUF_SIZE];
float        bRssi = 0.0f;
DecodeStatus bRes = DECODE_INVALID;
bool         bHave = false;

void bresserRaw(const uint8_t* data, size_t len, float rssi, DecodeStatus res)
{
    if (!rfTraceRunning()) return;
    size_t n = len < sizeof(bRaw) ? len : sizeof(bRaw);
    memcpy(bRaw, data, n);
    bRssi = rssi; bRes = res; bHave = true;
}

const char* bresserDecoderName(uint8_t d)
{
    switch (d) {
        case DECODER_5IN1:      return "Bresser 5-in-1";
        case DECODER_6IN1:      return "Bresser 6-in-1";
        case DECODER_7IN1:      return "Bresser 7-in-1";
        case DECODER_LIGHTNING: return "Bresser lightning";
        case DECODER_LEAKAGE:   return "Bresser leakage";
        default:                return "Bresser";
    }
}

const char* bresserResText(DecodeStatus r)
{
    switch (r) {
        case DECODE_PAR_ERR: return "parity fail";
        case DECODE_CHK_ERR: return "checksum fail";
        case DECODE_DIG_ERR: return "digest fail";
        case DECODE_SKIP:    return "ID not in the list";
        case DECODE_FULL:    return "library slots full";
        default:             return "no decoder matched";
    }
}

void traceBresser(DecodeStatus res, int slot, bool taken)
{
    char check[96], vals[160] = "";
    RfTracePacket tp;
    tp.phase = RF_TP_BRESSER;
    tp.rssi = bRssi;
    const bool sync = bRaw[0] == 0xD4;
    tp.raw = sync ? bRaw + 1 : bRaw;          // the bytes after the sync word
    tp.rawLen = sync ? sizeof(bRaw) - 1 : sizeof(bRaw);
    tp.protocol = "Bresser";
    tp.check = check;
    if (!sync) {
        tp.result = RF_TR_UNKNOWN;
        snprintf(check, sizeof(check), "last sync byte 0x%02X instead of D4", bRaw[0]);
    } else if (res == DECODE_OK && slot >= 0 && slot < (int)ws.sensor.size()) {
        const WeatherSensor::sensor_t& s = ws.sensor[slot];
        tp.result = taken ? RF_TR_OK : RF_TR_IGNORED;
        tp.protocol = bresserDecoderName(s.decoder);
        tp.hasId = true; tp.id = s.sensor_id;
        snprintf(check, sizeof(check), taken ? "digest ok" : "digest ok, node table full");
        const auto& w = s.w;
        size_t n = (size_t)snprintf(vals, sizeof(vals), "type %u ch%u", (unsigned)s.s_type, (unsigned)s.chan);
        if ((s.msg_ok & MSG_OK_TEMP) && n < sizeof(vals))     n += snprintf(vals + n, sizeof(vals) - n, " T %.1fC", w.temp_c);
        if ((s.msg_ok & MSG_OK_HUMIDITY) && n < sizeof(vals)) n += snprintf(vals + n, sizeof(vals) - n, " H %u%%", (unsigned)w.humidity);
        if ((s.msg_ok & MSG_OK_WIND) && n < sizeof(vals))
            n += snprintf(vals + n, sizeof(vals) - n, " W %.1f/%.1f m/s %.0f deg", w.wind_avg_meter_sec, w.wind_gust_meter_sec, w.wind_direction_deg);
        if ((s.msg_ok & MSG_OK_RAIN) && n < sizeof(vals))     n += snprintf(vals + n, sizeof(vals) - n, " R %.1fmm", w.rain_mm);
        if ((s.msg_ok & MSG_OK_UV) && n < sizeof(vals))       n += snprintf(vals + n, sizeof(vals) - n, " UV %.1f", w.uv);
        if ((s.msg_ok & MSG_OK_LIGHT) && n < sizeof(vals))    n += snprintf(vals + n, sizeof(vals) - n, " L %.0flx", w.light_lux);
        if (n < sizeof(vals)) snprintf(vals + n, sizeof(vals) - n, "%s%s", s.battery_ok ? " bat ok" : " BAT LOW", s.startup ? " startup" : "");
        tp.values = vals;
    } else if (res == DECODE_SKIP || res == DECODE_FULL) {
        tp.result = RF_TR_IGNORED;
        snprintf(check, sizeof(check), "%s", bresserResText(res));
    } else {
        tp.result = RF_TR_FAILED;
        snprintf(check, sizeof(check), "sync ok, no decoder accepted it (last: %s)", bresserResText(res));
    }
    rfTracePacket(tp);
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
    rfTracePoll();
    int mode = gCfg.rfEn;
    if (mode != activeMode) selectRadio(mode);
    // Davis (exclusive): its own receive task owns the radio (davis_rx.cpp).
    if (rfDavisMode(mode, gCfg.rfProto)) { davisRxLoop(); return; }
    if (davisRadioOwned()) {   // Davis mode just left: the radio needs a full set-up again
        davisRxStop();
        receiving = false;
        lastStartMs = 0;
        lastError = 0;
        protoSet = 0;
        appliedIds[0] = '\x01';
    }
    if (mode == RF_RADIO_OFF) return;
    if (!initialized) {
        gLoraRadioTakenHook = onLoraRadioTaken;
        WeatherSensor::setRawHook(bresserRaw);
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
    // Protocol set changed (settings, auto search locked) or time to switch
    // to the next phase. Auto (search) = all families in rotation.
    const int cfg = gCfg.rfProto;
    int protos = rfProtoSetFor(cfg);
    int set = rfPhaseSet(protos);
    if (cfg == RF_PROTO_AUTO && appliedCfg != RF_PROTO_AUTO) {
        detectedProto = 0;
        dbgSerial.println("RF sensors: auto search - Bresser, IT+ 17.241 kbit/s + Fine Offset and IT+ 9.579 kbit/s in turn");
    }
    appliedCfg = cfg;
    if (protos != protoSet) {
        protoSet = protos;
        phase = rfNextPhase(set, RF_PROTO_ITPLUS9);    // first enabled, starting with Bresser
        phaseStartMs = millis();
        receiving = false;
    } else if ((set & (set - 1)) &&                 // more than one phase
               millis() - phaseStartMs >= rfPhaseDwellMs(phase, fineOffsetPhase())) {
        phase = rfNextPhase(set, phase);
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
    bHave = false;
    DecodeStatus res = ws.getMessage();
    bool taken = false;
    if (res == DECODE_OK && ws.lastSlot >= 0) taken = copySlot(ws.lastSlot);
    if (bHave) { bHave = false; traceBresser(res, ws.lastSlot, taken); }
}

int rfSensorsDetectedProto() { return detectedProto && gCfg.rfProto == detectedProto ? detectedProto : 0; }

const char* rfSensorsListening()
{
    if (fineOffsetPhase())
        return (protoSet & RF_PROTO_ITPLUS17) ? "IT+ 17.241 kbit/s + Fine Offset" : "Fine Offset";
    return rfProtoName(phase);
}

bool     rfSensorsReceiving()   { return gCfg.rfEn && gCfg.rfEn == activeMode && receiving; }
int16_t  rfSensorsLastError()   { return lastError; }
uint32_t rfSensorsPacketsTotal() { return packetsTotal; }
