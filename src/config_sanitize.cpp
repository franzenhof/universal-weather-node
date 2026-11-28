// Keeps loaded settings inside what the settings form / applyParam() accept
// (sanitizeConfig(), config.h). Without NVS: host-tested
// (test/firmware_configsanitize.cpp).
#include "config.h"
#include "oled_screens.h"
#include "loxone.h"
#include "gnss_logic.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <memory>

namespace {

// Loaded values outside what applyParam() (params.cpp) and the portal accept
// - a corrupted NVS entry, or one written by an older firmware - must not
// reach the code that relies on those limits (e.g. windDirCalCount indexes
// a 16-entry table). Choices fall back to the default, ranges are clamped.
void choice(volatile int& v, int def, int a, int b, int c = -1)
{
    if (v != a && v != b && v != c) v = def;
}
void choice(int& v, int def, int a, int b, int c = -1)
{
    if (v != a && v != b && v != c) v = def;
}
void range(volatile int& v, int lo, int hi) { v = v < lo ? lo : (v > hi ? hi : v); }
void range(int& v, int lo, int hi)          { v = v < lo ? lo : (v > hi ? hi : v); }
void rangeF(volatile float& v, float def, float lo, float hi)
{
    if (!isfinite(v)) v = def;
    else v = v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

void sanitizeConfig(Config& c)
{
    // The defaults from config.h; on the heap, Config is ~7 KB.
    std::unique_ptr<const Config> defaults(new Config());
    const Config& d = *defaults;
    range (c.uplinkIntervalSec, 5, 3600);
    if (c.mqttIntervalSec != 0) range(c.mqttIntervalSec, 2, 3600);
    choice(c.loraWindAgg, d.loraWindAgg, 0, 1);
    choice(c.mqttWindAgg, d.mqttWindAgg, 0, 1);
    choice(c.rfEn, d.rfEn, RF_RADIO_OFF, RF_RADIO_SX1262);
    if (!rfProtoValid(c.rfProto)) c.rfProto = d.rfProto;   // bit mask RF_PROTO_*, RF_PROTO_AUTO or RF_PROTO_DAVIS
    range (c.davisTxId, 0, 8);
    range (c.davisOffsetHz, -100000, 100000);
    range (c.davisWindowMs, 5, 500);
    range (c.davisGuardMs, 5, 500);
    choice(c.davisRainFilter, d.davisRainFilter, 0, 1);
    range (c.davisRainMaxPkt, 1, 127);
    range (c.davisRainMaxMin, 1, 127);
    choice(c.davisBucket, d.davisBucket, 0, 1);
    choice(c.davisModel, d.davisModel, 0, 1);
    choice(c.davisSniffer, d.davisSniffer, 0, 1);
    choice(c.davisRaw, d.davisRaw, 0, 1);
    range (c.davisRawCh, 0, 4);
    range (c.davisScanKhz, 0, 60);
    choice(c.davisAfc, d.davisAfc, 0, 1);
    choice(c.davisTable, d.davisTable, 0, 1);
    if (!davisRxBwValid(c.davisRxBw)) c.davisRxBw = d.davisRxBw;
    range (c.windDirOffsetDeg, -180, 180);
    choice(c.windUnitMs, d.windUnitMs, 0, 1);
    choice(c.windEn, d.windEn, 0, 1);
    choice(c.rainEn, d.rainEn, 0, 1);
    choice(c.rainPersist, d.rainPersist, 0, 1);
    choice(c.lipoEn, d.lipoEn, 0, 1);
    choice(c.debugMode, d.debugMode, 0, 1);
    choice(c.cpuFreqMhz, d.cpuFreqMhz, 80, 240);
    choice(c.payloadFormat, d.payloadFormat, 0, 1);
    choice(c.unitSystem, d.unitSystem, 0, 1);
    rangeF(c.rainMmPerTip, d.rainMmPerTip, 0.001f, 5.0f);
    rangeF(c.windMsPerPps, d.windMsPerPps, 0.001f, 10.0f);
    range (c.altitudeM, 0, 4000);              // the form, the portal and GNSS store 0..4000
    range (c.awekasIntervalSec, 300, 3600);    // the weather-network intervals: as the form accepts them
    range (c.wuIntervalSec, 60, 3600);
    range (c.wcloudIntervalSec, 60, 3600);
    range (c.wnetproIntervalSec, 60, 3600);
    range (c.windyIntervalSec, 300, 3600);
    range (c.osemIntervalSec, 60, 3600);
    range (c.wowIntervalSec, 300, 3600);
    range (c.cwopIntervalSec, 300, 3600);
    range (c.cwopPasscode, -1, 32767);
    if (c.influxEn) c.influxEn = 1;
    range (c.influxIntervalSec, 10, 3600);
    range (c.calTempDc, -100, 100);
    range (c.calHumDpct, -200, 200);
    range (c.calPressDhpa, -200, 200);
    range (c.calRainPermil, 500, 2000);
    range (c.calWindPermil, 500, 2000);
    choice(c.sunMethod, d.sunMethod, 0, 1);
    range (c.dispMode, 0, 2);
    range (c.dispMinutes, 1, 120);
    range (c.dispScreens, 0, (1 << OS_COUNT) - 1);
    if (!c.dispScreens) c.dispScreens = 1;
    range (c.dispRotateSec, 2, 60);
    range (c.dispContrast, 1, 255);
    range (c.dispNightFrom, 0, 23);
    range (c.dispFlip, 0, 1);
    range (c.measureSec, 10, 600);
    range (c.dispBattery, 0, 1);
    range (c.dispLed, 0, 3);
    range (c.saverMode, 0, 4);
    range (c.saverMin, 1, 60);
    range (c.saverSec, 3, 30);
    range (c.dispNightTo, 0, 23);
    choice(c.windDirSensorType, d.windDirSensorType, 0, 1);
    range (c.windDirCalCount, 0, 16);
    choice(c.loraEn, d.loraEn, 0, 1);
    choice(c.lowPowerEn, d.lowPowerEn, 0, 1);
    choice(c.vanePowerSave, d.vanePowerSave, 0, 1);
    choice(c.wifiEn, d.wifiEn, 0, 1);
    choice(c.mqttEn, d.mqttEn, 0, 1);
    choice(c.webApiEn, d.webApiEn, 0, 1);
    choice(c.mqttCtrlEn, d.mqttCtrlEn, 0, 1);
    choice(c.httpsEn, d.httpsEn, 0, 1);
    choice(c.httpPortMode, d.httpPortMode, 0, 2);
    choice(c.staticIpEn, d.staticIpEn, 0, 1);
    choice(c.apMode, d.apMode, 0, 1, 2);
    choice(c.gnssEn, d.gnssEn, 0, 1);
    if (!gnssBaudValid(c.gnssBaud)) c.gnssBaud = d.gnssBaud;
    choice(c.gnssTime, d.gnssTime, 0, 1);
    choice(c.gnssPos, d.gnssPos, 0, 1);
    choice(c.gnssAlt, d.gnssAlt, 0, 1);
    range (c.apChannel, 0, 13);
    if (c.mqttPort < 1 || c.mqttPort > 65535) c.mqttPort = d.mqttPort;
    choice(c.mqttTlsMode, d.mqttTlsMode, MQTT_TLS_NONE, MQTT_TLS_INSECURE, MQTT_TLS_VERIFY);
    choice(c.loxEn, d.loxEn, 0, 1);
    if (!loxPortValid(c.loxPort)) c.loxPort = d.loxPort;
    if (!loxIntervalValid(c.loxSec)) c.loxSec = d.loxSec;
    loxSanitizePrefix(c.loxPrefix);

    choice(c.auxAinEn, d.auxAinEn, 0, 1);
    rangeF(c.auxAinScale, d.auxAinScale, 0.001f, 20.0f);
    choice(c.auxPulseEn, d.auxPulseEn, 0, 1);
    choice(c.wind2Mode, d.wind2Mode, 0, 1, 2);
    if (c.wind2Mode >= 1) c.auxPulseEn = 0;   // the 2nd wind sensor owns the AUX inputs it uses
    if (c.wind2Mode == 2) c.auxAinEn = 0;
    range (c.wind2DirOffsetDeg, -180, 180);
    choice(c.heaterMode, d.heaterMode, 0, 1, 2);
    range (c.heaterDutyPct, 1, 100);
    range (c.heaterOnBelowDc, -400, 400);
    range (c.heaterMinDutyPct, 1, 100);
    range (c.heaterFullBelowDc, -400, 400);
    if (c.heaterFullBelowDc > c.heaterOnBelowDc) c.heaterFullBelowDc = c.heaterOnBelowDc;
    choice(c.fanMode, d.fanMode, 0, 1, 2);
    range (c.fanDutyPct, 1, 100);
    range (c.fanOnAboveDc, -400, 600);
    range (c.fanMinPvW, 0, 5000);
    choice(c.tachoEn, d.tachoEn, 0, 1);
    range (c.tachoPpr, 1, 8);
    choice(c.veDirectEn, d.veDirectEn, 0, 1);
    range (c.outMinBattDv, 0, 600);
    choice(c.uvEn, d.uvEn, 0, 1);
    rangeF(c.uvAinScale, d.uvAinScale, 1.0f, 5.0f);
    choice(c.uvIndexEn, d.uvIndexEn, 0, 1);
    choice(c.uvCalibrated, d.uvCalibrated, 0, 1);
    rangeF(c.uvIndexSlope, d.uvIndexSlope, 0.001f, 30.0f);
    rangeF(c.uvIndexOffset, d.uvIndexOffset, -10.0f, 10.0f);
}

bool coordParse(const char* in, float limit, char* out, size_t outLen)
{
    char v[24];
    const size_t n = strlen(in);
    if (n >= sizeof(v) || n >= outLen) return false;
    memcpy(v, in, n + 1);
    char* comma = strchr(v, ',');
    if (comma) {   // the decimal comma: one, and not together with a point
        if (strchr(comma + 1, ',') || strchr(v, '.')) return false;
        *comma = '.';
    }
    if (v[0]) {
        char* end = nullptr;
        const float f = strtof(v, &end);
        if (*end != '\0' || !isfinite(f) || f < -limit || f > limit) return false;
    }
    memcpy(out, v, n + 1);
    return true;
}
