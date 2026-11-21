// Loxone UDP lines (src/loxone_lines.cpp): names, booleans, nested flattening,
// prefix, invalid values skipped, decimals; settings sanitizing.
#include "loxone.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static void collect(const char* line, size_t len, void* ctx)
{
    assert(strlen(line) == len);
    static_cast<std::vector<std::string>*>(ctx)->push_back(std::string(line, len));
}

static std::map<std::string, std::string> run(const JsonDocument& doc, const char* prefix, std::vector<std::string>& lines)
{
    lines.clear();
    int n = loxoneLines(doc, prefix, collect, &lines);
    assert(n == (int)lines.size());
    std::map<std::string, std::string> m;
    for (const std::string& l : lines) {
        size_t eq = l.find('=');
        assert(eq != std::string::npos && l.find('=', eq + 1) == std::string::npos);
        assert(!m.count(l.substr(0, eq)));   // every name once
        m[l.substr(0, eq)] = l.substr(eq + 1);
    }
    return m;
}

int main()
{
    JsonDocument doc;
    doc["windDirDeg"] = 338;
    doc["windSpeed"]  = 7.123456f;
    doc["windGust"]   = 26.5f;
    doc["windUnit"]   = "km/h";
    doc["rain"]       = 0.2f;
    doc["rainRate"]   = 2.412345f;
    doc["temp"]       = 6.6f;
    doc["tempUnit"]   = "C";
    doc["humidityPct"] = 88;
    doc["pressureQnh"] = 1015.8123f;
    doc["pressureTrend3h"] = -1.23f;
    doc["vpd"]        = 0.42f;
    doc["batteryVolt"] = 4.123f;
    doc["dewPoint"]   = -0.04f;            // "-0.0" must not appear
    doc["feelsLike"]  = NAN;               // invalid: skipped
    doc["uvEnabled"]  = true;
    doc["radioBatteryLow"] = false;
    doc["lightLux"]   = 5321.6f;
    doc["windDirText"] = "NNW";            // strings: not sent
    doc["lastRain"]   = "2026-11-19T10:00:00Z";
    doc["nextCycleInSec"] = 299UL;
    doc["cpuTemp"]    = 42.3f;
    doc["cpuTempUnit"] = "C";
    JsonArray ds = doc["ds18b20"].to<JsonArray>();
    ds.add(10.5f); ds.add(nullptr); ds.add(14.25f);
    JsonObject w = doc["warnings"].to<JsonObject>();
    w["frost"] = true; w["fog"] = false; w["sultry"] = false;
    JsonObject w2 = doc["wind2"].to<JsonObject>();
    w2["speed"] = 3.0f; w2["gust"] = 5.55f; w2["dirDeg"] = 90;
    JsonObject h = doc["heater"].to<JsonObject>();
    h["mode"] = "auto"; h["on"] = true; h["dutyPct"] = 60;
    JsonObject f = doc["fan"].to<JsonObject>();
    f["mode"] = "on"; f["on"] = false; f["dutyPct"] = 100; f["rpm"] = 2950; f["stalled"] = false;
    JsonObject v = doc["victron"].to<JsonObject>();
    v["valid"] = true; v["batteryV"] = 13.28f; v["pvW"] = 45.0f; v["yieldTodayKWh"] = 0.12f; v["chargeState"] = 3;
    JsonObject st = doc["status"].to<JsonObject>();
    st["bme280Present"] = true; st["sendFailCount"] = 0;
    doc["sources"]["tempHum"] = "board";
    JsonArray rf = doc["rfSensors"].to<JsonArray>();
    JsonObject r1 = rf.add<JsonObject>();
    r1["id"] = "00002A5B"; r1["slot"] = 2; r1["type"] = 1; r1["protocol"] = "Bresser";
    r1["rssi"] = -71; r1["ageSec"] = 12; r1["batteryOk"] = false;
    r1["temp"] = 4.5f; r1["humidityPct"] = 88; r1["windSpeed"] = 3.1f; r1["lightLux"] = 5322;
    r1["stale"].to<JsonArray>().add("wind");
    JsonObject r2 = rf.add<JsonObject>();   // no slot (not in the ID list): not sent
    r2["id"] = "00000099"; r2["rssi"] = -90; r2["temp"] = 1.0f;
    JsonObject r32 = rf.add<JsonObject>();  // wireless 32: sent as rf32_ (all 32 slots, not only the value sources)
    r32["id"] = "34000A1B"; r32["slot"] = 32; r32["type"] = 3; r32["model"] = "WN34"; r32["rssi"] = -80;
    r32["ageSec"] = 40; r32["batteryOk"] = true; r32["temp"] = 21.3f;
    JsonObject r3 = rf.add<JsonObject>();   // Fine Offset air quality + leak values: generic keys
    r3["id"] = "45003660"; r3["slot"] = 3; r3["type"] = 8; r3["model"] = "WH45"; r3["rssi"] = -60;
    r3["ageSec"] = 5; r3["batteryOk"] = true; r3["pm25"] = 3.5f; r3["pm10"] = 4.1f; r3["co2Ppm"] = 553;
    r3["externalPower"] = true; r3["leak"] = true; r3["leakRaw"] = 267; r3["leafWetnessPct"] = 7;
    r3["conductivityUsCm"] = 643; r3["pressure"] = 1001.7f;
    JsonObject r33 = rf.add<JsonObject>();  // beyond the table: never sent
    r33["id"] = "34000A1C"; r33["slot"] = 33; r33["temp"] = 5.0f;

    std::vector<std::string> lines;
    std::map<std::string, std::string> m = run(doc, "", lines);
    // top level
    assert(m["windDirDeg"] == "338" && m["windSpeed"] == "7.1" && m["windGust"] == "26.5");
    assert(m["temp"] == "6.6" && m["humidityPct"] == "88" && m["rain"] == "0.2" && m["rainRate"] == "2.4");
    assert(m["pressureQnh"] == "1015.8" && m["pressureTrend3h"] == "-1.23" && m["vpd"] == "0.42");
    assert(m["batteryVolt"] == "4.12" && m["dewPoint"] == "0.0" && m["lightLux"] == "5322");
    assert(m["uvEnabled"] == "1" && m["radioBatteryLow"] == "0" && m["nextCycleInSec"] == "299" && m["cpuTemp"] == "42.3");
    assert(!m.count("feelsLike") && !m.count("windDirText") && !m.count("lastRain") && !m.count("windUnit") &&
           !m.count("tempUnit") && !m.count("status") && !m.count("statusBme280Present") && !m.count("sources"));
    // nested
    assert(m["ds18b20_1"] == "10.5" && !m.count("ds18b20_2") && m["ds18b20_3"] == "14.2");
    assert(m["warnFrost"] == "1" && m["warnFog"] == "0" && m["warnSultry"] == "0");
    assert(m["wind2Speed"] == "3.0" && m["wind2Gust"] == "5.6" && m["wind2DirDeg"] == "90");
    assert(m["heaterOn"] == "1" && m["heaterDutyPct"] == "60" && !m.count("heaterMode"));
    assert(m["fanOn"] == "0" && m["fanDutyPct"] == "100" && m["fanRpm"] == "2950" && m["fanStalled"] == "0");
    assert(m["victronValid"] == "1" && m["victronBatteryV"] == "13.28" && m["victronPvW"] == "45" &&
           m["victronYieldTodayKWh"] == "0.12" && m["victronChargeState"] == "3");
    // wireless sensors: by value-source slot, stale values left out
    assert(m["rf2_temp"] == "4.5" && m["rf2_humidity"] == "88" && m["rf2_battery"] == "0" &&
           m["rf2_rssi"] == "-71" && m["rf2_age"] == "12" && m["rf2_lightLux"] == "5322");
    assert(!m.count("rf2_windSpeed") && !m.count("rf2_id") && !m.count("rf2_slot") && !m.count("rf2_type"));
    assert(m["rf3_pm25"] == "3.5" && m["rf3_pm10"] == "4.1" && m["rf3_co2Ppm"] == "553" && m["rf3_externalPower"] == "1");
    assert(m["rf3_leak"] == "1" && m["rf3_leakRaw"] == "267" && m["rf3_leafWetnessPct"] == "7");
    assert(m["rf3_conductivityUsCm"] == "643" && m["rf3_pressure"] == "1001.7" && !m.count("rf3_model"));
    assert(m["rf32_temp"] == "21.3" && m["rf32_rssi"] == "-80" && m["rf32_battery"] == "1" && m["rf32_age"] == "40");
    assert(!m.count("rf33_temp") && !m.count("rf32_model") && !m.count("rf32_type"));
    for (const auto& kv : m)
        assert(kv.first.compare(0, 3, "rf1") != 0 &&
               kv.first.find("rf_") == std::string::npos);
    // plain ASCII, '.' decimal separator, no exponent
    for (const std::string& l : lines)
    {
        for (char c : l) assert(c > ' ' && c < 127 && c != ',');
        std::string val = l.substr(l.find('=') + 1);
        assert(val.find('e') == std::string::npos && val.find('E') == std::string::npos);
    }

    // prefix on every name
    std::map<std::string, std::string> p = run(doc, "g75_", lines);
    assert(p.size() == m.size() && p["g75_temp"] == "6.6" && p["g75_rf2_temp"] == "4.5" && p["g75_warnFrost"] == "1");
    for (const std::string& l : lines) assert(l.compare(0, 4, "g75_") == 0);

    // imperial: rain and pressure with 2 decimals
    doc["cpuTempUnit"] = "F"; doc["rain"] = 0.0787f; doc["pressureQnh"] = 29.9967f;
    m = run(doc, "", lines);
    assert(m["rain"] == "0.08" && m["pressureQnh"] == "30.00" && m["temp"] == "6.6");

    // sun / moon: times as minutes since local midnight, numbers as they are, texts skipped
    {
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
        tzset();
        JsonDocument a;
        a["astro"]["sunrise"] = "2024-06-21T02:43:20Z";      // 04:43 CEST
        a["astro"]["moonset"] = "2024-12-21T10:48:00Z";      // 11:48 CET
        a["astro"]["dayLengthMin"] = 1010;
        a["astro"]["sunElevation"] = -3.25;
        a["astro"]["moonPhase"] = "full";
        a["astro"]["moonPhaseIndex"] = 4;
        m = run(a, "", lines);
        assert(m["astroSunrise"] == "283" && m["astroMoonset"] == "708" && m["astroDayLengthMin"] == "1010");
        assert(m["astroSunElevation"] == "-3.2" || m["astroSunElevation"] == "-3.3");
        assert(m["astroMoonPhaseIndex"] == "4" && !m.count("astroMoonPhase") && lines.size() == 5);
    }

    // empty document: nothing
    JsonDocument empty;
    assert(loxoneLines(empty, "", collect, &lines) == 0);

    // settings sanitizing
    assert(loxPrefixValid("") && loxPrefixValid("g75_") && loxPrefixValid("a.b-C_9") && loxPrefixValid("abcdefghijklmnop"));
    assert(!loxPrefixValid("abcdefghijklmnopq") && !loxPrefixValid("a b") && !loxPrefixValid("x=") && !loxPrefixValid("ä"));
    char pre[40] = "g 7=5_\xc3\xa4.x-yabcdefghijklmnopq";
    loxSanitizePrefix(pre);
    assert(!strcmp(pre, "g75_.x-yabcdefgh"));
    assert(loxPortValid(1) && loxPortValid(7000) && loxPortValid(65535) && !loxPortValid(0) && !loxPortValid(65536) && !loxPortValid(-1));
    assert(loxIntervalValid(0) && loxIntervalValid(2) && loxIntervalValid(3600) && !loxIntervalValid(1) &&
           !loxIntervalValid(3601) && !loxIntervalValid(-5));
    return 0;
}
