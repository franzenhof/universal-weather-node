#include "wifi_ap_logic.h"
#include <stdio.h>
#include <string.h>

const char* apModeName(int mode)
{
    switch (mode) {
        case AP_FALLBACK: return "fallback";
        case AP_ALWAYS:   return "always";
        default:          return "off";
    }
}

bool apPassValid(const char* pass)
{
    const size_t n = strlen(pass);
    if (n < 8 || n > 63) return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)pass[i] < 32 || (unsigned char)pass[i] > 126) return false;
    return true;
}

void apDefaultSsid(const uint8_t mac[6], char* out, size_t len)
{
    snprintf(out, len, "WeatherNode-%02X%02X", mac[4], mac[5]);
}

size_t wifiQrText(const char* ssid, const char* pass, char* out, size_t cap)
{
    size_t n = 0;
    bool ok = true;
    auto add = [&](char c) { if (n + 1 < cap) out[n++] = c; else ok = false; };
    auto addStr = [&](const char* s) { while (*s) add(*s++); };
    auto addEsc = [&](const char* s) {
        for (; *s; s++) {
            if (strchr("\\;,:\"", *s)) add('\\');
            add(*s);
        }
    };
    if (!cap) return 0;
    addStr("WIFI:T:WPA;S:");
    addEsc(ssid);
    addStr(";P:");
    addEsc(pass);
    addStr(";;");
    out[n] = '\0';
    if (!ok) { out[0] = '\0'; return 0; }
    return n;
}

bool ApFallback::update(uint8_t mode, bool staUpNow, uint64_t nowMs)
{
    if (!started || staUpNow != staUp) { started = true; staUp = staUpNow; sinceMs = nowMs; }
    if (mode == AP_ALWAYS) { active = true; return true; }
    if (mode != AP_FALLBACK) { active = false; return false; }
    const uint64_t held = nowMs - sinceMs;
    if (!active && !staUp && held >= DOWN_MS) active = true;
    else if (active && staUp && held >= UP_MS) active = false;
    return active;
}

bool apStationWanted(uint8_t mode, bool staConfigured)
{
    return mode != AP_ALWAYS || staConfigured;
}

// ---- captive checks ----

bool captiveProbePath(const char* path)
{
    static const char* const probes[] = {
        "/generate_204", "/gen_204",                       // Android, Chrome
        "/hotspot-detect.html", "/library/test/success.html", // Apple
        "/connecttest.txt", "/ncsi.txt", "/redirect",      // Windows
        "/canonical.html", "/success.txt",                 // Firefox
        "/check_network_status.txt", "/mobile/status.php", // Samsung / misc.
    };
    const char* q = strchr(path, '?');
    const size_t len = q ? (size_t)(q - path) : strlen(path);
    for (const char* p : probes)
        if (strlen(p) == len && !strncmp(p, path, len)) return true;
    return false;
}

static bool hostEq(const char* a, size_t an, const char* b, const char* suffix)
{
    const size_t bn = strlen(b), sn = strlen(suffix);
    if (an != bn + sn) return false;
    for (size_t i = 0; i < an; i++) {
        char x = a[i], y = i < bn ? b[i] : suffix[i - bn];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool captiveRedirect(const char* host, const char* path, const char* hostname)
{
    if (captiveProbePath(path)) return true;
    if (!host || !*host) return false;   // HTTP/1.0 without Host: serve
    size_t n = strlen(host);
    if (host[0] != '[') {                 // "name:port" (an IPv6 literal keeps its colons)
        const char* c = strchr(host, ':');
        if (c) n = (size_t)(c - host);
    }
    if (n && host[n - 1] == '.') n--;     // "example.com."
    if (hostEq(host, n, AP_IP_TEXT, "")) return false;
    if (hostname && *hostname && (hostEq(host, n, hostname, "") || hostEq(host, n, hostname, ".local"))) return false;
    return true;
}

size_t dnsCaptiveAnswer(const uint8_t* q, size_t n, const uint8_t ip[4], uint8_t* out, size_t cap)
{
    if (n < 12 || n > 512) return 0;
    if (q[2] & 0x80) return 0;                 // a response
    if ((q[2] >> 3 & 0x0F) != 0) return 0;     // opcode: standard query only
    if (q[4] != 0 || q[5] != 1) return 0;      // exactly one question
    size_t p = 12;
    for (;;) {
        if (p >= n) return 0;
        const uint8_t l = q[p];
        if (l == 0) { p++; break; }
        if (l > 63) return 0;                  // also no compression pointer in a question
        p += 1 + (size_t)l;
        if (p - 12 > 255) return 0;
    }
    if (p + 4 > n) return 0;
    const uint16_t qtype = (uint16_t)(q[p] << 8 | q[p + 1]), qclass = (uint16_t)(q[p + 2] << 8 | q[p + 3]);
    p += 4;
    const bool answer = (qclass == 1 || qclass == 255) && (qtype == 1 || qtype == 255);
    const size_t need = p + (answer ? 16 : 0);
    if (need > cap) return 0;
    memcpy(out, q, p);                         // header + question (EDNS records of the query are left out)
    out[2] = (uint8_t)(0x84 | (q[2] & 0x01));  // response, authoritative, RD as asked
    out[3] = 0x80;                             // RA, NOERROR
    out[6] = 0; out[7] = answer ? 1 : 0;       // answers
    out[8] = out[9] = out[10] = out[11] = 0;   // no authority / additional records
    if (answer) {
        static const uint8_t rr[12] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4 };   // name -> question, A, IN, TTL 30 s, 4 bytes
        memcpy(out + p, rr, sizeof(rr));
        memcpy(out + p + 12, ip, 4);
    }
    return need;
}

// ---- time from phones ----

PhoneTimeVerdict phoneTimeCheck(int64_t unixMs, bool onAp, TimeSource bestFresh)
{
    if (unixMs < 1704067200000LL || unixMs >= 4102444800000LL) return PT_INVALID;   // 2024-01-01 .. 2100-01-01
    if (onAp || bestFresh == TSRC_NONE) return PT_OFFER;
    return PT_REFUSED;
}

// ---- QR rendering ----

int qrOledRender(const QrCode& q, uint8_t buf[1024])
{
    if (!q.version) return 0;
    int scale = 64 / (q.size + 2);
    if (scale < 1) return 0;
    const int off = (64 - q.size * scale) / 2;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const int mx = (x - off) < 0 ? -1 : (x - off) / scale, my = (y - off) < 0 ? -1 : (y - off) / scale;
            const bool lit = !q.dark(mx, my);   // outside the code: lit (quiet zone)
            uint8_t& b = buf[x + 128 * (y >> 3)];
            if (lit) b |= (uint8_t)(1 << (y & 7));
            else     b &= (uint8_t)~(1 << (y & 7));
        }
    return scale;
}

void qrSvg(const QrCode& q, void (*emit)(const char* s, void* arg), void* arg)
{
    if (!q.version) return;
    char b[80];
    const int w = q.size + 4;
    snprintf(b, sizeof(b), "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 %d %d' ", w, w);
    emit(b, arg);
    emit("shape-rendering='crispEdges'><rect width='100%' height='100%' fill='#fff'/><path fill='#000' d='", arg);
    for (int y = 0; y < q.size; y++)
        for (int x = 0; x < q.size;) {
            if (!q.dark(x, y)) { x++; continue; }
            int e = x;
            while (e < q.size && q.dark(e, y)) e++;
            snprintf(b, sizeof(b), "M%d %dh%dv1h-%dz", x + 2, y + 2, e - x, e - x);
            emit(b, arg);
            x = e;
        }
    emit("'/></svg>", arg);
}
