// HTTPS (src/https_cert.cpp): the PEM text checks before a pair is stored,
// the cleaning of a textarea's text, the stored blob, the port-80 redirect
// rules (three modes, own access point, HTTPS not running), which pair the
// start uses, and the generated certificate's names and validity. The
// mbedTLS part (parse, key matches certificate, generation) runs on the
// device only (https_esp.cpp).
#include "https_cert.h"
#include "storage_layout.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
const char CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBszCCAVmgAwIBAgIUQ2xhdWRlIHRlc3QgY2VydGlmaWNhdGUwCgYIKoZIzj0E\n"
    "AwIwFTETMBEGA1UEAwwKd2VhdGhlcm5vZGUwHhcNMjYxMTI3MDAwMDAwWhcNMjgx\n"
    "-----END CERTIFICATE-----\n";
const char CHAIN[] =
    "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\nBBBB\n-----END CERTIFICATE-----\n";
const char KEY_PKCS8[] = "-----BEGIN PRIVATE KEY-----\nMIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg\n-----END PRIVATE KEY-----\n";
const char KEY_EC[] =    // what "openssl ecparam -genkey" writes: parameters, then the key
    "-----BEGIN EC PARAMETERS-----\nBggqhkjOPQMBBw==\n-----END EC PARAMETERS-----\n"
    "-----BEGIN EC PRIVATE KEY-----\nMHcCAQEEIA==\n-----END EC PRIVATE KEY-----\n";
const char KEY_RSA[] = "-----BEGIN RSA PRIVATE KEY-----\nMIIEpAIBAAKCAQEA\n-----END RSA PRIVATE KEY-----\n";
const char KEY_PKCS8_ENC[] = "-----BEGIN ENCRYPTED PRIVATE KEY-----\nMIIFHDBOBgkqhkiG9w0BBQ0w\n-----END ENCRYPTED PRIVATE KEY-----\n";
const char KEY_TRAD_ENC[] =
    "-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-256-CBC,0123\n\nMIIE\n-----END RSA PRIVATE KEY-----\n";
const char CSR[] = "-----BEGIN CERTIFICATE REQUEST-----\nMIIB\n-----END CERTIFICATE REQUEST-----\n";

HttpsPemCheck cert(const std::string& s) { return httpsCheckCertPem(s.c_str(), s.size()); }
HttpsPemCheck key(const std::string& s) { return httpsCheckKeyPem(s.c_str(), s.size()); }
} // namespace

int main()
{
    // ---- certificate field ----
    assert(cert(CERT) == HPEM_OK);
    assert(cert(CHAIN) == HPEM_OK);                              // node certificate + intermediate
    assert(cert("") == HPEM_EMPTY);
    assert(cert("hello") == HPEM_BROKEN);
    assert(cert("-----BEGIN CERTIFICATE-----\nAAAA\n") == HPEM_BROKEN);   // no END line
    assert(cert("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE----") == HPEM_BROKEN);
    assert(cert(std::string(CERT) + "\xC3\xA4") == HPEM_BROKEN);    // not plain ASCII
    // a key pasted into the public field is refused (it would be shown and backed up)
    assert(cert(std::string(CERT) + KEY_PKCS8) == HPEM_KEY_IN_CERT);
    assert(cert(KEY_EC) == HPEM_KEY_IN_CERT);
    assert(cert(KEY_PKCS8_ENC) == HPEM_KEY_IN_CERT);
    assert(cert(CSR) == HPEM_FOREIGN);
    assert(cert(std::string(CERT) + CSR) == HPEM_FOREIGN);
    {
        std::string big = CERT;
        while (big.size() <= HTTPS_CERT_MAX) big += CERT;
        assert(cert(big) == HPEM_TOO_LONG);
    }

    // ---- key field ----
    assert(key(KEY_PKCS8) == HPEM_OK);
    assert(key(KEY_EC) == HPEM_OK);
    assert(key(KEY_RSA) == HPEM_OK);
    assert(key(std::string("\r\n  ") + KEY_PKCS8) == HPEM_OK);   // surrounding blank space is fine
    assert(key("") == HPEM_EMPTY);
    assert(key(CERT) == HPEM_NO_KEY);                             // the certificate in the key field
    assert(key(KEY_PKCS8_ENC) == HPEM_KEY_ENCRYPTED);
    assert(key(KEY_TRAD_ENC) == HPEM_KEY_ENCRYPTED);
    assert(key(std::string(KEY_PKCS8) + KEY_RSA) == HPEM_FOREIGN);   // two keys
    assert(key(std::string(KEY_PKCS8) + CERT) == HPEM_FOREIGN);      // key and certificate in one field
    assert(key("-----BEGIN PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n") == HPEM_BROKEN);

    // ---- textarea text: CRLF and blank space removed, one closing newline ----
    {
        char out[256];
        const char in[] = "  \r\n-----BEGIN CERTIFICATE-----\r\nAAAA\r\n-----END CERTIFICATE-----\r\n\r\n ";
        const size_t n = httpsPemClean(in, strlen(in), out, sizeof(out));
        assert(n == strlen(out));
        assert(!strcmp(out, "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"));
        assert(httpsPemClean(" \r\n\t", 4, out, sizeof(out)) == 0 && out[0] == '\0');
        assert(httpsPemClean(in, strlen(in), out, 20) == 0);     // does not fit
        assert(httpsPemClean("AB", 2, out, 4) == 3 && !strcmp(out, "AB\n"));
        assert(httpsPemClean("ABC", 3, out, 4) == 0);            // "ABC\n" + 0 needs 5
    }

    // ---- stored blob ----
    {
        static uint8_t b[HTTPS_BLOB_MAX];
        const size_t n = httpsBlobEncode(CERT, strlen(CERT), KEY_EC, strlen(KEY_EC), b, sizeof(b));
        assert(n == 1 + 4 + strlen(CERT) + strlen(KEY_EC));
        assert(b[0] == STORED_HTTPS_VERSION && STORED_HTTPS_VERSION == 1);
        size_t len = 0;
        const uint8_t* p = storedBlobLoad(SF_HTTPS, b, n, STORED_HTTPS_VERSION, 0, nullptr, 0, &len);
        assert(p == b + 1 && len == n - 1);
        HttpsPemView v;
        assert(httpsBlobParse(p, len, v));
        assert(v.certLen == strlen(CERT) && !memcmp(v.cert, CERT, v.certLen));
        assert(v.keyLen == strlen(KEY_EC) && !memcmp(v.key, KEY_EC, v.keyLen));
        assert(!httpsBlobParse(p, len - 1, v) && !v.cert);        // lengths do not add up
        assert(!httpsBlobParse(p, 3, v));
        b[0] = STORED_HTTPS_VERSION + 1;                          // a newer firmware's blob: not read
        assert(!storedBlobLoad(SF_HTTPS, b, n, STORED_HTTPS_VERSION, 0, nullptr, 0));
        assert(httpsBlobEncode(CERT, strlen(CERT), KEY_EC, strlen(KEY_EC), b, 10) == 0);   // too small
        assert(httpsBlobEncode("", 0, "", 0, b, sizeof(b)) == 5);
    }

    // ---- port 80 while HTTPS runs ----
    {
        const char* browserAccept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
        assert(httpRequestIsBrowserPage(true, browserAccept));
        assert(!httpRequestIsBrowserPage(true, "*/*"));                  // curl
        assert(!httpRequestIsBrowserPage(true, "application/json"));     // Home Assistant REST
        assert(!httpRequestIsBrowserPage(true, ""));
        assert(!httpRequestIsBrowserPage(true, nullptr));
        assert(!httpRequestIsBrowserPage(false, browserAccept));         // a POST (settings form, firmware upload)
    }
    for (int mode : { HTTP_PORT_KEEP, HTTP_PORT_REDIRECT_PAGES, HTTP_PORT_HTTPS_ONLY })
        for (bool page : { false, true }) {
            assert(!httpsShouldRedirect(false, mode, false, false, page));   // HTTPS not running: port 80 serves everything
            assert(!httpsShouldRedirect(true, mode, true, false, page));     // already HTTPS
            assert(!httpsShouldRedirect(true, mode, false, true, page));     // own access point: stays HTTP
        }
    assert(!httpsShouldRedirect(true, HTTP_PORT_KEEP, false, false, true));          // "stays available"
    assert(httpsShouldRedirect(true, HTTP_PORT_REDIRECT_PAGES, false, false, true));  // a browser page
    assert(!httpsShouldRedirect(true, HTTP_PORT_REDIRECT_PAGES, false, false, false));// API, scripts, POSTs stay
    assert(httpsShouldRedirect(true, HTTP_PORT_HTTPS_ONLY, false, false, false));     // HTTPS only: API and upload too
    assert(httpsShouldRedirect(true, HTTP_PORT_HTTPS_ONLY, false, false, true));
    assert(!httpsShouldRedirect(true, 7, false, false, true));                        // unknown mode: no redirect
    {
        char u[128];
        assert(httpsRedirectUrl("192.168.1.20", "/status", 443, u, sizeof(u)) && !strcmp(u, "https://192.168.1.20/status"));
        assert(httpsRedirectUrl("wetter.local:80", "/api/data?x=1", 443, u, sizeof(u)) && !strcmp(u, "https://wetter.local/api/data?x=1"));
        assert(httpsRedirectUrl("[fe80::1]:80", "/", 443, u, sizeof(u)) && !strcmp(u, "https://[fe80::1]/"));
        assert(httpsRedirectUrl("node", "/", 8443, u, sizeof(u)) && !strcmp(u, "https://node:8443/"));
        assert(httpsRedirectUrl("node", "", 443, u, sizeof(u)) && !strcmp(u, "https://node/"));
        assert(!httpsRedirectUrl("", "/", 443, u, sizeof(u)));                 // no Host header: served on HTTP
        assert(!httpsRedirectUrl(":80", "/", 443, u, sizeof(u)));
        assert(!httpsRedirectUrl("evil.com\r\nX: y", "/", 443, u, sizeof(u)));   // no header injection
        assert(!httpsRedirectUrl("a b", "/", 443, u, sizeof(u)));
        assert(!httpsRedirectUrl("[fe80::1", "/", 443, u, sizeof(u)));
        assert(!httpsRedirectUrl("node", "/a b", 443, u, sizeof(u)));
        assert(!httpsRedirectUrl("node", "/status", 443, u, 12));               // does not fit
    }

    // ---- which pair the start uses ----
    // httpsPlanPair(enabled, uploadedValid, stationIpKnown, storedGeneratedFits)
    for (bool up : { false, true }) for (bool ip : { false, true }) for (bool fits : { false, true })
        assert(httpsPlanPair(false, up, ip, fits) == HTTPS_PLAN_OFF);
    for (bool ip : { false, true }) for (bool fits : { false, true })
        assert(httpsPlanPair(true, true, ip, fits) == HTTPS_PLAN_UPLOADED);       // the admin's pair wins, Wi-Fi or not
    assert(httpsPlanPair(true, false, false, true) == HTTPS_PLAN_WAIT_FOR_STATION_IP);   // only the own AP is up
    assert(httpsPlanPair(true, false, false, false) == HTTPS_PLAN_WAIT_FOR_STATION_IP);
    assert(httpsPlanPair(true, false, true, true) == HTTPS_PLAN_STORED_GENERATED);       // reused across restarts
    assert(httpsPlanPair(true, false, true, false) == HTTPS_PLAN_GENERATE);              // none / other names / expired

    // ---- names: when a stored generated certificate still fits ----
    {
        HttpsCertNames n;
        strcpy(n.dns[0], "garden.local");
        n.dnsCount = 1;
        const uint8_t ip[4] = { 192, 168, 1, 20 }, other[4] = { 192, 168, 1, 21 };
        memcpy(n.ipv4[0], ip, 4);
        n.ipv4Count = 1;
        assert(httpsCertNamesCover(n, "garden.local", ip));
        assert(httpsCertNamesCover(n, "Garden.LOCAL", ip));          // DNS names are case-insensitive
        assert(!httpsCertNamesCover(n, "garden.local", other));      // the DHCP address changed: regenerate
        assert(!httpsCertNamesCover(n, "kitchen.local", ip));        // the device name changed: regenerate
        assert(!httpsCertNamesCover(n, "garden.loca", ip));
        assert(httpsCertNamesCover(n, "garden.local", nullptr));     // address not asked
        assert(httpsCertNamesHaveIpv4(n, ip) && !httpsCertNamesHaveIpv4(n, other) && !httpsCertNamesHaveIpv4(n, nullptr));
        HttpsCertNames none;
        assert(!httpsCertNamesCover(none, "garden.local", nullptr));
        char t[64];
        assert(httpsCertNamesText(n, t, sizeof(t)) == strlen(t) && !strcmp(t, "garden.local, 192.168.1.20"));
        assert(httpsCertNamesText(n, t, 16) == 12 && !strcmp(t, "garden.local"));   // cut at the last whole name
        assert(httpsCertNamesText(none, t, sizeof(t)) == 0 && t[0] == '\0');
    }

    // ---- the generated certificate's name ----
    {
        char s[128], cn[HTTPS_GENERATED_CN_MAX + 1];
        const std::string P = HTTPS_GENERATED_CN_PREFIX;   // "Universal Weather Node "
        assert(httpsGeneratedSubject("HeltecV4", "heltecv4", s, sizeof(s)) &&
               !strcmp(s, "CN=Universal Weather Node HeltecV4,O=Universal Weather Node"));
        assert(httpsGeneratedCommonName("Garden", "garden", cn, sizeof(cn)) && P + "Garden" == cn);
        assert(httpsGeneratedCommonName("", "weathernode-a1b2c3", cn, sizeof(cn)) && P + "weathernode-a1b2c3" == cn);
        assert(httpsGeneratedCommonName(nullptr, "h", cn, sizeof(cn)) && P + "h" == cn);
        // characters an X.509 name string would need escaped: never passed on
        assert(httpsGeneratedCommonName("a,b+c=d\\e\"f<g>h;i#j\nk", "x", cn, sizeof(cn)) && P + "a-b-c-d-e-f-g-h-i-j-k" == cn);
        assert(httpsGeneratedCommonName("K\xC3\xBC" "che", "x", cn, sizeof(cn)) && P + "K\xC3\xBC" "che" == cn);   // UTF-8 kept
        const std::string longName(70, 'n');
        assert(httpsGeneratedCommonName(longName.c_str(), "x", cn, sizeof(cn)) && strlen(cn) == HTTPS_GENERATED_CN_MAX);
        std::string utf(HTTPS_GENERATED_CN_MAX - P.size() - 1, 'a');
        utf += "\xC3\xBC";                                          // a 2-byte character across the limit: dropped whole
        assert(httpsGeneratedCommonName(utf.c_str(), "x", cn, sizeof(cn)) && strlen(cn) == HTTPS_GENERATED_CN_MAX - 1);
        assert(!httpsGeneratedSubject("Garden", "g", s, 20));         // does not fit
    }

    // ---- the generated certificate's validity ----
    {
        char nb[15], na[15];
        httpsGeneratedValidity(1795737600, nb, na);                  // 2026-11-27 00:00 UTC, clock set
        assert(!strcmp(nb, "20261126000000"));                       // a day back: clients with a slow clock
        assert(!strcmp(na, "20290228000000"));                       // + 825 days
        httpsGeneratedValidity(1795780245, nb, na);                  // 2026-11-27 11:50:45
        assert(!strcmp(nb, "20261126115045") && !strcmp(na, "20290228115045"));
        httpsGeneratedValidity(0, nb, na);                           // clock not set: from 2026-11-01
        assert(!strcmp(nb, "20261031000000") && !strcmp(na, "20290202000000"));
        httpsGeneratedValidity(1600000000, nb, na);                  // 2020: still "not set"
        assert(!strcmp(nb, "20261031000000"));
        httpsGeneratedValidity(4102444800, nb, na);                  // 2100-01-01 (not a leap year in 2100)
        assert(!strcmp(nb, "20991231000000") && !strcmp(na, "21020405000000"));
    }

    // ---- start: memory, state and source names ----
    assert(!httpsEnoughMemoryToStart(HTTPS_MIN_FREE_START - 1) && httpsEnoughMemoryToStart(HTTPS_MIN_FREE_START));
    assert(!strcmp(httpsStateName(HTTPS_OFF), "off") && !strcmp(httpsStateName(HTTPS_RUNNING), "on"));
    assert(!strcmp(httpsStateName(HTTPS_WAITING_FOR_NETWORK), "waitingForNetwork"));
    assert(!strcmp(httpsStateName(HTTPS_GENERATE_FAILED), "generateFailed"));
    assert(!strcmp(httpsStateName(HTTPS_LOW_MEMORY), "lowMemory") && !strcmp(httpsStateName(HTTPS_START_FAILED), "startFailed"));
    assert(!strcmp(httpsCertSourceName(HTTPS_SOURCE_UPLOADED), "uploaded") && !strcmp(httpsCertSourceName(HTTPS_SOURCE_GENERATED), "generated"));
    assert(!strcmp(httpsCertSourceName(HTTPS_SOURCE_NONE), "none"));

    puts("https: ok");
    return 0;
}
