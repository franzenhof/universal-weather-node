// Own access point (src/wifi_ap_logic.cpp): fallback timing, station rule,
// passphrase / SSID / Wi-Fi QR text, captive-check classification, the
// captive DNS answer, the acceptance rule for a phone's time.
#include "wifi_ap_logic.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<uint8_t> query(const char* name, uint16_t type, uint16_t id = 0x1234, bool edns = false)
{
    std::vector<uint8_t> q = { (uint8_t)(id >> 8), (uint8_t)id, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, (uint8_t)(edns ? 1 : 0) };
    const char* p = name;
    while (*p) {
        const char* dot = strchr(p, '.');
        const size_t l = dot ? (size_t)(dot - p) : strlen(p);
        q.push_back((uint8_t)l);
        q.insert(q.end(), p, p + l);
        p += l + (dot ? 1 : 0);
    }
    q.push_back(0);
    q.push_back((uint8_t)(type >> 8)); q.push_back((uint8_t)type); q.push_back(0); q.push_back(1);
    if (edns) { const uint8_t opt[11] = { 0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0 }; q.insert(q.end(), opt, opt + 11); }
    return q;
}

int main()
{
    // ---- fallback timing ----
    {
        ApFallback f;
        uint64_t t = 1000;
        assert(!f.update(AP_FALLBACK, false, t));                  // boot, station down
        assert(!f.update(AP_FALLBACK, false, t + 119999));
        assert(f.update(AP_FALLBACK, false, t + 120000));          // 2 min down: AP on
        assert(f.update(AP_FALLBACK, true, t + 130000));           // station back: AP stays for 1 min
        assert(f.update(AP_FALLBACK, true, t + 189999));
        assert(f.update(AP_FALLBACK, false, t + 189999));          // dropped again before the minute: stays
        assert(f.update(AP_FALLBACK, true, t + 200000));
        assert(!f.update(AP_FALLBACK, true, t + 260000));          // up for 1 min: AP off
        assert(!f.update(AP_FALLBACK, false, t + 300000));         // down again: the 2 min start anew
        assert(!f.update(AP_FALLBACK, false, t + 419999));
        assert(f.update(AP_FALLBACK, false, t + 420000));
        assert(!f.update(AP_OFF, false, t + 430000));              // mode off: off at once
        assert(f.update(AP_ALWAYS, true, t + 440000));             // always: on, also with the station up
        ApFallback g;                                              // station up from the start: never
        for (uint64_t x = 0; x < 600000; x += 10000) assert(!g.update(AP_FALLBACK, true, x));
    }
    assert(apStationWanted(AP_OFF, false) && apStationWanted(AP_FALLBACK, false));
    assert(apStationWanted(AP_ALWAYS, true) && !apStationWanted(AP_ALWAYS, false));
    assert(!strcmp(apModeName(AP_OFF), "off") && !strcmp(apModeName(AP_FALLBACK), "fallback") && !strcmp(apModeName(AP_ALWAYS), "always"));

    // ---- passphrase, SSID, QR text ----
    assert(!apPassValid("") && !apPassValid("1234567") && apPassValid("12345678"));
    assert(apPassValid("a b;c,d:e\"f\\g"));
    char p64[65]; memset(p64, 'x', 64); p64[64] = 0;
    assert(!apPassValid(p64)); p64[63] = 0; assert(apPassValid(p64));
    assert(!apPassValid("abcdefg\xC3\xA4"));                       // not ASCII
    char s[40];
    const uint8_t mac[6] = { 0x48, 0xCA, 0x43, 0x12, 0xA1, 0x0B };
    apDefaultSsid(mac, s, sizeof(s));
    assert(!strcmp(s, "WeatherNode-A10B"));
    char t[160];
    assert(wifiQrText("My;Net", "p\\a:s,s\"word", t, sizeof(t)));
    assert(!strcmp(t, "WIFI:T:WPA;S:My\\;Net;P:p\\\\a\\:s\\,s\\\"word;;"));
    assert(wifiQrText("x", "12345678", t, 27) == 0 && t[0] == 0);  // too small: nothing
    assert(wifiQrText("x", "12345678", t, 28) == 27);              // exactly fits
    {   // the longest valid config (every character escaped) fits WIFI_QR_TEXT_MAX and a QR code at M
        char ws[AP_SSID_MAX + 1], wp[AP_PASS_MAX + 1], wt[WIFI_QR_TEXT_MAX];
        memset(ws, ',', AP_SSID_MAX); ws[AP_SSID_MAX] = 0;
        memset(wp, ';', AP_PASS_MAX); wp[AP_PASS_MAX] = 0;
        assert(apPassValid(wp));
        assert(wifiQrText(ws, wp, wt, sizeof(wt)) == WIFI_QR_TEXT_MAX - 1);
        QrCode q;
        assert(qrEncode((const uint8_t*)wt, WIFI_QR_TEXT_MAX - 1, QR_ECC_M, q));
    }

    // ---- captive checks ----
    assert(captiveProbePath("/generate_204") && captiveProbePath("/gen_204") && captiveProbePath("/hotspot-detect.html"));
    assert(captiveProbePath("/library/test/success.html") && captiveProbePath("/connecttest.txt") && captiveProbePath("/ncsi.txt"));
    assert(captiveProbePath("/generate_204?x=1") && !captiveProbePath("/generate_2045") && !captiveProbePath("/") && !captiveProbePath("/api/data"));
    // own host: served as usual
    assert(!captiveRedirect("192.168.4.1", "/", "wetter"));
    assert(!captiveRedirect("192.168.4.1:80", "/api/data", "wetter"));
    assert(!captiveRedirect("wetter.local", "/status", "wetter"));
    assert(!captiveRedirect("WETTER.local.", "/", "wetter"));
    assert(!captiveRedirect("wetter", "/", "wetter"));
    assert(!captiveRedirect("", "/", "wetter"));                     // no Host header
    // foreign host or a check path: redirect
    assert(captiveRedirect("connectivitycheck.gstatic.com", "/generate_204", "wetter"));
    assert(captiveRedirect("captive.apple.com", "/hotspot-detect.html", "wetter"));
    assert(captiveRedirect("www.msftconnecttest.com", "/connecttest.txt", "wetter"));
    assert(captiveRedirect("example.com", "/", "wetter"));
    assert(captiveRedirect("wetter.example.com", "/", "wetter"));
    assert(captiveRedirect("192.168.4.1", "/generate_204", "wetter"));   // a check path also on the own address
    assert(captiveRedirect("192.168.4.10", "/", "wetter"));

    // ---- captive DNS ----
    {
        uint8_t out[512];
        std::vector<uint8_t> q = query("connectivitycheck.gstatic.com", 1);
        const size_t n = dnsCaptiveAnswer(q.data(), q.size(), AP_IP, out, sizeof(out));
        assert(n == q.size() + 16);
        assert(out[0] == 0x12 && out[1] == 0x34);                  // id
        assert(out[2] == 0x85 && out[3] == 0x80);                  // response, AA, RD; RA, NOERROR
        assert(out[5] == 1 && out[7] == 1 && out[9] == 0 && out[11] == 0);
        assert(!memcmp(out + 12, q.data() + 12, q.size() - 12));   // question copied
        const uint8_t* a = out + q.size();
        const uint8_t rr[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
        assert(!memcmp(a, rr, 16));
        // AAAA: no answer, NOERROR
        q = query("captive.apple.com", 28);
        assert(dnsCaptiveAnswer(q.data(), q.size(), AP_IP, out, sizeof(out)) == q.size());
        assert(out[3] == 0x80 && out[7] == 0);
        // EDNS OPT record of the query is not echoed
        q = query("example.com", 1, 7, true);
        const size_t n2 = dnsCaptiveAnswer(q.data(), q.size(), AP_IP, out, sizeof(out));
        assert(n2 == q.size() - 11 + 16 && out[11] == 0 && out[7] == 1);
        // ANY: answered with the A record
        q = query("a.b", 255);
        assert(dnsCaptiveAnswer(q.data(), q.size(), AP_IP, out, sizeof(out)) == q.size() + 16);
        // dropped: a response, another opcode, two questions, truncated, a pointer, a too small buffer
        q = query("x.y", 1); q[2] |= 0x80; assert(!dnsCaptiveAnswer(q.data(), q.size(), AP_IP, out, sizeof(out)));
        q = query("x.y", 1); q[2] |= 0x10; assert(!dnsCaptiveAnswer(q.data(), q.size(), AP_IP, out, sizeof(out)));
        q = query("x.y", 1); q[5] = 2; assert(!dnsCaptiveAnswer(q.data(), q.size(), AP_IP, out, sizeof(out)));
        q = query("x.y", 1); assert(!dnsCaptiveAnswer(q.data(), q.size() - 1, AP_IP, out, sizeof(out)));
        assert(!dnsCaptiveAnswer(q.data(), 11, AP_IP, out, sizeof(out)));
        q = query("x.y", 1); q[12] = 0xC0; assert(!dnsCaptiveAnswer(q.data(), q.size(), AP_IP, out, sizeof(out)));
        q = query("x.y", 1); assert(!dnsCaptiveAnswer(q.data(), q.size(), AP_IP, out, q.size() + 15));
        q = query("x.y", 1); q[13] = 'z'; q[12] = 60; assert(!dnsCaptiveAnswer(q.data(), q.size(), AP_IP, out, sizeof(out)));   // label beyond the end
        std::vector<uint8_t> big(600, 0); assert(!dnsCaptiveAnswer(big.data(), big.size(), AP_IP, out, sizeof(out)));
        // a name longer than 255 bytes
        std::vector<uint8_t> lq = { 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
        for (int i = 0; i < 5; i++) { lq.push_back(63); lq.insert(lq.end(), 63, 'a'); }
        lq.push_back(0); lq.push_back(0); lq.push_back(1); lq.push_back(0); lq.push_back(1);
        assert(!dnsCaptiveAnswer(lq.data(), lq.size(), AP_IP, out, sizeof(out)));
    }

    // ---- phone time ----
    const int64_t now = 1795000000000LL;   // 2026-11
    assert(phoneTimeCheck(now, true, TSRC_NTP) == PT_OFFER);        // over the AP: always offered (the sources still block it)
    assert(phoneTimeCheck(now, true, TSRC_NONE) == PT_OFFER);
    assert(phoneTimeCheck(now, false, TSRC_NONE) == PT_OFFER);      // LAN without a fresh time: offered
    assert(phoneTimeCheck(now, false, TSRC_NTP) == PT_REFUSED);     // LAN with a fresh time: refused
    assert(phoneTimeCheck(now, false, TSRC_PHONE) == PT_REFUSED);
    assert(phoneTimeCheck(now, false, TSRC_LORAWAN) == PT_REFUSED);
    assert(phoneTimeCheck(1704067199999LL, true, TSRC_NONE) == PT_INVALID);
    assert(phoneTimeCheck(1704067200000LL, true, TSRC_NONE) == PT_OFFER);
    assert(phoneTimeCheck(4102444800000LL, true, TSRC_NONE) == PT_INVALID);
    assert(phoneTimeCheck(-5, true, TSRC_NONE) == PT_INVALID);

    // ---- with the time sources: a phone never overrides fresh NTP, sets the clock without one ----
    {
        TimeSources ts;
        int64_t step;
        assert(ts.offer(TSRC_NTP, now, 1000, 50, 1000, now, true, step) == TimeSources::TS_CONFIRMED);
        assert(ts.offer(TSRC_PHONE, now + 5000, 2000, 2000, 2000, now + 1000, true, step) == TimeSources::TS_BLOCKED);
        TimeSources t2;
        assert(t2.offer(TSRC_PHONE, now, 1000, 2000, 1000, 0, false, step) == TimeSources::TS_SET && step == now);
        assert(t2.offer(TSRC_PHONE, now + 60000 + 1500, 61000, 2000, 61000, now + 60000, true, step) == TimeSources::TS_CONFIRMED);   // 1.5 s: within the accuracy
    }
    puts("wifiap ok");
    return 0;
}
