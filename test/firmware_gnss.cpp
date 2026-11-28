// GNSS (B9): NMEA parser (src/nmea.cpp), take-over thresholds, timestamps,
// baud search (src/gnss_logic.h), the BME280 pin rule (src/heltec_board.h).
#include "nmea.h"
#include "gnss_logic.h"
#include "heltec_board.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

BoardInfo gBoard;

// "$" + body + "*" + checksum
static std::string withSum(const char* body)
{
    unsigned char s = 0;
    for (const char* p = body; *p; p++) s ^= (unsigned char)*p;
    char t[8];
    snprintf(t, sizeof(t), "*%02X", s);
    return std::string("$") + body + t;
}

static bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

static void testRmc()
{
    NmeaSentence s;
    // the NMEA reference sentence (GP talker, NMEA 2.x without mode)
    assert(nmeaParse("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A", s) == NMEA_RMC);
    assert(s.timeOk && s.msOfDay == (12 * 3600 + 35 * 60 + 19) * 1000u);
    assert(s.dateOk && s.ymd == 19940323 && s.active && s.mode == 0);
    assert(s.posOk && near(s.lat, 48.1173, 1e-4) && near(s.lon, 11.516667, 1e-5));
    // UC6580-style GN talker, NMEA 4.1 with mode and nav status, fraction, south / west
    std::string l = withSum("GNRMC,083559.00,A,3723.2475,S,12158.3416,W,0.02,,261126,,,A,V");
    assert(nmeaParse(l.c_str(), s) == NMEA_RMC);
    assert(s.active && s.mode == 'A' && s.dateOk && s.ymd == 20261126);
    assert(s.msOfDay == (8 * 3600 + 35 * 60 + 59) * 1000u);
    assert(s.posOk && near(s.lat, -37.387458, 1e-5) && near(s.lon, -121.97236, 1e-5));
    // milliseconds
    l = withSum("GNRMC,083559.250,A,3723.2475,N,12158.3416,E,,,261126,,,D");
    assert(nmeaParse(l.c_str(), s) == NMEA_RMC && s.msOfDay % 1000 == 250);
    // no fix: L76K before the first fix (time from its RTC, date may be missing)
    l = withSum("GNRMC,000012.00,V,,,,,,,,,,N,V");
    assert(nmeaParse(l.c_str(), s) == NMEA_RMC && !s.active && !s.posOk && !s.dateOk && s.timeOk);
    l = withSum("GPRMC,235959.00,V,,,,,,,060180,,,N");
    assert(nmeaParse(l.c_str(), s) == NMEA_RMC && !s.active && s.dateOk && s.ymd == 19800106);
    // 'A' with mode N (seen on clones): no fix
    l = withSum("GNRMC,083559.00,A,3723.2475,N,12158.3416,E,,,261126,,,N");
    assert(nmeaParse(l.c_str(), s) == NMEA_RMC && !s.active);
    // bad date / time / position fields
    l = withSum("GNRMC,246000.00,A,3723.2475,N,12158.3416,E,,,301326,,,A");
    assert(nmeaParse(l.c_str(), s) == NMEA_RMC && !s.timeOk && !s.dateOk);
    l = withSum("GNRMC,083559.00,A,3799.0000,N,12158.3416,X,,,290227,,,A");   // minutes 99, hemisphere X, 29 Feb 2027
    assert(nmeaParse(l.c_str(), s) == NMEA_RMC && !s.posOk && !s.dateOk);
    l = withSum("GNRMC,083559.00,A,3723.2475,N,12158.3416,E,,,290228,,,A");   // 2028 is a leap year
    assert(nmeaParse(l.c_str(), s) == NMEA_RMC && s.dateOk && s.ymd == 20280229);
}

static void testGga()
{
    NmeaSentence s;
    assert(nmeaParse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", s) == NMEA_GGA);
    assert(s.timeOk && s.quality == 1 && s.sats == 8 && near(s.hdop, 0.9, 1e-6));
    assert(s.altOk && near(s.altM, 545.4, 1e-4) && s.posOk && near(s.lat, 48.1173, 1e-4));
    // L76K-style, GN talker, no fix yet
    std::string l = withSum("GNGGA,000013.00,,,,,0,00,25.5,,,,,,");
    assert(nmeaParse(l.c_str(), s) == NMEA_GGA && s.quality == 0 && s.sats == 0 && !s.posOk && !s.altOk);
    // quality 0 with stale numbers: not a position
    l = withSum("GNGGA,083559.00,4807.038,N,01131.000,E,0,03,4.1,545.4,M,46.9,M,,");
    assert(nmeaParse(l.c_str(), s) == NMEA_GGA && !s.posOk && !s.altOk && s.sats == 3);
    // GL talker, negative altitude, DGPS
    l = withSum("GLGGA,083559.00,3130.0000,N,03530.0000,E,2,12,0.7,-415.2,M,20.1,M,1.0,0000");
    assert(nmeaParse(l.c_str(), s) == NMEA_GGA && s.quality == 2 && s.altOk && near(s.altM, -415.2, 1e-3));
}

static void testZdaAndOthers()
{
    NmeaSentence s;
    std::string l = withSum("GNZDA,083559.00,26,11,2026,00,00");
    assert(nmeaParse(l.c_str(), s) == NMEA_ZDA && s.timeOk && s.dateOk && s.ymd == 20261126);
    // 2026-11-26 08:35:59 UTC
    assert(nmeaUnixMs(20261126, (8 * 3600 + 35 * 60 + 59) * 1000u) == 1795682159LL * 1000);
    assert(nmeaUnixMs(19700101, 0) == 0);
    // fields out of range (a huge one would be undefined behaviour as an int): no date
    l = withSum("GNZDA,083559.00,99999999999,11,2026,00,00");
    assert(nmeaParse(l.c_str(), s) == NMEA_ZDA && s.timeOk && !s.dateOk);
    l = withSum("GNZDA,083559.00,26,-1e15,2026,00,00");
    assert(nmeaParse(l.c_str(), s) == NMEA_ZDA && !s.dateOk);
    l = withSum("GNZDA,083559.00,26,11,1e18,00,00");
    assert(nmeaParse(l.c_str(), s) == NMEA_ZDA && !s.dateOk);
    l = withSum("GNZDA,083559.00,31,11,2026,00,00");   // November 31
    assert(nmeaParse(l.c_str(), s) == NMEA_ZDA && !s.dateOk);
    l = withSum("GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00");
    assert(nmeaParse(l.c_str(), s) == NMEA_OTHER);
    l = withSum("PCAS06,0");   // proprietary (CASIC, L76K)
    assert(nmeaParse(l.c_str(), s) == NMEA_OTHER);
    l = withSum("GNTXT,01,01,02,ANTENNA OPEN");
    assert(nmeaParse(l.c_str(), s) == NMEA_OTHER);
}

static void testBad()
{
    NmeaSentence s;
    assert(nmeaParse("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6B", s) == NMEA_BAD);   // wrong sum
    assert(nmeaParse("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W", s) == NMEA_BAD);      // no sum
    assert(nmeaParse("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A", s) == NMEA_BAD);     // no '$'
    assert(nmeaParse("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*ZZ", s) == NMEA_BAD);    // not hex
    assert(nmeaParse("$GPRMC,123519,A,4807.0*", s) == NMEA_BAD);
    assert(nmeaParse("", s) == NMEA_NONE && nmeaParse("\r\n", s) == NMEA_NONE);
    // lower-case checksum digits are fine, CR LF too
    std::string l = withSum("GNZDA,083559.00,26,11,2026,00,00");
    for (size_t k = l.size() - 2; k < l.size(); k++) if (l[k] >= 'A' && l[k] <= 'F') l[k] = (char)(l[k] - 'A' + 'a');
    l += "\r\n";
    assert(nmeaParse(l.c_str(), s) == NMEA_ZDA);
}

static void testReader()
{
    NmeaReader r;
    NmeaSentence s;
    int rmc = 0, gga = 0, bad = 0, other = 0;
    auto feed = [&](const std::string& text) {
        for (char c : text) {
            const NmeaType t = r.feed(c, s);
            if (t == NMEA_RMC) rmc++;
            else if (t == NMEA_GGA) gga++;
            else if (t == NMEA_BAD) bad++;
            else if (t == NMEA_OTHER) other++;
        }
    };
    // noise before the first '$' (module power-up, wrong baud) is skipped
    feed("\x13\xfe garbage ");
    feed(withSum("GNRMC,083559.00,A,3723.2475,N,12158.3416,E,,,261126,,,A") + "\r\n");
    feed(withSum("GNGGA,083559.00,3723.2475,N,12158.3416,E,1,09,1.1,12.0,M,0,M,,") + "\r\n");
    assert(rmc == 1 && gga == 1 && bad == 0);
    // a line cut off by the next '$' (bytes lost), then a good one
    const std::string cut = withSum("GNRMC,083600.00,A,3723.2475,N,12158.3416,E,,,261126,,,A");
    feed(cut.substr(0, 20));
    feed(cut + "\r\n");
    assert(bad == 1 && rmc == 2);
    // too long: dropped at its end, the next line is read again
    feed("$GPGSV" + std::string(200, ',') + "*00\r\n");
    assert(bad == 2);
    feed(cut + "\n");
    assert(rmc == 3);
    // a partial line at the end stays pending
    feed(cut.substr(0, 30));
    assert(rmc == 3 && bad == 2 && other == 0);
}

static void testThresholds()
{
    // fix good enough: quality, satellites, HDOP
    assert(gnssFixGood(1, 4, 2.0f) && gnssFixGood(2, 12, 0.6f));
    assert(!gnssFixGood(0, 10, 0.8f) && !gnssFixGood(1, 3, 0.8f) && !gnssFixGood(1, 8, 2.1f) && !gnssFixGood(1, 8, 0.0f));
    assert(!gnssFixGood(6, 8, 1.0f));   // dead reckoning
    // distance: 0.001 deg latitude ~ 111 m
    assert(near(gnssDistanceM(47.0, 11.0, 47.001, 11.0), 111.2, 0.5));
    assert(near(gnssDistanceM(47.0, 11.0, 47.0, 11.001), 75.8, 0.5));   // cos(47)
    // position: none stored -> take; 80 m -> keep; 111 m -> take
    assert(gnssPositionChanged(false, 0, 0, 47.0, 11.0));
    assert(!gnssPositionChanged(true, 47.0, 11.0, 47.00072, 11.0));
    assert(gnssPositionChanged(true, 47.0, 11.0, 47.001, 11.0));
    // altitude: 5 m is not a change, 5.5 m is
    assert(!gnssAltitudeChanged(560, 565.0f) && !gnssAltitudeChanged(560, 555.0f));
    assert(gnssAltitudeChanged(560, 565.5f) && gnssAltitudeChanged(560, 554.4f));
    assert(gnssAltitudeSetting(565.5f) == 566 && gnssAltitudeSetting(-20.0f) == 0 && gnssAltitudeSetting(5000.0f) == 4000);
    // the 5-min mean: noise cancels
    GnssMean m;
    double la = 0, lo = 0;
    bool aOk = false;
    float a = 0;
    int windows = 0;
    for (int i = 0; i < 2 * (int)GnssMean::WINDOW; i++) {
        const double d = (i % 2 ? 1 : -1) * 0.0003;   // +-33 m
        if (m.add(47.0 + d, 11.0 - d, true, 560.0f + (i % 2 ? 8.0f : -8.0f), la, lo, aOk, a)) {
            windows++;
            assert(near(la, 47.0, 1e-9) && near(lo, 11.0, 1e-9) && aOk && near(a, 560.0, 1e-3));
            assert(!gnssPositionChanged(true, 47.0, 11.0, la, lo) && !gnssAltitudeChanged(560, a));
        }
    }
    assert(windows == 2);
    // altitude missing in most fixes: no altitude mean
    for (int i = 0; i < (int)GnssMean::WINDOW; i++)
        if (m.add(47.0, 11.0, i < 10, 600.0f, la, lo, aOk, a)) assert(!aOk);
    // the antimeridian: fixes +-1 m around 180 deg average to 180, not 0
    assert(near(gnssWrapLon(190.0), -170.0, 1e-9) && near(gnssWrapLon(-190.0), 170.0, 1e-9) && gnssWrapLon(180.0) == 180.0);
    assert(near(gnssDistanceM(-17.0, 179.9995, -17.0, -179.9995), 106.4, 0.5));   // across it: ~106 m, not 38 000 km
    windows = 0;
    for (int i = 0; i < (int)GnssMean::WINDOW; i++) {
        const double d = 0.00001;   // ~1 m
        if (m.add(-17.0, i % 2 ? 180.0 - d : -180.0 + d, false, 0.0f, la, lo, aOk, a)) {
            windows++;
            assert(near(std::fabs(lo), 180.0, 1e-6));
            assert(!gnssPositionChanged(true, -17.0, 180.0, la, lo) && !gnssPositionChanged(true, -17.0, -180.0, la, lo));
        }
    }
    assert(windows == 1);
}

static void testTimestamp()
{
    uint64_t rx;
    uint32_t acc;
    // PPS 300 ms before the line, whole second, the edge of its burst: the edge
    assert(gnssTimeStamp(10300, true, true, 10000, 5000, rx, acc) && rx == 10000 && acc == GNSS_ACC_PPS_MS);
    // edge too long before (a lost second), or after the line, or a fraction: the line time
    assert(!gnssTimeStamp(11000, true, true, 10000, 5000, rx, acc) && rx == 11000 && acc == GNSS_ACC_NMEA_MS);
    assert(!gnssTimeStamp(10300, true, true, 10400, 5000, rx, acc) && rx == 10300);
    assert(!gnssTimeStamp(10300, true, true, 10000, 5500, rx, acc) && rx == 10300);
    assert(!gnssTimeStamp(10300, false, true, 0, 5000, rx, acc) && rx == 10300 && acc == GNSS_ACC_NMEA_MS);
    // a late sentence (9600 baud, RMC at the end of a long burst) read 50 ms
    // after the NEXT second's edge: that edge is not of its burst - the line
    // time, not the 20-ms class 1 s off
    assert(!gnssTimeStamp(11050, true, false, 11000, 5000, rx, acc) && rx == 11050 && acc == GNSS_ACC_NMEA_MS);

    // bursts: the edge count when the first bytes after an idle poll came
    GnssBurst b;
    assert(!b.edgeOfBurst(0));
    b.poll(true, 4);                    // bytes without an idle poll before: unknown
    assert(!b.edgeOfBurst(4));
    b.poll(false, 4);                   // idle
    b.poll(true, 5);                    // burst of second 5 starts after edge 5
    assert(b.edgeOfBurst(5));
    b.poll(true, 5);                    // the burst goes on
    assert(b.edgeOfBurst(5));
    b.poll(true, 5);
    assert(!b.edgeOfBurst(6));          // edge 6 came during the burst: RMC read after it is not stamped with it
    b.poll(true, 6);                    // no idle poll between: burst of second 6 unknown
    assert(!b.edgeOfBurst(6));
    b.poll(false, 6);
    b.poll(true, 7);                    // next burst after an idle poll
    assert(b.edgeOfBurst(7) && !b.edgeOfBurst(8));
    // timely: drained and polled within 250 ms
    assert(gnssLineTimely(true, 50) && gnssLineTimely(true, 250));
    assert(!gnssLineTimely(true, 2600) && !gnssLineTimely(false, 50));
    // baud
    assert(gnssBaudValid(0) && gnssBaudValid(9600) && gnssBaudValid(38400) && gnssBaudValid(115200));
    assert(!gnssBaudValid(4800) && !gnssBaudValid(57600) && !gnssBaudValid(-1));
    assert(gnssNextBaud(9600) == 115200 && gnssNextBaud(115200) == 38400 && gnssNextBaud(38400) == 9600 && gnssNextBaud(1) == 9600);
}

static void testBmePins()
{
    // PCB: 41/42, nothing else probed
    BmePins p = bmePinSelect(true, false, false, true);
    assert(p.sda == 41 && p.scl == 42 && !p.oldWiring);
    assert(!bmeProbeOldPins(true, false, false));
    p = bmePinSelect(true, false, false, false);
    assert(p.sda == 41 && p.scl == 42 && !p.oldWiring);
    // wireless build: 47/48 found
    assert(!bmeProbeOldPins(false, false, true));
    p = bmePinSelect(false, false, true, false);
    assert(p.sda == 47 && p.scl == 48 && !p.oldWiring);
    p = bmePinSelect(false, true, true, false);
    assert(p.sda == 47 && p.scl == 48 && !p.oldWiring);
    // nothing on 47/48, GNSS off: probe 41/42; found -> old wiring
    assert(bmeProbeOldPins(false, false, false));
    p = bmePinSelect(false, false, false, true);
    assert(p.sda == 41 && p.scl == 42 && p.oldWiring);
    // ... not found: wait on 47/48
    p = bmePinSelect(false, false, false, false);
    assert(p.sda == 47 && p.scl == 48 && !p.oldWiring);
    // GNSS on: 41/42 never probed, never used
    assert(!bmeProbeOldPins(false, true, false));
    p = bmePinSelect(false, true, false, true);
    assert(p.sda == 47 && p.scl == 48 && !p.oldWiring);
    // GNSS only without PCB
    assert(gnssActiveFor(false, 1) && !gnssActiveFor(true, 1) && !gnssActiveFor(false, 0));
    // UART pins: module TX -> ESP RX 38 (one constant swaps both)
    assert(GNSS_PIN_ESP_RX == (GNSS_UART_SWAPPED ? 39 : 38) && GNSS_PIN_ESP_TX == (GNSS_UART_SWAPPED ? 38 : 39));
    assert(GNSS_PIN_PPS == 41 && GNSS_PIN_RESET == 42 && GNSS_PIN_WAKE == 40 && GNSS_PIN_VCTRL == 34);
}

int main()
{
    testRmc();
    testGga();
    testZdaAndOthers();
    testBad();
    testReader();
    testThresholds();
    testTimestamp();
    testBmePins();
    puts("gnss ok");
    return 0;
}
