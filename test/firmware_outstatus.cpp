// Send status per output (src/out_status.cpp): bookkeeping and JSON.
#include "Arduino.h"
#include "out_status.h"
#include <cassert>
#include <cstring>
#include <string>

int main()
{
    OutStatus s;
    outStatusFail(s, 1000, 401, "Unauthorized\r\nmore text that is far too long for the fixed buffer");
    outStatusFail(s, 2000, -1, "connection refused  ");
    assert(s.hasErr && !s.hasOk && s.fails == 2 && s.errMs == 2000 && s.code == -1);
    assert(std::string(s.err) == "connection refused");   // trailing blanks trimmed
    outStatusFail(s, 2500, 500, "line1\nline2");
    assert(std::string(s.err) == "line1 line2");           // no control characters
    outStatusOk(s, 3000);
    assert(s.hasOk && s.fails == 0 && s.okMs == 3000 && s.hasErr && s.code == 500);
    outStatusFail(s, 4000, 0, "x");
    char longText[100];
    memset(longText, 'a', sizeof(longText) - 1); longText[99] = '\0';
    outStatusFail(s, 4000, 0, longText);
    assert(strlen(s.err) == sizeof(s.err) - 1 && s.fails == 2);

    JsonDocument doc;
    outStatusJson(doc.to<JsonObject>(), s, 9000);
    assert(doc["lastOkSecAgo"] == 6 && doc["lastErrorSecAgo"] == 5 && doc["failsInRow"] == 2);
    assert(doc["lastErrorCode"].isNull());                 // code 0: left out
    OutStatus none;
    doc.clear();
    outStatusJson(doc.to<JsonObject>(), none, 9000);
    assert(doc.size() == 1 && doc["failsInRow"] == 0);

    // device part: thread-safe report + snapshot, keys
    outReport(OUT_MQTT, false, -2, "connect failed");
    outReport(OUT_COUNT, true);                            // out of range: ignored
    OutStatus all[OUT_COUNT];
    outSnapshot(all);
    assert(all[OUT_MQTT].fails == 1 && all[OUT_MQTT].code == -2 && !all[OUT_LORAWAN].hasErr);
    assert(std::string(outStatusKey(OUT_HA)) == "haDiscovery" && std::string(outStatusKey(OUT_WX_FIRST + 5)) == "opensensemap");
    assert(std::string(outStatusKey(OUT_INFLUX)) == "influxdb" && OUT_INFLUX == OUT_COUNT - 1);

    puts("outstatus ok");
    return 0;
}
