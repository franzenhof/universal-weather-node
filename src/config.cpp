#include "config.h"
#include "serial_log.h"
#include <Preferences.h>

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

} // namespace

Config loadConfig()
{
    Preferences prefs;
    Config c;
    // Own dedicated NVS-formatted partition (see partitions.csv) rather than
    // the stock "nvs" partition — keeps our config fully isolated from
    // WiFiManager's/ESP-IDF's own WiFi credential storage and from any other
    // firmware project that happens to share the "config" namespace name.
    //
    // On first boot this partition doesn't have a "config" namespace yet, so
    // begin() returns false. Return the all-defaults Config (devEui == 0),
    // which causes setup() to open the Wi-Fi configuration portal.
    if (!prefs.begin("config", true, "cfgdata")) {
        return c;
    }
    c.provisioned      = prefs.getInt    ("provisioned", c.provisioned);
    c.pcbVersion       = prefs.getInt    ("pcbver",   c.pcbVersion);
    c.altitudeM        = prefs.getInt    ("altitude", c.altitudeM);
    c.uplinkIntervalSec = prefs.getInt   ("uplinksec", prefs.getInt("delay", c.uplinkIntervalSec));
    c.sampleSec        = prefs.getInt    ("samplesec", c.sampleSec);
    c.windDirOffsetDeg = prefs.getInt    ("windoff",  c.windDirOffsetDeg);
    c.windUnitMs       = prefs.getInt    ("windunit", c.windUnitMs);
    c.windEn           = prefs.getInt    ("winden",   c.windEn);
    c.rainEn           = prefs.getInt    ("rainen",   c.rainEn);
    c.lipoEn           = prefs.getInt    ("lipoen",   c.lipoEn);
    c.debugMode        = prefs.getInt    ("debugmode", c.debugMode);
    c.cpuFreqMhz       = prefs.getInt    ("cpufreq",  c.cpuFreqMhz);
    c.payloadFormat    = prefs.getInt    ("payfmt",   c.payloadFormat);
    c.unitSystem       = prefs.getInt    ("unitsys",  c.unitSystem);
    c.rainMmPerTip     = prefs.getFloat  ("rainmmtip", c.rainMmPerTip);
    c.windMsPerPps     = prefs.getFloat  ("windmspp",  c.windMsPerPps);
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
    ok &= checkWrite   ("altitude",    prefs.putInt    ("altitude", c.altitudeM), 4);
    ok &= checkWrite   ("uplinksec",   prefs.putInt    ("uplinksec", c.uplinkIntervalSec), 4);
    ok &= checkWrite   ("samplesec",   prefs.putInt    ("samplesec", c.sampleSec), 4);
    ok &= checkWrite   ("windoff",     prefs.putInt    ("windoff",  c.windDirOffsetDeg), 4);
    ok &= checkWrite   ("windunit",    prefs.putInt    ("windunit", c.windUnitMs), 4);
    ok &= checkWrite   ("winden",      prefs.putInt    ("winden",   c.windEn), 4);
    ok &= checkWrite   ("rainen",      prefs.putInt    ("rainen",   c.rainEn), 4);
    ok &= checkWrite   ("lipoen",      prefs.putInt    ("lipoen",   c.lipoEn), 4);
    ok &= checkWrite   ("debugmode",   prefs.putInt    ("debugmode", c.debugMode), 4);
    ok &= checkWrite   ("cpufreq",     prefs.putInt    ("cpufreq",  c.cpuFreqMhz), 4);
    ok &= checkWrite   ("payfmt",      prefs.putInt    ("payfmt",   c.payloadFormat), 4);
    ok &= checkWrite   ("unitsys",     prefs.putInt    ("unitsys",  c.unitSystem), 4);
    ok &= checkWrite   ("rainmmtip",   prefs.putFloat  ("rainmmtip", c.rainMmPerTip), 4);
    ok &= checkWrite   ("windmspp",    prefs.putFloat  ("windmspp",  c.windMsPerPps), 4);
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
