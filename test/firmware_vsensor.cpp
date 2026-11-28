// Virtual sensors (src/vsensor.h): ID derivation, field mapping -> encoder,
// loopback through foDecode (the frame the TX path sends decodes to the
// mapped values and the derived ID), scheduling, airtime budget, stored form,
// and the red-line guards that the type system can check.
#include "vsensor.h"
#include "vs_store.h"
#include "storage_layout.h"
#include "FineOffsetDecoder.h"
#include "ItPlusDecoder.h"
#include "rf_fsk17.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>

// ---- red line, structural ----
// (a) a frame cannot be made from bytes: no default / byte constructor, only vsBuildFrame()
static_assert(!std::is_default_constructible<VsFrame>::value, "VsFrame must not be constructible outside vsBuildFrame()");
static_assert(!std::is_constructible<VsFrame, const uint8_t*, size_t>::value, "no VsFrame from raw bytes");
static_assert(!std::is_constructible<VsFrame, const uint8_t*>::value, "no VsFrame from raw bytes");
// (a) the radio entry point takes nothing but a VsFrame
static_assert(std::is_same<decltype(&vsRadioSend), bool (*)(const VsFrame&)>::value, "vsRadioSend(const VsFrame&) only");
// (c) the builder takes no ID: it derives it (slot + MAC)
static_assert(std::is_same<decltype(&vsBuildFrame),
                           VsFrame (*)(const VsSlotCfg&, int, const uint8_t*, const VsValues&, bool)>::value,
              "vsBuildFrame must not take an ID");
// (c) the configuration has no ID field
template <typename T, typename = void> struct HasId : std::false_type {};
template <typename T> struct HasId<T, decltype((void)std::declval<T>().id, void())> : std::true_type {};
template <typename T, typename = void> struct HasSensorId : std::false_type {};
template <typename T> struct HasSensorId<T, decltype((void)std::declval<T>().sensorId, void())> : std::true_type {};
static_assert(!HasId<VsSlotCfg>::value && !HasSensorId<VsSlotCfg>::value, "VsSlotCfg must not carry an ID");

namespace {
struct Src { const char* name; float v; };
struct Table { const Src* s; int n; };
bool lookup(const char* name, float& out, void* ctx)
{
    const Table& t = *static_cast<const Table*>(ctx);
    for (int i = 0; i < t.n; i++) if (!strcmp(t.s[i].name, name)) { out = t.s[i].v; return true; }
    return false;
}
const uint8_t MAC[6] = { 0x24, 0xAB, 0xCD, 0x12, 0x34, 0x56 };

VsSlotCfg slot(uint8_t type, std::initializer_list<std::pair<int, const char*>> m, uint8_t ch = 1)
{
    VsSlotCfg c;
    c.type = type; c.enabled = 1; c.channel = ch;
    for (auto& p : m) snprintf(c.src[p.first], VS_SRC_LEN, "%s", p.second);
    return c;
}

// build + decode; returns the decoded frame
FoFrame loop(const VsSlotCfg& c, int s, const Table& t, uint32_t& tableId)
{
    VsValues v;
    assert(vsResolve(c, lookup, (void*)&t, v));
    VsFrame f = vsBuildFrame(c, s, MAC, v, false);
    assert(f.size() == VS_TYPES[c.type].frameLen && f.kbps() == VS_BITRATE_KBPS);
    FoFrame fo;
    assert(foDecode(f.data(), f.size(), fo));
    tableId = rfFineOffsetId(fo);
    assert(tableId == f.id());
    assert(tableId == vsTableId(c.type, vsDeriveId(MAC, s, c.type), c.channel));
    return fo;
}

// IT+: build + decode
ItPlusFrame loopIt(const VsSlotCfg& c, int s, const Table& t, bool pairing, uint32_t& tableId)
{
    VsValues v;
    assert(vsResolve(c, lookup, (void*)&t, v));
    VsFrame f = vsBuildFrame(c, s, MAC, v, pairing);
    assert(f.size() == ITPLUS_FRAME_LEN && f.size() == VS_TYPES[c.type].frameLen);
    assert(f.kbps() == (c.type == VS_ITPLUS17 ? 17.241f : 9.579f));
    ItPlusFrame it;
    assert(itPlusDecode(f.data(), it));
    // the receiver's table ID (rf_sensors.h) of what it decodes = the frame's ID
    tableId = rfItPlusId(c.type == VS_ITPLUS17, it.sensorId, it.hasHumidity);
    assert(tableId == f.id());
    assert(it.sensorId == vsDeriveId(MAC, s, c.type));
    return it;
}
} // namespace

int main()
{
    {   // off by default: the feature and every slot
        VsSettings def;
        assert(!def.enabled && def.txPowerDbm == VS_DBM_DEFAULT);
        for (int k = 0; k < VS_SLOTS; k++) assert(!def.slot[k].enabled && def.slot[k].type == VS_OFF);
    }
    {   // the store (vs_store.h): starts off, copies in and out whole
        VsSettings s;
        vsSettingsRead(s);
        assert(!s.enabled && !vsSwitchedOn() && vsTxPowerDbm() == VS_DBM_DEFAULT);
        s.enabled = 1; s.txPowerDbm = 3;
        s.slot[7] = slot(VS_WH31, { { VF_TEMP, "temp" } }, 8);
        vsSettingsWrite(s);
        assert(vsSwitchedOn() && vsTxPowerDbm() == 3);
        assert(vsSlotRead(7).type == VS_WH31 && vsSlotRead(7).channel == 8 && !strcmp(vsSlotRead(7).src[VF_TEMP], "temp"));
        assert(vsSlotRead(8).type == VS_OFF && vsSlotRead(-1).type == VS_OFF);   // outside the table: an off slot
        VsSettingsCopy c = vsSettingsCopy();
        assert(c && c->enabled && c->slot[7].type == VS_WH31);
        vsSettingsWrite(VsSettings());
        assert(!vsSwitchedOn());
    }

    // ---- ID: fixed, in range, different per slot, depends on the node ----
    for (int t = VS_WH31; t < VS_TYPE_COUNT; t++) {
        uint32_t ids[VS_SLOTS];
        for (int s = 0; s < VS_SLOTS; s++) {
            ids[s] = vsDeriveId(MAC, s, (uint8_t)t);
            assert(ids[s] == vsDeriveId(MAC, s, (uint8_t)t));
            if (t == VS_WH40) assert(ids[s] >= 1 && ids[s] <= 0xFFFFE);
            else if (t == VS_ITPLUS17 || t == VS_ITPLUS9) assert(ids[s] <= 63);
            else              assert(ids[s] >= 1 && ids[s] <= 254);
            for (int k = 0; k < s; k++) assert(ids[k] != ids[s]);
        }
    }
    {
        int diff = 0;
        for (int b = 0; b < 32; b++) {
            uint8_t m2[6]; memcpy(m2, MAC, 6); m2[5] = (uint8_t)(MAC[5] + 1 + b);
            if (vsDeriveId(m2, 0, VS_WH31) != vsDeriveId(MAC, 0, VS_WH31)) diff++;
        }
        assert(diff >= 30);   // another node: (almost always) another ID
    }

    // ---- loopback: mapping -> encoder -> foDecode = the mapped values + the derived ID ----
    const Src src[] = { { "temp", 21.4f }, { "hum", 56.4f }, { "press", 987.6f }, { "@L3F9A2C_temp", -7.3f },
                        { "wind", 4.2f }, { "gust", 7.9f }, { "dir", 271.4f }, { "rain_total", 123.4f },
                        { "uv", 3.4f }, { "lux", 23456.0f }, { "ds1", 12.5f }, { "big", 7000.0f } };
    const Table tab = { src, (int)(sizeof(src) / sizeof(src[0])) };
    uint32_t tid;
    {   // WH31 on channel 3 from a logical sensor + the main humidity
        FoFrame f = loop(slot(VS_WH31, { { VF_TEMP, "@L3F9A2C_temp" }, { VF_HUM, "hum" } }, 3), 1, tab, tid);
        assert(f.family == FO_FAMILY_WH31 && f.channel == 3 && f.id == vsDeriveId(MAC, 1, VS_WH31));
        assert(std::fabs(f.tempC + 7.3f) < 0.05f && f.hasHumidity && f.humidity == 56 && f.batteryOk);
        assert((tid >> 24) == FO_FAMILY_WH31 && ((tid >> 8) & 0xFF) == 3);
    }
    {   // WH31 at saturation (fog: a BME280 reads ~100 %) or bone dry: still a WH31 with humidity, never a WN30
        const Src edge[] = { { "temp", 4.0f }, { "fog", 99.7f }, { "full", 100.0f }, { "dry", 0.2f } };
        const Table et = { edge, (int)(sizeof(edge) / sizeof(edge[0])) };
        for (const char* h : { "fog", "full", "dry" }) {
            FoFrame f = loop(slot(VS_WH31, { { VF_TEMP, "temp" }, { VF_HUM, h } }, 2), 0, et, tid);
            assert(!strcmp(f.model, "WH31") && f.hasHumidity && f.humidity == (strcmp(h, "dry") ? 99 : 1));
        }
    }
    {   // WH40 rain, and the 16-bit wrap
        FoFrame f = loop(slot(VS_WH40, { { VF_RAIN, "rain_total" } }), 0, tab, tid);
        assert(f.family == FO_FAMILY_WH40 && f.id == vsDeriveId(MAC, 0, VS_WH40) && std::fabs(f.rainMm - 123.4f) < 0.05f);
        f = loop(slot(VS_WH40, { { VF_RAIN, "big" } }), 0, tab, tid);
        assert(std::fabs(f.rainMm - (7000.0f - 6553.6f)) < 0.06f);
    }
    {   // WN32P (BME280)
        FoFrame f = loop(slot(VS_WN32P, { { VF_TEMP, "ds1" }, { VF_HUM, "hum" }, { VF_PRESS, "press" } }), 2, tab, tid);
        assert(f.family == FO_FAMILY_WH25 && f.id == vsDeriveId(MAC, 2, VS_WN32P));
        assert(std::fabs(f.tempC - 12.5f) < 0.05f && f.humidity == 56 && std::fabs(f.pressureHpa - 987.6f) < 0.05f);
    }
    {   // WH65 with everything
        FoFrame f = loop(slot(VS_WH65, { { VF_TEMP, "temp" }, { VF_HUM, "hum" }, { VF_WIND, "wind" }, { VF_GUST, "gust" },
                                         { VF_DIR, "dir" }, { VF_RAIN, "rain_total" }, { VF_UV, "uv" }, { VF_LUX, "lux" } }), 3, tab, tid);
        assert(f.family == FO_FAMILY_WH65 && f.id == vsDeriveId(MAC, 3, VS_WH65));
        assert(f.hasTemp && std::fabs(f.tempC - 21.4f) < 0.05f && f.hasHumidity && f.humidity == 56);
        assert(f.hasWind && std::fabs(f.windAvgMs - 4.2f) < 0.07f && std::fabs(f.windGustMs - 7.9f) < 0.3f && f.windDirDeg == 271);
        assert(f.hasRain && std::fabs(f.rainMm - 123.4f) < 0.13f);
        assert(f.hasUv && f.uvIndex == 3 && f.hasLight && std::fabs(f.lightLux - 23456.0f) < 1.0f);
    }
    {   // WH65 without wind direction: no wind; without gust: gust = mean
        FoFrame f = loop(slot(VS_WH65, { { VF_TEMP, "temp" }, { VF_WIND, "wind" } }), 0, tab, tid);
        assert(f.hasTemp && !f.hasWind);
        f = loop(slot(VS_WH65, { { VF_WIND, "wind" }, { VF_DIR, "dir" } }), 0, tab, tid);
        assert(f.hasWind && std::fabs(f.windGustMs - 4.08f) < 0.3f);
    }

    {   // IT+ TX29 style: temperature + humidity, the new-battery flag while pairing
        ItPlusFrame f = loopIt(slot(VS_ITPLUS17, { { VF_TEMP, "@L3F9A2C_temp" }, { VF_HUM, "hum" } }), 4, tab, true, tid);
        assert(f.newBattery && !f.weakBattery && f.tempC == -7.3f && f.hasHumidity && f.humidity == 56);
        assert(tid == vsSlotTableId(slot(VS_ITPLUS17, { { VF_TEMP, "t" }, { VF_HUM, "h" } }), 4, MAC));
        assert((tid & 0xFF000000UL) == RF_ID_ITPLUS17 && (tid & RF_ID_ITPLUS_HUM));
        f = loopIt(slot(VS_ITPLUS17, { { VF_TEMP, "temp" } }), 4, tab, false, tid);   // temperature only: 0x6A
        assert(!f.newBattery && f.tempC == 21.4f && !f.hasHumidity && !(tid & RF_ID_ITPLUS_HUM));
    }
    {   // IT+ TX35 style; humidity mapped but not available now: sent without it; the original's range
        const Src it[] = { { "cold", -62.0f }, { "hot", 71.3f }, { "fog", 100.0f } };
        const Table itt = { it, 3 };
        ItPlusFrame f = loopIt(slot(VS_ITPLUS9, { { VF_TEMP, "cold" }, { VF_HUM, "nope" } }), 7, itt, false, tid);
        assert(f.tempC == -40.0f && !f.hasHumidity && (tid & 0xFF000000UL) == RF_ID_ITPLUS9);
        f = loopIt(slot(VS_ITPLUS9, { { VF_TEMP, "hot" }, { VF_HUM, "fog" } }), 7, itt, true, tid);
        assert(f.tempC == 59.9f && f.hasHumidity && f.humidity == 99 && f.newBattery);
    }
    {   // collision with a received IT+ sensor: same 6-bit ID and rate, either humidity variant
        const VsSlotCfg c = slot(VS_ITPLUS17, { { VF_TEMP, "temp" } });
        const uint8_t id = (uint8_t)vsDeriveId(MAC, 2, VS_ITPLUS17);
        assert(vsSlotTableId(c, 2, MAC) == rfItPlusId(true, id, false));
        assert(vsIdCollides(c, 2, MAC, rfItPlusId(true, id, false)));
        assert(vsIdCollides(c, 2, MAC, rfItPlusId(true, id, true)));              // a received twin with humidity
        assert(!vsIdCollides(c, 2, MAC, rfItPlusId(false, id, false)));           // the other rate: another station
        assert(!vsIdCollides(c, 2, MAC, rfItPlusId(true, (uint8_t)((id + 1) & 63), false)));
        assert(!vsIdCollides(slot(VS_WH31, { { VF_TEMP, "temp" } }, 3), 2, MAC,   // no twin rule for Fine Offset
                             vsSlotTableId(slot(VS_WH31, { { VF_TEMP, "temp" } }, 3), 2, MAC) ^ 0x100));
        assert(vsIdCollides(slot(VS_WH31, { { VF_TEMP, "temp" } }, 3), 2, MAC, vsSlotTableId(slot(VS_WH31, { { VF_TEMP, "temp" } }, 3), 2, MAC)));
        assert(!vsIdCollides(VsSlotCfg(), 2, MAC, 0));
    }
    // pairing window: from arming, 5 minutes
    assert(vsPairing(1000, 1000) && vsPairing(1000 + VS_ITPLUS_PAIR_MS - 1, 1000));
    assert(!vsPairing(1000 + VS_ITPLUS_PAIR_MS, 1000) && !vsPairing(999, 1000));

    // ---- missing values ----
    {
        VsValues v;
        assert(!vsResolve(slot(VS_WH31, { { VF_TEMP, "nope" }, { VF_HUM, "hum" } }), lookup, (void*)&tab, v));   // temp missing: skip
        assert(vsResolve(slot(VS_WH31, { { VF_TEMP, "temp" }, { VF_HUM, "nope" } }), lookup, (void*)&tab, v));   // humidity left out
        assert(!(v.has & (1 << VF_HUM)));
        assert(!vsResolve(slot(VS_WH65, { { VF_TEMP, "temp" }, { VF_RAIN, "nope" } }), lookup, (void*)&tab, v)); // rain must not jump to 0
        assert(vsResolve(slot(VS_WH65, { { VF_TEMP, "temp" }, { VF_UV, "nope" } }), lookup, (void*)&tab, v));
        assert(!vsResolve(slot(VS_WH65, { { VF_UV, "nope" } }), lookup, (void*)&tab, v));                        // nothing at all
        assert(!vsResolve(VsSlotCfg(), lookup, (void*)&tab, v));
        VsValues none;
        assert(vsBuildFrame(slot(VS_WH31, { { VF_TEMP, "temp" } }), 0, MAC, none, false).size() == 0);
        assert(vsBuildFrame(slot(VS_WH31, { { VF_TEMP, "temp" } }), VS_SLOTS, MAC, v, false).size() == 0);   // no slot outside the table
    }

    // ---- settings check ----
    assert(vsCheckSlot(VsSlotCfg()) == VS_CHECK_OK);
    assert(vsCheckSlot(slot(VS_WH31, { { VF_HUM, "hum" } })) == VS_CHECK_MUSTMAP);
    assert(vsCheckSlot(slot(VS_WH40, {})) == VS_CHECK_MUSTMAP);
    assert(vsCheckSlot(slot(VS_WH65, {})) == VS_CHECK_MUSTMAP);
    assert(vsCheckSlot(slot(VS_WH65, { { VF_WIND, "wind" } })) == VS_CHECK_WIND);
    assert(vsCheckSlot(slot(VS_WH65, { { VF_DIR, "dir" } })) == VS_CHECK_WIND);
    assert(vsCheckSlot(slot(VS_WH65, { { VF_WIND, "wind" }, { VF_DIR, "dir" } })) == VS_CHECK_OK);
    assert(vsCheckSlot(slot(VS_WH31, { { VF_TEMP, "temp" } }, 9)) == VS_CHECK_CHANNEL);
    assert(vsCheckSlot(slot(VS_ITPLUS17, { { VF_HUM, "hum" } })) == VS_CHECK_MUSTMAP);   // IT+ needs a temperature
    assert(vsCheckSlot(slot(VS_ITPLUS9, { { VF_TEMP, "temp" } }, 9)) == VS_CHECK_OK);    // no channel
    {
        VsValues v;
        assert(!vsResolve(slot(VS_ITPLUS17, { { VF_TEMP, "nope" }, { VF_HUM, "hum" } }), lookup, (void*)&tab, v));   // skipped
    }

    // ---- value names ----
    {
        char o[VS_SRC_LEN];
        assert(vsCleanSrc("{garten_temp}", o, sizeof(o)) && !strcmp(o, "garten_temp"));
        assert(vsCleanSrc(" @L3F9A2C_temp ", o, sizeof(o)) && !strcmp(o, "@L3F9A2C_temp"));
        assert(vsCleanSrc("", o, sizeof(o)) && !o[0]);
        assert(!vsCleanSrc("a|b", o, sizeof(o)) && !o[0]);
        assert(!vsCleanSrc("a,b", o, sizeof(o)));
        assert(!vsCleanSrc("x=1", o, sizeof(o)));
        assert(!vsCleanSrc("0123456789abcdef", o, sizeof(o)));   // 16 characters: too long
        assert(vsCleanSrc("rf300007DD_temp", o, sizeof(o)));
    }

    // ---- stored form ----
    {
        VsSlotCfg a[VS_SLOTS], b[VS_SLOTS];
        a[0] = slot(VS_WH31, { { VF_TEMP, "@L3F9A2C_temp" }, { VF_HUM, "hum" } }, 5);
        a[2] = slot(VS_WH65, { { VF_WIND, "wind" }, { VF_DIR, "dir" }, { VF_RAIN, "rain_total" } });
        a[2].enabled = 0;
        a[5] = slot(VS_ITPLUS17, { { VF_TEMP, "temp" }, { VF_HUM, "hum" } });
        a[6] = slot(VS_ITPLUS9, { { VF_TEMP, "ds1" } });
        char s[512];
        vsPack(a, s, sizeof(s));
        vsUnpack(s, b);
        for (int i = 0; i < VS_SLOTS; i++) {
            assert(a[i].type == b[i].type);
            if (a[i].type == VS_OFF) continue;
            assert(a[i].enabled == b[i].enabled && a[i].channel == b[i].channel);
            for (int f = 0; f < VF_COUNT; f++) assert(!strcmp(a[i].src[f], b[i].src[f]));
        }
        assert(b[5].type == VS_ITPLUS17 && b[6].type == VS_ITPLUS9 && strstr(s, "tx29|") && strstr(s, "tx35|"));
        vsUnpack("bogus|1|1|temp=temp\nwh31|1|3|temp=te|mp", b);   // unknown type, damaged line: off
        assert(b[0].type == VS_OFF && b[1].type == VS_OFF);
        vsUnpack(nullptr, b);
        assert(b[0].type == VS_OFF);
    }
    {   // NVS blob "vsens": [version][enabled][dBm][text] through the loader of every versioned blob
        VsSettings a;
        a.enabled = 1; a.txPowerDbm = -3;   // out of range: clamped when read
        for (int i = 0; i < VS_SLOTS; i++)   // all 8 slots with the longest names
            a.slot[i] = slot(VS_WH65, { { VF_TEMP, "@L3F9A2C_temp15" }, { VF_HUM, "@L3F9A2C_humi15" }, { VF_WIND, "@L3F9A2C_wind15" },
                                        { VF_GUST, "@L3F9A2C_gust15" }, { VF_DIR, "@L3F9A2C_dirr15" }, { VF_RAIN, "@L3F9A2C_rain15" },
                                        { VF_UV, "@L3F9A2C_uvix15" }, { VF_LUX, "@L3F9A2C_luxx15" } }, (uint8_t)(1 + i));
        uint8_t blob[VS_BLOB_MAX], scratch[VS_BLOB_MAX];
        const size_t n = vsSettingsPack(a, blob, sizeof(blob));
        assert(n > 3 && blob[0] == STORED_VIRTUAL_SENSORS_VERSION && blob[1] == 1 && (int8_t)blob[2] == -3);
        size_t len = 0;
        const uint8_t* p = storedBlobLoad(SF_VIRTUAL_SENSORS, blob, n, STORED_VIRTUAL_SENSORS_VERSION, 0, scratch, sizeof(scratch), &len);
        assert(p == blob + 1 && len == n - 1);
        VsSettings b;
        assert(vsSettingsFromPayload(p, len, b));
        assert(b.enabled == 1 && b.txPowerDbm == VS_DBM_MIN);
        for (int i = 0; i < VS_SLOTS; i++) {
            assert(b.slot[i].type == VS_WH65);
            for (int f = 0; f < VF_COUNT; f++) assert(!strcmp(a.slot[i].src[f], b.slot[i].src[f]));
        }
        assert(vsSettingsPack(a, blob, 100) == 0);   // cut: refused, never stored half
        blob[0] = STORED_VIRTUAL_SENSORS_VERSION + 1;   // a newer firmware's blob: not read
        assert(!storedBlobLoad(SF_VIRTUAL_SENSORS, blob, n, STORED_VIRTUAL_SENSORS_VERSION, 0, scratch, sizeof(scratch)));
        assert(!vsSettingsFromPayload(p, 1, b) && !b.enabled);   // too short: defaults
    }

    // ---- airtime, duty cycle ----
    assert(vsAirtimeMs(17) == 14);   // (48 + 16 + 136) bits / 17.241 kbit/s = 11.6 ms, + 2
    assert(vsAirtimeMs(7) == 9);     // 120 bits = 7.0 ms -> 7, + 2
    // IT+: (48 + 16 + 40) bits = 6.0 ms at 17.241 kbit/s -> 7, + 2; 10.9 ms at 9.579 kbit/s -> 11, + 2
    assert(vsTypeAirtimeMs(VS_ITPLUS17) == 9 && vsTypeAirtimeMs(VS_ITPLUS9) == 13);
    assert(vsTypeAirtimeMs(VS_WH65) == 14 && vsTypeAirtimeMs(VS_OFF) == 0);
    {
        // IT+ every 15 s (TX29 style, 245 frames/h x 9 ms) / 22 s (TX35 style, 166 x 13 ms):
        // 8 slots of either fit the plan limit; at the originals' 4 s one TX35 / two TX29 would
        VsSlotCfg it[VS_SLOTS];
        for (int i = 0; i < VS_SLOTS; i++) it[i] = slot(VS_ITPLUS17, { { VF_TEMP, "temp" } });
        assert(vsPlannedMsPerHour(it) == 8 * 245 * 9 && vsPlannedMsPerHour(it) <= VS_PLAN_MAX_MS_PER_H);
        for (int i = 0; i < VS_SLOTS; i++) it[i] = slot(VS_ITPLUS9, { { VF_TEMP, "temp" } });
        assert(vsPlannedMsPerHour(it) == 8 * 166 * 13 && vsPlannedMsPerHour(it) <= VS_PLAN_MAX_MS_PER_H);
        // a shorter interval would not fit 8 (the intervals are the shortest that do, in whole seconds)
        assert(8 * ((3600000u + 13750u - 1) / 13750u) * 9 > VS_PLAN_MAX_MS_PER_H);
        assert(8 * ((3600000u + 20750u - 1) / 20750u) * 13 > VS_PLAN_MAX_MS_PER_H);
        // mixed with Fine Offset types: 4 x TX35 + 4 x WH65 = 8.6 + 12.8 s > 18 s: refused
        for (int i = 4; i < VS_SLOTS; i++) it[i] = slot(VS_WH65, { { VF_TEMP, "temp" } });
        assert(vsPlannedMsPerHour(it) == 4 * 166 * 13 + 4 * 229 * 14 && vsPlannedMsPerHour(it) > VS_PLAN_MAX_MS_PER_H);
        it[7].enabled = 0;                                             // 3 x WH65: 18.25 s, still too much
        assert(vsPlannedMsPerHour(it) > VS_PLAN_MAX_MS_PER_H);
        it[6].enabled = 0;                                             // 2 x WH65: 15.0 s
        assert(vsPlannedMsPerHour(it) <= VS_PLAN_MAX_MS_PER_H);
    }
    {
        // 8 x WH31 (the 8 channels of a gateway): 60 frames/h x 9 ms each = 4.3 s of the 36 s per hour
        VsSlotCfg wh31[VS_SLOTS];
        for (int i = 0; i < VS_SLOTS; i++) wh31[i] = slot(VS_WH31, { { VF_TEMP, "temp" } }, (uint8_t)(1 + i));
        assert(vsPlannedMsPerHour(wh31) == 8 * 60 * 9);
        // WH65 every 16 s: 229 frames/h x 14 ms = 3.2 s each - 5 fit the plan limit (half the budget), 6 do not
        VsSlotCfg all[VS_SLOTS];
        for (int i = 0; i < VS_SLOTS; i++) all[i] = slot(VS_WH65, { { VF_TEMP, "temp" } });
        assert(vsPlannedMsPerHour(all) == 8 * 229 * 14 && vsPlannedMsPerHour(all) > VS_PLAN_MAX_MS_PER_H);
        for (int i = 5; i < VS_SLOTS; i++) all[i].enabled = 0;
        assert(vsPlannedMsPerHour(all) == 5 * 229 * 14 && vsPlannedMsPerHour(all) <= VS_PLAN_MAX_MS_PER_H);
        all[5].enabled = 1;
        assert(vsPlannedMsPerHour(all) > VS_PLAN_MAX_MS_PER_H);
        VsSlotCfg off[VS_SLOTS];
        assert(vsPlannedMsPerHour(off) == 0);
    }
    {
        VsBandLedger l;
        uint64_t t = 1000000;
        assert(l.lastHourMs(t) == 0);
        for (int m = 0; m < 60; m++) l.add(t + m * 60000ULL, 590);   // 60 minutes x 590 ms = 35.4 s
        assert(l.lastHourMs(t + 59 * 60000ULL) == 60 * 590);
        assert(!vsBudgetAllows(l, t + 59 * 60000ULL, 601));          // 35.4 + 0.601 s > 36 s
        assert(vsBudgetAllows(l, t + 59 * 60000ULL, 600));
        assert(l.lastHourMs(t + 60 * 60000ULL) == 59 * 590);         // the oldest minute dropped
        assert(l.lastHourMs(t + 200 * 60000ULL) == 0);               // a long pause clears everything
        l.add(t + 200 * 60000ULL, 0xFFFFF);                          // saturates, never wraps
        assert(l.lastHourMs(t + 200 * 60000ULL) == 0xFFFF && !vsBudgetAllows(l, t + 200 * 60000ULL, 1));
    }

    // ---- scheduling ----
    assert(vsNextDue(10000, 10100, 16000, 0) == 26000);
    assert(vsNextDue(10000, 10100, 16000, 120) == 26120);
    assert(vsNextDue(10000, 10100, 16000, 9999) == 26000 + VS_JITTER_MS);   // jitter clamped
    assert(vsNextDue(10000, 90000, 16000, 0) == 106000);                    // after a long stall: from now
    assert(vsHoldForRx(10500, 10000, true) && !vsHoldForRx(10500, 10000, false));
    assert(!vsHoldForRx(10000 + VS_MAX_DEFER_MS, 10000, true));             // never held longer than the maximum
    assert(!vsHoldForRx(9000, 10000, true));

    // ---- type table ----
    for (int t = VS_WH31; t < VS_TYPE_COUNT; t++) {
        assert(vsTypeByKey(VS_TYPES[t].key) == t && VS_TYPES[t].intervalMs >= 15000 && VS_TYPES[t].frameLen <= VS_MAX_FRAME_LEN);
        assert((VS_TYPES[t].mustMap & ~VS_TYPES[t].fields) == 0 && (VS_TYPES[t].skipMissing & ~VS_TYPES[t].fields) == 0);
    }
    for (int f = 0; f < VF_COUNT; f++) assert(vsFieldByKey(vsFieldKey((uint8_t)f)) == f);

    printf("vsensor: ok\n");
    return 0;
}
