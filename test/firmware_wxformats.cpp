// Met Office WOW URL and CWOP / APRS packet (src/wx_formats.cpp).
#include "wx_formats.h"
#include <cassert>
#include <cstring>
#include <string>

int main()
{
    WxObs o;
    o.hasTemp = o.hasHum = o.hasPress = o.hasWind = o.hasRain = true;
    o.tempC = 20.0f; o.humidity = 50.0f; o.qnhHpa = 1013.2f;
    o.windMs = 2.0f; o.gustMs = 4.5f; o.dirDeg = 220;
    o.rain1hMm = 0.0f; o.rain24hMm = 1.27f; o.rainDayMm = 0.254f;
    char b[400];
    // WOW: WU-style imperial fields; dateutc encoded (space, colons)
    assert(wowUrl(o, "123456", "654321", "2026-11-20 10:32:55", "uwn-2.0", b, sizeof(b)));
    assert(std::string(b) == "http://wow.metoffice.gov.uk/automaticreading?siteid=123456&siteAuthenticationKey=654321"
                             "&dateutc=2026-11-20%2010%3A32%3A55&softwaretype=uwn-2.0&tempf=68.0&humidity=50&dewptf=48.7"
                             "&baromin=29.92&windspeedmph=4.5&windgustmph=10.1&winddir=220&dailyrainin=0.01");
    assert(!wowUrl(o, "123456", "654321", "2026-11-20 10:32:55", "uwn-2.0", b, 40));   // no room

    // APRS 1.01 complete weather report (the spec's example layout):
    // @092345z4903.50N/07201.75W_220/004g005t077r000p000P000h50b09900
    WxObs a;
    a.hasTemp = a.hasHum = a.hasPress = a.hasWind = a.hasRain = true;
    a.tempC = 25.0f; a.humidity = 50.0f; a.qnhHpa = 990.0f;
    a.windMs = 4.0f / 2.236936f; a.gustMs = 5.0f / 2.236936f; a.dirDeg = 220;
    assert(cwopPacket(a, "CW1234", 49.0583333, -72.0291667, 9, 23, 45, "", b, sizeof(b)));
    assert(std::string(b) == "CW1234>APRS,TCPIP*:@092345z4903.50N/07201.75W_220/004g005t077r000p000P000h50b09900");
    // negative temperature, humidity 100 % -> "00", southern / eastern hemisphere, software tag
    a.tempC = -20.6f; a.humidity = 100.0f; a.rain1hMm = 2.54f; a.rain24hMm = 25.4f; a.rainDayMm = 12.7f;
    assert(cwopPacket(a, "CW1234", -33.8688, 151.2093, 1, 0, 5, "UWN", b, sizeof(b)));
    assert(std::string(b) == "CW1234>APRS,TCPIP*:@010005z3352.13S/15112.56E_220/004g005t-05r010p100P050h00b09900UWN");
    // missing wind / temperature as dots; other groups left out
    WxObs m;
    m.hasPress = true; m.qnhHpa = 1013.25f;
    assert(cwopPacket(m, "DW5678", 47.8, 13.04, 20, 12, 0, "", b, sizeof(b)));
    assert(std::string(b) == "DW5678>APRS,TCPIP*:@201200z4748.00N/01302.40E_.../...g...t...b10133");
    assert(cwopLogin("CW1234", -1, "UWN", "2.0.0", b, sizeof(b)) && std::string(b) == "user CW1234 pass -1 vers UWN 2.0.0");
    puts("wxformats ok");
    return 0;
}
