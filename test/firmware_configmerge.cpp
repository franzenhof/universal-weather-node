// The settings POST's restart fields reach the live settings without
// reverting a downlink / MQTT change made meanwhile (src/config_merge.cpp).
#include "config_merge.h"
#include <cassert>
#include <cstring>
#include <memory>

int main()
{
    std::unique_ptr<Config> live(new Config()), before, after;
    strcpy(live->mqttHost, "old.broker");
    live->uplinkIntervalSec = 300;
    before.reset(new Config(*live));
    after.reset(new Config(*live));
    // the POST: another broker (a restart field) and a longer device name
    strcpy(after->mqttHost, "broker.lan");
    strcpy(after->deviceName, "Garden station");
    // meanwhile a downlink changes the uplink interval on the live settings
    live->uplinkIntervalSec = 900;
    configApplyChanges(*live, *before, *after);
    assert(!strcmp(live->mqttHost, "broker.lan") && !strcmp(live->deviceName, "Garden station"));
    assert(live->uplinkIntervalSec == 900);   // not reverted to 300
    // nothing changed by the POST: live stays as it is
    Config same(*live);
    live->sampleSec = 3;
    configApplyChanges(*live, same, same);
    assert(live->sampleSec == 3 && !strcmp(live->mqttHost, "broker.lan"));
    // a shorter value: the old tail bytes go too (the string ends at the new terminator)
    before.reset(new Config(*live));
    after.reset(new Config(*live));
    strcpy(after->mqttHost, "b");
    configApplyChanges(*live, *before, *after);
    assert(!strcmp(live->mqttHost, "b"));
    // both change the same number: one whole value wins, never a mix of bytes
    live->davisOffsetHz = 256;
    before.reset(new Config(*live));
    after.reset(new Config(*live));
    after->davisOffsetHz = 512;   // the POST
    live->davisOffsetHz = 257;    // meanwhile a downlink
    configApplyChanges(*live, *before, *after);
    assert(live->davisOffsetHz == 512);
    return 0;
}
