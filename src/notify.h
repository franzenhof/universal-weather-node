#pragma once
// Notification requests (docs/alarms.md): ntfy, Pushover, Telegram and the
// generic webhook, built into one fixed-size request that the upload task
// sends (wx_upload.cpp). Host-tested (test/firmware_notify.cpp). The URL
// may carry a token (Telegram, webhook): never log it.
#include <stddef.h>
#include <stdint.h>
#include "alarms.h"
#include "placeholders.h"

struct NotifyReq {
    bool post = false;
    char url[320] = "";
    char contentType[40] = "";
    char auth[80] = "";          // Authorization header value, "" = none
    char body[640] = "";
    bool truncated = false;      // a rendered URL / body did not fit: not to be sent (it would be cut, e.g. invalid JSON)
};

// One message: title (rule name), text (rendered), priority 1-5
struct NotifyMsg {
    const char* title;
    const char* text;
    uint8_t     priority;
};

// false = the channel is not configured
bool notifyNtfy(const AlarmChannels& ch, const NotifyMsg& m, NotifyReq& out);
bool notifyPushover(const AlarmChannels& ch, const NotifyMsg& m, NotifyReq& out);
bool notifyTelegram(const AlarmChannels& ch, const NotifyMsg& m, NotifyReq& out);
// URL and body templates rendered with lookup (placeholders.h): URL-encoded
// in the URL, JSON-escaped in the body (POST, application/json)
bool notifyWebhook(const AlarmChannels& ch, PhLookup lookup, void* ctx, NotifyReq& out);
// Pushover priority for 1-5: -2, -1, 0, 1, 2 (2 = emergency, needs retry / expire)
int notifyPushoverPriority(uint8_t p);
// The reply counts as success for this channel
bool notifyOk(uint8_t channel, int httpCode);
