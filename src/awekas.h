#pragma once
// AWEKAS upload string (wx_upload.cpp): the "val" parameter of
// http://data.awekas.at/eingabe_pruefung.php - semicolon-separated fields in
// a fixed order, an absent value stays empty (";;"). Field order and units
// from the AWEKAS upload API description (legacy "eingabe_pruefung.php"
// interface, also documented e.g. in github.com/wind101net/atmo issue 65):
//   1 user  2 password (MD5 hex)  3 date dd.mm.yyyy  4 time hh:mm (local)
//   5 temperature C  6 humidity %  7 air pressure hPa  8 rain today mm
//   9 wind speed km/h  10 wind direction deg  11 weather condition
//  12 warning text  13 snow height cm  14 language
//  15 pressure tendency (-2 high falling, -1 falling, 0 steady, 1 rising, 2 high rising)
//  16 wind gust km/h  17 solar radiation W/m2  18 UV index  19 brightness lux (int)
//  20 sunshine hours today (decimal)  21 soil temperature C  22 rain rate mm/h
//  23 software flag  24 longitude  25 latitude
// Pure C (host-tested, test/firmware_awekas.cpp).
#include <stddef.h>
#include <stdint.h>

struct AwekasData {
    const char* user = "";
    const char* md5Pass = "";
    const char* date = "";          // dd.mm.yyyy, local
    const char* time = "";          // hh:mm, local
    bool  hasTemp = false;     float tempC = 0;
    bool  hasHum = false;      float humidity = 0;
    bool  hasPress = false;    float pressureHpa = 0;   // sea level (QNH)
    bool  hasRain = false;     float dailyRainMm = 0, rainRateMmH = 0;
    bool  hasWind = false;     float windKmh = 0, gustKmh = 0; int dirDeg = 0;
    bool  hasTendency = false; int tendency = 0;        // -2..2, awekasTendency()
    bool  hasSolar = false;    float solarWm2 = 0;      // measured only (Davis solar radiation sensor), never from lux
    bool  hasUv = false;       float uvIndex = 0;
    bool  hasLux = false;      float lux = 0;
    bool  hasSunshine = false; float sunshineHours = 0; // today, from the node's sunshine statistics
    const char* software = "";
    const char* lon = "";
    const char* lat = "";
};

// Writes the val string (not URL-encoded); returns its length (truncated to len-1).
size_t awekasVal(const AwekasData& d, char* out, size_t len);

// The node's 3-h pressure tendency classes (derived.h pressureTendency())
// mapped to AWEKAS' -2..2: steady 0; slowly / normal +-1; quickly / very
// rapidly +-2.
int awekasTendency(float delta3hHpa);
