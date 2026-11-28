#include "config.h"
#include "ha_groups.h"
#include "series.h"
#include "oled_screens.h"
#include "sources.h"
#include "ha_rf.h"
#include "loxone.h"
#include "wind_window.h"
#include "serial_log.h"
#include "lsensors.h"
#include "gnss_logic.h"
#include <atomic>
#include "storage_layout.h"
#include "vs_store.h"
#include "storage_write.h"
#include <Preferences.h>
#include <esp_random.h>
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
    // Key set version (storage_layout.h): an older one is converted first by
    // the migration hook; a newer one (downgrade) is read as far as the keys
    // are known - saving writes this firmware's key set and version.
    if (prefs.isKey("cfgver")) {
        const int stored = prefs.getUChar("cfgver", STORED_CONFIG_VERSION);
        const StoredVersionAction a = storedVersionAction((uint8_t)stored, STORED_CONFIG_VERSION);
        if (a == STORED_MIGRATE) {
            prefs.end();
            storageMigrateConfig((uint8_t)stored);
            if (!prefs.begin("config", true, "cfgdata")) return;
        } else if (a == STORED_NEWER) {
            dbgSerial.printf("Config: stored by a newer firmware (key set %d, this one %d) - unknown keys are ignored.\n",
                             stored, (int)STORED_CONFIG_VERSION);
        }
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
    c.altitudeM        = prefs.getInt    ("altitude", c.altitudeM);
    c.uplinkIntervalSec = prefs.getInt   ("uplinksec", prefs.getInt("delay", c.uplinkIntervalSec));
    c.sampleSec        = windSampleSecNormalized(prefs.getInt("samplesec", c.sampleSec));
    c.mqttIntervalSec  = prefs.getInt    ("mqttsec",   c.mqttIntervalSec);
    c.loraWindAgg      = prefs.getInt    ("lorawagg",  c.loraWindAgg);
    c.mqttWindAgg      = prefs.getInt    ("mqttwagg",  c.mqttWindAgg);
    c.rfEn             = prefs.getInt    ("rfen",      c.rfEn);
    c.rfProto          = prefs.getInt    ("rfproto",   c.rfProto);
    prefs.getString("rfignore", c.rfIgnore, sizeof(c.rfIgnore));
    c.uiLang           = prefs.getInt    ("uilang",    c.uiLang);
    c.setupDone        = prefs.isKey("setupdone") ? prefs.getInt("setupdone", 0) : (prefs.isKey("deveui") ? 1 : 0);
    if (c.uiLang < 0 || c.uiLang > 5) c.uiLang = 0;
    c.rfOnlyMine       = prefs.getInt    ("rfonly",    c.rfOnlyMine);
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
        char kid[10]; snprintf(kid, sizeof(kid), "srciid%d", i);
        if (prefs.isKey(kid)) prefs.getString(kid, c.sourceInternalId[i], sizeof(c.sourceInternalId[i]));
        if (!lsIsInternalId(c.sourceInternalId[i])) c.sourceInternalId[i][0] = '\0';
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
    c.saverMode        = prefs.getInt    ("svmode",    c.saverMode);
    c.saverMin         = prefs.getInt    ("svmin",     c.saverMin);
    c.saverSec         = prefs.getInt    ("svsec",     c.saverSec);
    {   // network sensors (net_sensor.h): [version][the slots] (storage_layout.h)
        static_assert(1 + sizeof(c.net) <= STORED_BLOB_READ_MAX, "netsens beyond the loaders' read ceiling");
        const size_t n = prefs.isKey("netsens") ? prefs.getBytesLength("netsens") : 0;
        if (storedBlobLengthReadable(n)) {   // also longer than today's slots: an older layout for the hook
            std::unique_ptr<uint8_t[]> b(new (std::nothrow) uint8_t[n]), scratch(new (std::nothrow) uint8_t[1 + sizeof(c.net)]);
            const uint8_t* p = b && scratch && prefs.getBytes("netsens", b.get(), n) == n
                ? storedBlobLoad(SF_NETWORK_SENSORS, b.get(), n, STORED_NETWORK_SENSORS_VERSION, sizeof(c.net), scratch.get(), 1 + sizeof(c.net))
                : nullptr;
            if (p) memcpy(c.net, p, sizeof(c.net));
        }
    }
    for (NetSlotCfg& n : c.net) netSanitize(n);
    for (int i = 0; i < OLED_CUSTOM_COUNT; i++) {   // "dcus0", "dcus1", oled_custom.h
        char key[8], s[400] = "";   // layout + title + 5 lines
        snprintf(key, sizeof(key), "dcus%d", i);
        if (prefs.isKey(key)) prefs.getString(key, s, sizeof(s));
        oledCustomUnpack(s, c.dispCustom[i]);
    }
    c.dispNightTo      = prefs.getInt    ("dispnt",    c.dispNightTo);
    prefs.getString("rfcal", c.rfCal, sizeof(c.rfCal));
    prefs.getString("rfnames", c.rfNames, sizeof(c.rfNames));
    prefs.getString("rfminrssi", c.rfMinRssi, sizeof(c.rfMinRssi));
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
    c.httpsEn          = prefs.getInt    ("httpsen",  c.httpsEn);
    c.httpPortMode     = prefs.getInt    ("httpport", c.httpPortMode);
    prefs.getString("devname",    c.deviceName,   sizeof(c.deviceName));
    c.staticIpEn       = prefs.getInt    ("staticipen", c.staticIpEn);
    prefs.getString("staticip",   c.staticIp,      sizeof(c.staticIp));
    prefs.getString("staticgw",   c.staticGateway, sizeof(c.staticGateway));
    prefs.getString("staticsn",   c.staticSubnet,  sizeof(c.staticSubnet));
    prefs.getString("staticdns",  c.staticDns,     sizeof(c.staticDns));
    c.apMode           = prefs.getInt    ("apmode",   c.apMode);
    prefs.getString("apssid",     c.apSsid,        sizeof(c.apSsid));
    prefs.getString("appass",     c.apPass,        sizeof(c.apPass));
    c.apChannel        = prefs.getInt    ("apchan",   c.apChannel);
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
    c.gnssEn           = prefs.getInt    ("gnssen",   c.gnssEn);
    c.gnssBaud         = prefs.getInt    ("gnssbaud", c.gnssBaud);
    c.gnssTime         = prefs.getInt    ("gnsstime", c.gnssTime);
    c.gnssPos          = prefs.getInt    ("gnsspos",  c.gnssPos);
    c.gnssAlt          = prefs.getInt    ("gnssalt",  c.gnssAlt);
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
    c.uiFlashEn          = prefs.getBool("uiflash", c.uiFlashEn);
    c.uiAgeEn            = prefs.getBool("uiage",   c.uiAgeEn);
    if (prefs.isKey("updurl")) prefs.getString("updurl", c.updateUrl, sizeof(c.updateUrl));
    // Old default (the project's internal Gitea, not reachable for everyone):
    // move nodes that still have it to the public GitHub releases list.
    if (!strcmp(c.updateUrl, "https://git.lb.franzenhof.com/api/v1/repos/esp32/universal-weather-node/tags?limit=20"))
        strlcpy(c.updateUrl, "https://api.github.com/repos/franzenhof/universal-weather-node/releases", sizeof(c.updateUrl));

    c.influxEn           = prefs.getInt   ("influxen",  c.influxEn);
    prefs.getString("influxurl",  c.influxUrl,         sizeof(c.influxUrl));
    prefs.getString("influxtok",  c.influxToken,       sizeof(c.influxToken));
    if (prefs.isKey("influxmeas")) prefs.getString("influxmeas", c.influxMeasurement, sizeof(c.influxMeasurement));
    c.influxIntervalSec  = prefs.getInt   ("influxint", c.influxIntervalSec);

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

static bool lsSave(Preferences& prefs);
static bool seriesSave(Preferences& prefs);
static bool vsSettingsSave(Preferences& prefs);

namespace {
std::atomic<bool> heldForRestart{false};
}

void configHoldForRestart() { heldForRestart.store(true); }
bool configHeldForRestart() { return heldForRestart.load(); }

void saveConfig(const Config& c)
{
    if (heldForRestart.load()) {
        dbgSerial.println("Config save skipped: settings saved for the pending restart stay as they are.");
        return;
    }
    StorageWrite w("config");   // storage_write.h: refused once a reset sealed the storage
    if (!w.ok()) {
        dbgSerial.println(storageSealed() ? "Config save: storage sealed by a reset - nothing was saved."
                                          : "Config save: FAILED to open NVS namespace 'config' in partition 'cfgdata' — nothing was saved.");
        return;
    }
    const bool ok = saveConfigKeys(w.prefs(), c);

    // Non-secret summary only — never log AppKey/WiFi/MQTT credentials/CA cert.
    dbgSerial.printf("Config saved%s: loraEn=%d region=%s wifiEn=%d mqttEn=%d webApiEn=%d "
                      "staticIpEn=%d uplinkSec=%d sampleSec=%d\n",
                      ok ? "" : " (WITH ERRORS, see above)",
                      c.loraEn, c.region, c.wifiEn, c.mqttEn, c.webApiEn,
                      c.staticIpEn, c.uplinkIntervalSec, c.sampleSec);
}

bool saveConfigKeys(Preferences& prefs, const Config& c)
{
    bool ok = true;
    ok &= checkWrite   ("provisioned", prefs.putInt    ("provisioned", c.provisioned), 4);
    ok &= checkWrite   ("altitude",    prefs.putInt    ("altitude", c.altitudeM), 4);
    ok &= checkWrite   ("uplinksec",   prefs.putInt    ("uplinksec", c.uplinkIntervalSec), 4);
    ok &= checkWrite   ("samplesec",   prefs.putInt    ("samplesec", c.sampleSec), 4);
    ok &= checkWrite   ("mqttsec",     prefs.putInt    ("mqttsec",   c.mqttIntervalSec), 4);
    ok &= checkWrite   ("lorawagg",    prefs.putInt    ("lorawagg",  c.loraWindAgg), 4);
    ok &= checkWrite   ("mqttwagg",    prefs.putInt    ("mqttwagg",  c.mqttWindAgg), 4);
    ok &= checkWrite   ("rfen",        prefs.putInt    ("rfen",      c.rfEn), 4);
    ok &= checkWrite   ("rfproto",     prefs.putInt    ("rfproto",   c.rfProto), 4);
    if (prefs.isKey("rfids")) prefs.remove("rfids");   // the ★ list before A5c (not converted: gLogicalSensors starts empty)
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
        char kid[10]; snprintf(kid, sizeof(kid), "srciid%d", i);
        ok &= checkWriteStr(kid, prefs.putString(kid, c.sourceInternalId[i]), c.sourceInternalId[i]);
        snprintf(kid, sizeof(kid), "srcid%d", i);   // radio-ID binding before A5c (not converted)
        if (prefs.isKey(kid)) prefs.remove(kid);
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
    ok &= checkWrite   ("svmode",      prefs.putInt    ("svmode",    c.saverMode), 4);
    ok &= checkWrite   ("svmin",       prefs.putInt    ("svmin",     c.saverMin), 4);
    ok &= checkWrite   ("svsec",       prefs.putInt    ("svsec",     c.saverSec), 4);
    {
        std::unique_ptr<uint8_t[]> b(new (std::nothrow) uint8_t[1 + sizeof(c.net)]);
        if (b) {
            b[0] = STORED_NETWORK_SENSORS_VERSION;
            memcpy(b.get() + 1, c.net, sizeof(c.net));
            ok &= checkWrite("netsens", prefs.putBytes("netsens", b.get(), 1 + sizeof(c.net)), 1 + sizeof(c.net));
        } else ok = false;
    }
    for (int i = 0; i < OLED_CUSTOM_COUNT; i++) {
        char key[8], s[400];
        snprintf(key, sizeof(key), "dcus%d", i);
        oledCustomPack(c.dispCustom[i], s, sizeof(s));
        ok &= checkWriteStr(key, prefs.putString(key, s), s);
    }
    ok &= checkWrite   ("dispnt",      prefs.putInt    ("dispnt",    c.dispNightTo), 4);
    ok &= checkWriteStr("rfcal",       prefs.putString ("rfcal",     c.rfCal), c.rfCal);
    ok &= checkWriteStr("rfnames",     prefs.putString ("rfnames",   c.rfNames), c.rfNames);
    ok &= checkWriteStr("rfminrssi",   prefs.putString ("rfminrssi", c.rfMinRssi), c.rfMinRssi);
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
    ok &= checkWrite   ("httpsen",     prefs.putInt    ("httpsen",  c.httpsEn), 4);
    ok &= checkWrite   ("httpport",    prefs.putInt    ("httpport", c.httpPortMode), 4);
    ok &= checkWriteStr("devname",     prefs.putString ("devname",    c.deviceName), c.deviceName);
    ok &= checkWrite   ("staticipen",  prefs.putInt    ("staticipen", c.staticIpEn), 4);
    ok &= checkWriteStr("staticip",    prefs.putString ("staticip",   c.staticIp), c.staticIp);
    ok &= checkWriteStr("staticgw",    prefs.putString ("staticgw",   c.staticGateway), c.staticGateway);
    ok &= checkWriteStr("staticsn",    prefs.putString ("staticsn",   c.staticSubnet), c.staticSubnet);
    ok &= checkWriteStr("staticdns",   prefs.putString ("staticdns",  c.staticDns), c.staticDns);
    ok &= checkWrite   ("apmode",      prefs.putInt    ("apmode",   c.apMode), 4);
    ok &= checkWriteStr("apssid",      prefs.putString ("apssid",     c.apSsid), c.apSsid);
    ok &= checkWriteStr("appass",      prefs.putString ("appass",     c.apPass), c.apPass);
    ok &= checkWrite   ("apchan",      prefs.putInt    ("apchan",   c.apChannel), 4);
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
    ok &= checkWrite   ("gnssen",      prefs.putInt    ("gnssen",   c.gnssEn), 4);
    ok &= checkWrite   ("gnssbaud",    prefs.putInt    ("gnssbaud", c.gnssBaud), 4);
    ok &= checkWrite   ("gnsstime",    prefs.putInt    ("gnsstime", c.gnssTime), 4);
    ok &= checkWrite   ("gnsspos",     prefs.putInt    ("gnsspos",  c.gnssPos), 4);
    ok &= checkWrite   ("gnssalt",     prefs.putInt    ("gnssalt",  c.gnssAlt), 4);
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
    ok &= checkWrite   ("uiflash",     prefs.putBool   ("uiflash",    c.uiFlashEn), 1);
    ok &= checkWrite   ("uiage",       prefs.putBool   ("uiage",      c.uiAgeEn), 1);

    ok &= checkWrite   ("influxen",    prefs.putInt    ("influxen",   c.influxEn), 4);
    ok &= checkWriteStr("influxurl",   prefs.putString ("influxurl",  c.influxUrl), c.influxUrl);
    ok &= checkWriteStr("influxtok",   prefs.putString ("influxtok",  c.influxToken), c.influxToken);
    ok &= checkWriteStr("influxmeas",  prefs.putString ("influxmeas", c.influxMeasurement), c.influxMeasurement);
    ok &= checkWrite   ("influxint",   prefs.putInt    ("influxint",  c.influxIntervalSec), 4);

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
    ok &= checkWrite   ("cfgver",      prefs.putUChar  ("cfgver",   STORED_CONFIG_VERSION), 1);   // storage_layout.h
    ok &= lsSave(prefs);   // the logical sensor table (gLogicalSensors) is saved with every settings save
    ok &= seriesSave(prefs);   // the custom series (series.h), likewise
    ok &= vsSettingsSave(prefs);   // the virtual sensors (vs_store.h), likewise
    return ok;
}

// ---- logical sensor table (lsensors.h): NVS blob "lsens", same namespace
// (a factory reset clears it too). Written by saveConfig() when it changed.
uint32_t lsRandom() { return esp_random(); }

// The table's lock (scheme: lsensors.h, "concurrent access"). Created during
// static construction like the other mutexes here, so it exists before any
// task runs. Recursive: a writer may call helpers that lock again.
static SemaphoreHandle_t lsMutex = xSemaphoreCreateRecursiveMutex();
void lsLock()   { xSemaphoreTakeRecursive(lsMutex, portMAX_DELAY); }
void lsUnlock() { xSemaphoreGiveRecursive(lsMutex); }

void lsLoad()
{
    static_assert(LS_BLOB_MAX <= STORED_BLOB_READ_MAX, "lsens beyond the loaders' read ceiling");
    // unpacked into a local table (1.8 KB on the heap), put in place under the lock - never across NVS I/O
    std::unique_ptr<LogicalSensorTable> t(new (std::nothrow) LogicalSensorTable());
    if (!t) return;
    Preferences prefs;
    if (prefs.begin("config", true, "cfgdata")) {
        const size_t n = prefs.isKey("lsens") ? prefs.getBytesLength("lsens") : 0;
        if (storedBlobLengthReadable(n)) {   // also longer than LS_BLOB_MAX: an older layout for the hook
            std::unique_ptr<uint8_t[]> b(new (std::nothrow) uint8_t[n]), scratch(new (std::nothrow) uint8_t[LS_BLOB_MAX]);
            if (b && scratch && prefs.getBytes("lsens", b.get(), n) == n) {
                const uint8_t* use = b.get();
                size_t useLen = n;
                if (storedVersionAction(b[0], LS_BLOB_VERSION) == STORED_MIGRATE) {   // older layout: the hook (storage_layout.h)
                    useLen = storageMigrateBlob(SF_LOGICAL_SENSORS, b[0], b.get(), n, scratch.get(), LS_BLOB_MAX);
                    use = scratch.get();
                }
                if (!useLen || !lsUnpack(*t, use, useLen)) {
                    *t = LogicalSensorTable();
                    dbgSerial.println("Config: logical sensor table of an unknown layout - starting empty.");
                }
            }
        }
        prefs.end();
    }
    LsGuard g;
    gLogicalSensors = *t;
}

static bool lsSave(Preferences& prefs)
{
    std::unique_ptr<uint8_t[]> b(new (std::nothrow) uint8_t[LS_BLOB_MAX]), old(new (std::nothrow) uint8_t[LS_BLOB_MAX]);
    if (!b || !old) return false;
    size_t n;
    { LsGuard g; n = lsPack(gLogicalSensors, b.get(), LS_BLOB_MAX); }   // packed under the lock, written without it
    if (prefs.isKey("lsens") && prefs.getBytesLength("lsens") == n && prefs.getBytes("lsens", old.get(), n) == n &&
        !memcmp(old.get(), b.get(), n))
        return true;   // unchanged: no flash write
    return checkWrite("lsens", prefs.putBytes("lsens", b.get(), n), n);
}

// ---- custom series (series.h): NVS string "series", same namespace. Not in
// Config (see series.h); loaded once at start, written by saveConfig().
void seriesLoad()
{
    Preferences prefs;
    if (!prefs.begin("config", true, "cfgdata")) return;
    // [version][the stored form, no terminator] (storage_layout.h)
    const size_t n = prefs.isKey("series") ? prefs.getBytesLength("series") : 0;
    std::unique_ptr<uint8_t[]> scratch(new (std::nothrow) uint8_t[SERIES_PACKED_MAX + 1]);
    std::unique_ptr<char[]> s(new (std::nothrow) char[SERIES_PACKED_MAX + 1]);
    if (scratch && s) {
        s[0] = '\0';
        static_assert(SERIES_PACKED_MAX + 1 <= STORED_BLOB_READ_MAX, "series beyond the loaders' read ceiling");
        std::unique_ptr<uint8_t[]> stored(storedBlobLengthReadable(n) ? new (std::nothrow) uint8_t[n] : nullptr);
        if (stored && prefs.getBytes("series", stored.get(), n) == n) {   // also longer than today's form: an older layout for the hook
            size_t len = 0;
            const uint8_t* p = storedBlobLoad(SF_SERIES, stored.get(), n, STORED_SERIES_VERSION, 0, scratch.get(), SERIES_PACKED_MAX + 1, &len);
            if (p && len <= SERIES_PACKED_MAX) { memcpy(s.get(), p, len); s[len] = '\0'; }
        }
        seriesUnpackLocked(s.get());
    }
    prefs.end();
}

static bool seriesSave(Preferences& prefs)
{
    std::unique_ptr<uint8_t[]> b(new (std::nothrow) uint8_t[SERIES_PACKED_MAX + 1]), old(new (std::nothrow) uint8_t[SERIES_PACKED_MAX + 1]);
    if (!b || !old) return false;
    b[0] = STORED_SERIES_VERSION;
    const size_t n = 1 + seriesPackLocked((char*)b.get() + 1, SERIES_PACKED_MAX);
    if (prefs.isKey("series") && prefs.getBytesLength("series") == n && prefs.getBytes("series", old.get(), n) == n &&
        !memcmp(old.get(), b.get(), n))
        return true;   // unchanged: no flash write
    return checkWrite("series", prefs.putBytes("series", b.get(), n), n);
}

// ---- virtual sensors (vs_store.h): NVS blob "vsens", same namespace (a
// factory reset clears it too). Not in Config; loaded once at start.
void vsSettingsLoad()
{
    VsSettings s;   // defaults: off
    Preferences prefs;
    if (prefs.begin("config", true, "cfgdata")) {
        const size_t n = prefs.isKey("vsens") ? prefs.getBytesLength("vsens") : 0;
        static_assert(VS_BLOB_MAX <= STORED_BLOB_READ_MAX, "vsens beyond the loaders' read ceiling");
        // also longer than VS_BLOB_MAX: an older layout for the hook
        std::unique_ptr<uint8_t[]> b(storedBlobLengthReadable(n) ? new (std::nothrow) uint8_t[n] : nullptr),
                                   scratch(new (std::nothrow) uint8_t[VS_BLOB_MAX]);
        if (b && scratch && prefs.getBytes("vsens", b.get(), n) == n) {
            size_t len = 0;
            const uint8_t* p = storedBlobLoad(SF_VIRTUAL_SENSORS, b.get(), n, STORED_VIRTUAL_SENSORS_VERSION, 0,
                                              scratch.get(), VS_BLOB_MAX, &len);
            if (!p || !vsSettingsFromPayload(p, len, s))
                dbgSerial.println("Config: virtual sensor settings of an unknown layout - off.");
        }
        prefs.end();
    }
    vsSettingsWrite(s);
}

static bool vsSettingsSave(Preferences& prefs)
{
    std::unique_ptr<uint8_t[]> b(new (std::nothrow) uint8_t[VS_BLOB_MAX]), old(new (std::nothrow) uint8_t[VS_BLOB_MAX]);
    VsSettingsCopy cur = vsSettingsCopy();
    if (!b || !old || !cur) return false;
    const size_t n = vsSettingsPack(*cur, b.get(), VS_BLOB_MAX);
    if (!n) return false;
    if (prefs.isKey("vsens") && prefs.getBytesLength("vsens") == n && prefs.getBytes("vsens", old.get(), n) == n &&
        !memcmp(old.get(), b.get(), n))
        return true;   // unchanged: no flash write
    return checkWrite("vsens", prefs.putBytes("vsens", b.get(), n), n);
}

void saveRfProto(int proto)
{
    StorageWrite w("config");
    if (!w.ok()) {
        dbgSerial.println("Config save: storage sealed or NVS namespace 'config' not available - wireless sensor type not saved.");
        return;
    }
    Preferences& prefs = w.prefs();
    checkWrite("rfproto", prefs.putInt("rfproto", proto), 4);
}

void saveStationPosition(const Config& c, bool pos, bool alt)
{
    StorageWrite w("config");
    if (!w.ok()) {
        dbgSerial.println("Config save: storage sealed or NVS namespace 'config' not available - GNSS position not saved.");
        return;
    }
    Preferences& prefs = w.prefs();
    if (pos) {
        checkWriteStr("lat", prefs.putString("lat", c.latitude), c.latitude);
        checkWriteStr("lon", prefs.putString("lon", c.longitude), c.longitude);
    }
    if (alt) checkWrite("altitude", prefs.putInt("altitude", c.altitudeM), 4);
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
    StorageWrite w("config");
    if (!w.ok()) {
        dbgSerial.println("Config save: storage sealed or NVS namespace 'config' not available - HA wireless sensor list not saved.");
        return;
    }
    Preferences& prefs = w.prefs();
    checkWriteStr("harfids", prefs.putString("harfids", ids), ids);
}

// Value groups announced to Home Assistant (ha_groups.h): written only when a
// group is seen for the first time; same namespace, so a reset clears it.
uint8_t loadHaGroupsSeen()
{
    Preferences prefs;
    if (!prefs.begin("config", true, "cfgdata")) return 0;
    const uint8_t v = prefs.isKey("haseen") ? prefs.getUChar("haseen", 0) : 0;
    prefs.end();
    return v;
}

void saveHaGroupsSeen(uint8_t seen)
{
    StorageWrite w("config");
    if (!w.ok()) return;   // sealed by a reset: the next start announces from scratch
    checkWrite("haseen", w.prefs().putUChar("haseen", seen), 1);
}
