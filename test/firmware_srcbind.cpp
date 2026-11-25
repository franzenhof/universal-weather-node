// Value sources bound to a sensor ID (src/sources.cpp): the source stays on the
// chosen sensor whatever happens to the table, the Mine list or the settings form.
#include "globals.h"
#include "sources.h"
#include "params.h"
#include "rf_sensors.h"
#include "board_io.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static const uint32_t A = 0x300000AA, B = 0x300000BB;

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

// what saving the settings page without touching the source does
static void saveUnchanged()
{
    char v[12];
    srcFormValue(SG_AIR, v, sizeof(v));
    if (!srcSetFormId(SG_AIR, v)) assert(applyParam((uint8_t)srcParamId(SG_AIR), (int16_t)atoi(v)));
}
static void chooseB()
{
    gCfg.rfIds[0] = '\0';
    clear(); put(0, A); put(1, B);
    assert(applyParam((uint8_t)srcParamId(SG_AIR), (int16_t)srcSelForSlot(1)));
    assert(source() == B);
}

int main()
{
    hostNow = 100000;
    gCfg.rfEn = 1;
    bme280Present = false;

    // the table is filled in another order after a restart
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

    // a page rendered before the order changed, saved afterwards
    chooseB();
    char v[12];
    srcFormValue(SG_AIR, v, sizeof(v));
    assert(!strcmp(v, "r300000BB"));
    clear(); put(0, B); put(1, A);
    assert(srcSetFormId(SG_AIR, v) && source() == B);

    // the Mine list gets another order
    chooseB();
    strcpy(gCfg.rfIds, "300000AA,300000BB");
    assert(source() == B);
    strcpy(gCfg.rfIds, "300000BB,300000AA");
    assert(source() == B);

    // by number: a Mine sensor that is not received yet is bound as well
    clear();
    assert(applyParam((uint8_t)srcParamId(SG_AIR), (int16_t)srcSelForSlot(0)) && gCfg.srcId[SG_AIR] == B);

    // Auto / Board / Off drop the binding
    assert(applyParam((uint8_t)srcParamId(SG_AIR), SRC_AUTO) && gCfg.srcId[SG_AIR] == 0);
    srcFormValue(SG_AIR, v, sizeof(v));
    assert(!strcmp(v, "0"));

    // a setting from before the binding: by number until saved, then bound
    gCfg.rfIds[0] = '\0';
    clear(); put(0, A); put(1, B);
    gCfg.srcSel[SG_AIR] = (uint8_t)srcSelForSlot(1);
    gCfg.srcId[SG_AIR]  = 0;
    assert(source() == B);
    saveUnchanged();
    assert(gCfg.srcId[SG_AIR] == B);

    // a sensor that was never received (restored backup) and malformed values
    assert(srcSetFormId(SG_AIR, "r300000CC") && gCfg.srcId[SG_AIR] == 0x300000CC && srcSelSlot(gCfg.srcSel[SG_AIR]) >= 0);
    assert(source() == 0);
    for (const char* bad : { "", "3", "r", "r00000000", "r300000C", "r300000CCC", "r300000ZZ", "x300000CC" })
        assert(!srcSetFormId(SG_AIR, bad));
    assert(gCfg.srcId[SG_AIR] == 0x300000CC);

    // placeholder names: by number or by 8-digit ID
    gCfg.rfIds[0] = '\0';
    clear(); put(0, A); put(1, B);
    const char* field = nullptr;
    assert(rfIndexOfPlaceholder("rf2_temp", &field) == 1 && !strcmp(field, "temp"));
    assert(rfIndexOfPlaceholder("rf300000BB_hum", &field) == 1 && !strcmp(field, "hum"));
    assert(rfIndexOfPlaceholder("rf300000bb_hum", &field) == 1);
    clear(); put(0, B); put(1, A);
    assert(rfIndexOfPlaceholder("rf2_temp", &field) == 1 && gRfSensors[1].id == A);       // the number moved on ...
    assert(rfIndexOfPlaceholder("rf300000BB_temp", &field) == 0);                          // ... the ID did not
    put(2, 0x17000008);                                                                    // an ID of digits only
    assert(rfIndexOfPlaceholder("rf17000008_temp", &field) == 2);
    for (const char* bad : { "rf_temp", "rf0_temp", "rf33_temp", "rf123_temp", "rfA_temp", "rf300000CC_temp",
                             "rf300000BBB_temp", "rf300000BB", "temp", "rf00000000_temp" })
        assert(rfIndexOfPlaceholder(bad, &field) == -1);

    printf("Firmware srcbind regressions passed.\n");
    return 0;
}
