#pragma once

// GNSS module on the V4's GNSS connector (B9), only on a V4 without PCB.
// Pins: heltec_board.h. NMEA: nmea.h. Rules: gnss_logic.h.
//
// The UART is read in the main loop (gnssPoll(), non-blocking, bounded). A
// valid RMC (status A) - or a ZDA during a fix - gives the time as source
// TSRC_GNSS (time_sync.h), timestamped by the PPS edge when there is one.
// Good fixes (gnss_logic.h gnssFixGood) are averaged over 5 min; latitude /
// longitude and the station altitude (sea-level pressure) go into the
// settings when they changed by more than 100 m / 5 m - each switchable.

#include <stdint.h>

struct GnssStatus {
    bool     active = false;     // switched on and allowed (V4 without PCB)
    bool     seen = false;       // a valid sentence within the last 5 s
    uint32_t baud = 0;           // current rate (while searching: the one tried)
    bool     baudFound = false;  // locked on a rate that delivers valid sentences
    bool     fix = false;        // RMC status A or GGA quality >= 1 within the last 3 s
    uint8_t  quality = 0, sats = 0;
    float    hdop = 0;
    bool     posOk = false;
    double   lat = 0, lon = 0;
    bool     altOk = false;
    float    altM = 0;
    bool     pps = false;        // a PPS edge within the last 2 s
    bool     timeUsed = false;   // a GNSS time was applied / confirmed since the start
    uint32_t timeAgeSec = 0;     // ... this long ago
    bool     timePps = false;    // ... with the PPS edge
    uint32_t sentences = 0, bad = 0;
};

void gnssSetup();   // after sensorsInit(): powers the module, starts UART1 and the PPS interrupt
void gnssPoll();    // every loop() pass
void gnssStatus(GnssStatus& out);   // any task
bool gnssActive();
