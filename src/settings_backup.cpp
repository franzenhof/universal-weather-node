#include "settings_backup.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

bool backupIsSecret(const char* name)
{
    // write-only fields, and URLs that may carry a key (webhook, custom uploads)
    static const char* const secrets[] = { "webadminpw", "wifipass", "mqttpass", "appkey", "awkpass", "wukey",
                                           "wckey", "wnpkey", "windypw", "osemtok", "wowpin", "nttok", "potok",
                                           "pouser", "tgtok", "whurl", "cu0url", "cu1url", "influxtok", "httpskey",
                                           "appass" };
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
    return ver > SETTINGS_BACKUP_VERSION ? BK_NEWER : BK_OK;   // 1 and 2 are read
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
    auto notOnThisPage = [&](const char* key) { return cur[key].isNull() && !backupIsSecret(key); };
    int n = 0;
    char num[32];
    for (JsonPair kv : set) {
        const char* key = kv.key().c_str();
        if (notOnThisPage(key)) continue;   // dropped below
        const bool secret = backupIsSecret(key);
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
    // Drop every field the page does not have, however many: one at a time
    // (a removal invalidates the iterator), each search resuming at the
    // position of the last removal - the fields before it are known ones.
    size_t resumeAt = 0;
    for (;;) {
        size_t position = 0;
        JsonObject::iterator unknown = set.end();
        for (JsonObject::iterator it = set.begin(); it != set.end(); ++it, ++position) {
            if (position >= resumeAt && notOnThisPage(it->key().c_str())) { unknown = it; break; }
        }
        if (unknown == set.end()) break;
        set.remove(unknown);
        resumeAt = position;
    }
    return n;
}

void backupSensors(JsonArray a, const LogicalSensorTable& t)
{
    for (int i = 0; i < t.count(); i++) {
        const LogicalSensor& e = t.s[i];
        JsonObject o = a.add<JsonObject>();
        o["internalId"] = e.internalId;
        o["customId"] = e.customId;
        o["displayName"] = e.displayName;
        char id[12];
        snprintf(id, sizeof(id), "%08lX", (unsigned long)e.radioId);
        o["radioId"] = id;
        if (e.minRssiDbm) o["minRssiDbm"] = e.minRssiDbm;
    }
}

bool backupReadSensors(JsonVariantConst a, LogicalSensorTable& t, int* skipped)
{
    t = LogicalSensorTable();
    if (skipped) *skipped = 0;
    if (!a.is<JsonArrayConst>()) return false;
    for (JsonObjectConst o : a.as<JsonArrayConst>()) {
        LogicalSensor e;
        snprintf(e.internalId, sizeof(e.internalId), "%s", o["internalId"] | "");
        snprintf(e.customId, sizeof(e.customId), "%s", o["customId"] | "");
        snprintf(e.displayName, sizeof(e.displayName), "%s", o["displayName"] | "");
        char* end = nullptr;
        const char* rid = o["radioId"] | "";
        e.radioId = (uint32_t)strtoul(rid, &end, 16);
        const int mr = o["minRssiDbm"] | 0;
        e.minRssiDbm = (int8_t)(mr >= -140 && mr <= -20 ? mr : 0);   // RF_MINRSSI_LO..HI
        const bool customIdFits = strlen(o["customId"] | "") <= LS_CUSTOM_ID_MAX;   // a longer one is damaged, not cut to a valid one
        if (!customIdFits || (*rid && *end) || !e.radioId || !lsAppend(t, e)) { if (skipped) (*skipped)++; }
    }
    return true;
}

void backupDropV1Refs(JsonObject set)
{
    static const char* const src[] = { "srcair", "srcwind", "srcrain", "srcuv", "srclight", "srcpress" };
    for (const char* k : src) {
        const char* v = set[k] | "";
        if (v[0] == 'r') set.remove(k);
    }
    for (int i = 0; i < 32; i++) {
        char k[8];
        snprintf(k, sizeof(k), "se%ds", i);
        const char* v = set[k] | "";
        if (!v[0] || !strcmp(v, "B")) continue;
        set.remove(k);   // the whole slot: it keeps its current definition
        snprintf(k, sizeof(k), "se%dn", i); set.remove(k);
        snprintf(k, sizeof(k), "se%df", i); set.remove(k);
    }
}
