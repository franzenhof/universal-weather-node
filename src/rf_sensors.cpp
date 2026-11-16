#include "rf_sensors.h"
#include "globals.h"
#include "lora_transport.h"
#include "serial_log.h"
#include "sources.h"
#include <TfaWeatherSensor.h>

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

// Protocol time sharing: the protocol listened to now, since when, and the
// set it was chosen from. One protocol = listen continuously.
int           phase        = RF_PROTO_BRESSER;
unsigned long phaseStartMs = 0;
int           phaseSet     = 0;
constexpr unsigned long DWELL_BRESSER_MS = 17000;   // > Bresser send interval (12-16 s)
constexpr unsigned long DWELL_ITPLUS_MS  = 5000;    // > IT+ send interval (about 4 s)

TfaWeatherSensor tfa;

void onLoraRadioTaken() { receiving = false; }

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
    return tfa.startReceive(phase == RF_PROTO_ITPLUS17 ? TfaWeatherSensor::IT_PLUS_17241 : TfaWeatherSensor::IT_PLUS_9579);
}

// Table slot for a sensor ID (existing or free), -1 = table full.
int slotFor(uint32_t id)
{
    int freeIdx = -1;
    for (int i = 0; i < RF_MAX_SENSORS; i++) {
        if (gRfSensors[i].used && gRfSensors[i].id == id) return i;
        if (!gRfSensors[i].used && freeIdx < 0) freeIdx = i;
    }
    return freeIdx;
}

void readItPlus()
{
    ItPlusFrame f;
    float rssi;
    if (!tfa.getMessage(f, rssi)) return;
    uint32_t id = (phase == RF_PROTO_ITPLUS17 ? RF_ID_ITPLUS17 : RF_ID_ITPLUS9) | f.sensorId;
    if (!idFilter.empty() && std::find(idFilter.begin(), idFilter.end(), id) == idFilter.end()) return;
    int idx = slotFor(id);
    if (idx < 0) return;
    RfSensorReading& r = gRfSensors[idx];
    r.used      = true;
    r.id        = id;
    r.type      = SENSOR_TYPE_THERMO_HYGRO;
    r.decoder   = 0;
    r.proto     = phase;
    r.rssi      = rssi;
    r.batteryOk = !f.weakBattery;
    r.lastMs    = millis();
    r.packets++;
    packetsTotal++;
    r.hasTemp = true;  r.tempC = f.tempC;  r.tempMs = r.lastMs;
    if (f.hasHumidity) { r.hasHumidity = true; r.humidity = f.humidity; r.humMs = r.lastMs; }
    srcRfPacket(idx, false, 0.0f, false);
    dbgSerial.printf("RF sensor 0x%08lX (IT+): %.1f C%s, RSSI %.0f dBm%s%s\n", (unsigned long)id, f.tempC,
                     f.hasHumidity ? (String(", ") + f.humidity + " %").c_str() : "", rssi,
                     f.weakBattery ? ", battery low" : "", f.newBattery ? ", new battery" : "");
}

// Setting switched on or off at run time.
void selectRadio(int mode)
{
    if (mode == RF_RADIO_SX1262) { WeatherSensor::setRadio(&radio); TfaWeatherSensor::setRadio(&radio); }
    activeMode  = mode;
    receiving   = false;
    lastError   = 0;
    lastStartMs = 0;
    dbgSerial.printf("RF sensors: %s\n", mode == RF_RADIO_SX1262 ? "shared LoRa chip" : "off");
}

void copySlot(int slot)
{
    const WeatherSensor::sensor_t& s = ws.sensor[slot];
    // Keep our table keyed by sensor ID (the library slots are reused the same way).
    int idx = slotFor(s.sensor_id);
    if (idx < 0) return;

    RfSensorReading& r = gRfSensors[idx];
    r.used      = true;
    r.id        = s.sensor_id;
    r.type      = s.s_type;
    r.decoder   = s.decoder;
    r.proto     = RF_PROTO_BRESSER;
    r.rssi      = s.rssi;
    r.batteryOk = s.battery_ok;
    r.lastMs    = millis();
    r.packets++;
    packetsTotal++;

    bool weather = s.s_type == SENSOR_TYPE_WEATHER0 || s.s_type == SENSOR_TYPE_WEATHER1 ||
                   s.s_type == SENSOR_TYPE_WEATHER3 || s.s_type == SENSOR_TYPE_WEATHER8 ||
                   s.s_type == SENSOR_TYPE_THERMO_HYGRO;
    if (!weather) return;
    const auto& w = s.w;
    // 6-in-1 sends two alternating messages: keep values not in this one.
    const unsigned long now = r.lastMs;
    if (w.temp_ok)     { r.hasTemp = true;     r.tempC = w.temp_c;      r.tempMs = now; }
    if (w.humidity_ok) { r.hasHumidity = true; r.humidity = w.humidity; r.humMs = now; }
    if (w.wind_ok) {
        r.hasWind    = true;
        r.windMs     = now;
        r.windAvgMs  = w.wind_avg_meter_sec;
        r.windGustMs = w.wind_gust_meter_sec;
        r.windDirDeg = w.wind_direction_deg;
    }
    float rainDelta = 0.0f;
    if (w.rain_ok) {
        // increase since the previous packet; a smaller total = sensor reset (battery change)
        if (r.hasRain) rainDelta = w.rain_mm >= r.rainTotalMm ? w.rain_mm - r.rainTotalMm : w.rain_mm;
        r.hasRain = true;  r.rainTotalMm = w.rain_mm;  r.rainMs = now;
    }
    if (w.uv_ok)    { r.hasUv = true;    r.uvIndex = w.uv;           r.uvMs = now; }
    if (w.light_ok) { r.hasLight = true; r.lightLux = w.light_lux;   r.lightMs = now; }

    srcRfPacket(idx, w.wind_ok, rainDelta, w.light_ok);

    dbgSerial.printf("RF sensor 0x%08lX type %u: RSSI %.0f dBm%s%s\n", (unsigned long)r.id, (unsigned)r.type, r.rssi,
                     r.batteryOk ? "" : ", battery low", s.startup ? ", startup" : "");
}
} // namespace

void rfSensorsLoop()
{
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
        dbgSerial.printf("RF sensors: accepting %s\n", appliedIds[0] ? appliedIds : "any sensor ID");
    }
    // Protocol set changed (settings) or time to switch to the next protocol.
    int set = gCfg.rfProto & RF_PROTO_ALL;
    if (!set) set = RF_PROTO_BRESSER;
    if (set != phaseSet) {
        phaseSet = set;
        phase = nextPhase(set, RF_PROTO_ITPLUS9);    // first enabled, starting with Bresser
        phaseStartMs = millis();
        receiving = false;
    } else if ((set & (set - 1)) &&                 // more than one protocol
               millis() - phaseStartMs >= (phase == RF_PROTO_BRESSER ? DWELL_BRESSER_MS : DWELL_ITPLUS_MS)) {
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

    if (phase != RF_PROTO_BRESSER) { readItPlus(); return; }
    ws.lastSlot = -1;
    if (ws.getMessage() == DECODE_OK && ws.lastSlot >= 0)
        copySlot(ws.lastSlot);
}

const char* rfSensorsListening() { return rfProtoName(phase); }

bool     rfSensorsReceiving()   { return gCfg.rfEn && gCfg.rfEn == activeMode && receiving; }
int16_t  rfSensorsLastError()   { return lastError; }
uint32_t rfSensorsPacketsTotal() { return packetsTotal; }
