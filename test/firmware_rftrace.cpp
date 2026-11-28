// Radio trace (src/rf_trace.cpp): heap check, filters, statistics, ring
// wrap, frame classification, file name and a header without secrets.
#include "rf_trace.h"
#include "globals.h"
#include "rf_sensors.h"
#include "rf_fsk17.h"
#include "heltec_board.h"
#include <FineOffsetDecoder.h>
#include <ItPlusDecoder.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>

// wall clock (time_sync.cpp is not host-built)
static bool    hostSynced = false;
static int64_t hostEpoch0 = 1781524800;   // 2026-06-15T12:00:00Z at hostNow 0
bool timeIsValid() { return hostSynced; }
bool epochNow(int64_t& out)
{
    if (!hostSynced) return false;
    out = hostEpoch0 + (int64_t)(millis() / 1000);
    return true;
}
bool localTimeNow(struct tm& out)
{
    int64_t e;
    if (!epochNow(e)) return false;
    time_t t = (time_t)e;
    gmtime_r(&t, &out);   // TZ=UTC in this test
    return true;
}
void localTimeOf(time_t t, struct tm& out) { gmtime_r(&t, &out); }   // TZ=UTC in this test

static size_t hostFree = 200 * 1024, hostLargest = 100 * 1024;
static size_t fakeFree() { return hostFree; }
static size_t fakeLargest() { return hostLargest; }

static void collect(const char* s, size_t n, void* ctx) { static_cast<std::string*>(ctx)->append(s, n); }
static std::string file(const char* host = "garden")
{
    std::string out;
    rfTraceWriteFile(collect, &out, host, "receiving, now listening to Fine Offset");
    return out;
}
static size_t count(const std::string& s, const char* what)
{
    size_t n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) n++;
    return n;
}

static RfTracePacket okPacket(uint32_t id, const uint8_t* raw, size_t len)
{
    RfTracePacket p;
    p.phase = RF_TP_FSK17; p.rssi = -71.0f; p.raw = raw; p.rawLen = len;
    p.result = RF_TR_OK; p.check = "CRC ok, sum ok"; p.protocol = "Fine Offset"; p.model = "WH31";
    p.hasId = true; p.id = id; p.values = "T 21.5C H 42% bat ok";
    return p;
}

int main()
{
    setenv("TZ", "UTC0", 1);
    tzset();
    rfTraceFreeHeap = fakeFree;
    rfTraceLargestFreeBlock = fakeLargest;
    char err[160];
    const uint8_t wh31[] = {0x30, 0xc3, 0x81, 0xd5, 0x5c, 0x2a, 0xcf};

    // ---- nothing recorded without a capture; heap check before allocating ----
    rfTracePacket(okPacket(0x300001C3, wh31, sizeof(wh31)));
    assert(!rfTraceStatusNow().allocated && !rfTraceRunning());
    hostFree = 50 * 1024;
    assert(!rfTraceStart(5 * 60000UL, RF_TRACE_ALL, 0, false, err, sizeof(err)));
    assert(strstr(err, "not enough free memory") && !rfTraceStatusNow().allocated);
    hostFree = 200 * 1024; hostLargest = 20 * 1024;   // fragmented: no block for the ring
    assert(!rfTraceStart(5 * 60000UL, RF_TRACE_ALL, 0, false, err, sizeof(err)));
    hostLargest = 100 * 1024;
    assert(!rfTraceStart(5 * 60000UL, RF_TRACE_ONE_ID, 0, false, err, sizeof(err)));    // ID filter without ID
    assert(!rfTraceStart(121 * 60000UL, RF_TRACE_ALL, 0, false, err, sizeof(err)));     // too long

    // ---- all packets, duration ----
    assert(rfTraceStart(5 * 60000UL, RF_TRACE_ALL, 0, false, err, sizeof(err)));
    RfTraceStatus st = rfTraceStatusNow();
    assert(st.allocated && st.running && st.durationMs == 300000UL && st.leftMs == 300000UL);
    rfTracePacket(okPacket(0x300001C3, wh31, sizeof(wh31)));
    rfTraceText(RF_TP_DAVIS, "Davis: settings applied.\n", RF_TR_EVENT, false, 0);
    std::string f = file();
    assert(count(f, "FSK17    -71 dBm") == 1 && count(f, "OK      Fine Offset WH31 ID 300001C3 (CRC ok, sum ok) T 21.5C") == 1);
    assert(count(f, "| 7B 30C381D55C2ACF\n") == 1 && count(f, "DAVIS   Davis: settings applied.\n") == 1);
    delay(4 * 60000UL);
    rfTracePoll();
    assert(rfTraceRunning() && rfTraceStatusNow().leftMs == 60000UL);
    delay(60000UL);
    rfTracePoll();
    st = rfTraceStatusNow();
    assert(!st.running && st.allocated && !strcmp(st.stopReason, "time is up") && st.seen == 1 && st.recorded == 2);
    rfTracePacket(okPacket(0x300001C3, wh31, sizeof(wh31)));   // after the end: not recorded
    assert(rfTraceStatusNow().seen == 1);
    f = file();
    assert(f.find("finished after 300 s: time is up") != std::string::npos && f.find("# end of capture") != std::string::npos);

    // ---- filter: only failed / unknown packets (statistics count everything) ----
    assert(rfTraceStart(0, RF_TRACE_PROBLEMS, 0, false, err, sizeof(err)));
    assert(rfTraceStatusNow().seen == 0 && rfTraceStatusNow().durationMs == 0);   // the previous capture is gone
    assert(!rfTraceWants(RF_TR_OK, true, 1) && rfTraceWants(RF_TR_FAILED, false, 0) && rfTraceWants(RF_TR_UNKNOWN, false, 0));
    rfTracePacket(okPacket(0x300001C3, wh31, sizeof(wh31)));
    // a WH31 frame with a broken CRC
    uint8_t bad[FO_MAX_FRAME_LEN] = {};
    memcpy(bad, wh31, sizeof(wh31));
    bad[5] ^= 0x01;
    FoFrame fo; ItPlusFrame it;
    assert(rfFsk17Decode(bad, sizeof(bad), true, true, fo, it) == RF_FSK_NONE);
    RfTracePacket p; p.phase = RF_TP_FSK17; p.rssi = -90.0f; p.raw = bad; p.rawLen = sizeof(bad);
    char check[96];
    rfTraceClassifyFsk17(bad, sizeof(bad), true, true, p, check, sizeof(check));
    assert(p.result == RF_TR_FAILED && !strcmp(p.protocol, "Fine Offset") && strstr(check, "family 0x30") && strstr(check, "CRC fail"));
    rfTracePacket(p);
    // an unknown family
    uint8_t unk[FO_MAX_FRAME_LEN] = {0x77, 0x01, 0x02};
    RfTracePacket u; u.phase = RF_TP_FSK17; u.raw = unk; u.rawLen = sizeof(unk);
    rfTraceClassifyFsk17(unk, sizeof(unk), true, true, u, check, sizeof(check));
    assert(u.result == RF_TR_UNKNOWN && strstr(check, "first byte 0x77"));
    rfTracePacket(u);
    // the right CRC but a broken sum
    uint8_t sum[FO_MAX_FRAME_LEN] = {};
    memcpy(sum, wh31, sizeof(wh31));
    sum[6] ^= 0xFF;
    RfTracePacket s2; s2.phase = RF_TP_FSK17;
    rfTraceClassifyFsk17(sum, sizeof(sum), true, false, s2, check, sizeof(check));
    assert(s2.result == RF_TR_FAILED && strstr(check, "CRC ok, sum fail"));
    // IT+: wrong CRC / wrong model nibble
    uint8_t itb[ITPLUS_FRAME_LEN] = {0x91, 0x46, 0x26, 0x6A, 0x00};
    RfTracePacket i1; i1.phase = RF_TP_ITPLUS17;
    rfTraceClassifyItPlus(itb, i1, check, sizeof(check));
    assert(i1.result == RF_TR_FAILED && !strcmp(i1.protocol, "IT+") && strstr(check, "CRC fail") && !i1.hasId);
    itb[4] = itPlusCrc8(itb, 4);
    itb[1] = 0x4F;   // CRC recomputed below over an invalid BCD digit
    itb[4] = itPlusCrc8(itb, 4);
    rfTraceClassifyItPlus(itb, i1, check, sizeof(check));
    assert(i1.result == RF_TR_FAILED && strstr(check, "CRC ok, values invalid") && i1.hasId && i1.id == (RF_ID_ITPLUS17 | 0x05));
    itb[0] = 0x51;
    rfTraceClassifyItPlus(itb, i1, check, sizeof(check));
    assert(i1.result == RF_TR_UNKNOWN);
    st = rfTraceStatusNow();
    assert(st.seen == 3 && st.recorded == 2);
    f = file();
    assert(count(f, "FAILED  Fine Offset (family 0x30") == 1 && count(f, "UNKNOWN (no known format (first byte 0x77))") == 1);
    assert(count(f, "OK      Fine Offset WH31") == 0);
    // statistics cover the filtered-out frame
    assert(f.find("#   FSK17             1        0        1        1") != std::string::npos);
    assert(f.find("#   300001C3 FSK17   WH31              1 / 0 / 0") != std::string::npos);
    // a long unknown frame (Ecowitt IoT, first byte 0xB9) is recorded with all RF_TRACE_FRAME_LEN bytes
    {
        uint8_t iot[RF_TRACE_FRAME_LEN];
        for (size_t i = 0; i < sizeof(iot); i++) iot[i] = (uint8_t)(0xB9 + i);
        RfTracePacket l; l.phase = RF_TP_FSK17; l.raw = iot; l.rawLen = sizeof(iot);
        rfTraceClassifyFsk17(iot, sizeof(iot), true, true, l, check, sizeof(check));
        assert(l.result == RF_TR_UNKNOWN && strstr(check, "first byte 0xB9"));
        rfTracePacket(l);
        f = file();
        assert(f.find("| 64B B9BABBBC") != std::string::npos && f.find("F5F6F7F8\n") != std::string::npos);   // last byte 0xB9 + 63 = 0xF8
    }

    // ---- filter: one ID ----
    assert(rfTraceStart(15 * 60000UL, RF_TRACE_ONE_ID, 0x300001C3, false, err, sizeof(err)));
    rfTracePacket(okPacket(0x300001C3, wh31, sizeof(wh31)));
    rfTracePacket(okPacket(0x30000201, wh31, sizeof(wh31)));
    rfTraceText(RF_TP_DAVIS, "event\n", RF_TR_EVENT, false, 0);
    st = rfTraceStatusNow();
    assert(st.seen == 2 && st.recorded == 1 && st.filter == RF_TRACE_ONE_ID && st.filterId == 0x300001C3);
    assert(file().find("only ID") == std::string::npos);   // (status page text, not the file)
    assert(file().find("one sensor ID 300001C3") != std::string::npos);

    // ---- filter: one ID, short form the user typed still matches the full on-air ID ----
    assert(rfTraceStart(15 * 60000UL, RF_TRACE_ONE_ID, 0x01C3, false, err, sizeof(err)));   // low 2 bytes of 300001C3
    rfTracePacket(okPacket(0x300001C3, wh31, sizeof(wh31)));   // full ID: low bytes match -> recorded
    rfTracePacket(okPacket(0x300001C4, wh31, sizeof(wh31)));   // different low bytes -> not recorded
    st = rfTraceStatusNow();
    assert(st.seen == 2 && st.recorded == 1 && st.filterId == 0x01C3);

    // ---- reception rate per ID (repeats not counted) and read delay ----
    assert(rfTraceStart(30 * 60000UL, RF_TRACE_ALL, 0, false, err, sizeof(err)));
    {
        auto tx = [&](uint32_t readDelay) {
            RfTracePacket q = okPacket(0x300009A1, wh31, sizeof(wh31));
            q.nominalMs = 61000; q.hasReadDelay = true; q.readDelayMs = readDelay;
            rfTracePacket(q);
        };
        // 6 transmissions expected over 5 intervals: one missing, one sent twice (repeat 60 ms later)
        tx(5);  delay(60); tx(4);      // t = 0 and its repeat
        delay(60940); tx(250);         // t = 61 s, read 250 ms late
        delay(61000);                  // t = 122 s: missed
        delay(61000); tx(6);           // t = 183 s
        delay(61000); tx(7);           // t = 244 s
        delay(61000); tx(3);           // t = 305 s
        // a sensor of unknown interval and one seen only twice: no rate
        RfTracePacket u = okPacket(0x77000001, wh31, sizeof(wh31)); rfTracePacket(u); delay(5000); rfTracePacket(u);
    }
    f = file();
    assert(f.find("#   300009A1 FSK17   WH31              6 / 0 / 0   -71..-71 -71 dBm   5 of 6 sent (83 %)\n") != std::string::npos);
    assert(f.find("#   77000001 FSK17   WH31              2 / 0 / 0   -71..-71 -71 dBm\n") != std::string::npos);
    assert(f.find("# Read delay (packet to firmware, Fine Offset / IT+): avg 45 ms, max 250 ms, 1 of 6 reads later than 100 ms") != std::string::npos);

    // ---- ring wrap: the newest lines stay, the file starts at a whole line ----
    assert(rfTraceStart(60 * 60000UL, RF_TRACE_ALL, 0, false, err, sizeof(err)));
    int lines = 0;
    for (int i = 0; i < 400; i++) {
        RfTracePacket q = okPacket(0x30000000u + i, wh31, sizeof(wh31));
        rfTracePacket(q);
        lines++;
        delay(1000);
    }
    st = rfTraceStatusNow();
    assert(st.recorded == 400 && st.usedBytes == RF_TRACE_SIZE && st.overwrittenBytes > 0);
    f = file();
    assert(f.find("ID 3000018F") != std::string::npos && f.find("ID 30000000 ") == std::string::npos);   // last kept, first gone
    size_t body = f.find("# Lines:");
    body = f.find("#\n", body) + 2;
    // every line after the header is complete: "<uptime> FSK17 ..."
    size_t pos = body, whole = 0;
    while (pos < f.size() && f[pos] != '#') {
        size_t nl = f.find('\n', pos);
        std::string line = f.substr(pos, nl - pos);
        assert(line.find("FSK17") != std::string::npos && line.find(" | 7B 30C381D55C2ACF") != std::string::npos);
        whole++;
        pos = nl + 1;
    }
    assert(whole > 100 && whole < 400);
    // per-ID statistics are bounded
    assert(f.find("frames of further IDs not listed") != std::string::npos);

    // ---- file name ----
    char name[96];
    rfTraceFileName(name, sizeof(name), "garden");
    assert(!strncmp(name, "uwn-rftrace-garden-up", 21) && strstr(name, "s.txt"));
    hostSynced = true;
    assert(rfTraceStart(5 * 60000UL, RF_TRACE_ALL, 0, false, err, sizeof(err)));
    rfTraceFileName(name, sizeof(name), "garden");
    int64_t e; epochNow(e);
    time_t tt = (time_t)e; struct tm tm; gmtime_r(&tt, &tm);
    char want[96];
    snprintf(want, sizeof(want), "uwn-rftrace-garden-%04d%02d%02d-%02d%02d.txt", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    assert(!strcmp(name, want));

    // ---- the header never contains credentials ----
    const char* secrets[] = { "SECRET-mqtt-user", "SECRET-mqtt-pass", "SECRET-admin", "SECRET-awekas-pass", "SECRET-awekas-user",
                              "SECRET-wu-key", "SECRET-wu-id", "SECRET-wc-key", "SECRET-wnp-key", "SECRET-windy-pass",
                              "SECRET-osem-token", "SECRET-osem-box", "SECRET-mqtt-host", "SECRET-ca", "SECRET-lox-host",
                              "SECRET-wc-id", "SECRET-wnp-id", "SECRET-windy-id", "47.12345", "11.54321" };
    snprintf(gCfg.mqttUser, sizeof(gCfg.mqttUser), "%s", secrets[0]);
    snprintf(gCfg.mqttPass, sizeof(gCfg.mqttPass), "%s", secrets[1]);
    snprintf(gCfg.webAdminPass, sizeof(gCfg.webAdminPass), "%s", secrets[2]);
    snprintf(gCfg.awekasPass, sizeof(gCfg.awekasPass), "%s", secrets[3]);
    snprintf(gCfg.awekasUser, sizeof(gCfg.awekasUser), "%s", secrets[4]);
    snprintf(gCfg.wuStationKey, sizeof(gCfg.wuStationKey), "%s", secrets[5]);
    snprintf(gCfg.wuStationId, sizeof(gCfg.wuStationId), "%s", secrets[6]);
    snprintf(gCfg.wcloudKey, sizeof(gCfg.wcloudKey), "%s", secrets[7]);
    snprintf(gCfg.wnetproKey, sizeof(gCfg.wnetproKey), "%s", secrets[8]);
    snprintf(gCfg.windyPass, sizeof(gCfg.windyPass), "%s", secrets[9]);
    snprintf(gCfg.osemToken, sizeof(gCfg.osemToken), "%s", secrets[10]);
    snprintf(gCfg.osemBoxId, sizeof(gCfg.osemBoxId), "%s", secrets[11]);
    snprintf(gCfg.mqttHost, sizeof(gCfg.mqttHost), "%s", secrets[12]);
    snprintf(gCfg.mqttCaCert, sizeof(gCfg.mqttCaCert), "%s", secrets[13]);
    snprintf(gCfg.loxHost, sizeof(gCfg.loxHost), "%s", secrets[14]);
    snprintf(gCfg.wcloudId, sizeof(gCfg.wcloudId), "%s", secrets[15]);
    snprintf(gCfg.wnetproId, sizeof(gCfg.wnetproId), "%s", secrets[16]);
    snprintf(gCfg.windyId, sizeof(gCfg.windyId), "%s", secrets[17]);
    snprintf(gCfg.latitude, sizeof(gCfg.latitude), "%s", secrets[18]);
    snprintf(gCfg.longitude, sizeof(gCfg.longitude), "%s", secrets[19]);
    for (int i = 0; i < 16; i++) gCfg.appKey[i] = (uint8_t)(0xA0 + i);
    gCfg.devEui = 0x70B3D57ED0012345ULL;
    gCfg.joinEui = 0x0011223344556677ULL;
    gCfg.rfEn = 1;
    snprintf(gCfg.rfIds, sizeof(gCfg.rfIds), "300001C3, 9000342B");
    RfSensorReading& r = gRfSensors[0];
    r.used = true; r.id = 0x300001C3; r.proto = RF_PROTO_FINEOFFSET; r.model = "WH31"; r.packets = 12; r.rssi = -70.0f;
    rfTracePacket(okPacket(0x300001C3, wh31, sizeof(wh31)));
    f = file();
    for (const char* s : secrets) assert(f.find(s) == std::string::npos);
    for (const char* s : { "A0A1A2A3", "a0a1a2a3", "70B3D57ED0012345", "70b3d57ed0012345", "0011223344556677" })
        assert(f.find(s) == std::string::npos);
    // what it does contain
    assert(f.find("PRIVACY") != std::string::npos && f.find("neighbours") != std::string::npos);
    assert(f.find("300001C3, 9000342B") != std::string::npos && f.find("WH31") != std::string::npos);
    assert(f.find("receiving, now listening to Fine Offset") != std::string::npos && f.find("Firmware:") != std::string::npos);
    assert(f.find("Time (UTC):") != std::string::npos);

    // ---- discard frees the memory; a download without a capture is only the header ----
    rfTraceClear();
    assert(!rfTraceStatusNow().allocated && !rfTraceRunning());
    f = file();
    assert(f.find("no capture in memory") != std::string::npos);
    (void)lines;
    return 0;
}
