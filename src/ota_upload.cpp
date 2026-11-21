#include "ota_upload.h"
#include "ota_check.h"
#include <atomic>
#include <new>
#include <stdlib.h>

namespace {
const OtaUpdateOps*   ops      = nullptr;
OtaUploadFilter*      filter   = nullptr;   // heap, only during an upload (~3.3 KB)
uint8_t*              devicePt = nullptr;   // the device's partition table, same
const char*           tag      = "OTA";
OtaOutcome            outcome  = OTA_OUT_NONE;
const char*           error    = "";
bool                  factory  = false;
bool                  writing  = false;    // Update.begin() succeeded, not yet ended / aborted
bool                  writeErr = false;    // the last ops->write() failed (flash, not the file)
std::atomic<bool>     active{false};
std::atomic<uint32_t> lastMs{0};

void log(const char* msg) { if (ops && ops->log) ops->log(tag, msg); }

void release()
{
    delete filter;  filter = nullptr;
    free(devicePt); devicePt = nullptr;
    active.store(false);
}

void refuse(OtaOutcome o, const char* why)
{
    if (outcome == OTA_OUT_REFUSED || outcome == OTA_OUT_FAILED) return;   // the first reason counts
    outcome = o;
    error   = why ? why : "";
    if (writing) { ops->abort(); writing = false; }
    log(why);
    release();
}

bool writeFn(const uint8_t* d, size_t n, void*)
{
    if (ops->write(d, n)) return true;
    writeErr = true;
    return false;
}
} // namespace

void otaUploadStart(const OtaUpdateOps& o, bool authorized, const char* t)
{
    if (writing && ops) ops->abort();   // a previous upload that never ended
    release();
    ops = &o; tag = t ? t : "OTA";
    outcome = OTA_OUT_NONE; error = ""; factory = false; writing = false;
    if (!authorized) { outcome = OTA_OUT_NOT_AUTHORIZED; log("upload refused (not authenticated)"); return; }

    active.store(true);
    lastMs.store(ops->now());
    // memory first: Update keeps a 4 KB sector buffer, the filter ~3.3 KB, the
    // partition table 3 KB, plus the web server's request buffers
    if (ops->largestFreeBlock() < OTA_MIN_LARGEST) { refuse(OTA_OUT_REFUSED, "not enough free memory - try again after a restart"); return; }
    filter   = new (std::nothrow) OtaUploadFilter();
    devicePt = (uint8_t*)malloc(OTA_FACTORY_PT_LEN);
    if (!filter) { refuse(OTA_OUT_REFUSED, "out of memory"); return; }
    const bool ptOk = devicePt && ops->readPartitionTable(devicePt, OTA_FACTORY_PT_LEN);
    filter->begin(ptOk ? devicePt : nullptr);
    if (!ops->begin()) { refuse(OTA_OUT_FAILED, ops->error()); return; }
    writing = true;
    log("upload started");
}

void otaUploadChunk(const uint8_t* d, size_t n)
{
    if (!writing || !filter) return;   // refused / not authorized: the rest of the body is discarded
    lastMs.store(ops->now());
    writeErr = false;
    if (!filter->feed(d, n, writeFn, nullptr)) {
        if (writeErr) refuse(OTA_OUT_FAILED, ops->error());   // Update.write() failed: a flash error, not the file
        else          refuse(OTA_OUT_REFUSED, filter->error());
    }
}

void otaUploadFinish()
{
    if (!writing || !filter) { release(); return; }
    if (!filter->finish()) { refuse(OTA_OUT_REFUSED, filter->error()); return; }
    factory = filter->isFactory();
    writing = false;
    if (ops->end()) {
        outcome = OTA_OUT_OK;
        log(factory ? "application part of the factory image written" : "application image written");
    } else {
        outcome = OTA_OUT_FAILED;
        error   = ops->error();
        log(error);
    }
    release();
}

void otaUploadAbort()
{
    if (writing && ops) { ops->abort(); writing = false; }
    if (outcome == OTA_OUT_NONE) { outcome = OTA_OUT_FAILED; error = "upload aborted"; log(error); }
    release();
}

OtaOutcome  otaUploadOutcome()   { return outcome; }
const char* otaUploadError()     { return error; }
bool        otaUploadIsFactory() { return factory; }
void        otaUploadReset()     { if (!writing) { outcome = OTA_OUT_NONE; error = ""; } }

bool otaUploadActive()
{
    if (!active.load()) return false;
    if (ops && ops->now() - lastMs.load() > OTA_IDLE_MAX_MS) return false;   // connection gone without ABORTED
    return true;
}
