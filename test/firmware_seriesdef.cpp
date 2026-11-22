// Custom series definitions (src/series_def.cpp): stored form round trip,
// damaged lines, name clean-up, int16 scaling and clamping, keys.
#include "series_def.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>

int main()
{
    SeriesDef d[SERIES_MAX];
    strcpy(d[0].name, "Hochbeet Feuchte"); d[0].rfId = 0x510F651B; d[0].field = SF_MOIST;
    strcpy(d[2].name, "Blitze");           d[2].rfId = 0x57013CFF; d[2].field = SF_STRIKES;
    strcpy(d[3].name, "Fühler 2");         d[3].rfId = 0;          d[3].field = SF_DS2;
    char s[512];
    seriesPack(d, s, sizeof(s));
    assert(std::string(s) == "Hochbeet Feuchte|510F651B|moist\n\nBlitze|57013CFF|strikes\nF\xC3\xBChler 2|00000000|ds2\n\n\n\n");
    SeriesDef u[SERIES_MAX];
    seriesUnpack(s, u);
    assert(u[0].used() && !strcmp(u[0].name, "Hochbeet Feuchte") && u[0].rfId == 0x510F651B && u[0].field == SF_MOIST);
    assert(!u[1].used());
    assert(u[2].used() && u[2].field == SF_STRIKES);
    assert(u[3].used() && u[3].rfId == 0 && u[3].field == SF_DS2);
    for (int i = 4; i < SERIES_MAX; i++) assert(!u[i].used());

    // damaged / foreign: unused slots, nothing else touched
    seriesUnpack("x|zz|moist\nA|12|nope\nB|12\n|12|moist\nC|0|moist", u);
    for (int i = 0; i < SERIES_MAX; i++) assert(!u[i].used());   // bad hex, unknown key, short, no name, wireless value without ID
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
    b = a; b.rfId++;              assert(seriesKey(a) != seriesKey(b));
    b = a; strcpy(b.name, "X");   assert(seriesKey(a) != seriesKey(b));
    assert(seriesFieldByKey("pm25") == SF_PM25 && seriesFieldByKey("x") == -1);
    return 0;
}
