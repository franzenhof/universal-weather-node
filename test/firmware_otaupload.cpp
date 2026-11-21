// Browser firmware upload handler (ota_upload.h), shared by the config portal
// and POST /api/ota, with the real release images in firmware/ and fake
// Update operations: the application image and the application part of the
// factory image are written in any chunk size; missing login, low memory,
// foreign files, flash errors, an aborted or vanished connection are refused
// cleanly (Update aborted, nothing left active).
#include "ota_upload.h"
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

// fake device
static std::vector<uint8_t> written, devicePt;
static bool   beginOk, writeOk, endOk, ptOk, begun, aborted;
static size_t largest;
static uint32_t clockMs;
static const char* fakeError;

static void resetFake()
{
    written.clear();
    beginOk = writeOk = endOk = ptOk = true;
    begun = aborted = false;
    largest = 100 * 1024;
    fakeError = "No Error";
}

static const OtaUpdateOps FAKE = {
    []() { begun = beginOk; aborted = false; if (!beginOk) fakeError = "Not Enough Space"; return beginOk; },
    [](const uint8_t* d, size_t n) {
        assert(begun && !aborted);
        if (!writeOk) { fakeError = "Flash Write Failed"; return false; }
        written.insert(written.end(), d, d + n);
        return true;
    },
    []() { if (!endOk) fakeError = "MD5 Check Failed"; return endOk; },
    []() { aborted = true; written.clear(); },
    []() { return fakeError; },
    [](uint8_t* b, size_t n) {
        if (!ptOk) return false;
        assert(n == devicePt.size());
        memcpy(b, devicePt.data(), n);
        return true;
    },
    []() { return largest; },
    []() { return clockMs; },
    [](const char*, const char*) {},
};

// one request: start, the file in chunks, end
static OtaOutcome upload(const std::vector<uint8_t>& file, size_t chunk, bool authorized = true)
{
    otaUploadReset();
    otaUploadStart(FAKE, authorized, "test");
    for (size_t o = 0; o < file.size(); o += chunk) {
        otaUploadChunk(file.data() + o, file.size() - o < chunk ? file.size() - o : chunk);
        clockMs += 10;
    }
    otaUploadFinish();
    assert(!otaUploadActive());
    return otaUploadOutcome();
}

int main()
{
    auto app = load("firmware/universal-weather-node-2.0.0-beta.3.bin");
    auto fac = load("firmware/universal-weather-node-2.0.0-beta.3-factory.bin");
    assert(app.size() > 0x10000 && fac.size() == 0x10000 + app.size());
    devicePt.assign(fac.begin() + OTA_FACTORY_PT_OFFSET, fac.begin() + OTA_FACTORY_PT_OFFSET + OTA_FACTORY_PT_LEN);

    assert(otaUploadOutcome() == OTA_OUT_NONE && !otaUploadActive());

    for (size_t chunk : {(size_t)1436, (size_t)4096, (size_t)7, (size_t)65536}) {
        // application image: written unchanged
        resetFake();
        assert(upload(app, chunk) == OTA_OUT_OK);
        assert(written == app && !otaUploadIsFactory() && !aborted && strcmp(otaUploadError(), "") == 0);
        // factory image: exactly its application part
        resetFake();
        assert(upload(fac, chunk) == OTA_OUT_OK);
        assert(written == app && otaUploadIsFactory() && !aborted);
    }
    otaUploadReset();
    assert(otaUploadOutcome() == OTA_OUT_NONE);

    // active while chunks arrive, not before / after
    resetFake();
    otaUploadStart(FAKE, true, "test");
    assert(otaUploadActive());
    otaUploadChunk(app.data(), 4096);
    assert(otaUploadActive());
    // connection gone without UPLOAD_FILE_ABORTED: expires after OTA_IDLE_MAX_MS
    clockMs += OTA_IDLE_MAX_MS + 1;
    assert(!otaUploadActive());
    // the next upload aborts the stale Update and works
    resetFake();
    begun = true;   // the stale Update is still running
    assert(upload(app, 1436) == OTA_OUT_OK && written == app);

    // not logged in: no Update.begin(), the body is discarded, nothing active
    resetFake();
    assert(upload(app, 1436, false) == OTA_OUT_NOT_AUTHORIZED);
    assert(!begun && written.empty());

    // low memory: refused before Update.begin()
    resetFake();
    largest = OTA_MIN_LARGEST - 1;
    assert(upload(app, 1436) == OTA_OUT_REFUSED);
    assert(!begun && written.empty() && strstr(otaUploadError(), "memory"));

    // Update.begin() fails
    resetFake();
    beginOk = false;
    assert(upload(app, 1436) == OTA_OUT_FAILED && strstr(otaUploadError(), "Space") && written.empty());

    // not firmware at all: refused, Update aborted, nothing written
    resetFake();
    std::vector<uint8_t> zip(8192, 0x41);
    zip[0] = 'P'; zip[1] = 'K';
    assert(upload(zip, 1436) == OTA_OUT_REFUSED);
    assert(aborted && written.empty() && strstr(otaUploadError(), "not an ESP32"));

    // factory image with another partition table / without the device table: refused
    resetFake();
    devicePt[32 + 8] ^= 0x10;
    assert(upload(fac, 1436) == OTA_OUT_REFUSED && aborted && written.empty() && strstr(otaUploadError(), "partition table"));
    devicePt[32 + 8] ^= 0x10;
    resetFake();
    ptOk = false;
    assert(upload(fac, 1436) == OTA_OUT_REFUSED && aborted && written.empty() && strstr(otaUploadError(), "factory"));

    // truncated application image: the filter passes it through, Update.end()
    // (image verification) refuses it - its error is the answer
    resetFake();
    endOk = false;
    std::vector<uint8_t> shortApp(app.begin(), app.begin() + app.size() / 2);
    assert(upload(shortApp, 1436) == OTA_OUT_FAILED && strstr(otaUploadError(), "MD5"));

    // empty file
    resetFake();
    assert(upload(std::vector<uint8_t>(), 1436) == OTA_OUT_REFUSED && aborted);

    // flash write error: FAILED with the Update error, aborted
    resetFake();
    writeOk = false;
    assert(upload(app, 1436) == OTA_OUT_FAILED && aborted && strstr(otaUploadError(), "Flash Write"));

    // verification at the end fails
    resetFake();
    endOk = false;
    assert(upload(app, 1436) == OTA_OUT_FAILED && strstr(otaUploadError(), "MD5"));

    // connection aborted mid-upload
    resetFake();
    otaUploadReset();
    otaUploadStart(FAKE, true, "test");
    otaUploadChunk(app.data(), 4096);
    otaUploadAbort();
    assert(otaUploadOutcome() == OTA_OUT_FAILED && aborted && !otaUploadActive() && strstr(otaUploadError(), "aborted"));
    otaUploadReset();
    assert(otaUploadOutcome() == OTA_OUT_NONE && strcmp(otaUploadError(), "") == 0);

    puts("otaupload ok");
    return 0;
}
