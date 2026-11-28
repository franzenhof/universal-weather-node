// Custom series definitions (src/series_def.cpp): stored form round trip
// (all 32 slots, aggregation overrides), damaged lines, name clean-up, int16
// scaling and clamping, keys, Home Assistant entities; the node's store
// (src/series_store.cpp). A wireless
// source is a logical sensor's internal ID (A5c); lines of the radio-ID form
// before it are dropped (no conversion), board probes are kept.
#include "series_def.h"
#include "series.h"
#include "placeholders.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

void* bigAlloc(size_t n) { return malloc(n); }   // psram.h: the host has no PSRAM

int main()
{
    SeriesDef d[SERIES_MAX];
    strcpy(d[0].name, "Hochbeet Feuchte"); strcpy(d[0].sensorInternalId, "L3F9A2C"); d[0].field = SF_MOIST;
    strcpy(d[2].name, "Blitze");           strcpy(d[2].sensorInternalId, "L0A0B0C"); d[2].field = SF_STRIKES;
    strcpy(d[3].name, "Fühler 2");                                         d[3].field = SF_DS2;
    assert(d[0].used() && d[3].used());
    char s[512];
    seriesPack(d, s, sizeof(s));
    assert(std::string(s) == "Hochbeet Feuchte|L3F9A2C|moist\n\nBlitze|L0A0B0C|strikes\nF\xC3\xBChler 2|B|ds2" + std::string(SERIES_MAX - 4, '\n'));
    SeriesDef u[SERIES_MAX];
    seriesUnpack(s, u);
    assert(u[0].used() && !strcmp(u[0].name, "Hochbeet Feuchte") && !strcmp(u[0].sensorInternalId, "L3F9A2C") && u[0].field == SF_MOIST);
    assert(!u[1].used());
    assert(u[2].used() && u[2].field == SF_STRIKES && !strcmp(u[2].sensorInternalId, "L0A0B0C"));
    assert(u[3].used() && !u[3].sensorInternalId[0] && u[3].field == SF_DS2);
    for (int i = 4; i < SERIES_MAX; i++) assert(!u[i].used());

    // a board value with a sensor, a wireless value without one: unused
    SeriesDef w;
    strcpy(w.name, "x"); w.field = SF_DS1; strcpy(w.sensorInternalId, "L3F9A2C"); assert(!w.used());
    w.field = SF_TEMP; w.sensorInternalId[0] = '\0';                               assert(!w.used());

    // damaged / foreign: unused slots, nothing else touched
    seriesUnpack("x|zz|moist\nA|L3F9A2C|nope\nB|L3F9A2C\n|L3F9A2C|moist\nC|0|moist\nD|garten|temp\nE|l3f9a2c|temp", u);
    for (int i = 0; i < SERIES_MAX; i++) assert(!u[i].used());   // no ID, unknown key, short, no name, no ID, a key, lower case
    // before A5c: a wireless line (radio ID) is dropped, a board probe line ("00000000") kept
    seriesUnpack("Feuchte|510F651B|moist\nProbe|00000000|ds1", u);
    assert(!u[0].used() && u[1].used() && u[1].field == SF_DS1 && !u[1].sensorInternalId[0]);
    seriesUnpack(nullptr, u);
    assert(!u[0].used());

    // names: '|' and control characters dropped, trimmed, UTF-8 not split
    char n[8];
    seriesCleanName("  a|b\tc  ", n, sizeof(n));          assert(!strcmp(n, "abc"));
    seriesCleanName("abcdef\xC3\xBC", n, sizeof(n));      assert(!strcmp(n, "abcdef"));
    seriesCleanName(nullptr, n, sizeof(n));               assert(!n[0]);

    // scaling
    assert(seriesEncode(SF_TEMP, 21.37f) == 214);
    assert(std::fabs(seriesDecode(SF_TEMP, 214) - 21.4f) < 1e-4);
    assert(seriesEncode(SF_LUX, 123456.0f) == 12346);       // 10-lx steps
    assert(seriesEncode(SF_CO2, 99999.0f) == 32767);        // clamped, never the gap
    assert(seriesEncode(SF_TEMP, -99999.0f) == -32767);
    assert(seriesEncode(SF_TEMP, NAN) == SERIES_GAP);
    assert(seriesEncode(SF_COUNT, 1.0f) == SERIES_GAP);

    // keys: same definition, same key; any change, another key
    SeriesDef a = d[0], b = d[0];
    assert(seriesKey(a) == seriesKey(b));
    b.field = SF_LEAF;            assert(seriesKey(a) != seriesKey(b));
    b = a; b.sensorInternalId[6] = 'D';      assert(seriesKey(a) != seriesKey(b));
    b = a; strcpy(b.name, "X");   assert(seriesKey(a) != seriesKey(b));
    assert(seriesFieldByKey("pm25") == SF_PM25 && seriesFieldByKey("x") == -1);
    // a board series keeps its key (and so its stored history) across the A5c change: the
    // key mixed a 4-byte radio ID of 0 before - the same bytes as now for "no sensor"
    {
        SeriesDef p;
        strcpy(p.name, "Probe"); p.field = SF_DS1;
        uint32_t h = 2166136261u;
        auto mix = [&h](const void* q, size_t n) { const uint8_t* x = (const uint8_t*)q; for (size_t i = 0; i < n; i++) { h ^= x[i]; h *= 16777619u; } };
        const uint32_t zero = 0;
        mix(p.name, strlen(p.name)); mix(&zero, 4); mix(&p.field, 1);
        assert(seriesKey(p) == (h ? h : 1));
    }

    // all 32 slots with the longest names and an own aggregation on every other one
    {
        SeriesDef f[SERIES_MAX];
        for (int i = 0; i < SERIES_MAX; i++) {
            memset(f[i].name, 'a' + i % 26, SERIES_NAME_LEN - 1);
            f[i].name[SERIES_NAME_LEN - 1] = '\0';
            snprintf(f[i].sensorInternalId, sizeof(f[i].sensorInternalId), "L%06X", 0xA00000 + i);
            f[i].field = (uint8_t)(i % SF_DS1);   // the wireless values
            seriesSetAgg(f[i], (uint8_t)(i % 2 ? SA_MAX : SA_DEFAULT));
            assert(f[i].used());
        }
        static char big[SERIES_PACKED_MAX];
        const size_t n = seriesPack(f, big, sizeof(big));
        assert(n > 0 && n < sizeof(big) - 1 && strlen(big) == n);
        SeriesDef g[SERIES_MAX];
        seriesUnpack(big, g);
        for (int i = 0; i < SERIES_MAX; i++) {
            assert(g[i].used() && !strcmp(g[i].name, f[i].name) && !strcmp(g[i].sensorInternalId, f[i].sensorInternalId));
            assert(g[i].field == f[i].field && seriesAgg(g[i]) == seriesAgg(f[i]) && seriesKey(g[i]) == seriesKey(f[i]));
        }
        assert(seriesAgg(g[1]) == SA_MAX && seriesAgg(g[4]) == SERIES_FIELDS[SF_STRIKES].agg && g[4].agg == SA_DEFAULT);
        // a buffer one byte short: cut at a whole line, never a partial one
        SeriesDef h2[SERIES_MAX];
        const size_t m = seriesPack(f, big, n);
        assert(m < n);
        seriesUnpack(big, h2);
        for (int i = 0; i < SERIES_MAX - 1; i++) assert(h2[i].used());
        assert(!h2[SERIES_MAX - 1].used());
    }

    // aggregation: stored only when not the value's default; unknown keys give the default
    {
        SeriesDef arr[SERIES_MAX];
        SeriesDef& a2 = arr[0];
        strcpy(a2.name, "Blitz"); strcpy(a2.sensorInternalId, "L0A0B0C"); a2.field = SF_STRIKES;
        seriesSetAgg(a2, SA_SUM);   assert(a2.agg == SA_DEFAULT && seriesAgg(a2) == SA_SUM);
        const uint32_t k0 = seriesKey(a2);
        seriesSetAgg(a2, SA_MAX);   assert(a2.agg == SA_MAX && seriesKey(a2) != k0);
        char t[96];
        seriesPack(arr, t, sizeof(t));
        assert(!strncmp(t, "Blitz|L0A0B0C|strikes|max\n", 26));
        SeriesDef u2[SERIES_MAX];
        seriesUnpack("A|L0A0B0C|strikes|min\nB|L0A0B0C|temp|sum\nC|L0A0B0C|temp|weird\nD|B|ds1|mean", u2);
        assert(seriesAgg(u2[0]) == SA_MIN && seriesAgg(u2[1]) == SA_SUM);
        assert(u2[2].used() && u2[2].agg == SA_DEFAULT && seriesAgg(u2[2]) == SA_MEAN);
        assert(u2[3].used() && u2[3].agg == SA_DEFAULT);
        assert(seriesAggByKey("max") == SA_MAX && seriesAggByKey("avg") == -1);
        SeriesDef off;
        assert(seriesAgg(off) == SA_MEAN);
    }

    // Home Assistant: object id per slot, a template on the "series" object, unit and class from the value
    {
        SeriesDef t2;
        strcpy(t2.name, "Gew\xC3\xA4" "chshaus"); strcpy(t2.sensorInternalId, "L3F9A2C"); t2.field = SF_TEMP;
        SeriesHaEntity e;
        seriesHaEntity(2, t2, false, e);
        assert(!strcmp(e.key, "series_3") && !strcmp(e.unit, "\xC2\xB0" "C") && !strcmp(e.deviceClass, "temperature"));
        assert(!strcmp(e.tmpl, "{{ (value_json.series | default([]) | selectattr('slot', 'eq', 3) | map(attribute='value') | first) | default(None) }}"));
        seriesHaEntity(2, t2, true, e);     assert(!strcmp(e.unit, "\xC2\xB0" "F"));
        t2.field = SF_CO2;   seriesHaEntity(31, t2, false, e);
        assert(!strcmp(e.key, "series_32") && !strcmp(e.unit, "ppm") && !strcmp(e.deviceClass, "carbon_dioxide") && strlen(e.tmpl) < sizeof(e.tmpl) - 1);
        t2.field = SF_STRIKES; seriesHaEntity(0, t2, false, e);   assert(!e.unit[0] && !e.deviceClass);
    }

    // the node's store: definitions and the last cycle's values
    {
        SeriesDef* defs = seriesDefs();
        assert(defs && !defs[0].used() && !seriesDefAt(-1).used() && !seriesDefAt(SERIES_MAX).used());
        strcpy(defs[5].name, "CO2"); strcpy(defs[5].sensorInternalId, "L3F9A2C"); defs[5].field = SF_CO2;
        float v[SERIES_MAX] = {};
        bool ok[SERIES_MAX] = {};
        v[5] = 612; ok[5] = true; v[6] = 1; ok[6] = true;   // slot 6 has a value but no definition
        seriesSetNow(v, ok);
        float x = 0;
        assert(seriesNow(5, x) && x == 612 && !seriesNow(6, x) && !seriesNow(SERIES_MAX, x) && !seriesNow(-1, x));
        assert(seriesNowOk()[5] && seriesNowValues()[5] == 612);
        assert(!seriesDefsChangedTake());
        seriesDefsChanged();
        assert(seriesDefsChangedTake() && !seriesDefsChangedTake());

        // placeholders {series_<n>}: slot numbers 1-32 as on the settings page
        strcpy(defs[0].name, "Bed"); defs[0].field = SF_DS1;   // board probe, a temperature
        strcpy(defs[1].name, "Baro"); strcpy(defs[1].sensorInternalId, "L3F9A2C"); defs[1].field = SF_PRESS;
        v[0] = 21.5f; ok[0] = true; v[1] = 1013.3f; ok[1] = true;
        seriesSetNow(v, ok);
        assert(seriesPlaceholderSlot("series_6") == 5 && seriesPlaceholderSlot("series_32") == 31);
        for (const char* bad : { "series_0", "series_33", "series_06", "series_6x", "series_", "series", "Series_6", "series_6_temp" })
            assert(seriesPlaceholderSlot(bad) == -1);
        auto look = [](const char* name, PhValue& o, void*) {
            const int slot = seriesPlaceholderSlot(name);
            return slot >= 0 && seriesPlaceholder(slot, o);
        };
        char out[160];
        const int empty = phRender("{series_6} ppm|{series_6:.1}|{series_1}|{series_1:f}|{series_2}|{series_2:inhg}|{series_7}|{series_33}",
                                   out, sizeof(out), PE_NONE, look, nullptr);
        assert(!strcmp(out, "612 ppm|612.0|21.5|70.7|1013.3|29.92||") && empty == 2);   // slot 7 unused, 33 none
    }
    {   // a slot rewritten on one task (the settings POST) while another copies it:
        // every copy is one whole definition, never a mix of the old and the new
        SeriesDef a, b;
        strcpy(a.name, "aaaaaaaaaaaaaaaaaaaaaaa"); strcpy(a.sensorInternalId, "LAAAAAA"); a.field = SF_MOIST;
        strcpy(b.name, "bbbbbbbbbbbbbbbbbbbbbbb"); b.field = SF_DS2;
        SeriesDef* defs = seriesDefs();
        { SeriesGuard g; defs[9] = a; }
        std::thread writer([&] {
            for (int i = 0; i < 200000; i++) { SeriesGuard g; defs[9] = (i & 1) ? b : a; }
        });
        int torn = 0;
        for (int i = 0; i < 200000; i++) {
            const SeriesDef c = seriesDefAt(9);
            const bool isA = !memcmp(&c, &a, sizeof(c)), isB = !memcmp(&c, &b, sizeof(c));
            if (!isA && !isB) torn++;
        }
        writer.join();
        assert(torn == 0);
        // the stored form under the lock: round trip of all slots
        char packed[SERIES_PACKED_MAX];
        seriesPackLocked(packed, sizeof(packed));
        SeriesDef before[SERIES_MAX];
        for (int i = 0; i < SERIES_MAX; i++) before[i] = seriesDefAt(i);
        seriesUnpackLocked(packed);
        for (int i = 0; i < SERIES_MAX; i++) assert(seriesKey(seriesDefAt(i)) == seriesKey(before[i]));
    }
    return 0;
}
