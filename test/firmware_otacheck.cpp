// Browser-OTA plausibility check (ota_check.h): the real release images in
// firmware/ - application images must pass, -factory images must be refused.
#include "ota_check.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<uint8_t> head(const char* path)
{
    std::vector<uint8_t> b(64);
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    b.resize(fread(b.data(), 1, b.size(), f));
    fclose(f);
    return b;
}

int main()
{
    auto app = head("firmware/universal-weather-node-2.0.0-beta.3.bin");
    auto fac = head("firmware/universal-weather-node-2.0.0-beta.3-factory.bin");
    assert(!app.empty() && !fac.empty());
    assert(otaImageKind(app.data(), app.size()) == OTA_IMAGE_APP);
    assert(otaImageKind(fac.data(), fac.size()) == OTA_IMAGE_FACTORY_OR_BOOTLOADER);
    assert(strstr(otaImageError(OTA_IMAGE_FACTORY_OR_BOOTLOADER), "-factory"));

    const uint8_t text[64] = "PK\x03\x04 a zip file, not firmware ...............................";
    assert(otaImageKind(text, sizeof(text)) == OTA_IMAGE_NOT_ESP32);
    assert(otaImageKind(app.data(), 0x23) == OTA_IMAGE_TOO_SHORT);
    assert(otaImageKind(app.data(), 0x24) == OTA_IMAGE_APP);
    puts("otacheck ok");
    return 0;
}
