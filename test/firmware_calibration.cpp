// Calibration (src/calibration.cpp): main-value offsets / factors after the
// value source, per-sensor offsets applied when a packet is stored, and the
// settings text of the per-sensor list.
#include "Arduino.h"
#include "calibration.h"
#include "globals.h"
#include "sources.h"
#include "rf_sensors.h"
#include "derived.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>

static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

int main()
{
    // ---- pure math ----
    gCfg.calTempDc = -12; gCfg.calHumDpct = 35; gCfg.calPressDhpa = 15; gCfg.calRainPermil = 1100; gCfg.calWindPermil = 900;
    assert(near(calTemp(20.0f), 18.8f));
    assert(near(calHumidity(50.0f), 53.5f));
    assert(near(calHumidity(98.0f), 100.0f));   // clamped
    gCfg.calHumDpct = -50;
    assert(near(calHumidity(3.0f), 0.0f));
    assert(near(calStationPressure(950.0f), 951.5f));
    assert(near(calRain(0.2f), 0.22f, 0.0001f));
    WindStats w; w.valid = true; w.meanMs = 10.0f; w.gustMs = 20.0f; w.dirValid = true; w.dirDeg = 270;
    WindStats c = calWind(w);
    assert(near(c.meanMs, 9.0f) && near(c.gustMs, 18.0f) && c.dirDeg == 270 && c.valid);

    // ---- per-sensor list ----
    char list[640];
    assert(rfCalParseText("1a2b3c4d:-0.4:+3, 00000F12:1.25", list, sizeof(list)));
    assert(std::string(list) == "1A2B3C4D:-4:30,F12:13:0");   // tenths, hum optional, rounded
    int t, h;
    assert(rfCalLookup(list, 0x1A2B3C4D, t, h) && t == -4 && h == 30);
    assert(rfCalLookup(list, 0xF12, t, h) && t == 13 && h == 0);
    assert(!rfCalLookup(list, 0x1234, t, h) && t == 0 && h == 0);
    char text[800];
    rfCalFormatText(list, text, sizeof(text));
    assert(std::string(text) == "1A2B3C4D:-0.4:+3.0, 00000F12:+1.3:+0.0");
    // round trip
    char list2[640];
    assert(rfCalParseText(text, list2, sizeof(list2)) && std::string(list2) == list);
    // zero entries are dropped; empty text clears
    assert(rfCalParseText("ABC:0:0", list2, sizeof(list2)) && list2[0] == '\0');
    assert(rfCalParseText("", list2, sizeof(list2)) && list2[0] == '\0');
    // invalid: syntax, range, ID too long - out unchanged
    strcpy(list2, "keep");
    assert(!rfCalParseText("ABC", list2, sizeof(list2)));
    assert(!rfCalParseText("ABC:x", list2, sizeof(list2)));
    assert(!rfCalParseText("ABC:11", list2, sizeof(list2)));
    assert(!rfCalParseText("ABC:1:21", list2, sizeof(list2)));
    assert(!rfCalParseText("123456789:1", list2, sizeof(list2)));
    assert(!rfCalParseText("ABC:1 junk", list2, sizeof(list2)));
    assert(std::string(list2) == "keep");
    // 32 worst-case entries fit Config::rfCal, a 33rd is refused
    std::string many;
    char e[24];
    for (int i = 0; i < 33; i++) {
        snprintf(e, sizeof(e), "%s%08X:-10:-20", i ? "," : "", 0xF0000000u + i);
        many += e;
    }
    assert(!rfCalParseText(many.c_str(), list2, sizeof(gCfg.rfCal)));
    many = many.substr(0, many.rfind(','));
    assert(rfCalParseText(many.c_str(), list2, sizeof(gCfg.rfCal)));
    assert(strlen(list2) < sizeof(gCfg.rfCal));

    // ---- applied after the value source (sources.cpp) ----
    gCfg.calTempDc = 5; gCfg.calHumDpct = -20; gCfg.calPressDhpa = 10; gCfg.altitudeM = 500;
    gCfg.srcSel[SG_AIR] = SRC_BOARD; gCfg.srcSel[SG_PRESS] = SRC_BOARD;
    bme280Present = true;
    bme280TempC = 10.0f; bme280Humidity = 60.0f; bme280PressAbs = 950.0f; bme280PressSea = 1007.0f;
    srcUpdateCycle();
    assert(gSrc.tempValid && near(gSrc.tempC, 10.5f));
    assert(near(gSrc.humidity, 58.0f));
    assert(near(gSrc.pressAbsHpa, 951.0f));
    assert(near(gSrc.pressSeaHpa, seaLevelPressureHpa(951.0f, 500)));   // QNH from the corrected station pressure

    // ---- per sensor: when a packet is stored (rf_sensors_data.cpp) ----
    assert(rfCalParseText("0000ABCD:+0.6:-5", gCfg.rfCal, sizeof(gCfg.rfCal)));
    RfSensorReading s;
    s.id = 0xABCD;
    RfValues v;
    v.ok = RF_VAL_TEMP | RF_VAL_HUM; v.tempC = 21.0f; v.humidity = 97;
    unsigned long gap = 0;
    rfStoreValues(s, v, millis(), gap);
    assert(near(s.tempC, 21.6f) && s.humidity == 92);
    v.humidity = 3;   // clamped at 0
    rfStoreValues(s, v, millis(), gap);
    assert(s.humidity == 0);
    RfSensorReading o;
    o.id = 0x1234;    // not listed: unchanged
    rfStoreValues(o, v, millis(), gap);
    assert(near(o.tempC, 21.0f) && o.humidity == 3);

    // ---- per-sensor gains (rain, wind, UV, light) and the WN20 preset ----
    {
        RfCal c;
        // entries of older firmware ("ID:t:h") read with gains 1.000
        assert(rfCalGet("ABCD:5:-20", 0xABCD, c) && c.t10 == 5 && c.h10 == -20 && c.rain == 1000 && c.wind == 1000 && !c.wn20);
        char l[1024];
        assert(rfCalParseText("20001234:0:0:0.95:1.1:1:0.8, 20005678:0:0:1:1:1:1:wn20, ABCD:+0.5:-2", l, sizeof(l)));
        assert(std::string(l) == "20001234:0:0:950:1100:1000:800:0,20005678:0:0:1000:1000:1000:1000:1,ABCD:5:-20");
        assert(rfCalGet(l, 0x20001234, c) && c.rain == 950 && c.wind == 1100 && c.lux == 800 && !c.wn20);
        assert(rfCalGet(l, 0x20005678, c) && c.wn20);
        // text round trip
        char txt[2400], l2[1024];
        rfCalFormatText(l, txt, sizeof(txt));
        assert(std::string(txt) == "20001234:+0.0:+0.0:0.950:1.100:1.000:0.800, 20005678:+0.0:+0.0:1.000:1.000:1.000:1.000:wn20, 0000ABCD:+0.5:-2.0");
        assert(rfCalParseText(txt, l2, sizeof(l2)) && std::string(l2) == l);
        // gains outside 0.5-2 refused
        assert(!rfCalParseText("1:0:0:0.4", l2, sizeof(l2)) && !rfCalParseText("1:0:0:1:2.1", l2, sizeof(l2)));
        // WN20 preset (Ecowitt wiki): 0.908 up to 5 mm/h, 0.773 above; it replaces the plain rain gain
        assert(rfWn20Gain(0.0f) == 0.908f && rfWn20Gain(5.0f) == 0.908f && rfWn20Gain(5.01f) == 0.773f && rfWn20Gain(30.0f) == 0.773f);
        RfCal w; w.wn20 = true; w.rain = 1500;
        assert(rfRainGain(w, 2.0f) == 0.908f && rfRainGain(w, 8.0f) == 0.773f);
        RfCal g; g.rain = 950;
        assert(near(rfRainGain(g, 50.0f), 0.95f));

        // WH40 preset (0.1016 mm tips reported as 0.1 mm) and the soil moisture two-point calibration
        assert(rfCalParseText("40001234:0:0:1:1:1:1:wh40, 51001234:0:0:1:1:1:1:ad=70-340", l, sizeof(l)));
        assert(std::string(l) == "40001234:0:0:1000:1000:1000:1000:2,51001234:0:0:1000:1000:1000:1000:0:70:340");
        assert(rfCalGet(l, 0x40001234, c) && c.wh40 && !c.wn20 && near(rfRainGain(c, 0.0f), 1.016f));
        c.rain = 2000;
        assert(near(rfRainGain(c, 0.0f), 1.016f));                       // the preset replaces the plain factor
        assert(rfCalGet(l, 0x51001234, c) && c.ad0 == 70 && c.ad100 == 340);
        uint8_t pct = 0;
        assert(rfCalMoisture(c, 205, pct) && pct == 50);
        assert(rfCalMoisture(c, 30, pct) && pct == 0 && rfCalMoisture(c, 400, pct) && pct == 100);   // clamped
        RfCal none;
        assert(!rfCalMoisture(none, 200, pct));
        rfCalFormatText(l, txt, sizeof(txt));
        assert(std::string(txt) == "40001234:+0.0:+0.0:1.000:1.000:1.000:1.000:wh40, 51001234:+0.0:+0.0:1.000:1.000:1.000:1.000:ad=70-340");
        assert(rfCalParseText(txt, l2, sizeof(l2)) && std::string(l2) == l);
        // the presets exclude each other; an AD range must have two different ends
        assert(!rfCalParseText("1:0:0:1:1:1:1:wn20:wh40", l2, sizeof(l2)) && !rfCalParseText("1:0:0:1:1:1:1:ad=5-5", l2, sizeof(l2)));
        // older entries (without flags / AD) still read
        assert(rfCalGet("ABC:1:2:950:1000:1000:1000:1", 0xABC, c) && c.wn20 && !c.wh40 && c.ad0 == c.ad100);

        // applied when stored: wind / UV / light at once; rain on the increment, the raw counter unchanged
        strcpy(gCfg.rfCal, "20001234:0:0:500:1100:1200:800:0");
        RfSensorReading x;
        x.id = 0x20001234;
        RfValues pv;
        pv.ok = RF_VAL_WIND | RF_VAL_UV | RF_VAL_LIGHT | RF_VAL_RAIN;
        pv.windAvgMs = 10.0f; pv.windGustMs = 20.0f; pv.uvIndex = 5.0f; pv.lightLux = 1000.0f; pv.rainTotalMm = 100.0f;
        unsigned long gp;
        assert(rfStoreValues(x, pv, millis(), gp) == 0.0f);                 // first rain value: no increase
        assert(near(x.windAvgMs, 11.0f) && near(x.windGustMs, 22.0f) && near(x.uvIndex, 6.0f) && near(x.lightLux, 800.0f));
        pv.rainTotalMm = 102.0f;
        assert(near(rfStoreValues(x, pv, millis(), gp), 1.0f));            // 2 mm x 0.5
        assert(near(x.rainTotalMm, 102.0f));                                // the counter as the sensor sends it
        gCfg.rfCal[0] = '\0';
    }

    puts("calibration ok");
    return 0;
}
