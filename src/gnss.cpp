#include "gnss.h"
#include "gnss_logic.h"
#include "nmea.h"
#include "heltec_board.h"
#include "globals.h"
#include "config.h"
#include "time_sync.h"
#include "wx_upload.h"
#include "serial_log.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <mutex>
#include <stdlib.h>

namespace {

constexpr size_t   UART_RX_BUFFER = 1024;   // 115200 baud: ~580 bytes per 50-ms loop pass
constexpr size_t   POLL_BUDGET = 2048;      // bytes per gnssPoll() at most
constexpr uint32_t SEEN_MS = 5000, FIX_MS = 3000, PPS_RECENT_MS = 2000;
constexpr uint32_t OFFER_EVERY_MS = 60000;  // once the clock follows GNSS (fresh 1 h)

bool          active = false;
NmeaReader    reader;
GnssMean      mean;
std::mutex    lock;          // st: written by the main loop, read by the web task
GnssStatus    st;

// baud search (gnss_logic.h)
bool     baudFixed = false, baudLocked = false;
uint32_t baud = 9600;
uint64_t baudSinceMs = 0, lastValidMs = 0;

// timing of the reads
uint64_t lastPollMs = 0;
bool     drained = false;
GnssBurst burst;             // which PPS edge the sentences of a burst belong to

// fix state of the last sentences
uint64_t lastRmcMs = 0, lastGgaMs = 0;
bool     rmcActive = false;
uint8_t  ggaQuality = 0;

// time
uint64_t lastOfferMs = 0, lastTimeMs = 0;
bool     haveTime = false, lastPps = false;
bool     timeOn = false;     // gCfg.gnssTime as of the last poll (switched off: the GNSS time slot expires)

// PPS edges (ISR)
volatile int64_t  ppsUs = 0;
volatile uint32_t ppsCount = 0;

void IRAM_ATTR ppsIsr()
{
    ppsUs = esp_timer_get_time();
    ppsCount = ppsCount + 1;
}

// The last edge (uptime ms) and the edge count, consistent against a
// concurrent interrupt.
bool lastPpsMs(uint64_t& ms, uint32_t& count)
{
    uint32_t c;
    int64_t us;
    do {
        c = ppsCount;
        us = ppsUs;
    } while (c != ppsCount);
    count = c;
    if (!c) return false;
    ms = (uint64_t)(us / 1000);
    return true;
}

void startUart(uint32_t b)
{
    Serial1.updateBaudRate(b);
    reader.reset();
    baudSinceMs = uptimeMs64();
}

void offerTime(int32_t ymd, uint32_t msOfDay, bool timely, uint64_t lineMs)
{
    if (!gCfg.gnssTime || !timely) return;
    if (haveTime && lineMs - lastOfferMs < OFFER_EVERY_MS) return;
    uint64_t ppsMs = 0;
    uint32_t edges = 0;
    const bool havePps = lastPpsMs(ppsMs, edges) && ppsMs <= lineMs && lineMs - ppsMs < PPS_RECENT_MS;
    uint64_t rxMs;
    uint32_t accMs;
    const bool withPps = gnssTimeStamp(lineMs, havePps, burst.edgeOfBurst(edges), ppsMs, msOfDay, rxMs, accMs);
    lastOfferMs = lineMs;
    const TimeSources::Result r = timeSourceOffer(TSRC_GNSS, nmeaUnixMs(ymd, msOfDay), rxMs, accMs);
    if (r != TimeSources::TS_SET && r != TimeSources::TS_CONFIRMED) return;
    if (!haveTime || withPps != lastPps) {
        if (withPps) dbgSerial.printf("GNSS: time %s (PPS edge, ~20 ms; sentence read %lu ms after the edge)\n",
                                      haveTime ? "now" : "taken", (unsigned long)(lineMs - ppsMs));
        else         dbgSerial.printf("GNSS: time %s (NMEA only, ~1 s%s)\n", haveTime ? "now" : "taken",
                                      havePps ? "; PPS edge not of the sentence's second" : "; no PPS");
    }
    haveTime = true;
    lastPps = withPps;
    lastTimeMs = lineMs;
}

// A 5-min mean of good fixes: into the settings if it moved (gnss_logic.h).
void takeOver(double lat, double lon, bool altOk, float alt)
{
    bool posSave = false, altSave = false;
    if (gCfg.gnssPos) {
        wxUploadLock();   // gCfg.latitude / longitude: read by the upload and web tasks
        char* e1;
        char* e2;
        const double la0 = strtod(gCfg.latitude, &e1), lo0 = strtod(gCfg.longitude, &e2);
        const bool have = gCfg.latitude[0] && gCfg.longitude[0] && e1 != gCfg.latitude && e2 != gCfg.longitude;
        if (gnssPositionChanged(have, la0, lo0, lat, lon)) {
            snprintf(gCfg.latitude, sizeof(gCfg.latitude), "%.5f", lat);
            snprintf(gCfg.longitude, sizeof(gCfg.longitude), "%.5f", lon);
            posSave = true;
        }
        wxUploadUnlock();
    }
    if (gCfg.gnssAlt && altOk && gnssAltitudeChanged(gCfg.altitudeM, alt)) {
        const int old = gCfg.altitudeM;
        gCfg.altitudeM = gnssAltitudeSetting(alt);
        altSave = gCfg.altitudeM != old;
    }
    if (!posSave && !altSave) return;
    saveStationPosition(gCfg, posSave, altSave);
    if (posSave) dbgSerial.printf("GNSS: position %.5f, %.5f taken into the settings (moved > 100 m)\n", lat, lon);
    if (altSave) dbgSerial.printf("GNSS: altitude %d m taken into the settings (sea-level pressure)\n", gCfg.altitudeM);
}

void handle(const NmeaSentence& s, bool timely, uint64_t now)
{
    if (s.type == NMEA_BAD) {
        std::lock_guard<std::mutex> g(lock);
        st.bad++;
        return;
    }
    lastValidMs = now;
    if (!baudLocked) {
        baudLocked = true;
        dbgSerial.printf("GNSS: module answers at %lu baud\n", (unsigned long)baud);
    }
    {
        std::lock_guard<std::mutex> g(lock);
        st.sentences++;
    }
    if (s.type == NMEA_RMC) {
        lastRmcMs = now;
        rmcActive = s.active;
        if (s.active && s.timeOk && s.dateOk) offerTime(s.ymd, s.msOfDay, timely, now);
        if (s.active && s.posOk && now - lastGgaMs > FIX_MS) {   // a module without GGA: the position from RMC
            std::lock_guard<std::mutex> g(lock);
            st.posOk = true; st.lat = s.lat; st.lon = s.lon;
        }
    } else if (s.type == NMEA_GGA) {
        lastGgaMs = now;
        ggaQuality = s.quality;
        {
            std::lock_guard<std::mutex> g(lock);
            st.quality = s.quality; st.sats = s.sats; st.hdop = s.hdop;
            if (s.posOk) { st.posOk = true; st.lat = s.lat; st.lon = s.lon; }
            if (s.altOk) { st.altOk = true; st.altM = s.altM; }
        }
        if (s.posOk && gnssFixGood(s.quality, s.sats, s.hdop)) {
            double la, lo;
            bool aOk;
            float a;
            if (mean.add(s.lat, s.lon, s.altOk, s.altM, la, lo, aOk, a)) takeOver(la, lo, aOk, a);
        }
    } else if (s.type == NMEA_ZDA) {
        const bool fix = (now - lastRmcMs < FIX_MS && rmcActive) || (now - lastGgaMs < FIX_MS && ggaQuality >= 1);
        if (fix && s.timeOk && s.dateOk) offerTime(s.ymd, s.msOfDay, timely, now);
    }
}

} // namespace

bool gnssActive() { return active; }

void gnssSetup()
{
    if (!gnssActiveFor(boardPcbAllowed(), gCfg.gnssEn)) {
        if (gCfg.gnssEn) dbgSerial.println("GNSS: switched on, but not on a V4 with PCB (pins 34, 38-42 belong to the PCB) - off.");
        return;
    }
    // module awake, out of reset, then its supply on
    pinMode(GNSS_PIN_RESET, OUTPUT);
    digitalWrite(GNSS_PIN_RESET, HIGH);
    pinMode(GNSS_PIN_WAKE, OUTPUT);
    digitalWrite(GNSS_PIN_WAKE, HIGH);
    pinMode(GNSS_PIN_VCTRL, OUTPUT);
    digitalWrite(GNSS_PIN_VCTRL, GNSS_VCTRL_ON ? HIGH : LOW);
    pinMode(GNSS_PIN_PPS, INPUT_PULLDOWN);   // no module: no stray edges
    attachInterrupt(digitalPinToInterrupt(GNSS_PIN_PPS), ppsIsr, RISING);

    baudFixed = gCfg.gnssBaud != 0;
    baud = baudFixed ? (uint32_t)gCfg.gnssBaud : 9600;
    Serial1.setRxBufferSize(UART_RX_BUFFER);
    Serial1.begin(baud, SERIAL_8N1, GNSS_PIN_ESP_RX, GNSS_PIN_ESP_TX);
    baudSinceMs = lastPollMs = uptimeMs64();
    active = true;
    dbgSerial.printf("GNSS: on (supply GPIO%d %s, RX GPIO%d, TX GPIO%d, PPS GPIO%d, reset GPIO%d, wake GPIO%d), %s %lu baud\n",
                     GNSS_PIN_VCTRL, GNSS_VCTRL_ON ? "HIGH" : "LOW", GNSS_PIN_ESP_RX, GNSS_PIN_ESP_TX, GNSS_PIN_PPS,
                     GNSS_PIN_RESET, GNSS_PIN_WAKE, baudFixed ? "fixed" : "searching from", (unsigned long)baud);
}

void gnssPoll()
{
    if (!active) return;
    const uint64_t now = uptimeMs64();
    const bool timely = gnssLineTimely(drained, now - lastPollMs);
    lastPollMs = now;
    if (timeOn && !gCfg.gnssTime) {   // "Take the time" switched off: other sources take over at once
        timeSourceExpire(TSRC_GNSS);
        haveTime = false;
    }
    timeOn = gCfg.gnssTime;
    const uint32_t edgesBefore = ppsCount;
    size_t budget = POLL_BUDGET;
    int c;
    while (budget && (c = Serial1.read()) >= 0) {
        if (budget == POLL_BUDGET) burst.poll(true, edgesBefore);
        budget--;
        NmeaSentence s;
        const NmeaType t = reader.feed((char)c, s);
        if (t != NMEA_NONE) handle(s, timely, now);
    }
    if (budget == POLL_BUDGET) burst.poll(false, edgesBefore);
    drained = Serial1.available() == 0;

    // baud search; a module silent for 30 s (unplugged, another one): search again
    if (baudLocked && now - lastValidMs > GNSS_SILENT_MS) {
        baudLocked = false;
        baudSinceMs = now;
        dbgSerial.println("GNSS: no valid NMEA for 30 s");
    }
    if (!baudFixed && !baudLocked && now - baudSinceMs > GNSS_BAUD_TRY_MS) {
        baud = gnssNextBaud(baud);
        startUart(baud);
    }

    std::lock_guard<std::mutex> g(lock);
    st.active = true;
    st.baud = baud;
    st.baudFound = baudLocked;
    st.seen = lastValidMs && now - lastValidMs < SEEN_MS;
    st.fix = (lastRmcMs && now - lastRmcMs < FIX_MS && rmcActive) || (lastGgaMs && now - lastGgaMs < FIX_MS && ggaQuality >= 1);
    uint64_t p;
    uint32_t n;
    st.pps = lastPpsMs(p, n) && (p > now || now - p < PPS_RECENT_MS);
    st.timeUsed = haveTime;
    st.timePps = lastPps;
    st.timeAgeSec = haveTime ? (uint32_t)((now - lastTimeMs) / 1000) : 0;
    if (!st.seen) { st.quality = 0; st.sats = 0; }
    if (!st.fix) st.posOk = st.altOk = false;   // the last position is not a current one
}

void gnssStatus(GnssStatus& out)
{
    std::lock_guard<std::mutex> g(lock);
    out = st;
    out.active = active;
}
