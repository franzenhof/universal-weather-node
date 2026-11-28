#pragma once
// Browser OTA upload (portal /update): accepts the application image
// (universal-weather-node-<version>.bin) AND the factory image
// (...-factory.bin = bootloader + partition table + boot_app0 + application,
// laid out for flash address 0x0). Of a factory image only the application
// part (from 0x10000) is written; bootloader and partition table can't be
// changed over OTA, so the factory image's partition table must match the
// one in the device - otherwise the upload is refused (flash over USB).
// Anything else is refused before a single byte is written.
//
// Arduino's Update only checks the first byte (0xE9), which the bootloader
// has too - without this filter a factory image would be written into the
// app slot and leave a node that no longer boots (firmware <= 2.0.0-beta.2).
//
// App image layout: 24-byte image header (byte 0 = 0xE9), 8-byte header of
// the first segment, then esp_app_desc_t starting with 0xABCD5432 at 0x20.
// The bootloader has no app descriptor there.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum OtaImageKind : uint8_t { OTA_IMAGE_APP, OTA_IMAGE_FACTORY_OR_BOOTLOADER, OTA_IMAGE_NOT_ESP32, OTA_IMAGE_TOO_SHORT };

constexpr size_t OTA_APP_HEADER_LEN    = 0x94;     // up to the start of the descriptor's ESP-IDF version (0x90)
constexpr size_t OTA_FACTORY_PT_OFFSET = 0x8000;    // partition table in the factory image
constexpr size_t OTA_FACTORY_PT_LEN    = 0xC00;     // ESP_PARTITION_TABLE_MAX_LEN
constexpr size_t OTA_FACTORY_APP_OFFSET = 0x10000;  // app0 = first OTA slot (partitions.csv)

inline OtaImageKind otaImageKind(const uint8_t* d, size_t n)
{
    if (n < OTA_APP_HEADER_LEN) return OTA_IMAGE_TOO_SHORT;
    if (d[0] != 0xE9) return OTA_IMAGE_NOT_ESP32;
    if (d[0x20] == 0x32 && d[0x21] == 0x54 && d[0x22] == 0xCD && d[0x23] == 0xAB) return OTA_IMAGE_APP;
    return OTA_IMAGE_FACTORY_OR_BOOTLOADER;
}

// Firmware 4.x (Heltec V4 only, other partition table): built with ESP-IDF 5,
// 1.x / 2.x with ESP-IDF 4 - the app descriptor (0x20) has the version at +0x70
// ("v4.4.7-dirty", "5.5.6..."). A 3.x from this code base stays on ESP-IDF 4 and is accepted.
// Over the air 4.x would leave a V3 without a
// working firmware.
inline bool otaImageIsV4Line(const uint8_t* d, size_t n)
{
    if (otaImageKind(d, n) != OTA_IMAGE_APP) return false;
    const uint8_t* v = d + 0x90;
    if (*v == 'v') v++;
    return *v >= '5' && *v <= '9';
}

// Compares the partition entries (32-byte records with magic 0xAA50, up to
// the first other record; the MD5 record 0xEBEB is skipped) of two tables.
inline bool otaPartitionTablesMatch(const uint8_t* a, const uint8_t* b, size_t len)
{
    size_t entries = 0;
    for (size_t o = 0; o + 32 <= len; o += 32) {
        bool ea = a[o] == 0xAA && a[o + 1] == 0x50, eb = b[o] == 0xAA && b[o + 1] == 0x50;
        if (!ea && !eb) break;
        if (ea != eb || memcmp(a + o, b + o, 32) != 0) return false;
        entries++;
    }
    return entries > 0;
}

// Streaming filter for the multipart upload: feed() every chunk in order,
// write() receives exactly the application image bytes, finish() at the end.
class OtaUploadFilter {
public:
    typedef bool (*WriteFn)(const uint8_t* d, size_t n, void* ctx);

    // devicePt: the device's own partition table (OTA_FACTORY_PT_LEN bytes),
    // needed to accept a factory image; nullptr = factory images are refused.
    void begin(const uint8_t* devicePt)
    {
        devicePt_ = devicePt; mode_ = UNDECIDED; factory_ = false; pos_ = 0; hdrLen_ = 0; error_ = "";
        memset(pt_, 0xFF, sizeof(pt_));   // no 3 KB temporary on the (web task) stack
    }

    // false = refused (error() says why) or the write callback failed.
    bool feed(const uint8_t* d, size_t n, WriteFn write, void* ctx)
    {
        while (n && mode_ != REJECTED) {
            if (mode_ == UNDECIDED) {                     // first 0x24 bytes of the file
                size_t k = take(hdr_, hdrLen_, OTA_APP_HEADER_LEN, d, n);
                pos_ += k; d += k; n -= k;
                if (hdrLen_ < OTA_APP_HEADER_LEN) return true;
                OtaImageKind kind = otaImageKind(hdr_, hdrLen_);
                if (otaImageIsV4Line(hdr_, hdrLen_))
                    return reject("this is firmware 4.x for the Heltec V4 only - on a V4 install its -factory.bin over USB; a V3 stays on 2.x");
                if (kind == OTA_IMAGE_APP) {
                    mode_ = APP; factory_ = false;
                    if (!write(hdr_, hdrLen_, ctx)) return reject("write failed");
                } else if (kind == OTA_IMAGE_FACTORY_OR_BOOTLOADER) {
                    if (!devicePt_) return reject("factory image not accepted here - use universal-weather-node-<version>.bin");
                    mode_ = FACTORY_SKIP; factory_ = true;
                } else {
                    return reject("not an ESP32 firmware image (.bin)");
                }
            } else if (mode_ == FACTORY_SKIP) {           // bootloader, partition table, boot_app0: not written
                size_t skip = n < OTA_FACTORY_APP_OFFSET - pos_ ? n : OTA_FACTORY_APP_OFFSET - pos_;
                for (size_t i = 0; i < skip; i++) {       // keep the partition table for the check
                    size_t p = pos_ + i;
                    if (p >= OTA_FACTORY_PT_OFFSET && p < OTA_FACTORY_PT_OFFSET + OTA_FACTORY_PT_LEN)
                        pt_[p - OTA_FACTORY_PT_OFFSET] = d[i];
                }
                pos_ += skip; d += skip; n -= skip;
                if (pos_ < OTA_FACTORY_APP_OFFSET) return true;
                if (!otaPartitionTablesMatch(pt_, devicePt_, OTA_FACTORY_PT_LEN))
                    return reject("the factory image has a different partition table than this node - "
                                  "flash it over USB (see Installing the Firmware)");
                mode_ = FACTORY_APP_HDR; hdrLen_ = 0;
            } else if (mode_ == FACTORY_APP_HDR) {        // app header inside the factory image
                size_t k = take(hdr_, hdrLen_, OTA_APP_HEADER_LEN, d, n);
                pos_ += k; d += k; n -= k;
                if (hdrLen_ < OTA_APP_HEADER_LEN) return true;
                if (otaImageKind(hdr_, hdrLen_) != OTA_IMAGE_APP) return reject("no valid firmware at 0x10000 of the factory image");
                mode_ = APP;
                if (!write(hdr_, hdrLen_, ctx)) return reject("write failed");
            } else {                                      // APP: pass through
                if (!write(d, n, ctx)) return reject("write failed");
                pos_ += n; n = 0;
            }
        }
        return mode_ != REJECTED;
    }

    // true = a complete application image was passed to write().
    bool finish()
    {
        if (mode_ == APP) return true;
        if (mode_ != REJECTED) reject("file too short for a firmware image");
        return false;
    }

    bool        isFactory() const { return factory_; }
    const char* error() const { return error_; }

private:
    enum Mode : uint8_t { UNDECIDED, FACTORY_SKIP, FACTORY_APP_HDR, APP, REJECTED };

    static size_t take(uint8_t* buf, size_t& have, size_t want, const uint8_t* d, size_t n)
    {
        size_t k = want - have < n ? want - have : n;
        memcpy(buf + have, d, k);
        have += k;
        return k;
    }
    bool reject(const char* why) { mode_ = REJECTED; error_ = why; return false; }

    const uint8_t* devicePt_ = nullptr;
    Mode     mode_ = UNDECIDED;
    bool     factory_ = false;
    size_t   pos_ = 0;
    uint8_t  hdr_[OTA_APP_HEADER_LEN] = {};
    size_t   hdrLen_ = 0;
    uint8_t  pt_[OTA_FACTORY_PT_LEN] = {};
    const char* error_ = "";
};
