// Web app manifest (src/web_manifest.cpp): valid JSON with the fields the
// browsers need, the device name, short_name <= 12 bytes without a cut UTF-8
// character, relative URLs.
#include "web_manifest.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

Config gCfg;

static JsonDocument roundTrip(const Config& c)
{
    JsonDocument d;
    buildManifest(d, c);
    std::string s;
    serializeJson(d, s);
    JsonDocument back;
    assert(!deserializeJson(back, s));   // parses
    return back;
}

int main()
{
    Config c;
    JsonDocument m = roundTrip(c);
    assert(m["name"] == "Weather Node" && m["short_name"] == "WeatherNode" && m["id"] == "/");
    assert(m["start_url"] == "./" && m["scope"] == "./" && m["display"] == "standalone");
    assert(m["background_color"] == "#f4f6f8" && m["theme_color"] == "#0f9d94" && m["lang"] == "en");
    assert(m["icons"].size() == 3 && m["icons"][2]["purpose"] == "maskable" && m["icons"][0]["src"] == "icon-192.png");
    for (JsonVariant i : m["icons"].as<JsonArray>()) assert(i["src"].as<std::string>()[0] != '/');   // relative
    // device name, language, short name cut at 12 bytes on a character boundary
    strcpy(c.deviceName, "Wetter G\xC3\xA4rtnerei-Nord");   // "ä" at bytes 8-9
    c.uiLang = 2;
    m = roundTrip(c);
    assert(m["name"] == "Wetter G\xC3\xA4rtnerei-Nord" && m["lang"] == "de");
    assert(std::string(m["short_name"].as<const char*>()) == "Wetter G\xC3\xA4rt");   // 12 bytes, whole characters
    strcpy(c.deviceName, "ABCDEFGHIJK\xC3\xA4");             // the "ä" would be split at byte 12: dropped
    m = roundTrip(c);
    assert(std::string(m["short_name"].as<const char*>()) == "ABCDEFGHIJK");
    puts("manifest ok");
    return 0;
}
