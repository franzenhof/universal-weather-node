// Settings loaded from NVS are kept inside what the settings form accepts
// (src/config_sanitize.cpp, sanitizeConfig()).
#include "config.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    {   // coordinates: decimal point or decimal comma, stored with a point
        char o[12] = "keep";
        assert(coordParse("49.88", 90, o, sizeof(o)) && !strcmp(o, "49.88"));
        assert(coordParse("49,88", 90, o, sizeof(o)) && !strcmp(o, "49.88"));
        assert(coordParse("-112,4301", 180, o, sizeof(o)) && !strcmp(o, "-112.4301"));
        assert(coordParse("", 90, o, sizeof(o)) && !o[0]);   // empty = not set
        strcpy(o, "keep");
        assert(!coordParse("49,88,1", 90, o, sizeof(o)) && !coordParse("1.234,5", 90, o, sizeof(o)));
        assert(!coordParse("91", 90, o, sizeof(o)) && !coordParse("-180,5", 180, o, sizeof(o)));
        assert(!coordParse("49,88x", 90, o, sizeof(o)) && !coordParse("abc", 90, o, sizeof(o)) && !coordParse(",", 90, o, sizeof(o)));
        assert(!coordParse("12.3456789012", 90, o, sizeof(o)));   // longer than the field
        assert(!strcmp(o, "keep"));
    }
    {   // the defaults pass unchanged
        Config c;
        const Config d;
        sanitizeConfig(c);
        assert(c.altitudeM == d.altitudeM && c.awekasIntervalSec == d.awekasIntervalSec && c.wuIntervalSec == d.wuIntervalSec);
    }
    {   // weather-network intervals and the altitude: clamped to the form's ranges
        Config c;
        c.awekasIntervalSec = 0;  c.wuIntervalSec = 0;  c.wcloudIntervalSec = -5;
        c.wnetproIntervalSec = 59; c.windyIntervalSec = 100000; c.osemIntervalSec = 7200;
        c.altitudeM = -20;
        sanitizeConfig(c);
        assert(c.awekasIntervalSec == 300 && c.wuIntervalSec == 60 && c.wcloudIntervalSec == 60);
        assert(c.wnetproIntervalSec == 60 && c.windyIntervalSec == 3600 && c.osemIntervalSec == 3600);
        assert(c.altitudeM == 0);
        c.altitudeM = 9000;
        sanitizeConfig(c);
        assert(c.altitudeM == 4000);
    }
    {   // values inside the ranges are kept
        Config c;
        c.wuIntervalSec = 120; c.windyIntervalSec = 600; c.altitudeM = 1234;
        sanitizeConfig(c);
        assert(c.wuIntervalSec == 120 && c.windyIntervalSec == 600 && c.altitudeM == 1234);
    }
    std::puts("config sanitize ok");
    return 0;
}
