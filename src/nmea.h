#pragma once

// NMEA 0183 reader for a GNSS module (B9; own code, host-tested in
// test/firmware_gnss.cpp). Only what the node needs: RMC (UTC date + time,
// status, position), GGA (fix quality, satellites, HDOP, altitude above mean
// sea level, position) and ZDA (UTC date + time). Any two-letter talker
// (GP, GN, GL, GA, GB, BD ...) is accepted; every sentence needs a valid
// checksum ("*hh"), others are counted as bad and ignored.

#include <stdint.h>
#include <stddef.h>

enum NmeaType : uint8_t {
    NMEA_NONE = 0,   // no complete line yet / empty line
    NMEA_RMC,
    NMEA_GGA,
    NMEA_ZDA,
    NMEA_OTHER,      // valid checksum, a sentence the node does not read (GSV, GSA, TXT ...)
    NMEA_BAD         // no '$', no or wrong checksum, too long, malformed field
};

struct NmeaSentence {
    NmeaType type = NMEA_NONE;
    // UTC time of day (all three types); timeOk = the field was present and valid
    bool     timeOk = false;
    uint32_t msOfDay = 0;
    // UTC date (RMC, ZDA) as YYYYMMDD
    bool     dateOk = false;
    int32_t  ymd = 0;
    // RMC: status 'A' (valid fix) / 'V'; mode indicator (NMEA 2.3+: A, D, E, F, R, N), 0 = none
    bool     active = false;
    char     mode = 0;
    // position (RMC, GGA), decimal degrees, north / east positive
    bool     posOk = false;
    double   lat = 0, lon = 0;
    // GGA: fix quality (0 none, 1 GPS, 2 DGPS, 4/5 RTK, 6 dead reckoning), satellites in use,
    // HDOP (0 = field empty), altitude above mean sea level (m)
    uint8_t  quality = 0;
    uint8_t  sats = 0;
    float    hdop = 0;
    bool     altOk = false;
    float    altM = 0;
};

// Parse one line ("$GNRMC,...*hh", with or without the trailing CR/LF).
NmeaType nmeaParse(const char* line, NmeaSentence& out);

// Unix time (ms) of a date (YYYYMMDD) and a time of day.
int64_t nmeaUnixMs(int32_t ymd, uint32_t msOfDay);

// Line assembly from the UART byte by byte, bounded: a line longer than the
// buffer (NMEA allows 82 characters) is dropped as NMEA_BAD at its end.
class NmeaReader {
public:
    static constexpr size_t MAX_LINE = 100;
    // Feed one byte; at the end of a line the sentence is parsed into out and
    // its type returned, NMEA_NONE otherwise.
    NmeaType feed(char c, NmeaSentence& out);
    void reset() { len = 0; overflow = false; }
private:
    char   buf[MAX_LINE + 1];
    size_t len = 0;
    bool   overflow = false;
};
