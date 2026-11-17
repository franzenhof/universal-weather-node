#pragma once
// Plausibility check for browser OTA uploads (portal /update): the first
// bytes must be an ESP32 *application* image, not the -factory image (which
// starts with the bootloader) or any other file. Arduino's Update only checks
// the first byte (0xE9), which the bootloader has too - a factory image would
// be written into the app slot and leave a node that no longer boots.
//
// App image layout: 24-byte image header (byte 0 = 0xE9), 8-byte header of
// the first segment, then esp_app_desc_t starting with 0xABCD5432 at 0x20.
// The bootloader has no app descriptor there.
#include <stddef.h>
#include <stdint.h>

enum OtaImageKind : uint8_t { OTA_IMAGE_APP, OTA_IMAGE_FACTORY_OR_BOOTLOADER, OTA_IMAGE_NOT_ESP32, OTA_IMAGE_TOO_SHORT };

inline OtaImageKind otaImageKind(const uint8_t* d, size_t n)
{
    if (n < 0x24) return OTA_IMAGE_TOO_SHORT;
    if (d[0] != 0xE9) return OTA_IMAGE_NOT_ESP32;
    if (d[0x20] == 0x32 && d[0x21] == 0x54 && d[0x22] == 0xCD && d[0x23] == 0xAB) return OTA_IMAGE_APP;
    return OTA_IMAGE_FACTORY_OR_BOOTLOADER;
}

inline const char* otaImageError(OtaImageKind k)
{
    switch (k) {
        case OTA_IMAGE_FACTORY_OR_BOOTLOADER:
            return "This is a factory image (bootloader + partition table + firmware) - it is only for the first "
                   "installation over USB. For OTA use universal-weather-node-<version>.bin (without -factory).";
        case OTA_IMAGE_NOT_ESP32: return "Not an ESP32 firmware image (.bin).";
        case OTA_IMAGE_TOO_SHORT: return "File too short for a firmware image.";
        default: return "";
    }
}
