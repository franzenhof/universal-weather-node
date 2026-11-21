#include "settings_backup.h"
#include <stdio.h>
#include <string.h>

bool backupIsSecret(const char* name)
{
    // write-only fields, and URLs that may carry a key (webhook, custom uploads)
    static const char* const secrets[] = { "webadminpw", "wifipass", "mqttpass", "appkey", "awkpass", "wukey",
                                           "wckey", "wnpkey", "windypw", "osemtok", "wowpin", "nttok", "potok",
                                           "pouser", "tgtok", "whurl", "cu0url", "cu1url" };
    for (const char* s : secrets) if (!strcmp(s, name)) return true;
    return false;
}

void backupHeader(JsonDocument& doc, const char* firmware, const char* host, const char* date, bool secrets)
{
    doc["format"]   = "uwn-settings";
    doc["version"]  = SETTINGS_BACKUP_VERSION;
    doc["firmware"] = firmware;
    doc["host"]     = host;
    doc["date"]     = date;
    doc["secrets"]  = secrets;
}

BackupCheck backupCheck(JsonDocument& in)
{
    const int ver = in["version"] | 0;
    if (strcmp(in["format"] | "", "uwn-settings") != 0 || !in["settings"].is<JsonObject>() || ver < 1) return BK_INVALID;
    return ver > SETTINGS_BACKUP_VERSION ? BK_NEWER : BK_OK;
}

void backupValue(JsonVariantConst v, char* out, size_t len)
{
    if (!len) return;
    out[0] = '\0';
    if (v.is<const char*>())      snprintf(out, len, "%s", v.as<const char*>());
    else if (v.is<bool>())        snprintf(out, len, "%d", v.as<bool>() ? 1 : 0);
    else if (v.is<long>())        snprintf(out, len, "%ld", v.as<long>());
    else if (v.is<double>())      snprintf(out, len, "%g", v.as<double>());
}

int backupPrepare(JsonObject set, JsonObjectConst cur, JsonArray changes, bool (*secretSame)(const char*, const char*))
{
    const char* drop[96];
    size_t ndrop = 0;
    int n = 0;
    char num[32];
    for (JsonPair kv : set) {
        const char* key = kv.key().c_str();
        const bool secret = backupIsSecret(key);
        if (cur[key].isNull() && !secret) {
            if (ndrop < sizeof(drop) / sizeof(drop[0])) drop[ndrop++] = key;
            continue;
        }
        // strings as they are (the CA certificate is long), anything else as the form sends it
        const char* v = kv.value().as<const char*>();
        if (!v) { backupValue(kv.value(), num, sizeof(num)); v = num; }
        if (secret) {
            if (!v[0] || (secretSame && secretSame(key, v))) continue;   // empty: keeps the stored one
            JsonObject c = changes.add<JsonObject>();
            c["key"] = key; c["secret"] = true;
            n++;
            continue;
        }
        const char* old = cur[key] | "";
        if (!strcmp(v, old)) continue;
        JsonObject c = changes.add<JsonObject>();
        c["key"] = key; c["old"] = old; c["new"] = v;
        n++;
    }
    for (size_t i = 0; i < ndrop; i++) set.remove(drop[i]);
    return n;
}
