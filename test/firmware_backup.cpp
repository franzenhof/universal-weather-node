// Settings backup (src/settings_backup.cpp): header, format check, secret
// omission, value conversion and the restore comparison / round trip;
// version 2: the logical sensor table, version 1 read without its references.
#include "settings_backup.h"
#include <cassert>
#include <cstring>
#include <string>

static bool sameSecret(const char* key, const char* v) { return !strcmp(key, "mqttpass") && !strcmp(v, "same"); }

int main()
{
    // a backup as web_api.cpp writes it (fields recorded from the settings page)
    JsonDocument b;
    backupHeader(b, "2.0.0", "garden", "2026-11-20", false);
    JsonObject s = b["settings"].to<JsonObject>();
    s["devname"] = "Garden"; s["unitsys"] = "0"; s["caltemp"] = "-0.4"; s["rainen"] = "1";
    s["mqttcacert"] = std::string(1500, 'A').c_str();
    assert(backupCheck(b) == BK_OK);
    // the secret list: every write-only field of the settings page
    for (const char* k : { "webadminpw", "wifipass", "mqttpass", "appkey", "awkpass", "wukey", "wckey", "wnpkey", "windypw", "osemtok",
                           "wowpin", "nttok", "potok", "pouser", "tgtok", "whurl", "cu0url", "cu1url", "appass" })
        assert(backupIsSecret(k));
    assert(!backupIsSecret("mqttuser") && !backupIsSecret("deveui") && !backupIsSecret("wifissid") && !backupIsSecret("apssid"));
    assert(!backupIsSecret("nttopic") && !backupIsSecret("tgchat") && !backupIsSecret("whbody") && !backupIsSecret("cu0body"));

    // round trip: serialized, parsed back, compared with the same settings -> nothing changes
    std::string text;
    serializeJsonPretty(b, text);
    assert(text.find("\"secrets\": false") != std::string::npos);
    JsonDocument in;
    assert(!deserializeJson(in, text) && backupCheck(in) == BK_OK);
    JsonDocument changes;
    assert(backupPrepare(in["settings"].as<JsonObject>(), b["settings"].as<JsonObjectConst>(), changes.to<JsonArray>(), sameSecret) == 0);
    assert(changes.as<JsonArray>().size() == 0);

    // changes, values typed by hand (numbers, true), unknown fields dropped, secrets
    JsonDocument cur;
    cur.set(b["settings"]);
    in["settings"]["unitsys"] = 1;                 // number instead of "1"
    in["settings"]["rainen"] = false;              // bool instead of "0"
    in["settings"]["auxainen"] = "1";              // not on this node's page (no PCB v2.0)
    in["settings"]["mqttpass"] = "same";           // unchanged secret
    in["settings"]["wukey"] = "new-key";           // changed secret
    in["settings"]["awkpass"] = "";                // empty secret: keeps the stored one
    changes.clear();
    JsonObject set = in["settings"].as<JsonObject>();
    const int n = backupPrepare(set, cur.as<JsonObjectConst>(), changes.to<JsonArray>(), sameSecret);
    assert(n == 3);
    assert(set["auxainen"].isNull() && !set["wukey"].isNull());
    std::string seen;
    for (JsonObjectConst c : changes.as<JsonArrayConst>()) {
        seen += c["key"].as<std::string>() + ";";
        if (c["key"] == "wukey") assert(c["secret"] == true && c["new"].isNull());   // the secret itself is never listed
        if (c["key"] == "unitsys") assert(c["old"] == "0" && c["new"] == "1");
        if (c["key"] == "rainen") assert(c["old"] == "1" && c["new"] == "0");
    }
    assert(seen == "unitsys;rainen;wukey;");

    // refused: other format, missing settings, newer major version
    JsonDocument bad;
    deserializeJson(bad, "{\"format\":\"other\",\"version\":1,\"settings\":{}}");
    assert(backupCheck(bad) == BK_INVALID);
    deserializeJson(bad, "{\"format\":\"uwn-settings\",\"version\":1}");
    assert(backupCheck(bad) == BK_INVALID);
    deserializeJson(bad, "{\"format\":\"uwn-settings\",\"version\":3,\"settings\":{}}");
    assert(backupCheck(bad) == BK_NEWER);
    // versions 1 and 2 are read; a backup is written as version 2
    deserializeJson(bad, "{\"format\":\"uwn-settings\",\"version\":1,\"settings\":{}}");
    assert(backupCheck(bad) == BK_OK);
    deserializeJson(bad, "{\"format\":\"uwn-settings\",\"version\":2,\"settings\":{}}");
    assert(backupCheck(bad) == BK_OK);
    assert(b["version"] == 2 && SETTINGS_BACKUP_VERSION == 2);

    // ---- version 2: the logical sensor table (keys differ from the internal IDs) ----
    {
        LogicalSensorTable t;
        LogicalSensor e;
        strcpy(e.internalId, "L3F9A2C"); strcpy(e.customId, "garten"); strcpy(e.displayName, "Garten S\xC3\xBC" "d"); e.radioId = 0x17000008; e.minRssiDbm = -95;
        assert(lsAppend(t, e));
        e = LogicalSensor();
        strcpy(e.internalId, "L0A0B0C"); strcpy(e.customId, "900181A2"); e.radioId = 0x900181A2;
        assert(lsAppend(t, e));
        JsonDocument d;
        backupHeader(d, "4.0.0", "garden", "2026-11-26", false);
        backupSensors(d["sensors"].to<JsonArray>(), t);
        std::string txt;
        serializeJson(d, txt);
        assert(txt.find("\"internalId\":\"L3F9A2C\",\"customId\":\"garten\"") != std::string::npos);
        assert(txt.find("\"radioId\":\"17000008\",\"minRssiDbm\":-95") != std::string::npos);
        assert(txt.find("\"radioId\":\"900181A2\"}") != std::string::npos);   // no minimum set: none written
        JsonDocument r;
        assert(!deserializeJson(r, txt) && backupCheck(r) == BK_INVALID);        // no "settings" here
        LogicalSensorTable u;
        int skipped = -1;
        assert(backupReadSensors(r["sensors"], u, &skipped) && skipped == 0 && lsEqual(t, u));
        assert(u.s[0].minRssiDbm == -95 && !strcmp(u.s[1].customId, "900181A2") && u.s[1].minRssiDbm == 0);
        // damaged / duplicate entries are left out, the rest is kept
        deserializeJson(r, "[{\"internalId\":\"L3F9A2C\",\"customId\":\"garten\",\"displayName\":\"a|b\",\"radioId\":\"17000008\"},"
                           "{\"internalId\":\"L3F9A2C\",\"customId\":\"other\",\"radioId\":\"17000009\"},"     // internalId taken
                           "{\"internalId\":\"L111111\",\"customId\":\"GARTEN\",\"radioId\":\"17000010\"},"    // key taken
                           "{\"internalId\":\"L222222\",\"customId\":\"x\",\"radioId\":\"17000008\"},"         // radio ID taken
                           "{\"internalId\":\"garten\",\"customId\":\"y\",\"radioId\":\"17000011\"},"          // not an internal ID
                           "{\"internalId\":\"L333333\",\"customId\":\"temp\",\"radioId\":\"17000012\"},"      // key refused
                           "{\"internalId\":\"L444444\",\"customId\":\"z\",\"radioId\":\"zz\"},"               // radio ID damaged
                           "{\"internalId\":\"L666666\",\"customId\":\"abcdefghijklmnopq\",\"radioId\":\"17000014\"},"   // key of 17 characters: damaged, not cut
                           "{\"internalId\":\"L555555\",\"customId\":\"beet\",\"radioId\":\"17000013\",\"minRssiDbm\":-5}]");
        assert(backupReadSensors(r.as<JsonVariantConst>(), u, &skipped) && skipped == 7 && u.count() == 2);
        assert(!strcmp(u.s[0].displayName, "ab") && !strcmp(u.s[1].customId, "beet") && u.s[1].minRssiDbm == 0);   // name cleaned, minimum out of range
        assert(!backupReadSensors(JsonVariantConst(), u) && u.count() == 0);
    }

    // ---- version 1: references by radio ID are dropped (not converted), the rest stays ----
    {
        JsonDocument d;
        deserializeJson(d, "{\"srcair\":\"r17000008\",\"srcwind\":\"2\",\"srcrain\":\"0\",\"se0n\":\"Feuchte\",\"se0s\":\"510F651B\","
                           "\"se0f\":\"2\",\"se1n\":\"Probe\",\"se1s\":\"B\",\"se1f\":\"15\",\"se2s\":\"\",\"devname\":\"x\"}");
        backupDropV1Refs(d.as<JsonObject>());
        JsonObjectConst o = d.as<JsonObjectConst>();
        assert(o["srcair"].isNull() && o["srcwind"] == "2" && o["srcrain"] == "0");
        assert(o["se0n"].isNull() && o["se0s"].isNull() && o["se0f"].isNull());
        assert(o["se1s"] == "B" && o["se1n"] == "Probe" && o["se2s"] == "" && o["devname"] == "x");
    }

    char v[8];
    backupValue(JsonVariantConst(), v, sizeof(v));
    assert(v[0] == '\0');
    {   // every field the current page does not have is dropped - not only the first 96
        JsonDocument in2, cur2, changes2;
        JsonObject set = in2["settings"].to<JsonObject>();
        cur2["devname"] = "x";
        cur2["altitude"] = "560";
        set["devname"] = "y";
        char k[16];
        for (int i = 0; i < 120; i++) { snprintf(k, sizeof(k), "unk%d", i); set[k] = "1"; }
        set["altitude"] = "600";   // a known field after them
        set["wukey"] = "secret";   // secrets stay: the page renders them empty
        const int n = backupPrepare(set, cur2.as<JsonObjectConst>(), changes2.to<JsonArray>(), nullptr);
        assert(n == 3 && set.size() == 3);
        assert(!strcmp(set["devname"] | "", "y") && !strcmp(set["altitude"] | "", "600") && set["wukey"].is<const char*>());
    }
    puts("backup ok");
    return 0;
}
