#pragma once
// Upload formats of Met Office WOW and CWOP (APRS-IS), pure and
// host-tested (test/firmware_wxformats.cpp). Metric input (Reading-like).
#include <stddef.h>

struct WxObs {
    bool  hasTemp = false, hasHum = false, hasPress = false, hasWind = false, hasRain = false;
    float tempC = 0, humidity = 0, qnhHpa = 0;
    float windMs = 0, gustMs = 0;
    bool  hasDir = false;     // with hasWind: the direction is known (else left out / dots)
    int   dirDeg = 0;
    float rain1hMm = 0, rain24hMm = 0, rainDayMm = 0;
};

// Met Office WOW "automaticreading" (wow.metoffice.gov.uk/support/dataformats):
// siteid, siteAuthenticationKey, dateutc "YYYY-mm-DD HH:mm:ss" (UTC), softwaretype,
// WU-style imperial values (tempf, humidity, dewptf, baromin, windspeedmph,
// windgustmph, winddir, dailyrainin). false = no room.
bool wowUrl(const WxObs& o, const char* siteId, const char* pin, const char* dateUtc, const char* software,
            char* out, size_t len);

// One APRS weather report for CWOP (APRS 1.01 "complete weather report",
// position with timestamp, symbol "/_"):
//   CALL>APRS,TCPIP*:@DDHHMMzDDMM.mmN/DDDMM.mmE_ddd/sssgGGGtTTTrRRRpPPPPPPPhHHbBBBBB<software>
// wind mph, temperature °F, rain hundredths of an inch (last hour, 24 h,
// since midnight), humidity % (100 = "00"), pressure tenths of hPa (QNH);
// missing values as dots. day/hour/minute UTC.
bool cwopPacket(const WxObs& o, const char* call, double lat, double lon, int day, int hour, int minute,
                const char* software, char* out, size_t len);
// APRS-IS login line ("user CALL pass -1 vers SOFT VER"); CWOP IDs (CW, DW,
// EW + digits) use passcode -1, licensed hams their APRS passcode.
bool cwopLogin(const char* call, int passcode, const char* software, const char* version, char* out, size_t len);
