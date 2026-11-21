// Records (src/records.cpp): periods, roll-over at local midnight / month /
// year, daily-rain maxima, reset, the NVS blob and the JSON.
#include "records.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>

Config gCfg;

static struct tm day(int y, int m, int d)
{
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = 12;
    return t;
}

int main()
{
    Records r;
    // day 1
    assert(recordsRoll(r, day(2026, 12, 30)) && r.ymd == 20261230);
    assert(!recordsRoll(r, day(2026, 12, 30)));
    recordsUpdate(r, RQ_TEMP, 5.0f, 1000);
    recordsUpdate(r, RQ_TEMP, -2.0f, 2000);
    recordsUpdate(r, RQ_TEMP, 3.0f, 3000);
    recordsUpdate(r, RQ_GUST, 12.0f, 2500);
    recordsUpdate(r, RQ_TEMP, NAN, 3500);          // ignored
    recordsDailyRain(r, 4.0f);
    assert(r.mn[RQ_TEMP][RP_TODAY].v == -2.0f && r.mn[RQ_TEMP][RP_TODAY].t == 2000);
    assert(r.mx[RQ_TEMP][RP_TODAY].v == 5.0f && r.mx[RQ_TEMP][RP_ALL].t == 1000);
    assert(r.mn[RQ_GUST][RP_TODAY].t == 0);        // gust: maximum only
    assert(r.rainDay[0].v == 4.0f && r.rainDay[0].t == 20261230);

    // day 2 (new year): yesterday = day 1, month and year cleared, all time kept
    assert(recordsRoll(r, day(2027, 1, 1)));
    assert(r.mn[RQ_TEMP][RP_YESTERDAY].v == -2.0f && r.mx[RQ_TEMP][RP_YESTERDAY].v == 5.0f);
    assert(r.mx[RQ_TEMP][RP_TODAY].t == 0 && r.mx[RQ_TEMP][RP_MONTH].t == 0 && r.mx[RQ_TEMP][RP_YEAR].t == 0);
    assert(r.mx[RQ_TEMP][RP_ALL].v == 5.0f && r.rainDay[2].v == 4.0f && r.rainDay[0].t == 0 && r.rainDay[1].t == 0);
    recordsUpdate(r, RQ_TEMP, 7.0f, 90000);
    recordsDailyRain(r, 1.0f);
    assert(r.mx[RQ_TEMP][RP_ALL].v == 7.0f && r.mn[RQ_TEMP][RP_ALL].v == -2.0f);
    assert(r.rainDay[1].v == 1.0f && r.rainDay[2].v == 4.0f);

    // a day later in the same month: month kept
    assert(recordsRoll(r, day(2027, 1, 2)));
    assert(r.mx[RQ_TEMP][RP_MONTH].v == 7.0f && r.mx[RQ_TEMP][RP_YESTERDAY].v == 7.0f && r.mx[RQ_TEMP][RP_TODAY].t == 0);

    // blob round trip; corruption / wrong size refused
    uint8_t blob[RECORDS_BLOB_SIZE];
    recordsEncode(r, blob);
    Records back;
    assert(recordsDecode(blob, sizeof(blob), back) && memcmp(&back, &r, sizeof(r)) == 0);
    blob[10] ^= 1;
    assert(!recordsDecode(blob, sizeof(blob), back));
    assert(!recordsDecode(blob, sizeof(blob) - 1, back));

    // JSON: units, entries only where recorded, rainDayMax date
    JsonDocument doc;
    gCfg.unitSystem = 0;
    recordsJson(doc, r, gCfg, true);
    assert(doc["valid"] == true && doc["tempUnit"] == std::string("C"));
    assert(doc["month"]["tempMax"]["value"].as<float>() == 7.0f);
    assert(std::string(doc["month"]["tempMax"]["time"].as<const char*>()) == "1970-01-02T01:00:00Z");
    assert(doc["today"]["tempMax"].isNull() && doc["allTime"]["gustMax"]["value"].as<float>() == 12.0f);
    assert(std::string(doc["allTime"]["rainDayMax"]["date"].as<const char*>()) == "2026-12-30");
    assert(doc["today"]["rainDayMax"].isNull());
    gCfg.unitSystem = 1;
    recordsJson(doc, r, gCfg, true);
    assert(doc["tempUnit"] == std::string("F") && std::fabs(doc["month"]["tempMax"]["value"].as<float>() - 44.6f) < 0.01f);
    recordsJson(doc, r, gCfg, false);
    assert(doc["valid"] == false);

    // reset: periods only, then everything
    recordsReset(r, false);
    assert(r.mx[RQ_TEMP][RP_MONTH].t == 0 && r.mx[RQ_TEMP][RP_YESTERDAY].t == 0 && r.mx[RQ_TEMP][RP_ALL].v == 7.0f && r.rainDay[2].t);
    recordsReset(r, true);
    assert(r.mx[RQ_TEMP][RP_ALL].t == 0 && r.rainDay[2].t == 0 && r.ymd == 20270102);

    puts("records ok");
    return 0;
}
