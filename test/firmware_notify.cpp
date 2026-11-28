// Notification requests (src/notify.cpp): ntfy, Pushover, Telegram and
// webhook payloads; unconfigured channels; escaping.
#include "notify.h"
#include <cassert>
#include <cstring>
#include <string>

Config gCfg;

static bool lookup(const char* n, PhValue& o, void*)
{
    if (!strcmp(n, "name"))  { o.kind = PK_TEXT; o.text = "Frost \"A\""; return true; }
    if (!strcmp(n, "value")) { o.kind = PK_TEXT; o.text = "-1.5"; return true; }
    if (!strcmp(n, "long"))  { static const std::string t(100, 'x'); o.kind = PK_TEXT; o.text = t.c_str(); return true; }
    return false;
}

int main()
{
    AlarmChannels ch;
    NotifyReq r;
    const NotifyMsg m{ "Frost", "Garden: -1.5 \"C\"\nok", 4 };
    // nothing configured: no request
    assert(!notifyNtfy(ch, m, r) && !notifyPushover(ch, m, r) && !notifyTelegram(ch, m, r) && !notifyWebhook(ch, lookup, nullptr, r));

    // ntfy: JSON publish to the server root, tags as array, bearer token
    strcpy(ch.ntfyUrl, "https://ntfy.example.org/");
    strcpy(ch.ntfyTopic, "garden");
    strcpy(ch.ntfyToken, "tk_abc");
    strcpy(ch.ntfyTags, "warning, snowflake");
    assert(notifyNtfy(ch, m, r) && r.post);
    assert(std::string(r.url) == "https://ntfy.example.org/" && std::string(r.auth) == "Bearer tk_abc");
    assert(std::string(r.body) == "{\"topic\":\"garden\",\"title\":\"Frost\",\"message\":\"Garden: -1.5 \\\"C\\\"\\nok\",\"priority\":4,\"tags\":[\"warning\",\"snowflake\"]}");

    // Pushover: form-encoded, priority mapping, emergency with retry / expire
    strcpy(ch.poToken, "apptoken"); strcpy(ch.poUser, "userkey");
    assert(notifyPushover(ch, m, r) && std::string(r.contentType) == "application/x-www-form-urlencoded");
    assert(std::string(r.url) == "https://api.pushover.net/1/messages.json");
    assert(std::string(r.body) == "token=apptoken&user=userkey&title=Frost&message=Garden%3A%20-1.5%20%22C%22%0Aok&priority=1");
    const NotifyMsg urgent{ "Leak", "water", 5 };
    ch.poRetrySec = 10; ch.poExpireSec = 20000;   // clamped to 30 / 10800
    assert(notifyPushover(ch, urgent, r) && std::string(r.body).find("&priority=2&retry=30&expire=10800") != std::string::npos);
    assert(notifyPushoverPriority(1) == -2 && notifyPushoverPriority(2) == -1 && notifyPushoverPriority(3) == 0);

    // Telegram: token in the URL path, quiet for low priority
    strcpy(ch.tgToken, "123:ABC"); strcpy(ch.tgChat, "-1001");
    assert(notifyTelegram(ch, m, r));
    assert(std::string(r.url) == "https://api.telegram.org/bot123:ABC/sendMessage");
    assert(std::string(r.body) == "{\"chat_id\":\"-1001\",\"text\":\"Frost: Garden: -1.5 \\\"C\\\"\\nok\"}");
    const NotifyMsg low{ "Info", "x", 2 };
    assert(notifyTelegram(ch, low, r) && std::string(r.body).find("\"disable_notification\":true") != std::string::npos);

    // webhook: GET with URL-encoded placeholders; POST with JSON-escaped body
    strcpy(ch.whUrl, "http://h.local/hook?a={name}&v={value}&x={unknown}");
    assert(notifyWebhook(ch, lookup, nullptr, r) && !r.post);
    assert(std::string(r.url) == "http://h.local/hook?a=Frost%20%22A%22&v=-1.5&x=");
    ch.whPost = 1;
    strcpy(ch.whBody, "{\"alarm\":\"{name}\",\"value\":{value}}");
    assert(notifyWebhook(ch, lookup, nullptr, r) && r.post && std::string(r.contentType) == "application/json");
    assert(std::string(r.body) == "{\"alarm\":\"Frost \\\"A\\\"\",\"value\":-1.5}");
    assert(!r.truncated);
    // a body that does not fit is flagged (the sender refuses it instead of sending cut JSON)
    strcpy(ch.whBody, "{\"a\":\"{long}{long}{long}{long}{long}{long}{long}{long}\"}");   // 8 x 100 > 640
    assert(notifyWebhook(ch, lookup, nullptr, r) && r.truncated);
    assert(notifyOk(AC_NTFY, 200) && !notifyOk(AC_NTFY, 401) && !notifyOk(AC_NTFY, -1));
    puts("notify ok");
    return 0;
}
