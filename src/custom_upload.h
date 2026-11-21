#pragma once
// Custom HTTP uploads (docs/weather-networks.md "Custom upload"): up to
// CUSTOM_UPLOADS targets, each a URL template, GET or POST with an optional
// body template, and an interval. Placeholders: placeholders.h / ph_live.h
// (URL-encoded in the URL, JSON-escaped in a body that starts with { or [).
// Stored in their own NVS blob (namespace "cupload"); sent by the upload
// task (wx_upload.cpp) with the 10-min wind like the weather networks.
#include <stdint.h>

constexpr int CUSTOM_UPLOADS = 2;

struct CustomUpload {
    char     url[256] = "";       // "" = off; treated as a secret in the backup (may carry a key)
    char     body[256] = "";      // POST body template ("" with POST = empty body)
    uint8_t  post = 0;
    uint16_t intervalSec = 300;   // 60 - 3600
};

void customUploadSetup();
void customUploadLock();
void customUploadUnlock();
CustomUpload* customUploads();    // CUSTOM_UPLOADS entries, under the lock
void customUploadSave();          // under the lock
void customUploadsConfigured(bool* on);   // CUSTOM_UPLOADS entries: URL set
