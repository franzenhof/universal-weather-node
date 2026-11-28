#include "notify.h"
#include <stdio.h>
#include <string.h>

namespace {
// appends s JSON-escaped (without quotes)
void jsonStr(char* out, size_t len, const char* s)
{
    size_t n = strlen(out);
    for (; s && *s && n + 7 < len; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, len - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = '\0';
}

void urlStr(char* out, size_t len, const char* s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(out);
    for (; s && *s && n + 4 < len; s++) {
        const unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.' || c == '~')
            out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = '\0';
}

void cat(char* out, size_t len, const char* s) { strlcat(out, s ? s : "", len); }
}

bool notifyNtfy(const AlarmChannels& ch, const NotifyMsg& m, NotifyReq& r)
{
    if (!ch.ntfyTopic[0]) return false;
    r = NotifyReq();
    r.post = true;
    // JSON publishing to the server root (title / tags need no header encoding)
    snprintf(r.url, sizeof(r.url), "%s", ch.ntfyUrl[0] ? ch.ntfyUrl : "https://ntfy.sh");
    size_t n = strlen(r.url);
    while (n && r.url[n - 1] == '/') r.url[--n] = '\0';
    cat(r.url, sizeof(r.url), "/");
    strcpy(r.contentType, "application/json");
    if (ch.ntfyToken[0]) snprintf(r.auth, sizeof(r.auth), "Bearer %s", ch.ntfyToken);
    cat(r.body, sizeof(r.body), "{\"topic\":\"");   jsonStr(r.body, sizeof(r.body), ch.ntfyTopic);
    cat(r.body, sizeof(r.body), "\",\"title\":\""); jsonStr(r.body, sizeof(r.body), m.title);
    cat(r.body, sizeof(r.body), "\",\"message\":\""); jsonStr(r.body, sizeof(r.body), m.text);
    char p[24];
    snprintf(p, sizeof(p), "\",\"priority\":%d", m.priority < 1 ? 1 : m.priority > 5 ? 5 : m.priority);
    cat(r.body, sizeof(r.body), p);
    if (ch.ntfyTags[0]) {   // "warning,cloud" -> ["warning","cloud"]
        cat(r.body, sizeof(r.body), ",\"tags\":[");
        char tags[sizeof(ch.ntfyTags)];
        strlcpy(tags, ch.ntfyTags, sizeof(tags));
        bool first = true;
        for (char* t = strtok(tags, ", "); t; t = strtok(nullptr, ", ")) {
            cat(r.body, sizeof(r.body), first ? "\"" : ",\"");
            jsonStr(r.body, sizeof(r.body), t);
            cat(r.body, sizeof(r.body), "\"");
            first = false;
        }
        cat(r.body, sizeof(r.body), "]");
    }
    cat(r.body, sizeof(r.body), "}");
    return true;
}

int notifyPushoverPriority(uint8_t p) { return p <= 1 ? -2 : p >= 5 ? 2 : (int)p - 3; }

bool notifyPushover(const AlarmChannels& ch, const NotifyMsg& m, NotifyReq& r)
{
    if (!ch.poToken[0] || !ch.poUser[0]) return false;
    r = NotifyReq();
    r.post = true;
    strcpy(r.url, "https://api.pushover.net/1/messages.json");
    strcpy(r.contentType, "application/x-www-form-urlencoded");
    cat(r.body, sizeof(r.body), "token=");    urlStr(r.body, sizeof(r.body), ch.poToken);
    cat(r.body, sizeof(r.body), "&user=");    urlStr(r.body, sizeof(r.body), ch.poUser);
    cat(r.body, sizeof(r.body), "&title=");   urlStr(r.body, sizeof(r.body), m.title);
    cat(r.body, sizeof(r.body), "&message="); urlStr(r.body, sizeof(r.body), m.text);
    const int prio = notifyPushoverPriority(m.priority);
    char p[64];
    snprintf(p, sizeof(p), "&priority=%d", prio);
    cat(r.body, sizeof(r.body), p);
    if (prio == 2) {   // emergency: repeated until acknowledged (retry >= 30 s, expire <= 10800 s)
        snprintf(p, sizeof(p), "&retry=%u&expire=%u", ch.poRetrySec < 30 ? 30u : (unsigned)ch.poRetrySec,
                 ch.poExpireSec > 10800 ? 10800u : (unsigned)ch.poExpireSec);
        cat(r.body, sizeof(r.body), p);
    }
    return true;
}

bool notifyTelegram(const AlarmChannels& ch, const NotifyMsg& m, NotifyReq& r)
{
    if (!ch.tgToken[0] || !ch.tgChat[0]) return false;
    r = NotifyReq();
    r.post = true;
    snprintf(r.url, sizeof(r.url), "https://api.telegram.org/bot%s/sendMessage", ch.tgToken);
    strcpy(r.contentType, "application/json");
    cat(r.body, sizeof(r.body), "{\"chat_id\":\""); jsonStr(r.body, sizeof(r.body), ch.tgChat);
    cat(r.body, sizeof(r.body), "\",\"text\":\""); jsonStr(r.body, sizeof(r.body), m.title);
    cat(r.body, sizeof(r.body), ": ");             jsonStr(r.body, sizeof(r.body), m.text);
    cat(r.body, sizeof(r.body), m.priority <= 2 ? "\",\"disable_notification\":true}" : "\"}");
    return true;
}

bool notifyWebhook(const AlarmChannels& ch, PhLookup lookup, void* ctx, NotifyReq& r)
{
    if (!ch.whUrl[0]) return false;
    r = NotifyReq();
    bool cut = false;
    phRender(ch.whUrl, r.url, sizeof(r.url), PE_URL, lookup, ctx, &cut);
    r.truncated = cut;
    if (ch.whPost) {
        r.post = true;
        strcpy(r.contentType, "application/json");
        phRender(ch.whBody, r.body, sizeof(r.body), PE_JSON, lookup, ctx, &cut);
        r.truncated = r.truncated || cut;
    }
    return true;
}

bool notifyOk(uint8_t, int code) { return code >= 200 && code < 300; }
