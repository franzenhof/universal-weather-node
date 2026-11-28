// Browser-OTA upload filter (ota_check.h) with the real release images in
// firmware/: the application image passes through unchanged; of the factory
// image exactly the application part is written (any chunk size); a
// different partition table, a truncated file or a non-ESP32 file is refused.
#include "ota_check.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<uint8_t> load(const char* path)
{
    std::vector<uint8_t> b;
    FILE* f = fopen(path, "rb");
    if (!f) return b;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    return b;
}

static bool sink(const uint8_t* d, size_t n, void* ctx)
{
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    out->insert(out->end(), d, d + n);
    return true;
}

// Feeds `file` in chunks of `chunk` bytes; returns the bytes written (empty + ok=false on refusal).
static std::vector<uint8_t> run(const std::vector<uint8_t>& file, size_t chunk, const uint8_t* devicePt,
                                bool& ok, const char** err = nullptr, bool* factory = nullptr)
{
    static OtaUploadFilter f;   // large (partition table buffer) - like the static in portal.cpp
    std::vector<uint8_t> out;
    f.begin(devicePt);
    ok = true;
    for (size_t o = 0; ok && o < file.size(); o += chunk)
        ok = f.feed(file.data() + o, file.size() - o < chunk ? file.size() - o : chunk, sink, &out);
    if (ok) ok = f.finish();
    if (err) *err = f.error();
    if (factory) *factory = f.isFactory();
    return out;
}

int main()
{
    auto app = load("firmware/universal-weather-node-2.0.0.bin");
    auto fac = load("firmware/universal-weather-node-2.0.0-factory.bin");
    assert(app.size() > 0x10000 && fac.size() == 0x10000 + app.size());
    std::vector<uint8_t> devicePt(fac.begin() + OTA_FACTORY_PT_OFFSET, fac.begin() + OTA_FACTORY_PT_OFFSET + OTA_FACTORY_PT_LEN);

    assert(otaImageKind(app.data(), app.size()) == OTA_IMAGE_APP);
    assert(otaImageKind(fac.data(), fac.size()) == OTA_IMAGE_FACTORY_OR_BOOTLOADER);

    bool ok, isFactory;
    const char* err;
    // firmware 4.x (ESP-IDF 5): refused before a byte is written, whatever the chunk size
    for (const char* idf : { "5.5.6.261115", "v5.1" }) {
        auto v4 = app;
        memcpy(v4.data() + 0x90, idf, strlen(idf) + 1);
        assert(otaImageIsV4Line(v4.data(), v4.size()));
        for (size_t chunk : {(size_t)1, (size_t)7, (size_t)1436}) {
            auto out = run(v4, chunk, devicePt.data(), ok, &err);
            assert(!ok && out.empty() && strstr(err, "4.x"));
        }
    }
    for (const char* f : { "firmware/universal-weather-node-1.1.1.bin", "firmware/universal-weather-node-2.0.5.bin" }) {
        auto own = load(f);
        assert(own.size() > 0x10000 && !otaImageIsV4Line(own.data(), own.size()));
    }
    for (size_t chunk : {(size_t)1436, (size_t)7, (size_t)0x24, (size_t)0x10001, (size_t)65536, (size_t)1}) {
        // application image: passed through unchanged
        auto out = run(app, chunk, devicePt.data(), ok, &err, &isFactory);
        assert(ok && !isFactory && out == app);
        // factory image: exactly the application part is written
        out = run(fac, chunk, devicePt.data(), ok, &err, &isFactory);
        assert(ok && isFactory && out == app);
    }

    // factory image with a different partition table (e.g. a future layout): refused, nothing written
    auto otherPt = devicePt;
    otherPt[32 + 8] ^= 0x10;   // second entry, size field
    auto out = run(fac, 1436, otherPt.data(), ok, &err);
    assert(!ok && out.empty() && strstr(err, "partition table"));

    // no device table available: factory refused
    out = run(fac, 1436, nullptr, ok, &err);
    assert(!ok && out.empty() && strstr(err, "factory"));

    // truncated factory image (bootloader part only): refused, nothing written
    std::vector<uint8_t> shortFac(fac.begin(), fac.begin() + 0x9000);
    out = run(shortFac, 1436, devicePt.data(), ok, &err);
    assert(!ok && out.empty() && strstr(err, "too short"));

    // factory layout without a valid app at 0x10000: refused
    auto badFac = fac;
    badFac[0x10000 + 0x20] = 0;
    out = run(badFac, 1436, devicePt.data(), ok, &err);
    assert(!ok && out.empty());

    // not firmware at all
    std::vector<uint8_t> zip(4096, 0x41);
    zip[0] = 'P'; zip[1] = 'K';
    out = run(zip, 1436, devicePt.data(), ok, &err);
    assert(!ok && out.empty() && strstr(err, "not an ESP32"));

    // partition-table compare: identical tables match, empty tables don't
    std::vector<uint8_t> empty(OTA_FACTORY_PT_LEN, 0xFF);
    assert(otaPartitionTablesMatch(devicePt.data(), devicePt.data(), devicePt.size()));
    assert(!otaPartitionTablesMatch(empty.data(), empty.data(), empty.size()));
    assert(!otaPartitionTablesMatch(devicePt.data(), empty.data(), empty.size()));

    puts("otacheck ok");
    return 0;
}
