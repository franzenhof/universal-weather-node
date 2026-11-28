#pragma once
// Home Assistant discovery of the value groups a wireless sensor may feed
// (temperature, humidity, wind, rain, UV, light, pressure): announced only
// once a source has delivered the group. "Seen" is remembered in NVS
// (config/haseen, loadHaGroupsSeen()): after a restart the entities are
// announced at once instead of being removed until the first packet - a
// removed config deletes HA's registry entry with the user's names and areas.
// A group goes again only when its source is set to Off (or by a reset).
// Header-only, host-tested (test/firmware_hagroups.cpp).
#include <stdint.h>

enum HaGroupBit : uint8_t {
    HG_TEMP = 1, HG_HUM = 2, HG_WIND = 4, HG_RAIN = 8, HG_UV = 16, HG_LIGHT = 32, HG_PRESS = 64,
};

struct HaGroupsNow {   // what the sources deliver in this cycle
    bool temp = false, hum = false, wind = false, rain = false, uv = false, light = false, press = false;
};

inline uint8_t haGroupsDelivering(const HaGroupsNow& n)
{
    return (uint8_t)((n.temp ? HG_TEMP : 0) | (n.hum ? HG_HUM : 0) | (n.wind ? HG_WIND : 0) | (n.rain ? HG_RAIN : 0) |
                     (n.uv ? HG_UV : 0) | (n.light ? HG_LIGHT : 0) | (n.press ? HG_PRESS : 0));
}

// The groups to announce: seen and their source not Off. srcOff[] in the
// order of SrcGroup (sources.h): air (temperature + humidity), wind, rain, UV,
// light, pressure.
inline uint8_t haGroupsWanted(uint8_t seen, const bool srcOff[6])
{
    uint8_t off = 0;
    if (srcOff[0]) off |= HG_TEMP | HG_HUM;
    if (srcOff[1]) off |= HG_WIND;
    if (srcOff[2]) off |= HG_RAIN;
    if (srcOff[3]) off |= HG_UV;
    if (srcOff[4]) off |= HG_LIGHT;
    if (srcOff[5]) off |= HG_PRESS;
    return (uint8_t)(seen & ~off);
}

// NVS (config.cpp); 0 = none seen yet (also a missing key)
uint8_t loadHaGroupsSeen();
void    saveHaGroupsSeen(uint8_t seen);
