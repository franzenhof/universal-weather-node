#pragma once
// Browser firmware upload, shared by the config portal (/u) and the web
// interface (POST /api/ota): one streaming handler around OtaUploadFilter
// (ota_check.h) and Update. Host-tested (test/firmware_otaupload.cpp) with
// the real release images; the device operations (Update, flash read, heap)
// are in ota_upload_esp.cpp.
//
// Call order per request: otaUploadStart() (UPLOAD_FILE_START, after the
// caller checked the login), otaUploadChunk() for every UPLOAD_FILE_WRITE,
// then otaUploadFinish() (UPLOAD_FILE_END) or otaUploadAbort()
// (UPLOAD_FILE_ABORTED). The request handler then answers from
// otaUploadOutcome() / otaUploadError().
//
// While an upload runs (otaUploadActive()), HTTPS requests, notifications,
// custom uploads, weather networks and LoRaWAN uplinks wait. A connection that
// dies without UPLOAD_FILE_ABORTED expires after OTA_IDLE_MAX_MS.
#include <stddef.h>
#include <stdint.h>

constexpr size_t   OTA_MIN_LARGEST = 16 * 1024;   // largest free block needed before Update.begin()
constexpr uint32_t OTA_IDLE_MAX_MS = 30000;       // no chunk for this long = upload gone

struct OtaUpdateOps {
    bool        (*begin)();                                // Update.begin(UPDATE_SIZE_UNKNOWN)
    bool        (*write)(const uint8_t* d, size_t n);      // all n bytes written
    bool        (*end)();                                  // Update.end(true)
    void        (*abort)();
    const char* (*error)();                                // Update.errorString()
    bool        (*readPartitionTable)(uint8_t* buf, size_t len);   // the device's own table at 0x8000
    size_t      (*largestFreeBlock)();
    uint32_t    (*now)();
    void        (*log)(const char* tag, const char* msg);
};

enum OtaOutcome : uint8_t {
    OTA_OUT_NONE = 0,       // no upload in this request
    OTA_OUT_OK,             // written and verified: restart
    OTA_OUT_NOT_AUTHORIZED, // login missing (the body was discarded)
    OTA_OUT_REFUSED,        // not a matching firmware image / low memory - nothing written
    OTA_OUT_FAILED,         // Update failed (flash / verification) - see otaUploadError()
};

void        otaUploadStart(const OtaUpdateOps& ops, bool authorized, const char* tag);
void        otaUploadChunk(const uint8_t* d, size_t n);
void        otaUploadFinish();
void        otaUploadAbort();
OtaOutcome  otaUploadOutcome();
const char* otaUploadError();       // reason for REFUSED / FAILED ("" otherwise)
bool        otaUploadIsFactory();   // the -factory image's application part was written
bool        otaUploadActive();      // an upload is being received right now
void        otaUploadReset();       // after the answer: back to OTA_OUT_NONE

// device: Update / esp_flash_read / heap (ota_upload_esp.cpp)
const OtaUpdateOps& otaEspOps();
