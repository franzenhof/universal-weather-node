#include "web_manifest.h"
#include "i18n.h"
#include <string.h>

void buildManifest(JsonDocument& doc, const Config& c)
{
    doc.clear();
    const char* name = c.deviceName[0] ? c.deviceName : "Weather Node";
    char shortName[13];
    strlcpy(shortName, c.deviceName[0] ? c.deviceName : "WeatherNode", sizeof(shortName));   // <= 12 characters
    {   // never cut inside a UTF-8 character
        const size_t n = strlen(shortName);
        size_t i = n;
        while (i > 0 && ((unsigned char)shortName[i - 1] & 0xC0) == 0x80) i--;
        if (i > 0) {
            const unsigned char lead = (unsigned char)shortName[i - 1];
            const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (n - (i - 1) < need) shortName[i - 1] = '\0';
        }
    }
    doc["id"] = "/";
    doc["name"] = name;
    doc["short_name"] = shortName;
    doc["start_url"] = "./";
    doc["scope"] = "./";
    doc["display"] = "standalone";
    doc["background_color"] = "#f4f6f8";
    doc["theme_color"] = "#0f9d94";
    doc["lang"] = c.uiLang > 0 && c.uiLang <= LANG_COUNT ? langCode((uint8_t)(c.uiLang - 1)) : "en";
    JsonArray icons = doc["icons"].to<JsonArray>();
    const struct { const char* src; const char* sizes; const char* purpose; } ic[] = {
        { "icon-192.png", "192x192", "any" }, { "icon-512.png", "512x512", "any" }, { "icon-maskable-512.png", "512x512", "maskable" } };
    for (const auto& i : ic) {
        JsonObject o = icons.add<JsonObject>();
        o["src"] = i.src; o["sizes"] = i.sizes; o["type"] = "image/png"; o["purpose"] = i.purpose;
    }
}
