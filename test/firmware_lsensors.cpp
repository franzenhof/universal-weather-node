// Logical sensors (src/lsensors.cpp, A5c): key rules, add / find / re-key /
// re-bind / remove / move, the NVS blob, placeholder conversion, and the ★
// state of the sensor table (rf_sensors_data.cpp) on the table. Keys in these tests
// always differ from the internal IDs, so a mix-up of the two fails here.
#include "Arduino.h"
#include "lsensors.h"
#include "globals.h"
#include "rf_sensors.h"
#include "rf_names.h"
#include "rf_minrssi.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

static uint32_t seq = 0;
static uint32_t fakeRnd() { return 0xA1B2C3u + seq++; }
static uint32_t dupRnd()  { return seq++ < 3 ? 0x111111u : 0x222222u; }   // collides on purpose

int main()
{
    // ---- key rules ----
    assert(!lsCustomIdProblem("garten") && !lsCustomIdProblem("17000008") && !lsCustomIdProblem("Garten_Sued-2"));
    assert(lsCustomIdProblem("") && lsCustomIdProblem("a b") && lsCustomIdProblem("garten!") && lsCustomIdProblem("ä"));
    assert(lsCustomIdProblem("abcdefghijklmnopq"));                         // 17 characters
    assert(!lsCustomIdProblem("abcdefghijklmnop"));                         // 16
    assert(lsCustomIdCheck("series") == LS_ID_BUILTIN_START && lsCustomIdCheck("Series") == LS_ID_BUILTIN_START);   // {series_<n>}
    assert(!lsCustomIdProblem("series1") && !lsCustomIdProblem("myseries"));
    assert(lsCustomIdProblem("L3F9A2C") && lsCustomIdProblem("l3f9a2c"));         // internal-ID shape
    assert(!lsCustomIdProblem("L3F9A2X") && !lsCustomIdProblem("L3F9A2C1"));     // not that shape
    assert(lsCustomIdProblem("temp") && lsCustomIdProblem("TEMP") && lsCustomIdProblem("u_temp") && lsCustomIdProblem("clock"));
    assert(lsCustomIdProblem("rain") && lsCustomIdProblem("time") && lsCustomIdProblem("u") && lsCustomIdProblem("date"));   // prefixes of built-ins
    assert(!lsCustomIdProblem("rainy") && !lsCustomIdProblem("gartentemp"));
    assert(lsCustomIdProblem("rf3") && lsCustomIdProblem("rf17000008") && lsCustomIdProblem("RF2A"));   // legacy wireless placeholders
    assert(!lsCustomIdProblem("rfgarten") && !lsCustomIdProblem("rf"));

    // ---- add, find ----
    LogicalSensorTable t;
    assert(t.count() == 0);
    const int a = lsAdd(t, 0x17000008, "Garten Süd", fakeRnd);
    assert(a == 0 && t.count() == 1);
    assert(std::string(t.s[0].customId) == "17000008" && t.s[0].radioId == 0x17000008);
    assert(strlen(t.s[0].internalId) == LS_INTERNAL_ID_LEN && t.s[0].internalId[0] == 'L' && lsCustomIdProblem(t.s[0].internalId));   // never a valid key
    assert(std::string(t.s[0].displayName) == "Garten Süd");
    assert(lsAdd(t, 0x17000008, "again", fakeRnd) < 0);              // radio ID bound already
    const int b = lsAdd(t, 0x900181A2, nullptr, fakeRnd);
    assert(b == 1 && strcmp(t.s[0].internalId, t.s[1].internalId) != 0);
    assert(lsFindByCustomId(t, "900181a2") == 1 && lsFindByRadioId(t, 0x900181A2) == 1 && lsFindByInternalId(t, t.s[1].internalId) == 1);
    assert(lsFindByCustomId(t, t.s[1].internalId) < 0);                            // an internal ID is no key
    assert(lsFindByInternalId(t, "900181A2") < 0);                             // a key is no internal ID

    // ---- re-key ----
    assert(lsSetCustomId(t, 0, "garten"));
    assert(lsFindByCustomId(t, "GARTEN") == 0 && lsFindByCustomId(t, "17000008") < 0);
    assert(!lsSetCustomId(t, 1, "Garten"));                                // taken (case-insensitive)
    assert(!lsSetCustomId(t, 1, "temp") && !lsSetCustomId(t, 1, "a b"));
    const std::string internalId0 = t.s[0].internalId;
    assert(lsSetCustomId(t, 0, "beet") && internalId0 == t.s[0].internalId);             // the internal ID never changes

    // ---- re-bind (battery change) ----
    assert(lsRebindRadioId(t, 0, 0x17000023) && t.s[0].radioId == 0x17000023);
    assert(lsFindByRadioId(t, 0x17000008) < 0 && lsFindByRadioId(t, 0x17000023) == 0);
    assert(std::string(t.s[0].customId) == "beet" && internalId0 == t.s[0].internalId);
    assert(!lsRebindRadioId(t, 1, 0x17000023));                              // bound to the other one

    // a new sensor whose proposed key is taken gets a suffix
    assert(lsSetCustomId(t, 1, "17000042"));
    const int c = lsAdd(t, 0x17000042, nullptr, fakeRnd);
    assert(c == 2 && std::string(t.s[2].customId) == "17000042-2");

    // ---- move, remove (order kept, packed) ----
    lsMove(t, 2, -1);
    assert(t.s[1].radioId == 0x17000042 && t.s[2].radioId == 0x900181A2);
    lsMove(t, 0, -1);                                                 // no-op at the top
    assert(t.s[0].radioId == 0x17000023);
    lsRemove(t, 0);
    assert(t.count() == 2 && t.s[0].radioId == 0x17000042 && t.s[1].radioId == 0x900181A2 && !t.s[2].internalId[0]);

    // ---- internal IDs stay unique although the random source repeats ----
    {
        LogicalSensorTable u; seq = 0;
        lsAdd(u, 1, nullptr, dupRnd);
        lsAdd(u, 2, nullptr, dupRnd);
        assert(strcmp(u.s[0].internalId, u.s[1].internalId) != 0);
    }

    // ---- NVS blob round trip (lsPack / lsUnpack) ----
    {
        LogicalSensorTable p; seq = 0;
        assert(lsAdd(p, 0x17000008, "Garten S\xC3\xBC" "d", fakeRnd) == 0 && lsSetCustomId(p, 0, "garten"));
        assert(lsAdd(p, 0x900181A2, nullptr, fakeRnd) == 1);
        p.s[0].minRssiDbm = -95;
        uint8_t blob[LS_BLOB_MAX];
        const size_t n = lsPack(p, blob, sizeof blob);
        assert(n == 2 + 2 * LS_REC_SIZE && blob[0] == LS_BLOB_VERSION && blob[1] == 2);
        LogicalSensorTable q;
        assert(lsUnpack(q, blob, n) && lsEqual(p, q) && q.s[0].minRssiDbm == -95 && !strcmp(q.s[0].customId, "garten"));
        assert(lsPack(p, blob, n - 1) == 0);                          // too small: nothing
        // empty table
        LogicalSensorTable e;
        assert(lsPack(e, blob, sizeof blob) == 2 && lsUnpack(q, blob, 2) && q.count() == 0);
        // other layout / size / count: refused, empty
        lsPack(p, blob, sizeof blob);
        blob[0] = 2;  assert(!lsUnpack(q, blob, n) && q.count() == 0);
        blob[0] = LS_BLOB_VERSION;
        assert(!lsUnpack(q, blob, n - 1) && !lsUnpack(q, blob, 1) && !lsUnpack(q, nullptr, 0));
        // a damaged entry is dropped, the others kept
        blob[2 + LS_REC_SIZE] = 'x';                                  // the 2nd internal ID
        assert(lsUnpack(q, blob, n) && q.count() == 1 && !strcmp(q.s[0].customId, "garten"));
        // a full table fits LS_BLOB_MAX
        LogicalSensorTable f; seq = 0;
        for (int i = 0; i < LS_MAX; i++) assert(lsAdd(f, 0x40000000u + i, "abcdefghijklmnopqrstuvwx", fakeRnd) == i);
        assert(lsPack(f, blob, sizeof blob) == LS_BLOB_MAX && lsUnpack(q, blob, LS_BLOB_MAX) && lsEqual(f, q));
        // lsEqual sees every field
        q.s[5].minRssiDbm = -90; assert(!lsEqual(f, q));
        // lsAppend: as it is, but never a second internalId / key / radio ID
        LogicalSensorTable g;
        LogicalSensor x; strcpy(x.internalId, "L3F9A2C"); strcpy(x.customId, "garten"); x.radioId = 1;
        assert(lsAppend(g, x) && !lsAppend(g, x));
        strcpy(x.internalId, "L3F9A2D"); assert(!lsAppend(g, x));          // key taken
        strcpy(x.customId, "beet");    assert(!lsAppend(g, x));          // radio ID taken
        x.radioId = 2;            assert(lsAppend(g, x) && g.count() == 2);
        strcpy(x.internalId, "L3F9A2E"); strcpy(x.customId, "L3F9A2F"); x.radioId = 3; assert(!lsAppend(g, x));   // key of the internalId shape
        assert(lsRadioIds(g).size() == 2 && lsRadioIds(g)[1] == 2);
    }

    // ---- internal ID shape ----
    assert(lsIsInternalId("L3F9A2C") && lsIsInternalId("L000000"));
    assert(!lsIsInternalId("l3f9a2c") && !lsIsInternalId("L3F9A2") && !lsIsInternalId("L3F9A2C0") && !lsIsInternalId("L3f9A2C") && !lsIsInternalId("") && !lsIsInternalId(nullptr));

    // ---- placeholders: stored "{@internalId_f}" <-> key "{key_f}"; old forms converted on save ----
    {
        LogicalSensorTable p;
        LogicalSensor x;
        strcpy(x.internalId, "L3F9A2C"); strcpy(x.customId, "garten_sued"); x.radioId = 0x17000008; assert(lsAppend(p, x));
        strcpy(x.internalId, "L0A0B0C"); strcpy(x.customId, "b-2");         x.radioId = 0x900181A2; assert(lsAppend(p, x));
        const char* f = nullptr;
        assert(lsOfPlaceholder(p, "@L3F9A2C_temp", &f) == 0 && !strcmp(f, "temp"));
        assert(lsOfPlaceholder(p, "garten_sued_hum", &f) == 0 && !strcmp(f, "hum"));   // a key with '_': split at the last one
        assert(lsOfPlaceholder(p, "B-2_temp", &f) == 1);                                // keys: any case
        assert(lsOfPlaceholder(p, "rf17000008_temp", &f) == 0 && lsOfPlaceholder(p, "rf2_temp", &f) == 1);
        for (const char* no : { "@L3F9A2D_temp", "@garten_sued_temp", "L3F9A2C_temp", "garten_temp", "rf3_temp", "rf17000009_temp",
                                "garten_sued_", "temp", "_temp", "rain_day", "" }) {
            assert(lsOfPlaceholder(p, no, &f) < 0 && !f);
        }
        char out[160];
        // key form -> stored, suffixes kept, other text and placeholders untouched
        assert(lsPlaceholdersToStored(p, "T {garten_sued_temp:f:.1} H {B-2_hum} {temp} {x_y} {\"j\":1}", out, sizeof out));
        assert(std::string(out) == "T {@L3F9A2C_temp:f:.1} H {@L0A0B0C_hum} {temp} {x_y} {\"j\":1}");
        // and back: the key as stored in the table
        char back[160];
        assert(lsPlaceholdersToCustomId(p, out, back, sizeof back));
        assert(std::string(back) == "T {garten_sued_temp:f:.1} H {b-2_hum} {temp} {x_y} {\"j\":1}");
        // old forms of a ★ sensor: radio ID and ★ number -> stored; a received-only sensor's stays
        assert(lsPlaceholdersToStored(p, "{rf17000008_temp}{rf2_uv}{rf3_uv}{rf30000001_temp}", out, sizeof out));
        assert(std::string(out) == "{@L3F9A2C_temp}{@L0A0B0C_uv}{rf3_uv}{rf30000001_temp}");
        // stored form stays stored; an unknown internal ID stays as it is in both directions
        assert(lsPlaceholdersToStored(p, "{@L3F9A2C_temp}{@L999999_temp}", out, sizeof out) && std::string(out) == "{@L3F9A2C_temp}{@L999999_temp}");
        assert(lsPlaceholdersToCustomId(p, "{@L999999_temp} {", back, sizeof back) && std::string(back) == "{@L999999_temp} {");
        // the key changes: every stored template shows the new one
        lsSetCustomId(p, 0, "beet");
        assert(lsPlaceholdersToCustomId(p, "{@L3F9A2C_temp}", back, sizeof back) && std::string(back) == "{beet_temp}");
        // too long: false, cut and terminated
        char small[12];
        assert(!lsPlaceholdersToStored(p, "{beet_temp}", small, sizeof small) && strlen(small) == sizeof small - 1);
        assert(lsPlaceholdersToStored(p, nullptr, out, sizeof out) && !out[0]);
    }

    // ---- the ★ state on the table (rf_sensors_data.cpp): name and minimum move with it ----
    {
        gLogicalSensors = LogicalSensorTable(); gCfg.rfIgnore[0] = '\0';
        snprintf(gCfg.rfNames, sizeof(gCfg.rfNames), "17000008:Garten|30000001:Nachbar");
        snprintf(gCfg.rfMinRssi, sizeof(gCfg.rfMinRssi), "17000008:-95");
        assert(rfSetState(0x17000008, RF_STATE_MINE) && gLogicalSensors.count() == 1);
        const LogicalSensor& m = gLogicalSensors.s[0];
        assert(!strcmp(m.displayName, "Garten") && m.minRssiDbm == -95 && !strcmp(m.customId, "17000008") && lsIsInternalId(m.internalId));
        assert(std::string(gCfg.rfNames) == "30000001:Nachbar" && !gCfg.rfMinRssi[0]);   // moved, not copied
        assert(!strcmp(rfNameOf(0x17000008), "Garten") && rfMinRssiOf(0x17000008) == -95 && !strcmp(rfNameOf(0x30000001), "Nachbar"));
        gLogicalSensors.s[0].minRssiDbm = -90;                                       // edited in the table
        assert(rfMinRssiOf(0x17000008) == -90);
        assert(rfStateOf(0x17000008) == RF_STATE_MINE && rfStateOf(0x30000001) == RF_STATE_RECEIVED);
        // given up: back to the received-only lists
        assert(rfSetState(0x17000008, RF_STATE_RECEIVED) && gLogicalSensors.count() == 0);
        assert(!strcmp(rfNameOf(0x17000008), "Garten") && rfMinRssiOf(0x17000008) == -90);
        // ★ again: a new internal ID (never reused)
        assert(rfSetState(0x17000008, RF_STATE_MINE) && gLogicalSensors.count() == 1);
        // hidden from ★: out of the table, into the hidden list
        assert(rfSetState(0x17000008, RF_STATE_HIDDEN) && gLogicalSensors.count() == 0 && rfStateOf(0x17000008) == RF_STATE_HIDDEN);
        assert(rfSetState(0x17000008, RF_STATE_MINE) && rfStateOf(0x17000008) == RF_STATE_MINE && !gCfg.rfIgnore[0]);
        // the ★ order on the table
        assert(rfSetState(0x30000001, RF_STATE_MINE) && gLogicalSensors.count() == 2 && gLogicalSensors.s[1].radioId == 0x30000001);
        assert(rfMoveMine(0x30000001, -1) && gLogicalSensors.s[0].radioId == 0x30000001 && !rfMoveMine(0x30000001, -1));
        assert(!rfMoveMine(0x17000008, 1) && !rfMoveMine(0x12345678, -1));
        // a full table refuses, nothing changes
        for (uint32_t k = 0; gLogicalSensors.count() < LS_MAX; k++) assert(rfSetState(0x50000000u + k, RF_STATE_MINE));
        assert(!rfSetState(0x60000000u, RF_STATE_MINE) && rfStateOf(0x60000000u) == RF_STATE_RECEIVED);
        gLogicalSensors = LogicalSensorTable(); gCfg.rfNames[0] = gCfg.rfMinRssi[0] = '\0';
    }

    // ---- battery change: same kind, "replaces?" candidate ----
    {
        gLogicalSensors = LogicalSensorTable(); gCfg.rfIgnore[0] = '\0';
        auto mk = [](uint32_t id, int proto, uint8_t type, const char* model, unsigned long first, unsigned long last) {
            RfSensorReading r; r.used = true; r.id = id; r.proto = proto; r.type = type; r.model = model;
            r.firstMs = first; r.lastMs = last; return r;
        };
        const RfSensorReading it = mk(0x17000008, RF_PROTO_ITPLUS17, 0, nullptr, 1, 1000);
        const RfSensorReading itHum = mk(0x17000108, RF_PROTO_ITPLUS17, 0, nullptr, 1, 1000);
        assert(rfSameKind(it, mk(0x17000023, RF_PROTO_ITPLUS17, 0, nullptr, 1, 1)));
        assert(!rfSameKind(it, itHum));                                            // humidity variant
        assert(!rfSameKind(it, mk(0x09000023, RF_PROTO_ITPLUS9, 0, nullptr, 1, 1)));   // other protocol
        assert(rfSameKind(mk(1, RF_PROTO_FINEOFFSET, 0, "WS90", 1, 1), mk(2, RF_PROTO_FINEOFFSET, 0, "WS90", 1, 1)));
        assert(!rfSameKind(mk(1, RF_PROTO_FINEOFFSET, 0, "WS90", 1, 1), mk(2, RF_PROTO_FINEOFFSET, 0, "WH31", 1, 1)));
        assert(rfSameKind(mk(1, RF_PROTO_BRESSER, 1, nullptr, 1, 1), mk(2, RF_PROTO_BRESSER, 1, nullptr, 1, 1)));
        assert(!rfSameKind(mk(1, RF_PROTO_BRESSER, 1, nullptr, 1, 1), mk(2, RF_PROTO_BRESSER, 2, nullptr, 1, 1)));

        assert(rfSetState(0x17000008, RF_STATE_MINE));
        const unsigned long fresh = rfFreshMs(it), later = 1000 + fresh + 5000;
        RfSensorReading all[4];
        all[0] = it;
        all[1] = mk(0x17000023, RF_PROTO_ITPLUS17, 0, nullptr, later - 2000, later - 1000);   // new, after the silence
        assert(rfReplaceCandidate(all, 4, all[0], later) == 1);
        assert(rfReplaceCandidate(all, 4, all[0], 1000 + fresh - 1) == -1);                  // star still fresh
        all[2] = mk(0x17000040, RF_PROTO_ITPLUS17, 0, nullptr, later - 500, later - 100);    // a second candidate
        assert(rfReplaceCandidate(all, 4, all[0], later) == -1);                             // ambiguous: no offer
        all[2] = mk(0x17000141, RF_PROTO_ITPLUS17, 0, nullptr, later - 500, later - 100);    // other variant: no candidate
        assert(rfReplaceCandidate(all, 4, all[0], later) == 1);
        all[2] = mk(0x17000050, RF_PROTO_ITPLUS17, 0, nullptr, 500, later - 100);            // there before the silence: neighbour
        assert(rfReplaceCandidate(all, 4, all[0], later) == 1);                              // an ID that was already there is no candidate; still one
        all[2] = RfSensorReading();
        all[1].firstMs = 1000 + 30000;                                                        // battery change: new ID 30 s after the last packet
        assert(rfReplaceCandidate(all, 4, all[0], later) == 1);                              // offered (inside the old freshness window)
        all[1].firstMs = 900;                                                                 // heard while the star spoke
        assert(rfReplaceCandidate(all, 4, all[0], later) == -1);
        all[1].firstMs = later - 2000;
        assert(rfSetState(0x17000023, RF_STATE_HIDDEN));                                      // hidden: no offer
        assert(rfReplaceCandidate(all, 4, all[0], later) == -1);
        assert(rfSetState(0x17000023, RF_STATE_RECEIVED));
        // the re-bind itself keeps internalId / key / name; the new ID is ★, the old one received-only again
        char internalId[8]; strcpy(internalId, gLogicalSensors.s[0].internalId);
        assert(lsRebindRadioId(gLogicalSensors, 0, 0x17000023) && rfStateOf(0x17000023) == RF_STATE_MINE && rfStateOf(0x17000008) == RF_STATE_RECEIVED);
        assert(std::string(gLogicalSensors.s[0].internalId) == internalId && std::string(gLogicalSensors.s[0].customId) == "17000008");
        gLogicalSensors = LogicalSensorTable();
    }

    printf("Firmware logical sensors passed.\n");
    return 0;
}
