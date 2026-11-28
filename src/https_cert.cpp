#include "https_cert.h"
#include "storage_layout.h"
#include <stdio.h>
#include <string.h>

namespace {

// Walks the -----BEGIN <label>----- ... -----END <label>----- blocks of a PEM
// text. Counts certificates, private keys (and their kind) and foreign
// blocks; false = a broken block or bytes that are not printable ASCII.
struct PemScan {
    int certs = 0, keys = 0, encrypted = 0, params = 0, foreign = 0;
};

bool labelIs(const char* l, size_t n, const char* want) { return strlen(want) == n && !memcmp(l, want, n); }

bool scanPem(const char* s, size_t len, PemScan& out)
{
    static const char BEGIN[] = "-----BEGIN ", END[] = "-----END ", DASH[] = "-----";
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c != '\n' && c != '\r' && c != '\t' && (c < 0x20 || c > 0x7E)) return false;
    }
    size_t i = 0;
    bool any = false;
    while (i < len) {
        const char* b = (const char*)memmem(s + i, len - i, BEGIN, sizeof(BEGIN) - 1);
        if (!b) break;
        const char* label = b + sizeof(BEGIN) - 1;
        const char* rest = s + len;
        const char* le = (const char*)memmem(label, rest - label, DASH, sizeof(DASH) - 1);
        if (!le) return false;
        const size_t ln = le - label;
        if (!ln || memchr(label, '\n', ln)) return false;
        // the matching END line
        char endLine[80];
        if (ln + sizeof(END) + sizeof(DASH) > sizeof(endLine)) return false;
        memcpy(endLine, END, sizeof(END) - 1);
        memcpy(endLine + sizeof(END) - 1, label, ln);
        memcpy(endLine + sizeof(END) - 1 + ln, DASH, sizeof(DASH));
        const char* body = le + sizeof(DASH) - 1;
        const char* e = (const char*)memmem(body, rest - body, endLine, strlen(endLine));
        if (!e) return false;
        if (labelIs(label, ln, "CERTIFICATE")) out.certs++;
        else if (labelIs(label, ln, "PRIVATE KEY") || labelIs(label, ln, "EC PRIVATE KEY") || labelIs(label, ln, "RSA PRIVATE KEY")) {
            out.keys++;
            if (memmem(body, e - body, "Proc-Type:", 10)) out.encrypted++;   // "Proc-Type: 4,ENCRYPTED" (traditional format)
        }
        else if (labelIs(label, ln, "ENCRYPTED PRIVATE KEY")) { out.keys++; out.encrypted++; }
        else if (labelIs(label, ln, "EC PARAMETERS")) out.params++;   // "openssl ecparam -genkey" puts it before the key
        else out.foreign++;
        any = true;
        i = (e - s) + strlen(endLine);
    }
    return any;
}

} // namespace

HttpsPemCheck httpsCheckCertPem(const char* s, size_t len)
{
    if (!s || !len) return HPEM_EMPTY;
    if (len > HTTPS_CERT_MAX) return HPEM_TOO_LONG;
    PemScan p;
    if (!scanPem(s, len, p)) return HPEM_BROKEN;
    if (p.keys) return HPEM_KEY_IN_CERT;   // checked first: never store a key in the public field
    if (p.foreign || p.params) return HPEM_FOREIGN;
    return p.certs ? HPEM_OK : HPEM_NO_CERT;
}

HttpsPemCheck httpsCheckKeyPem(const char* s, size_t len)
{
    if (!s || !len) return HPEM_EMPTY;
    if (len > HTTPS_KEY_MAX) return HPEM_TOO_LONG;
    PemScan p;
    if (!scanPem(s, len, p)) return HPEM_BROKEN;
    if (p.encrypted) return HPEM_KEY_ENCRYPTED;
    if (p.keys != 1 || p.certs || p.foreign) return p.keys ? HPEM_FOREIGN : HPEM_NO_KEY;
    return HPEM_OK;
}

size_t httpsPemClean(const char* in, size_t len, char* out, size_t cap)
{
    size_t a = 0, b = len;
    auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (a < b && blank(in[a])) a++;
    while (b > a && blank(in[b - 1])) b--;
    if (a == b || !cap) { if (cap) out[0] = '\0'; return 0; }
    size_t n = 0;
    for (size_t i = a; i < b; i++) {
        if (in[i] == '\r') continue;
        if (n + 2 >= cap) { out[0] = '\0'; return 0; }   // room for "\n" and the terminator
        out[n++] = in[i];
    }
    out[n++] = '\n';
    out[n] = '\0';
    return n;
}

size_t httpsBlobEncode(const char* cert, size_t certLen, const char* key, size_t keyLen, uint8_t* out, size_t cap)
{
    if (certLen > 0xFFFF || keyLen > 0xFFFF) return 0;
    const size_t n = 1 + 4 + certLen + keyLen;
    if (n > cap) return 0;
    out[0] = STORED_HTTPS_VERSION;
    out[1] = (uint8_t)certLen; out[2] = (uint8_t)(certLen >> 8);
    out[3] = (uint8_t)keyLen;  out[4] = (uint8_t)(keyLen >> 8);
    if (certLen) memcpy(out + 5, cert, certLen);
    if (keyLen) memcpy(out + 5 + certLen, key, keyLen);
    return n;
}

bool httpsBlobParse(const uint8_t* p, size_t len, HttpsPemView& out)
{
    out = HttpsPemView();
    if (!p || len < 4) return false;
    const size_t c = p[0] | (size_t)p[1] << 8, k = p[2] | (size_t)p[3] << 8;
    if (4 + c + k != len || c > HTTPS_CERT_MAX || k > HTTPS_KEY_MAX) return false;
    out.cert = (const char*)p + 4;
    out.certLen = c;
    out.key = (const char*)p + 4 + c;
    out.keyLen = k;
    return true;
}

bool httpRequestIsBrowserPage(bool getOrHead, const char* accept)
{
    return getOrHead && accept && strstr(accept, "text/html");
}

bool httpsShouldRedirect(bool httpsRunning, int mode, bool requestIsTls, bool onAp, bool browserPage)
{
    if (!httpsRunning || requestIsTls || onAp) return false;
    return mode == HTTP_PORT_HTTPS_ONLY || (mode == HTTP_PORT_REDIRECT_PAGES && browserPage);
}

bool httpsRedirectUrl(const char* host, const char* uri, uint16_t port, char* out, size_t cap)
{
    if (!host || !*host || !uri || !cap) return false;
    // host part without a port: "name:80" -> "name", "[fe80::1]:80" -> "[fe80::1]"
    size_t hl = strlen(host);
    if (host[0] == '[') {
        const char* close = strchr(host, ']');
        if (!close) return false;
        hl = close - host + 1;
        if (host[hl] && host[hl] != ':') return false;
    } else {
        const char* colon = strchr(host, ':');
        if (colon) hl = colon - host;
    }
    if (!hl) return false;
    for (size_t i = 0; i < hl; i++) {
        const char c = host[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '-' || c == '_' || c == '[' || c == ']' || c == ':';
        if (!ok) return false;
    }
    for (const char* u = uri; *u; u++) if ((unsigned char)*u <= 0x20 || *u == 0x7F) return false;
    char portText[8] = "";
    if (port && port != 443) snprintf(portText, sizeof(portText), ":%u", (unsigned)port);
    const int n = snprintf(out, cap, "https://%.*s%s%s", (int)hl, host, portText, uri[0] == '/' ? uri : "/");
    return n > 0 && (size_t)n < cap;
}

namespace {
bool sameTextIgnoringCase(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return *a == *b;
}
} // namespace

bool httpsCertNamesHaveIpv4(const HttpsCertNames& n, const uint8_t* ipv4)
{
    if (!ipv4) return false;
    for (int i = 0; i < n.ipv4Count && i < HTTPS_CERT_NAMES_MAX; i++)
        if (!memcmp(n.ipv4[i], ipv4, 4)) return true;
    return false;
}

bool httpsCertNamesCover(const HttpsCertNames& n, const char* dnsName, const uint8_t* ipv4)
{
    bool dns = false;
    for (int i = 0; i < n.dnsCount && i < HTTPS_CERT_NAMES_MAX; i++) dns |= dnsName && sameTextIgnoringCase(n.dns[i], dnsName);
    return dns && (!ipv4 || httpsCertNamesHaveIpv4(n, ipv4));
}

size_t httpsCertNamesText(const HttpsCertNames& n, char* out, size_t cap)
{
    if (!cap) return 0;
    out[0] = '\0';
    size_t len = 0;
    auto add = [&](const char* item) {
        const int w = snprintf(out + len, cap - len, "%s%s", len ? ", " : "", item);
        if (w < 0 || (size_t)w >= cap - len) { out[len] = '\0'; return false; }   // cut at the last whole name
        len += w;
        return true;
    };
    for (int i = 0; i < n.dnsCount && i < HTTPS_CERT_NAMES_MAX; i++) if (!add(n.dns[i])) return len;
    for (int i = 0; i < n.ipv4Count && i < HTTPS_CERT_NAMES_MAX; i++) {
        char ip[16];
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", n.ipv4[i][0], n.ipv4[i][1], n.ipv4[i][2], n.ipv4[i][3]);
        if (!add(ip)) return len;
    }
    return len;
}

bool httpsGeneratedCommonName(const char* deviceName, const char* hostname, char* out, size_t cap)
{
    const char* name = deviceName && *deviceName ? deviceName : hostname ? hostname : "";
    const size_t prefixLen = strlen(HTTPS_GENERATED_CN_PREFIX);
    if (cap < prefixLen + 2) return false;
    memcpy(out, HTTPS_GENERATED_CN_PREFIX, prefixLen);
    size_t n = prefixLen;
    for (const char* p = name; *p && n < HTTPS_GENERATED_CN_MAX; p++) {
        const unsigned char c = (unsigned char)*p;
        const bool special = c < 0x20 || c == 0x7F || strchr(",+=\\\"<>;#", c);
        if (n + 1 >= cap) return false;
        out[n++] = special ? '-' : (char)c;
    }
    // cut at the length limit inside a UTF-8 character: drop the whole character
    size_t lead = n;
    while (lead > prefixLen && ((unsigned char)out[lead - 1] & 0xC0) == 0x80) lead--;
    if (lead > prefixLen && ((unsigned char)out[lead - 1] & 0x80)) {
        const unsigned char l = (unsigned char)out[lead - 1];
        const size_t need = l >= 0xF0 ? 4 : l >= 0xE0 ? 3 : 2;
        if (n - (lead - 1) < need) n = lead - 1;
    }
    if (n == prefixLen) out[n++] = '-';   // nothing usable
    out[n] = '\0';
    return true;
}

bool httpsGeneratedSubject(const char* deviceName, const char* hostname, char* out, size_t cap)
{
    char commonName[HTTPS_GENERATED_CN_MAX + 1];
    if (!httpsGeneratedCommonName(deviceName, hostname, commonName, sizeof(commonName))) return false;
    const int n = snprintf(out, cap, "CN=%s,O=%s", commonName, HTTPS_GENERATED_ORGANIZATION);
    return n > 0 && (size_t)n < cap;
}

void httpsGeneratedValidity(int64_t now, char notBefore[15], char notAfter[15])
{
    constexpr int64_t CLOCK_SET_AFTER = 1704067200;   // 2024-01-01: before that the clock is not set
    constexpr int64_t DAY = 86400;
    const int64_t start = (now >= CLOCK_SET_AFTER ? now : HTTPS_GENERATED_UNSET_CLOCK_START) - DAY;
    const int64_t end = start + (int64_t)HTTPS_GENERATED_VALID_DAYS * DAY;
    // civil date from days since 1970-01-01 (H. Hinnant's algorithm), UTC
    auto format = [](int64_t t, char out[15]) {
        int64_t days = t / DAY, sec = t % DAY;
        if (sec < 0) { sec += DAY; days--; }
        days += 719468;
        const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
        const int64_t doe = days - era * 146097;
        const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const int64_t mp = (5 * doy + 2) / 153;
        const int d = (int)(doy - (153 * mp + 2) / 5 + 1);
        const int m = (int)(mp < 10 ? mp + 3 : mp - 9);
        const int y = (int)(yoe + era * 400 + (m <= 2));
        snprintf(out, 15, "%04d%02d%02d%02d%02d%02d", y % 10000, m, d, (int)(sec / 3600), (int)(sec / 60 % 60), (int)(sec % 60));
    };
    format(start, notBefore);
    format(end, notAfter);
}

HttpsPairPlan httpsPlanPair(bool enabled, bool uploadedValid, bool stationIpKnown, bool storedGeneratedFits)
{
    if (!enabled) return HTTPS_PLAN_OFF;
    if (uploadedValid) return HTTPS_PLAN_UPLOADED;   // the admin's pair always wins, with or without Wi-Fi
    if (!stationIpKnown) return HTTPS_PLAN_WAIT_FOR_STATION_IP;
    return storedGeneratedFits ? HTTPS_PLAN_STORED_GENERATED : HTTPS_PLAN_GENERATE;
}

const char* httpsStateName(HttpsState s)
{
    switch (s) {
        case HTTPS_RUNNING:             return "on";
        case HTTPS_WAITING_FOR_NETWORK: return "waitingForNetwork";
        case HTTPS_GENERATE_FAILED:     return "generateFailed";
        case HTTPS_LOW_MEMORY:          return "lowMemory";
        case HTTPS_START_FAILED:        return "startFailed";
        default:                        return "off";
    }
}

const char* httpsCertSourceName(HttpsCertSource s)
{
    switch (s) {
        case HTTPS_SOURCE_UPLOADED:  return "uploaded";
        case HTTPS_SOURCE_GENERATED: return "generated";
        default:                     return "none";
    }
}
