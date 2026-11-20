// Davis ISS decoder and hop tracker (lib/DavisSensorReceiver), host tests.
// Sample packets: published captures (kobuki/VPTools docs/packet-samples,
// weewx-meteostick comments, DavisRFM69 wiki "Message Protocol"), all in
// firmware byte order; their CRCs were checked independently.
#include "DavisDecoder.h"
#include "DavisHopper.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace {

bool near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; }

void hex(const char* s, uint8_t* out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)strtoul(s, nullptr, 16);
        s += 3;
    }
}

// Decodes a firmware-order sample given as hex (8 bytes = FF FF appended).
DavisPacket sample(const char* s, DavisModel m = DAVIS_VP2)
{
    uint8_t b[DAVIS_PACKET_LEN] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
    size_t n = (strlen(s) + 1) / 3;
    hex(s, b, n);
    DavisPacket p;
    bool ok = davisDecodeOrdered(b, p, m);
    if (!ok) { fprintf(stderr, "sample %s: CRC failed\n", s); assert(false); }
    assert(p.crc == DAVIS_CRC_OK);
    return p;
}

void toAir(const uint8_t* fw, uint8_t* air)
{
    for (size_t i = 0; i < DAVIS_PACKET_LEN; i++) air[i] = davisReverseBits(fw[i]);
}

void testBasics()
{
    assert(davisReverseBits(0x01) == 0x80 && davisReverseBits(0xCB) == 0xD3 && davisReverseBits(0xFF) == 0xFF);
    for (int i = 0; i < 256; i++) assert(davisReverseBits(davisReverseBits((uint8_t)i)) == i);
    const uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    assert(davisCrc16(check, 9) == 0x31C3);   // CRC-16/XMODEM check value
    assert(davisIntervalUs(1) == 2562500 && davisIntervalUs(8) == 3000000 && davisIntervalUs(0) == 0 && davisIntervalUs(9) == 0);
    assert(near(davisRainRateMmH(18.0f, 0.2f), 40.0f) && davisRainRateMmH(0.0f, 0.2f) == 0.0f);
    assert(!strcmp(davisTypeName(DAVIS_MSG_TEMP), "temp") && !strcmp(davisTypeName(0xE), "rain"));
}

void testSamples()
{
    // EU captures (868.42 / 868.30 MHz, station 2): rain counter 0, no sensor
    DavisPacket p = sample("E1 00 72 80 03 00 0F 8C FF FF");
    assert(p.type == DAVIS_MSG_RAIN && p.txId == 2 && !p.batteryLow && !p.hasRainCount);
    assert(p.windMph == 0 && p.windDirRaw == 0x72 && p.hasWindDir && near(p.windDirDeg, 9.0f + 113 * 342.0f / 253.0f));
    p = sample("C1 01 72 00 03 00 AB 8F FF FF");
    assert(p.type == DAVIS_MSG_UNKNOWNC && p.windMph == 1);
    p = sample("E0 00 00 80 05 00 4F 97 FF FF");           // US, station 1, no wind data
    assert(p.txId == 1 && !p.hasWindDir);
    // supercap (Vue)
    p = sample("20 04 C3 D4 C1 81 89 EE", DAVIS_VUE);
    assert(p.type == DAVIS_MSG_SUPERCAP && p.hasSupercap && p.raw10 == 851 && near(p.supercapV, 851 / 300.0f));
    assert(near(p.windDirDeg, 0xC3 * 1.40625f + 0.3f) && p.windMph == 4);
    // UV
    p = sample("40 00 00 12 45 00 B5 2A");
    assert(p.hasUv && p.raw10 == 73 && near(p.uvIndex, 1.46f));
    assert(!sample("41 00 DE FF C3 00 A9 8D").hasUv);
    // rain rate: no rain, light (seconds), heavy (1/16 s)
    p = sample("50 00 00 FF 75 00 48 5B");
    assert(p.hasRainRate && p.rainTipSec == 0.0f);
    p = sample("50 00 00 FE 75 00 7F 6B");
    assert(p.hasRainRate && near(p.rainTipSec, 1022.0f));
    p = sample("50 00 00 1B 15 00 3F 80");
    assert(p.hasRainRate && near(p.rainTipSec, 283 / 16.0f) && near(davisRainRateMmH(p.rainTipSec, 0.2f), 40.7f, 0.1f));
    // solar radiation
    p = sample("61 00 DB 00 43 00 F4 3B");
    assert(p.hasSolar && p.raw10 == 1 && near(p.solarWm2, 1.757936f));
    assert(!sample("60 00 00 FF C5 00 79 DA").hasSolar);
    // solar cell (Vue)
    p = sample("70 01 F5 CE 43 86 58 E2", DAVIS_VUE);
    assert(p.hasSolarCell && p.raw10 == (0xCE << 2 | 1) && near(p.solarCellV, (0xCE << 2 | 1) / 300.0f));
    // temperature: digital
    p = sample("80 00 00 33 8D 00 25 11");
    assert(p.hasTemp && near(p.tempF, 82.4f) && near(p.tempC, 28.0f, 0.02f));
    p = sample("80 04 70 0F 99 00 91 11");
    assert(p.hasTemp && near(p.tempF, 24.9f) && near(p.tempC, -3.94f, 0.01f));
    // analog thermistor: not decoded; no sensor
    p = sample("81 00 00 59 45 00 A3 E6");
    assert(!p.hasTemp && p.tempAnalog);
    p = sample("81 00 DB FF C3 00 AB F8");
    assert(!p.hasTemp && !p.tempAnalog);
    // gust (10-min high) with index
    p = sample("91 00 DB 00 03 0E 89 85");
    assert(p.hasGust && p.gustMph == 0 && p.gustIndex == 0);
    // humidity: digital variants, analog, no sensor
    p = sample("A0 00 00 C9 3D 00 2A 87");
    assert(p.hasHumidity && near(p.humidity, 96.9f));
    p = sample("A0 06 52 83 38 00 5A C8");
    assert(p.hasHumidity && near(p.humidity, 89.9f) && p.windMph == 6);
    p = sample("A0 01 3A 80 3B 00 ED 0E");
    assert(p.hasHumidity && near(p.humidity, 89.6f));
    p = sample("A0 00 00 22 85 00 ED E3");
    assert(p.hasHumidity && near(p.humidity, 2082 * -0.301f + 710.23f, 0.05f));
    assert(!sample("A1 00 DB 00 03 00 47 C7").hasHumidity);
    // rain counter
    p = sample("E0 00 00 05 05 00 9F 3D");
    assert(p.hasRainCount && p.rainCount == 5);
    p = sample("E0 00 00 4E 05 00 72 61");
    assert(p.rainCount == 0x4E);
    assert(!sample("E1 00 DB 80 03 00 16 8D").hasRainCount);
    // solar "FF" = no sensor (wiki example), battery bit
    p = sample("60 06 D3 FF C0 00 78 75");
    assert(!p.hasSolar && p.txId == 1);
}

void testErrorsAndAir()
{
    uint8_t fw[DAVIS_PACKET_LEN], air[DAVIS_PACKET_LEN];
    hex("80 04 70 0F 99 00 91 11 FF FF", fw, 10);
    toAir(fw, air);
    DavisPacket p;
    assert(davisDecode(air, p) && p.crc == DAVIS_CRC_OK && p.hasTemp && p.bytes[0] == 0x80);
    // one flipped bit -> CRC fail, fields not decoded
    air[3] ^= 0x10;
    assert(!davisDecode(air, p) && p.crc == DAVIS_CRC_FAIL && !p.hasTemp && p.bytes[0] == 0x80);
    // all zero passes the CRC arithmetically, but is rejected
    uint8_t zero[DAVIS_PACKET_LEN] = {};
    assert(!davisDecodeOrdered(zero, p) && p.crc == DAVIS_CRC_FAIL);
    // repeater: CRC over bytes 0-5 + 8-9
    hex("A0 06 52 83 38 00 00 00 12 34", fw, 10);
    uint16_t c = davisCrc16(fw, 6);
    c = davisCrc16(fw + 8, 2, c);
    fw[6] = (uint8_t)(c >> 8); fw[7] = (uint8_t)c;
    assert(davisDecodeOrdered(fw, p) && p.crc == DAVIS_CRC_REPEATER && p.hasHumidity);
    assert(davisTrailerZeroBits(fw) > 2);
    // battery low flag, ID 8
    hex("8F 00 00 00 08 00 00 00 FF FF", fw, 10);
    c = davisCrc16(fw, 6); fw[6] = (uint8_t)(c >> 8); fw[7] = (uint8_t)c;
    assert(davisDecodeOrdered(fw, p) && p.batteryLow && p.txId == 8 && p.hasTemp && p.tempF == 0.0f);
    // negative temperature: -12.3 F = 0xF85 (12 bit), digital flag
    hex("80 00 00 F8 58 00 00 00 FF FF", fw, 10);
    c = davisCrc16(fw, 6); fw[6] = (uint8_t)(c >> 8); fw[7] = (uint8_t)c;
    assert(davisDecodeOrdered(fw, p) && near(p.tempF, -12.3f));
    // wind direction formulas
    assert(davisWindDirDeg(0, DAVIS_VP2) == 0.0f && davisWindDirDeg(1, DAVIS_VP2) == 9.0f &&
           near(davisWindDirDeg(254, DAVIS_VP2), 351.0f) && davisWindDirDeg(255, DAVIS_VP2) == 355.0f);
    assert(near(davisWindDirDeg(128, DAVIS_VUE), 180.3f) && davisWindDirDeg(255, DAVIS_VUE) < 360.0f);
}

void testRainCounter()
{
    DavisRainCounter rc;
    assert(rc.update(120) == 0 && rc.valid());          // first count: reference only
    assert(rc.update(120) == 0);
    assert(rc.update(125) == 5);
    assert(rc.update(2) == 5);                          // 125 -> 127 -> 0 -> 2: wrap
    assert(rc.update(0x80 | 3) == 1);                   // high bit ignored
    assert(rc.totalTips() == 11);
    assert(rc.update(100) == 0);                        // jump of 97: counter reset, no rain
    assert(rc.update(101) == 1 && rc.totalTips() == 12);
    rc.reset();
    assert(!rc.valid() && rc.update(7) == 0);
}

// ---- hop tracker against a simulated transmitter ----

struct Tx {
    uint8_t id = 1;
    int64_t t0 = 1000000;        // first packet (RX done)
    double  periodUs = 2562500;
    uint8_t startCh = 2;
    int32_t offsetHz = 0;        // transmitter's frequency error
    int     n = 0;               // next slot
    int64_t at(int k) const { return t0 + (int64_t)llround(k * periodUs); }
    uint8_t ch(int k) const { return (uint8_t)((startCh + k) % 5); }
    void packet(int k, uint8_t* air, uint8_t idOverride = 0) const
    {
        static const uint8_t types[] = { 0x8, 0xE, 0x5, 0x4, 0x8, 0xE, 0x5, 0x9, 0x8, 0xE, 0x5, 0xA };
        uint8_t id2 = idOverride ? idOverride : id;
        uint8_t fw[DAVIS_PACKET_LEN] = { (uint8_t)(types[k % 12] << 4 | (id2 - 1)), 3, 100, 0x02, 0x58, 0, 0, 0, 0xFF, 0xFF };
        if ((types[k % 12]) == 0xE) fw[3] = (uint8_t)(k / 12);
        uint16_t c = davisCrc16(fw, 6);
        fw[6] = (uint8_t)(c >> 8); fw[7] = (uint8_t)c;
        toAir(fw, air);
    }
};

struct Sim {
    DavisHopper h;
    Tx tx;
    int64_t now = 0;
    int heard = 0, deliveredOk = 0, missEv = 0, lostEv = 0, scanEv = 0, afcEv = 0;
    int32_t plateauHz = 4000;   // receiver decodes within +-4 kHz of the transmitter
    std::function<bool(int)> silent = [](int) { return false; };

    void drain()
    {
        for (;;) {
            DavisEvent e = h.poll(now);
            if (!e.kind) return;
            if (e.kind == DAVIS_EV_MISS) missEv++;
            if (e.kind == DAVIS_EV_LOST) lostEv++;
            if (e.kind == DAVIS_EV_SCAN_STEP) scanEv++;
            if (e.kind == DAVIS_EV_AFC_STEP) afcEv++;
        }
    }
    void runUntil(int64_t end)
    {
        for (;;) {
            int64_t t = tx.at(tx.n);
            // the caller wakes at every deadline before the next packet
            for (;;) {
                int64_t d = h.nextDeadlineUs();
                if (d > t || d > end) break;
                if (d > now) now = d;
                drain();
            }
            if (t > end) { now = end; drain(); return; }
            now = t;
            if (!silent(tx.n) && h.channel() == tx.ch(tx.n) && std::llabs((long long)h.tuneOffsetHz() - tx.offsetHz) <= plateauHz) {
                uint8_t air[DAVIS_PACKET_LEN];
                tx.packet(tx.n, air);
                DavisRxResult r = h.onPacket(air, t);
                heard++;
                if (r.cls == DAVIS_PKT_ACCEPTED) deliveredOk++;
            }
            drain();
            tx.n++;
        }
    }
};

void testAcquireAndTrack()
{
    Sim s;
    DavisHopperConfig c;   // first heard, offset 0
    s.h.configure(c, 0);
    assert(s.h.state() == DAVIS_ST_ACQUIRE && s.h.channel() == 0 && s.h.lockedId() == 0);
    // parked on channel 0: the transmitter (starting on channel 2) comes by in slot 3
    s.runUntil(s.tx.at(3));
    assert(s.h.state() == DAVIS_ST_TRACK && s.h.lockedId() == 1 && s.deliveredOk == 1);
    assert(s.h.channel() == 1 && std::fabs(s.h.periodUs() - 2562500.0) < 1);
    // then every packet
    s.runUntil(s.tx.at(203));
    assert(s.deliveredOk == 201 && s.h.misses == 0 && s.h.accepted == 201 && s.h.trackSlots == 200);
    assert(s.h.state() == DAVIS_ST_TRACK);
    // three silent slots: misses, but the timing coasts and picks up again
    s.silent = [](int k) { return k >= 210 && k < 213; };
    s.runUntil(s.tx.at(230));
    assert(s.h.misses == 3 && s.missEv == 3 && s.lostEv == 0 && s.h.state() == DAVIS_ST_TRACK);
    assert(s.deliveredOk == 201 + 27 - 3 && s.h.missStreak() == 0);
}

void testDriftPll()
{
    Sim s;
    s.tx.periodUs = 2562500.0 * (1.0 + 150e-6);   // transmitter crystal 150 ppm slow
    s.tx.id = 3;
    s.tx.periodUs = davisIntervalUs(3) * (1.0 + 150e-6);
    DavisHopperConfig c;
    c.txId = 3;
    c.windowMs = 5; c.guardMs = 5;                 // tight window: the PLL has to follow
    s.h.configure(c, 0);
    // long silent stretches would accumulate 0.4 ms per period without the PLL
    s.silent = [](int k) { return k % 40 >= 30; };
    s.runUntil(s.tx.at(800));
    assert(s.h.state() == DAVIS_ST_TRACK && s.lostEv == 0);
    double ppm = (s.h.periodUs() / davisIntervalUs(3) - 1.0) * 1e6;
    assert(ppm > 120 && ppm < 180);
    assert(s.h.accepted > 590);
}

void testOtherIdAndWindow()
{
    DavisHopper h;
    DavisHopperConfig c;
    c.txId = 2;
    h.configure(c, 0);
    Tx tx; tx.id = 2; tx.startCh = 0; tx.periodUs = davisIntervalUs(2);
    uint8_t air[DAVIS_PACKET_LEN];
    tx.packet(0, air, 5);                         // ID 5: ignored
    DavisRxResult r = h.onPacket(air, tx.at(0));
    assert(r.cls == DAVIS_PKT_OTHER_ID && h.state() == DAVIS_ST_ACQUIRE && h.otherPackets == 1);
    tx.packet(0, air);
    r = h.onPacket(air, tx.at(0));
    assert(r.cls == DAVIS_PKT_ACCEPTED && r.locked && h.state() == DAVIS_ST_TRACK && h.channel() == 1);
    // a valid packet of the tracked ID 300 ms early: another station with the same ID
    tx.packet(1, air);
    r = h.onPacket(air, tx.at(1) - 300000);
    assert(r.cls == DAVIS_PKT_OUT_OF_WINDOW && r.hasTiming && r.timingErrUs == -300000);
    // on time (+3 ms)
    r = h.onPacket(air, tx.at(1) + 3000);
    assert(r.cls == DAVIS_PKT_ACCEPTED && r.timingErrUs == 3000 && h.channel() == 2);
    // CRC failure counts, does not move anything
    air[2] ^= 1;
    r = h.onPacket(air, tx.at(2));
    assert(r.cls == DAVIS_PKT_CRC_FAIL && h.crcErrors == 1 && h.channel() == 2);
    // bad trailer
    uint8_t fw[DAVIS_PACKET_LEN] = { 0x81, 0, 0, 0, 8, 0, 0, 0, 0x00, 0x00 };
    uint16_t cc = davisCrc16(fw, 6); fw[6] = (uint8_t)(cc >> 8); fw[7] = (uint8_t)cc;
    toAir(fw, air);
    assert(h.onPacket(air, tx.at(2)).cls == DAVIS_PKT_BAD_TRAILER);
}

void testLostAndResync()
{
    Sim s;
    DavisHopperConfig c;
    c.lostAfter = 6;
    s.h.configure(c, 0);
    s.runUntil(s.tx.at(50));
    assert(s.h.state() == DAVIS_ST_TRACK);
    s.silent = [](int k) { return k >= 60 && k < 120; };   // transmitter off for 2.5 min
    s.runUntil(s.tx.at(80));
    assert(s.lostEv == 1 && s.h.state() == DAVIS_ST_LOST && s.h.misses == 6);
    s.runUntil(s.tx.at(140));
    assert(s.h.state() == DAVIS_ST_TRACK && s.h.resyncs == 1);
    assert(s.h.lastPacketUs() == s.tx.at(140));
}

void testOffsetScan()
{
    Sim s;
    s.tx.offsetHz = -17000;                       // transmitter 17 kHz low
    DavisHopperConfig c;
    c.scanSpanKhz = 30;
    s.h.configure(c, 0);
    // scan 0, +6, -6, +12, -12, +18, -18: -18 kHz is within 4 kHz -> found in the 7th dwell
    s.runUntil(s.tx.at(60));
    assert(s.h.state() == DAVIS_ST_TRACK && s.h.learnedOffsetHz() == -18000 && s.scanEv == 6);
    // without the scan it never locks
    Sim f;
    f.tx.offsetHz = -17000;
    c.scanSpanKhz = 0;
    f.h.configure(c, 0);
    f.runUntil(f.tx.at(200));
    assert(f.h.state() == DAVIS_ST_ACQUIRE && f.h.accepted == 0 && f.scanEv > 10);
    // the base offset alone also works
    Sim g;
    g.tx.offsetHz = -17000;
    c.offsetHz = -16000;
    g.h.configure(c, 0);
    g.runUntil(g.tx.at(30));
    assert(g.h.state() == DAVIS_ST_TRACK && g.h.learnedOffsetHz() == -16000);
}

void testAfc()
{
    Sim s;
    s.tx.offsetHz = 0;
    s.plateauHz = 7000;                           // shoulders at +-6 kHz decode when centred
    DavisHopperConfig c;
    c.afc = true;
    s.h.configure(c, 0);
    s.runUntil(s.tx.at(100));
    assert(s.h.state() == DAVIS_ST_TRACK && s.h.learnedOffsetHz() == 0 && s.h.misses == 0);
    // the transmitter drifts 5 kHz up (cold night): the +6 probe still hears
    // it, the -6 probe no longer; AFC walks the offset up
    s.tx.offsetHz = 5000;
    s.plateauHz = 4000;
    s.runUntil(s.tx.at(400));
    assert(s.afcEv >= 2 && s.h.learnedOffsetHz() >= 3000 && s.h.learnedOffsetHz() <= 6000);
    assert(s.h.state() == DAVIS_ST_TRACK && s.lostEv == 0);
}

void testRawAndUpdate()
{
    DavisHopper h;
    DavisHopperConfig c;
    c.raw = true; c.rawChannel = 3; c.offsetHz = 2000;
    h.configure(c, 0);
    assert(h.state() == DAVIS_ST_RAW && h.channel() == 3 && std::fabs(h.tuneMHz() - (868.1792f + 0.002f)) < 1e-4);
    Tx tx; tx.id = 4;
    uint8_t air[DAVIS_PACKET_LEN];
    tx.packet(0, air);
    assert(h.onPacket(air, 1000).cls == DAVIS_PKT_RAW_OK && h.channel() == 3);
    air[1] ^= 4;
    assert(h.onPacket(air, 2000).cls == DAVIS_PKT_CRC_FAIL && h.crcErrors == 1);
    assert(h.poll(5000000000LL).kind == DAVIS_EV_NONE && h.channel() == 3);
    // live update: window changes in place, raw off restarts acquisition
    c.windowMs = 50;
    h.update(c, 10);
    assert(h.state() == DAVIS_ST_RAW && h.config().windowMs == 50 && h.crcErrors == 1);
    c.raw = false;
    h.update(c, 20);
    assert(h.state() == DAVIS_ST_ACQUIRE && h.crcErrors == 0 && h.tuneOffsetHz() == 2000);
    c.offsetHz = -3000;
    h.update(c, 30);
    assert(h.state() == DAVIS_ST_ACQUIRE && h.tuneOffsetHz() == -3000);
    c.band = &DAVIS_BAND_EU_CLASSIC;
    h.update(c, 40);
    assert(std::fabs(h.channelMHz(2) - 868.5275f) < 1e-4);
    assert(!strcmp(davisStateName(DAVIS_ST_LOST), "LOST") && !strcmp(davisPktClassName(DAVIS_PKT_CRC_FAIL), "CRC-FAIL"));
}

} // namespace

int main()
{
    testBasics();
    testSamples();
    testErrorsAndAir();
    testRainCounter();
    testAcquireAndTrack();
    testDriftPll();
    testOtherIdAndWindow();
    testLostAndResync();
    testOffsetScan();
    testAfc();
    testRawAndUpdate();
    return 0;
}
