// Value sources bound to a logical sensor's internal ID (src/sources.cpp, A5c):
// the source stays on the chosen ★ sensor whatever happens to the reception
// table, the ★ order, its radio ID (battery change) or the settings form.
// Placeholder names of wireless sensors (stored / key / old forms) as well.
// Keys always differ from the internal IDs here, so a mix-up of the two fails.
#include "globals.h"
#include "ls_host.h"
#include "sources.h"
#include "params.h"
#include "rf_sensors.h"
#include "board_io.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static const uint32_t A = 0x300000AA, B = 0x300000BB, C = 0x300000CC;

static void put(int i, uint32_t id)
{
    RfSensorReading r;
    r.used = true; r.id = id; r.proto = RF_PROTO_FINEOFFSET;
    gRfSensors[i] = r;
    RfValues v;
    v.ok = RF_VAL_TEMP; v.tempC = 20.0f;
    unsigned long gap;
    rfStoreValues(gRfSensors[i], v, hostNow, gap);
}
static void clear() { for (RfSensorReading& r : gRfSensors) r = RfSensorReading(); }
static uint32_t source() { int i = srcResolve(SG_AIR); return i >= 0 ? gRfSensors[i].id : 0; }
static std::string internalIdOf(uint32_t id) { const int i = lsFindByRadioId(gLogicalSensors, id); return i >= 0 ? gLogicalSensors.s[i].internalId : ""; }

// what saving the settings page without touching the source does
static void saveUnchanged()
{
    char v[12];
    srcFormValue(SG_AIR, v, sizeof(v));
    if (!srcSetFormId(SG_AIR, v)) assert(applyParam((uint8_t)srcParamId(SG_AIR), (int16_t)atoi(v)));
}
// ★ A, B (keys "aussen", "garten"), both received; the source set to wireless 2 = B
static void chooseB()
{
    hostSetMine("300000AA, 300000BB");
    assert(lsSetCustomId(gLogicalSensors, 0, "aussen") && lsSetCustomId(gLogicalSensors, 1, "garten"));
    clear(); put(0, A); put(1, B);
    assert(applyParam((uint8_t)srcParamId(SG_AIR), (int16_t)srcSelForSlot(1)));
    assert(gCfg.sourceInternalId[SG_AIR] == internalIdOf(B) && source() == B);
}

int main()
{
    hostNow = 100000;
    gCfg.rfEn = 1;
    bme280Present = false;

    // the reception table is filled in another order after a restart
    chooseB(); clear(); put(0, B); put(1, A);
    assert(source() == B);

    // the chosen sensor is silent: no value, never another sensor's
    chooseB(); clear(); put(1, A);
    assert(source() == 0);

    // settings saved meanwhile / right after a restart: the binding survives
    chooseB(); clear(); put(1, A); saveUnchanged(); put(0, B);
    assert(source() == B);
    chooseB(); clear(); saveUnchanged(); put(0, B); put(1, A);
    assert(source() == B);

    // the form's value is the internal ID - never the key or the radio ID
    chooseB();
    char v[12];
    srcFormValue(SG_AIR, v, sizeof(v));
    assert(v == internalIdOf(B) && strcmp(v, "garten") && lsIsInternalId(v));

    // the ★ order changes (a page rendered before, saved afterwards): still B, the number follows
    lsMove(gLogicalSensors, 1, -1);
    assert(source() == B);
    assert(srcSetFormId(SG_AIR, v) && source() == B && gCfg.srcSel[SG_AIR] == srcSelForSlot(0));

    // a battery change: B's radio ID re-bound to C - the source follows, A never
    chooseB();
    assert(lsRebindRadioId(gLogicalSensors, 1, C));
    clear(); put(0, A); put(1, C);
    assert(source() == C);
    put(2, B);                                   // the old ID heard again (a neighbour): not ours any more
    assert(source() == C);

    // a key change does not touch the binding
    chooseB();
    assert(lsSetCustomId(gLogicalSensors, 1, "beet") && source() == B);

    // by number: a ★ sensor that is not received yet is bound as well
    chooseB(); clear();
    assert(applyParam((uint8_t)srcParamId(SG_AIR), (int16_t)srcSelForSlot(0)) && gCfg.sourceInternalId[SG_AIR] == internalIdOf(A));

    // Auto / Board / Off drop the binding
    assert(applyParam((uint8_t)srcParamId(SG_AIR), SRC_AUTO) && !gCfg.sourceInternalId[SG_AIR][0]);
    srcFormValue(SG_AIR, v, sizeof(v));
    assert(!strcmp(v, "0"));

    // a received-only sensor (no logical sensor) chosen by number: bound by its number
    gLogicalSensors = LogicalSensorTable();
    clear(); put(0, A); put(1, B);
    assert(applyParam((uint8_t)srcParamId(SG_AIR), (int16_t)srcSelForSlot(1)) && !gCfg.sourceInternalId[SG_AIR][0]);
    assert(source() == B);
    srcFormValue(SG_AIR, v, sizeof(v));
    assert(atoi(v) == srcSelForSlot(1));
    saveUnchanged();
    assert(source() == B && !gCfg.sourceInternalId[SG_AIR][0]);

    // the bound ★ sensor given up: the source delivers nothing (no fallback to its number)
    chooseB();
    assert(rfSetState(B, RF_STATE_RECEIVED) && gLogicalSensors.count() == 1);
    assert(source() == 0);
    srcFormValue(SG_AIR, v, sizeof(v));
    assert(lsIsInternalId(v) && srcSetFormId(SG_AIR, v) && source() == 0);   // saved unchanged: stays as it is

    // a version-1 backup's radio ID and an unknown internal ID are swallowed, nothing changes;
    // malformed values go to the parameter path (false)
    chooseB();
    const std::string before = gCfg.sourceInternalId[SG_AIR];
    assert(srcSetFormId(SG_AIR, "r300000AA") && gCfg.sourceInternalId[SG_AIR] == before);
    assert(srcSetFormId(SG_AIR, "L000001") && gCfg.sourceInternalId[SG_AIR] == before);
    for (const char* bad : { "", "3", "r", "garten", "L00001", "L0000011", "l000001", "LXXXXXX" })
        assert(!srcSetFormId(SG_AIR, bad));
    assert(gCfg.sourceInternalId[SG_AIR] == before && source() == B);
    // another ★ sensor by its internal ID
    assert(srcSetFormId(SG_AIR, internalIdOf(A).c_str()) && source() == A && gCfg.srcSel[SG_AIR] == srcSelForSlot(0));

    // ---- placeholder names ----
    chooseB();
    const char* field = nullptr;
    const std::string sb = "@" + internalIdOf(B) + "_temp";
    assert(rfIndexOfPlaceholder(sb.c_str(), &field) == 1 && !strcmp(field, "temp"));        // stored form
    assert(rfIndexOfPlaceholder("garten_hum", &field) == 1 && !strcmp(field, "hum"));       // key form
    assert(rfIndexOfPlaceholder("GARTEN_hum", &field) == 1);                                 // keys: any case
    assert(rfIndexOfPlaceholder("rf2_temp", &field) == 1 && !strcmp(field, "temp"));          // old: number
    assert(rfIndexOfPlaceholder("rf300000BB_hum", &field) == 1);                              // old: radio ID
    assert(rfIndexOfPlaceholder("rf300000bb_hum", &field) == 1);
    // the reception order changes: stored / key / radio ID stay on B, the number follows the ★ order
    clear(); put(0, B); put(1, A);
    assert(rfIndexOfPlaceholder(sb.c_str(), &field) == 0 && rfIndexOfPlaceholder("garten_temp", &field) == 0);
    assert(rfIndexOfPlaceholder("rf2_temp", &field) == 0 && rfIndexOfPlaceholder("rf300000BB_temp", &field) == 0);
    // a battery change: stored and key forms follow, the old radio ID does not
    assert(lsRebindRadioId(gLogicalSensors, 1, C));
    clear(); put(0, A); put(3, C);
    assert(rfIndexOfPlaceholder(sb.c_str(), &field) == 3 && rfIndexOfPlaceholder("garten_temp", &field) == 3);
    assert(rfIndexOfPlaceholder("rf300000BB_temp", &field) == -1 && field);
    // a sensor known but not received: field set, no index (renders empty)
    clear();
    assert(rfIndexOfPlaceholder("garten_temp", &field) == -1 && field && !strcmp(field, "temp"));
    // an ID of digits only (received-only sensor)
    put(2, 0x17000008);
    assert(rfIndexOfPlaceholder("rf17000008_temp", &field) == 2);
    // not a wireless sensor's: built-in names and unknown keys leave field unset
    for (const char* other : { "temp", "rain_day", "time_local", "nogarden_temp", "@L000001_temp", "garten" }) {
        assert(rfIndexOfPlaceholder(other, &field) == -1);
        assert(!field);
    }
    for (const char* bad : { "rf_temp", "rf0_temp", "rf33_temp", "rf123_temp", "rfA_temp", "rf300000DD_temp",
                             "rf300000BBB_temp", "rf300000BB", "rf00000000_temp" })
        assert(rfIndexOfPlaceholder(bad, &field) == -1);

    printf("Firmware srcbind regressions passed.\n");
    return 0;
}
