// Network-facing name checks: MQTT client id / Home Assistant ids
// (wifi_mqtt.h: mqttSafeId) and update-server version names
// (wx_upload.h: wxValidVersionTag).
#include "wifi_mqtt.h"
#include "wx_upload.h"
#include <cassert>
#include <cstring>

static bool safeIs(const char* in, const char* expected, size_t outLen = 40)
{
    char out[40];
    mqttSafeId(in, out, outLen);
    return strcmp(out, expected) == 0;
}

int main()
{
    // already valid names stay identical (existing HA entities keep their ids)
    assert(safeIs("weather-node_01", "weather-node_01"));
    assert(safeIs("Garden", "Garden"));
    // spaces, topic separators/wildcards and other characters become '_'
    assert(safeIs("my station", "my_station"));
    assert(safeIs("a/b+c#d", "a_b_c_d"));
    assert(safeIs("K\xC3\xA4" "fer", "K__fer"));
    assert(safeIs("", ""));
    // bounded by the output buffer
    assert(safeIs("abcdefgh", "abc", 4));

    // version names from the update server
    assert(wxValidVersionTag("2.0.0-beta.3"));
    assert(wxValidVersionTag("v2.1.0+build.7"));
    assert(!wxValidVersionTag(""));
    assert(!wxValidVersionTag(nullptr));
    assert(!wxValidVersionTag("2.0<script>"));
    assert(!wxValidVersionTag("2.0 1"));
    assert(!wxValidVersionTag("1.0'"));
    return 0;
}
