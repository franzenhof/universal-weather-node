// Settings backup (src/settings_backup.cpp): header, format check, secret
// omission, value conversion and the restore comparison / round trip.
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
                           "wowpin", "nttok", "potok", "pouser", "tgtok", "whurl", "cu0url", "cu1url" })
        assert(backupIsSecret(k));
    assert(!backupIsSecret("mqttuser") && !backupIsSecret("deveui") && !backupIsSecret("wifissid"));
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
    deserializeJson(bad, "{\"format\":\"uwn-settings\",\"version\":2,\"settings\":{}}");
    assert(backupCheck(bad) == BK_NEWER);

    char v[8];
    backupValue(JsonVariantConst(), v, sizeof(v));
    assert(v[0] == '\0');
    puts("backup ok");
    return 0;
}
