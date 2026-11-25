#include "config.h"
#include "oled_screens.h"
#include "sources.h"
#include "ha_rf.h"
#include "loxone.h"
#include "wind_window.h"
#include "serial_log.h"
#include <Preferences.h>
#include <memory>

uint64_t hexToUint64(const char* s) { return strtoull(s, nullptr, 16); }

void hexToBytes(const char* s, uint8_t* out, size_t maxLen)
{
    for (size_t i = 0; i < strlen(s) && (i / 2) < maxLen; i += 2) {
        char b[] = { s[i], s[i + 1], '\0' };
        out[i / 2] = (uint8_t)strtol(b, nullptr, 16);
    }
}

void uint64ToHex(uint64_t value, char* out, size_t outSize)
{
    if (outSize > 0) {
        snprintf(out, outSize, "%016llX", (unsigned long long)value);
    }
}

void bytesToHex(const uint8_t* bytes, size_t byteCount, char* out, size_t outSize)
{
    size_t pos = 0;
    if (outSize == 0) return;

    for (size_t i = 0; i < byteCount && (pos + 2) < outSize; ++i) {
        pos += snprintf(out + pos, outSize - pos, "%02X", bytes[i]);
    }
}

bool isSupportedRegion(const String& region)
{
    static const char* const kRegions[] = {
        "EU868", "US915", "AU915", "AS923", "IN865", "KR920", "CN470", "RU864"
    };
    for (size_t i = 0; i < (sizeof(kRegions) / sizeof(kRegions[0])); ++i) {
        if (region == kRegions[i]) return true;
    }
    return false;
}

void normalizeRegionInput(const char* raw, char* out, size_t outSize)
{
    String region = raw ? String(raw) : String("");
    region.trim();
    region.toUpperCase();
    region.replace(" ", "");

    if (!isSupportedRegion(region)) {
        region = "EU868";
    }
    strlcpy(out, region.c_str(), outSize);
}

namespace {

// Preferences::put*() never surfaces failures (e.g. NVS partition full) to
// the caller other than through its return value, and nothing previously
// checked it — a write could silently drop with no indication anywhere.
// putInt/putFloat/putULong64/putBytes return exactly the number of bytes
// written on success (never 0 for our non-empty calls), so a 0 return is an
// unambiguous failure. putString instead returns strlen(value), which is
// legitimately 0 for an empty (but successfully written) optional field —
// comparing against the expected length instead of a flat 0 handles that.
bool checkWrite(const char* key, size_t written, size_t expected)
{
    if (written != expected) {
        dbgSerial.printf("Config save: FAILED to write '%s' (NVS full or error?)\n", key);
        return false;
    }
    return true;
}

bool checkWriteStr(const char* key, size_t written, const char* value)
{
    return checkWrite(key, written, strlen(value));
}

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

void sanitizeConfig(Config& c)
{
    // The defaults from config.h; on the heap, Config is ~7 KB.
    std::unique_ptr<const Config> defaults(new Config());
    const Config& d = *defaults;
    choice(c.pcbVersion, d.pcbVersion, PCB_REV_1_2, PCB_REV_1_3, PCB_REV_2_0);
    choice(c.boardSel, d.boardSel, 0, 3, 4);   // BOARD_SEL_AUTO, BOARD_V3, BOARD_V4
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
    range (c.wowIntervalSec, 300, 3600);
    range (c.cwopIntervalSec, 300, 3600);
    range (c.cwopPasscode, -1, 32767);
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
    choice(c.staticIpEn, d.staticIpEn, 0, 1);
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

} // namespace

void loadConfigInto(Config& c)
{
    Preferences prefs;
    // Own dedicated NVS-formatted partition (see partitions.csv) rather than
    // the stock "nvs" partition — keeps our config fully isolated from
    // WiFiManager's/ESP-IDF's own WiFi credential storage and from any other
    // firmware project that happens to share the "config" namespace name.
    //
    // On first boot this partition doesn't have a "config" namespace yet, so
    // begin() returns false. Return the all-defaults Config (devEui == 0),
    // which causes setup() to open the Wi-Fi configuration portal.
    if (!prefs.begin("config", true, "cfgdata")) {
        return;
    }
    // A config stored by any earlier firmware always has "deveui" (saveConfig()
    // writes every key; a factory reset clears them all). For such a node, a
    // key added later falls back to the pre-2.0.0-rc.2 default, not to the
    // new-node default (ConfigLegacyDefaults, config.h).
    if (prefs.isKey("deveui")) {
        const ConfigLegacyDefaults l;
        c.loraEn = l.loraEn;  c.wifiEn = l.wifiEn;  c.webApiEn = l.webApiEn;
        c.rfEn = l.rfEn;      c.rfProto = l.rfProto;
        c.windEn = l.windEn;  c.rainEn = l.rainEn;
    }
    c.provisioned      = prefs.getInt    ("provisioned", c.provisioned);
    c.pcbVersion       = prefs.getInt    ("pcbver",   c.pcbVersion);
    c.boardSel         = prefs.getInt    ("board",    c.boardSel);
    c.altitudeM        = prefs.getInt    ("altitude", c.altitudeM);
    c.uplinkIntervalSec = prefs.getInt   ("uplinksec", prefs.getInt("delay", c.uplinkIntervalSec));
    c.sampleSec        = windSampleSecNormalized(prefs.getInt("samplesec", c.sampleSec));
    c.mqttIntervalSec  = prefs.getInt    ("mqttsec",   c.mqttIntervalSec);
    c.loraWindAgg      = prefs.getInt    ("lorawagg",  c.loraWindAgg);
    c.mqttWindAgg      = prefs.getInt    ("mqttwagg",  c.mqttWindAgg);
    c.rfEn             = prefs.getInt    ("rfen",      c.rfEn);
    c.rfProto          = prefs.getInt    ("rfproto",   c.rfProto);
    prefs.getString("rfids", c.rfIds, sizeof(c.rfIds));
    prefs.getString("rfignore", c.rfIgnore, sizeof(c.rfIgnore));
    c.uiLang           = prefs.getInt    ("uilang",    c.uiLang);
    c.setupDone        = prefs.isKey("setupdone") ? prefs.getInt("setupdone", 0) : (prefs.isKey("deveui") ? 1 : 0);
    if (c.uiLang < 0 || c.uiLang > 5) c.uiLang = 0;
    c.rfOnlyMine       = prefs.isKey("rfonly") ? prefs.getInt("rfonly", 0) : rfOnlyMineDefault(c.rfIds);
    c.davisTxId        = prefs.getInt    ("dvid",      c.davisTxId);
    c.davisOffsetHz    = prefs.getInt    ("dvoff",     c.davisOffsetHz);
    c.davisWindowMs    = prefs.getInt    ("dvwin",     c.davisWindowMs);
    c.davisGuardMs     = prefs.getInt    ("dvguard",   c.davisGuardMs);
    c.davisRainFilter  = prefs.getInt    ("dvrfilt",   c.davisRainFilter);
    c.davisRainMaxPkt  = prefs.getInt    ("dvrpkt",    c.davisRainMaxPkt);
    c.davisRainMaxMin  = prefs.getInt    ("dvrmin",    c.davisRainMaxMin);
    c.davisBucket      = prefs.getInt    ("dvbucket",  c.davisBucket);
    c.davisModel       = prefs.getInt    ("dvmodel",   c.davisModel);
    c.davisSniffer     = prefs.getInt    ("dvsniff",   c.davisSniffer);
    c.davisRaw         = prefs.getInt    ("dvraw",     c.davisRaw);
    c.davisRawCh       = prefs.getInt    ("dvrawch",   c.davisRawCh);
    c.davisScanKhz     = prefs.getInt    ("dvscan",    c.davisScanKhz);
    c.davisAfc         = prefs.getInt    ("dvafc",     c.davisAfc);
    c.davisTable       = prefs.getInt    ("dvtable",   c.davisTable);
    c.davisRxBw        = prefs.getInt    ("dvbw",      c.davisRxBw);
    for (int i = 0; i < (int)sizeof(c.srcSel); i++) {
        char key[8]; snprintf(key, sizeof(key), "src%d", i);
        int v = prefs.getInt(key, c.srcSel[i]);
        c.srcSel[i] = (uint8_t)(srcSelValid(v) ? v : 0);
        char kid[8]; snprintf(kid, sizeof(kid), "srcid%d", i);
        c.srcId[i] = prefs.getUInt(kid, c.srcId[i]);
    }
    c.windDirOffsetDeg = prefs.getInt    ("windoff",  c.windDirOffsetDeg);
    c.windUnitMs       = prefs.getInt    ("windunit", c.windUnitMs);
    c.windEn           = prefs.getInt    ("winden",   c.windEn);
    c.rainEn           = prefs.getInt    ("rainen",   c.rainEn);
    c.rainPersist      = prefs.getInt    ("rainpersist", c.rainPersist);
    c.lipoEn           = prefs.getInt    ("lipoen",   c.lipoEn);
    c.debugMode        = prefs.getInt    ("debugmode", c.debugMode);
    c.cpuFreqMhz       = prefs.getInt    ("cpufreq",  c.cpuFreqMhz);
    c.payloadFormat    = prefs.getInt    ("payfmt",   c.payloadFormat);
    c.unitSystem       = prefs.getInt    ("unitsys",  c.unitSystem);
    c.rainMmPerTip     = prefs.getFloat  ("rainmmtip", c.rainMmPerTip);
    c.windMsPerPps     = prefs.getFloat  ("windmspp",  c.windMsPerPps);
    c.calTempDc        = prefs.getInt    ("caltemp",   c.calTempDc);
    c.calHumDpct       = prefs.getInt    ("calhum",    c.calHumDpct);
    c.calPressDhpa     = prefs.getInt    ("calpress",  c.calPressDhpa);
    c.calRainPermil    = prefs.getInt    ("calrain",   c.calRainPermil);
    c.calWindPermil    = prefs.getInt    ("calwind",   c.calWindPermil);
    c.sunMethod        = prefs.getInt    ("sunmethod", c.sunMethod);
    c.dispMode         = prefs.getInt    ("dispmode",  c.dispMode);
    c.dispMinutes      = prefs.getInt    ("dispmin",   c.dispMinutes);
    c.dispScreens      = prefs.getInt    ("dispscr",   c.dispScreens);
    c.dispRotateSec    = prefs.getInt    ("disprot",   c.dispRotateSec);
    c.dispContrast     = prefs.getInt    ("dispcon",   c.dispContrast);
    c.dispNightFrom    = prefs.getInt    ("dispnf",    c.dispNightFrom);
    c.dispFlip         = prefs.getInt    ("dispflip",  c.dispFlip);
    c.measureSec       = prefs.getInt    ("measec",    c.measureSec);
    c.dispBattery      = prefs.getInt    ("dispbat",   c.dispBattery);
    c.dispLed          = prefs.getInt    ("displed",   c.dispLed);
    {   // custom series (series_def.h)
        char s[480] = "";
        if (prefs.isKey("series")) prefs.getString("series", s, sizeof(s));
        seriesUnpack(s, c.series);
    }
    for (int i = 0; i < OLED_CUSTOM_COUNT; i++) {   // "dcus0", "dcus1", oled_custom.h
        char key[8], s[300] = "";
        snprintf(key, sizeof(key), "dcus%d", i);
        if (prefs.isKey(key)) prefs.getString(key, s, sizeof(s));
        oledCustomUnpack(s, c.dispCustom[i]);
    }
    c.dispNightTo      = prefs.getInt    ("dispnt",    c.dispNightTo);
    prefs.getString("rfcal", c.rfCal, sizeof(c.rfCal));
    prefs.getString("rfnames", c.rfNames, sizeof(c.rfNames));
    c.windDirSensorType = prefs.getInt   ("winddirtyp", c.windDirSensorType);
    c.windDirCalCount  = prefs.getInt    ("wdcalcnt",  c.windDirCalCount);
    prefs.getBytes("wdcaladc", c.windDirCalAdc, sizeof(c.windDirCalAdc));
    prefs.getBytes("wdcaldeg", c.windDirCalDeg, sizeof(c.windDirCalDeg));
    c.devEui           = prefs.getULong64("deveui",   0);
    c.joinEui          = prefs.getULong64("joineui",  0);
    prefs.getBytes("appkey", c.appKey, 16);
    String region      = prefs.getString("region", c.region);
    normalizeRegionInput(region.c_str(), c.region, sizeof(c.region));

    c.loraEn           = prefs.getInt    ("loraen",   c.loraEn);
    c.lowPowerEn       = prefs.getInt    ("lowpower", c.lowPowerEn);
    c.vanePowerSave    = prefs.getInt    ("vanesave", c.vanePowerSave);
    c.wifiEn           = prefs.getInt    ("wifien",   c.wifiEn);
    c.mqttEn           = prefs.getInt    ("mqtten",   c.mqttEn);
    c.webApiEn         = prefs.getInt    ("webapien", c.webApiEn);
    c.mqttCtrlEn       = prefs.getInt    ("mqttctrlen", c.mqttCtrlEn);
    prefs.getString("devname",    c.deviceName,   sizeof(c.deviceName));
    c.staticIpEn       = prefs.getInt    ("staticipen", c.staticIpEn);
    prefs.getString("staticip",   c.staticIp,      sizeof(c.staticIp));
    prefs.getString("staticgw",   c.staticGateway, sizeof(c.staticGateway));
    prefs.getString("staticsn",   c.staticSubnet,  sizeof(c.staticSubnet));
    prefs.getString("staticdns",  c.staticDns,     sizeof(c.staticDns));
    prefs.getString("mqtthost",   c.mqttHost,     sizeof(c.mqttHost));
    c.mqttPort         = prefs.getInt    ("mqttport", c.mqttPort);
    prefs.getString("mqttuser",   c.mqttUser,     sizeof(c.mqttUser));
    prefs.getString("mqttpass",   c.mqttPass,     sizeof(c.mqttPass));
    prefs.getString("mqtttopic",  c.mqttTopic,    sizeof(c.mqttTopic));
    c.mqttTlsMode      = prefs.getInt    ("mqtttls",  c.mqttTlsMode);
    prefs.getString("mqttcacert", c.mqttCaCert,   sizeof(c.mqttCaCert));
    c.loxEn            = prefs.getInt    ("loxen",    c.loxEn);
    prefs.getString("loxhost",    c.loxHost,      sizeof(c.loxHost));
    c.loxPort          = prefs.getInt    ("loxport",  c.loxPort);
    prefs.getString("loxprefix",  c.loxPrefix,    sizeof(c.loxPrefix));
    c.loxSec           = prefs.getInt    ("loxsec",   c.loxSec);
    prefs.getString("webadminpw", c.webAdminPass, sizeof(c.webAdminPass));

    prefs.getString("tz",         c.timeZone,     sizeof(c.timeZone));
    prefs.getString("ntpserver",  c.ntpServer,    sizeof(c.ntpServer));
    prefs.getString("lat",        c.latitude,     sizeof(c.latitude));
    prefs.getString("lon",        c.longitude,    sizeof(c.longitude));
    prefs.getString("awkuser",    c.awekasUser,   sizeof(c.awekasUser));
    prefs.getString("awkpass",    c.awekasPass,   sizeof(c.awekasPass));
    c.awekasIntervalSec  = prefs.getInt("awkint", c.awekasIntervalSec);
    prefs.getString("wuid",       c.wuStationId,  sizeof(c.wuStationId));
    prefs.getString("wukey",      c.wuStationKey, sizeof(c.wuStationKey));
    c.wuIntervalSec      = prefs.getInt("wuint",  c.wuIntervalSec);
    prefs.getString("wcid",       c.wcloudId,     sizeof(c.wcloudId));
    prefs.getString("wckey",      c.wcloudKey,    sizeof(c.wcloudKey));
    c.wcloudIntervalSec  = prefs.getInt("wcint",  c.wcloudIntervalSec);
    prefs.getString("wnpid",      c.wnetproId,    sizeof(c.wnetproId));
    prefs.getString("wnpkey",     c.wnetproKey,   sizeof(c.wnetproKey));
    c.wnetproIntervalSec = prefs.getInt("wnpint", c.wnetproIntervalSec);
    prefs.getString("windyid",    c.windyId,      sizeof(c.windyId));
    prefs.getString("windypw",    c.windyPass,    sizeof(c.windyPass));
    c.windyIntervalSec   = prefs.getInt("windyint", c.windyIntervalSec);
    prefs.getString("osemid",     c.osemBoxId,    sizeof(c.osemBoxId));
    prefs.getString("osemtok",    c.osemToken,    sizeof(c.osemToken));
    prefs.getString("osemmap",    c.osemSensors,  sizeof(c.osemSensors));
    c.osemIntervalSec    = prefs.getInt("osemint", c.osemIntervalSec);
    prefs.getString("wowid",      c.wowSiteId,    sizeof(c.wowSiteId));
    prefs.getString("wowpin",     c.wowPin,       sizeof(c.wowPin));
    c.wowIntervalSec     = prefs.getInt("wowint", c.wowIntervalSec);
    prefs.getString("cwopcall",   c.cwopCall,     sizeof(c.cwopCall));
    c.cwopPasscode       = prefs.getInt("cwoppass", c.cwopPasscode);
    c.cwopIntervalSec    = prefs.getInt("cwopint", c.cwopIntervalSec);
    c.updateCheckEn      = prefs.getBool("updchk", c.updateCheckEn);
    if (prefs.isKey("updurl")) prefs.getString("updurl", c.updateUrl, sizeof(c.updateUrl));
    // Old default (the project's internal Gitea, not reachable for everyone):
    // move nodes that still have it to the public GitHub releases list.
    if (!strcmp(c.updateUrl, "https://git.lb.franzenhof.com/api/v1/repos/esp32/universal-weather-node/tags?limit=20"))
        strlcpy(c.updateUrl, "https://api.github.com/repos/franzenhof/universal-weather-node/releases", sizeof(c.updateUrl));

    c.auxAinEn         = prefs.getInt    ("auxainen",   c.auxAinEn);
    c.auxAinScale      = prefs.getFloat  ("auxainscale", c.auxAinScale);
    c.auxPulseEn       = prefs.getInt    ("auxpulseen", c.auxPulseEn);
    c.wind2Mode        = prefs.getInt    ("wind2",      c.wind2Mode);
    c.wind2DirOffsetDeg = prefs.getInt   ("wind2off",   c.wind2DirOffsetDeg);
    c.heaterMode       = prefs.getInt    ("heatmode",   c.heaterMode);
    c.heaterDutyPct    = prefs.getInt    ("heatduty",   c.heaterDutyPct);
    c.heaterOnBelowDc  = prefs.getInt    ("heatbelow",  c.heaterOnBelowDc);
    c.heaterMinDutyPct = prefs.getInt    ("heatminduty", c.heaterMinDutyPct);
    c.heaterFullBelowDc = prefs.getInt   ("heatfull",   c.heaterFullBelowDc);
    c.fanMode          = prefs.getInt    ("fanmode",    c.fanMode);
    c.fanDutyPct       = prefs.getInt    ("fanduty",    c.fanDutyPct);
    c.fanOnAboveDc     = prefs.getInt    ("fanabove",   c.fanOnAboveDc);
    c.fanMinPvW        = prefs.getInt    ("fanminpv",   c.fanMinPvW);
    c.tachoEn          = prefs.getInt    ("tachoen",    c.tachoEn);
    c.tachoPpr         = prefs.getInt    ("tachoppr",   c.tachoPpr);
    c.veDirectEn       = prefs.getInt    ("vedirecten", c.veDirectEn);
    c.outMinBattDv     = prefs.getInt    ("outminbatt", c.outMinBattDv);
    c.uvEn             = prefs.getInt    ("uven", c.uvEn);
    c.uvAinScale       = prefs.getFloat  ("uvscale", c.uvAinScale);
    c.uvIndexEn        = prefs.getInt    ("uvindexen", c.uvIndexEn);
    c.uvCalibrated     = prefs.getInt    ("uvcal", c.uvCalibrated);
    c.uvIndexSlope     = prefs.getFloat  ("uvslope", c.uvIndexSlope);
    c.uvIndexOffset    = prefs.getFloat  ("uvoffset", c.uvIndexOffset);

    prefs.end();
    sanitizeConfig(c);
}

Config loadConfig()
{
    Config c;
    loadConfigInto(c);
    return c;
}

void saveConfig(const Config& c)
{
    Preferences prefs;
    if (!prefs.begin("config", false, "cfgdata")) {
        dbgSerial.println("Config save: FAILED to open NVS namespace 'config' in partition 'cfgdata' — nothing was saved.");
        return;
    }

    bool ok = true;
    ok &= checkWrite   ("provisioned", prefs.putInt    ("provisioned", c.provisioned), 4);
    ok &= checkWrite   ("pcbver",      prefs.putInt    ("pcbver",   c.pcbVersion), 4);
    ok &= checkWrite   ("board",       prefs.putInt    ("board",    c.boardSel), 4);
    ok &= checkWrite   ("altitude",    prefs.putInt    ("altitude", c.altitudeM), 4);
    ok &= checkWrite   ("uplinksec",   prefs.putInt    ("uplinksec", c.uplinkIntervalSec), 4);
    ok &= checkWrite   ("samplesec",   prefs.putInt    ("samplesec", c.sampleSec), 4);
    ok &= checkWrite   ("mqttsec",     prefs.putInt    ("mqttsec",   c.mqttIntervalSec), 4);
    ok &= checkWrite   ("lorawagg",    prefs.putInt    ("lorawagg",  c.loraWindAgg), 4);
    ok &= checkWrite   ("mqttwagg",    prefs.putInt    ("mqttwagg",  c.mqttWindAgg), 4);
    ok &= checkWrite   ("rfen",        prefs.putInt    ("rfen",      c.rfEn), 4);
    ok &= checkWrite   ("rfproto",     prefs.putInt    ("rfproto",   c.rfProto), 4);
    ok &= checkWriteStr("rfids",       prefs.putString ("rfids",     c.rfIds), c.rfIds);
    ok &= checkWriteStr("rfignore",    prefs.putString ("rfignore",  c.rfIgnore), c.rfIgnore);
    ok &= checkWrite   ("uilang",      prefs.putInt    ("uilang",    c.uiLang), 4);
    ok &= checkWrite   ("setupdone",   prefs.putInt    ("setupdone", c.setupDone), 4);
    ok &= checkWrite   ("rfonly",      prefs.putInt    ("rfonly",    c.rfOnlyMine), 4);
    ok &= checkWrite   ("dvid",        prefs.putInt    ("dvid",      c.davisTxId), 4);
    ok &= checkWrite   ("dvoff",       prefs.putInt    ("dvoff",     c.davisOffsetHz), 4);
    ok &= checkWrite   ("dvwin",       prefs.putInt    ("dvwin",     c.davisWindowMs), 4);
    ok &= checkWrite   ("dvguard",     prefs.putInt    ("dvguard",   c.davisGuardMs), 4);
    ok &= checkWrite   ("dvrfilt",     prefs.putInt    ("dvrfilt",   c.davisRainFilter), 4);
    ok &= checkWrite   ("dvrpkt",      prefs.putInt    ("dvrpkt",    c.davisRainMaxPkt), 4);
    ok &= checkWrite   ("dvrmin",      prefs.putInt    ("dvrmin",    c.davisRainMaxMin), 4);
    ok &= checkWrite   ("dvbucket",    prefs.putInt    ("dvbucket",  c.davisBucket), 4);
    ok &= checkWrite   ("dvmodel",     prefs.putInt    ("dvmodel",   c.davisModel), 4);
    ok &= checkWrite   ("dvsniff",     prefs.putInt    ("dvsniff",   c.davisSniffer), 4);
    ok &= checkWrite   ("dvraw",       prefs.putInt    ("dvraw",     c.davisRaw), 4);
    ok &= checkWrite   ("dvrawch",     prefs.putInt    ("dvrawch",   c.davisRawCh), 4);
    ok &= checkWrite   ("dvscan",      prefs.putInt    ("dvscan",    c.davisScanKhz), 4);
    ok &= checkWrite   ("dvafc",       prefs.putInt    ("dvafc",     c.davisAfc), 4);
    ok &= checkWrite   ("dvtable",     prefs.putInt    ("dvtable",   c.davisTable), 4);
    ok &= checkWrite   ("dvbw",        prefs.putInt    ("dvbw",      c.davisRxBw), 4);
    for (int i = 0; i < (int)sizeof(c.srcSel); i++) {
        char key[8]; snprintf(key, sizeof(key), "src%d", i);
        ok &= checkWrite(key, prefs.putInt(key, c.srcSel[i]), 4);
        char kid[8]; snprintf(kid, sizeof(kid), "srcid%d", i);
        ok &= checkWrite(kid, prefs.putUInt(kid, c.srcId[i]), 4);
    }
    ok &= checkWrite   ("windoff",     prefs.putInt    ("windoff",  c.windDirOffsetDeg), 4);
    ok &= checkWrite   ("windunit",    prefs.putInt    ("windunit", c.windUnitMs), 4);
    ok &= checkWrite   ("winden",      prefs.putInt    ("winden",   c.windEn), 4);
    ok &= checkWrite   ("rainen",      prefs.putInt    ("rainen",   c.rainEn), 4);
    ok &= checkWrite   ("rainpersist", prefs.putInt    ("rainpersist", c.rainPersist), 4);
    ok &= checkWrite   ("lipoen",      prefs.putInt    ("lipoen",   c.lipoEn), 4);
    ok &= checkWrite   ("debugmode",   prefs.putInt    ("debugmode", c.debugMode), 4);
    ok &= checkWrite   ("cpufreq",     prefs.putInt    ("cpufreq",  c.cpuFreqMhz), 4);
    ok &= checkWrite   ("payfmt",      prefs.putInt    ("payfmt",   c.payloadFormat), 4);
    ok &= checkWrite   ("unitsys",     prefs.putInt    ("unitsys",  c.unitSystem), 4);
    ok &= checkWrite   ("rainmmtip",   prefs.putFloat  ("rainmmtip", c.rainMmPerTip), 4);
    ok &= checkWrite   ("windmspp",    prefs.putFloat  ("windmspp",  c.windMsPerPps), 4);
    ok &= checkWrite   ("caltemp",     prefs.putInt    ("caltemp",   c.calTempDc), 4);
    ok &= checkWrite   ("calhum",      prefs.putInt    ("calhum",    c.calHumDpct), 4);
    ok &= checkWrite   ("calpress",    prefs.putInt    ("calpress",  c.calPressDhpa), 4);
    ok &= checkWrite   ("calrain",     prefs.putInt    ("calrain",   c.calRainPermil), 4);
    ok &= checkWrite   ("calwind",     prefs.putInt    ("calwind",   c.calWindPermil), 4);
    ok &= checkWrite   ("sunmethod",   prefs.putInt    ("sunmethod", c.sunMethod), 4);
    ok &= checkWrite   ("dispmode",    prefs.putInt    ("dispmode",  c.dispMode), 4);
    ok &= checkWrite   ("dispmin",     prefs.putInt    ("dispmin",   c.dispMinutes), 4);
    ok &= checkWrite   ("dispscr",     prefs.putInt    ("dispscr",   c.dispScreens), 4);
    ok &= checkWrite   ("disprot",     prefs.putInt    ("disprot",   c.dispRotateSec), 4);
    ok &= checkWrite   ("dispcon",     prefs.putInt    ("dispcon",   c.dispContrast), 4);
    ok &= checkWrite   ("dispnf",      prefs.putInt    ("dispnf",    c.dispNightFrom), 4);
    ok &= checkWrite   ("dispflip",    prefs.putInt    ("dispflip",  c.dispFlip), 4);
    ok &= checkWrite   ("measec",      prefs.putInt    ("measec",    c.measureSec), 4);
    ok &= checkWrite   ("dispbat",     prefs.putInt    ("dispbat",   c.dispBattery), 4);
    ok &= checkWrite   ("displed",     prefs.putInt    ("displed",   c.dispLed), 4);
    {
        char s[480];
        seriesPack(c.series, s, sizeof(s));
        ok &= checkWriteStr("series", prefs.putString("series", s), s);
    }
    for (int i = 0; i < OLED_CUSTOM_COUNT; i++) {
        char key[8], s[300];
        snprintf(key, sizeof(key), "dcus%d", i);
        oledCustomPack(c.dispCustom[i], s, sizeof(s));
        ok &= checkWriteStr(key, prefs.putString(key, s), s);
    }
    ok &= checkWrite   ("dispnt",      prefs.putInt    ("dispnt",    c.dispNightTo), 4);
    ok &= checkWriteStr("rfcal",       prefs.putString ("rfcal",     c.rfCal), c.rfCal);
    ok &= checkWriteStr("rfnames",     prefs.putString ("rfnames",   c.rfNames), c.rfNames);
    ok &= checkWrite   ("winddirtyp",  prefs.putInt    ("winddirtyp", c.windDirSensorType), 4);
    ok &= checkWrite   ("wdcalcnt",    prefs.putInt    ("wdcalcnt",  c.windDirCalCount), 4);
    ok &= checkWrite   ("wdcaladc",    prefs.putBytes  ("wdcaladc",  c.windDirCalAdc, sizeof(c.windDirCalAdc)), sizeof(c.windDirCalAdc));
    ok &= checkWrite   ("wdcaldeg",    prefs.putBytes  ("wdcaldeg",  c.windDirCalDeg, sizeof(c.windDirCalDeg)), sizeof(c.windDirCalDeg));
    ok &= checkWrite   ("deveui",      prefs.putULong64("deveui",   c.devEui), 8);
    ok &= checkWrite   ("joineui",     prefs.putULong64("joineui",  c.joinEui), 8);
    ok &= checkWrite   ("appkey",      prefs.putBytes  ("appkey",   c.appKey, 16), 16);
    ok &= checkWriteStr("region",      prefs.putString ("region",   c.region), c.region);

    ok &= checkWrite   ("loraen",      prefs.putInt    ("loraen",   c.loraEn), 4);
    ok &= checkWrite   ("lowpower",    prefs.putInt    ("lowpower", c.lowPowerEn), 4);
    ok &= checkWrite   ("vanesave",    prefs.putInt    ("vanesave", c.vanePowerSave), 4);
    ok &= checkWrite   ("wifien",      prefs.putInt    ("wifien",   c.wifiEn), 4);
    ok &= checkWrite   ("mqtten",      prefs.putInt    ("mqtten",   c.mqttEn), 4);
    ok &= checkWrite   ("webapien",    prefs.putInt    ("webapien", c.webApiEn), 4);
    ok &= checkWrite   ("mqttctrlen",  prefs.putInt    ("mqttctrlen", c.mqttCtrlEn), 4);
    ok &= checkWriteStr("devname",     prefs.putString ("devname",    c.deviceName), c.deviceName);
    ok &= checkWrite   ("staticipen",  prefs.putInt    ("staticipen", c.staticIpEn), 4);
    ok &= checkWriteStr("staticip",    prefs.putString ("staticip",   c.staticIp), c.staticIp);
    ok &= checkWriteStr("staticgw",    prefs.putString ("staticgw",   c.staticGateway), c.staticGateway);
    ok &= checkWriteStr("staticsn",    prefs.putString ("staticsn",   c.staticSubnet), c.staticSubnet);
    ok &= checkWriteStr("staticdns",   prefs.putString ("staticdns",  c.staticDns), c.staticDns);
    ok &= checkWriteStr("mqtthost",    prefs.putString ("mqtthost",   c.mqttHost), c.mqttHost);
    ok &= checkWrite   ("mqttport",    prefs.putInt    ("mqttport", c.mqttPort), 4);
    ok &= checkWriteStr("mqttuser",    prefs.putString ("mqttuser",   c.mqttUser), c.mqttUser);
    ok &= checkWriteStr("mqttpass",    prefs.putString ("mqttpass",   c.mqttPass), c.mqttPass);
    ok &= checkWriteStr("mqtttopic",   prefs.putString ("mqtttopic",  c.mqttTopic), c.mqttTopic);
    ok &= checkWrite   ("mqtttls",     prefs.putInt    ("mqtttls",  c.mqttTlsMode), 4);
    ok &= checkWriteStr("mqttcacert",  prefs.putString ("mqttcacert", c.mqttCaCert), c.mqttCaCert);
    ok &= checkWrite   ("loxen",       prefs.putInt    ("loxen",    c.loxEn), 4);
    ok &= checkWriteStr("loxhost",     prefs.putString ("loxhost",  c.loxHost), c.loxHost);
    ok &= checkWrite   ("loxport",     prefs.putInt    ("loxport",  c.loxPort), 4);
    ok &= checkWriteStr("loxprefix",   prefs.putString ("loxprefix", c.loxPrefix), c.loxPrefix);
    ok &= checkWrite   ("loxsec",      prefs.putInt    ("loxsec",   c.loxSec), 4);
    ok &= checkWriteStr("webadminpw",  prefs.putString ("webadminpw", c.webAdminPass), c.webAdminPass);

    ok &= checkWriteStr("tz",          prefs.putString ("tz",         c.timeZone), c.timeZone);
    ok &= checkWriteStr("ntpserver",   prefs.putString ("ntpserver",  c.ntpServer), c.ntpServer);
    ok &= checkWriteStr("lat",         prefs.putString ("lat",        c.latitude), c.latitude);
    ok &= checkWriteStr("lon",         prefs.putString ("lon",        c.longitude), c.longitude);
    ok &= checkWriteStr("awkuser",     prefs.putString ("awkuser",    c.awekasUser), c.awekasUser);
    ok &= checkWriteStr("awkpass",     prefs.putString ("awkpass",    c.awekasPass), c.awekasPass);
    ok &= checkWrite   ("awkint",      prefs.putInt    ("awkint",     c.awekasIntervalSec), 4);
    ok &= checkWriteStr("wuid",        prefs.putString ("wuid",       c.wuStationId), c.wuStationId);
    ok &= checkWriteStr("wukey",       prefs.putString ("wukey",      c.wuStationKey), c.wuStationKey);
    ok &= checkWrite   ("wuint",       prefs.putInt    ("wuint",      c.wuIntervalSec), 4);
    ok &= checkWriteStr("wcid",        prefs.putString ("wcid",       c.wcloudId), c.wcloudId);
    ok &= checkWriteStr("wckey",       prefs.putString ("wckey",      c.wcloudKey), c.wcloudKey);
    ok &= checkWrite   ("wcint",       prefs.putInt    ("wcint",      c.wcloudIntervalSec), 4);
    ok &= checkWriteStr("wnpid",       prefs.putString ("wnpid",      c.wnetproId), c.wnetproId);
    ok &= checkWriteStr("wnpkey",      prefs.putString ("wnpkey",     c.wnetproKey), c.wnetproKey);
    ok &= checkWrite   ("wnpint",      prefs.putInt    ("wnpint",     c.wnetproIntervalSec), 4);
    ok &= checkWriteStr("windyid",     prefs.putString ("windyid",    c.windyId), c.windyId);
    ok &= checkWriteStr("windypw",     prefs.putString ("windypw",    c.windyPass), c.windyPass);
    ok &= checkWrite   ("windyint",    prefs.putInt    ("windyint",   c.windyIntervalSec), 4);
    ok &= checkWriteStr("osemid",      prefs.putString ("osemid",     c.osemBoxId), c.osemBoxId);
    ok &= checkWriteStr("osemtok",     prefs.putString ("osemtok",    c.osemToken), c.osemToken);
    ok &= checkWriteStr("osemmap",     prefs.putString ("osemmap",    c.osemSensors), c.osemSensors);
    ok &= checkWrite   ("osemint",     prefs.putInt    ("osemint",    c.osemIntervalSec), 4);
    ok &= checkWriteStr("wowid",       prefs.putString ("wowid",      c.wowSiteId), c.wowSiteId);
    ok &= checkWriteStr("wowpin",      prefs.putString ("wowpin",     c.wowPin), c.wowPin);
    ok &= checkWrite   ("wowint",      prefs.putInt    ("wowint",     c.wowIntervalSec), 4);
    ok &= checkWriteStr("cwopcall",    prefs.putString ("cwopcall",   c.cwopCall), c.cwopCall);
    ok &= checkWrite   ("cwoppass",    prefs.putInt    ("cwoppass",   c.cwopPasscode), 4);
    ok &= checkWrite   ("cwopint",     prefs.putInt    ("cwopint",    c.cwopIntervalSec), 4);
    ok &= checkWrite   ("updchk",      prefs.putBool   ("updchk",     c.updateCheckEn), 1);
    ok &= checkWriteStr("updurl",      prefs.putString ("updurl",     c.updateUrl), c.updateUrl);

    ok &= checkWrite   ("auxainen",    prefs.putInt    ("auxainen",   c.auxAinEn), 4);
    ok &= checkWrite   ("auxainscale", prefs.putFloat  ("auxainscale", c.auxAinScale), 4);
    ok &= checkWrite   ("auxpulseen",  prefs.putInt    ("auxpulseen", c.auxPulseEn), 4);
    ok &= checkWrite   ("wind2",       prefs.putInt    ("wind2",      c.wind2Mode), 4);
    ok &= checkWrite   ("wind2off",    prefs.putInt    ("wind2off",   c.wind2DirOffsetDeg), 4);
    ok &= checkWrite   ("heatmode",    prefs.putInt    ("heatmode",   c.heaterMode), 4);
    ok &= checkWrite   ("heatduty",    prefs.putInt    ("heatduty",   c.heaterDutyPct), 4);
    ok &= checkWrite   ("heatbelow",   prefs.putInt    ("heatbelow",  c.heaterOnBelowDc), 4);
    ok &= checkWrite   ("heatminduty", prefs.putInt    ("heatminduty", c.heaterMinDutyPct), 4);
    ok &= checkWrite   ("heatfull",    prefs.putInt    ("heatfull",   c.heaterFullBelowDc), 4);
    ok &= checkWrite   ("fanmode",     prefs.putInt    ("fanmode",    c.fanMode), 4);
    ok &= checkWrite   ("fanduty",     prefs.putInt    ("fanduty",    c.fanDutyPct), 4);
    ok &= checkWrite   ("fanabove",    prefs.putInt    ("fanabove",   c.fanOnAboveDc), 4);
    ok &= checkWrite   ("fanminpv",    prefs.putInt    ("fanminpv",   c.fanMinPvW), 4);
    ok &= checkWrite   ("tachoen",     prefs.putInt    ("tachoen",    c.tachoEn), 4);
    ok &= checkWrite   ("tachoppr",    prefs.putInt    ("tachoppr",   c.tachoPpr), 4);
    ok &= checkWrite   ("vedirecten",  prefs.putInt    ("vedirecten", c.veDirectEn), 4);
    ok &= checkWrite   ("outminbatt",  prefs.putInt    ("outminbatt", c.outMinBattDv), 4);
    ok &= checkWrite   ("uven",        prefs.putInt    ("uven", c.uvEn), 4);
    ok &= checkWrite   ("uvscale",     prefs.putFloat  ("uvscale", c.uvAinScale), 4);
    ok &= checkWrite   ("uvindexen",   prefs.putInt    ("uvindexen", c.uvIndexEn), 4);
    ok &= checkWrite   ("uvcal",       prefs.putInt    ("uvcal", c.uvCalibrated), 4);
    ok &= checkWrite   ("uvslope",     prefs.putFloat  ("uvslope", c.uvIndexSlope), 4);
    ok &= checkWrite   ("uvoffset",    prefs.putFloat  ("uvoffset", c.uvIndexOffset), 4);
    prefs.end();

    // Non-secret summary only — never log AppKey/WiFi/MQTT credentials/CA cert.
    dbgSerial.printf("Config saved%s: loraEn=%d region=%s wifiEn=%d mqttEn=%d webApiEn=%d "
                      "staticIpEn=%d uplinkSec=%d sampleSec=%d\n",
                      ok ? "" : " (WITH ERRORS, see above)",
                      c.loraEn, c.region, c.wifiEn, c.mqttEn, c.webApiEn,
                      c.staticIpEn, c.uplinkIntervalSec, c.sampleSec);
}

void saveRfProto(int proto)
{
    Preferences prefs;
    if (!prefs.begin("config", false, "cfgdata")) {
        dbgSerial.println("Config save: FAILED to open NVS namespace 'config' - wireless sensor type not saved.");
        return;
    }
    checkWrite("rfproto", prefs.putInt("rfproto", proto), 4);
    prefs.end();
}

// IDs whose Home Assistant configs were published (ha_rf.h): written only
// when that set changes. Not part of Config (no setting), but in the same
// namespace, so a factory reset clears it too.
void loadHaRfIds(char* out, size_t len)
{
    if (!len) return;
    out[0] = '\0';
    Preferences prefs;
    if (!prefs.begin("config", true, "cfgdata")) return;
    if (prefs.isKey("harfids")) prefs.getString("harfids", out, len);
    prefs.end();
}

void saveHaRfIds(const char* ids)
{
    Preferences prefs;
    if (!prefs.begin("config", false, "cfgdata")) {
        dbgSerial.println("Config save: FAILED to open NVS namespace 'config' - HA wireless sensor list not saved.");
        return;
    }
    checkWriteStr("harfids", prefs.putString("harfids", ids), ids);
    prefs.end();
}
