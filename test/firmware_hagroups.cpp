// Home Assistant value groups (src/ha_groups.h): announced once seen, gone only with source Off.
#include "ha_groups.h"
#include <cassert>
#include <cstdio>

int main()
{
    const bool none[6] = {false, false, false, false, false, false};
    HaGroupsNow n;
    assert(haGroupsDelivering(n) == 0);

    // an IT+ temperature-only sensor: temperature, no humidity
    n.temp = true;
    uint8_t seen = haGroupsDelivering(n);
    assert(seen == HG_TEMP && haGroupsWanted(seen, none) == HG_TEMP);

    // a weather station joins later: the new groups add up, nothing seen is lost
    HaGroupsNow later;
    later.hum = later.wind = later.rain = true;
    seen |= haGroupsDelivering(later);
    assert(seen == (HG_TEMP | HG_HUM | HG_WIND | HG_RAIN));
    // the station falls silent: still announced (seen is sticky)
    seen |= haGroupsDelivering(HaGroupsNow());
    assert(haGroupsWanted(seen, none) == (HG_TEMP | HG_HUM | HG_WIND | HG_RAIN));

    // source Off removes the group; air Off takes temperature and humidity
    const bool windOff[6] = {false, true, false, false, false, false};
    assert(haGroupsWanted(seen, windOff) == (HG_TEMP | HG_HUM | HG_RAIN));
    const bool airOff[6] = {true, false, false, false, false, false};
    assert(haGroupsWanted(seen, airOff) == (HG_WIND | HG_RAIN));

    // every group maps to its own bit
    HaGroupsNow all;
    all.temp = all.hum = all.wind = all.rain = all.uv = all.light = all.press = true;
    assert(haGroupsDelivering(all) == 127);
    const bool allOff[6] = {true, true, true, true, true, true};
    assert(haGroupsWanted(127, allOff) == 0);

    puts("HA groups ok");
    return 0;
}
