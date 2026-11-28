#pragma once
// HTTPS with the admin's own certificate or a self-signed one the node
// generates (docs/settings.md#https): the pure rules, host-tested
// (test/firmware_https.cpp). The TLS listener itself is http_server.cpp
// (esp_https_server), the mbedTLS checks, the generation and the NVS store
// https_esp.cpp, the settings and the start web_api.cpp.
//
//   - the PEM text checks before anything is stored: a certificate field must
//     hold certificates only (a private key pasted there would be shown on the
//     settings page and land in every backup), the key field one unencrypted
//     private key
//   - the stored blob: [version][u16 cert length][u16 key length][cert][key]
//     (storage_layout.h STORED_HTTPS_VERSION) - the uploaded pair under
//     "config"/"https", the generated one under "config"/"httpsgen"
//   - port 80 while HTTPS runs: stays available, redirects browser pages, or
//     redirects everything (307) - never for a client on the own access point
//   - which pair the start uses (uploaded, stored generated, generate a new
//     one, wait for the station address), the generated certificate's names
//     and validity
//   - the start decision and the state names for the status page / /api/info
// No Arduino types here.
#include <stddef.h>
#include <stdint.h>

// PEM sizes: a certificate with two intermediates is ~5 KB, an RSA-4096 key
// ~3.3 KB (EC P-256: ~230 B).
constexpr size_t HTTPS_CERT_MAX = 8192;
constexpr size_t HTTPS_KEY_MAX  = 4096;
constexpr size_t HTTPS_BLOB_MAX = 1 + 4 + HTTPS_CERT_MAX + HTTPS_KEY_MAX;

// ---- PEM text checks ----
enum HttpsPemCheck : uint8_t {
    HPEM_OK = 0,
    HPEM_EMPTY,          // nothing given
    HPEM_TOO_LONG,       // over HTTPS_CERT_MAX / HTTPS_KEY_MAX
    HPEM_BROKEN,         // no complete -----BEGIN/END----- block, or not plain text
    HPEM_NO_CERT,        // certificate field without a CERTIFICATE block
    HPEM_NO_KEY,         // key field without a PRIVATE KEY block
    HPEM_KEY_IN_CERT,    // a private key in the certificate field
    HPEM_KEY_ENCRYPTED,  // a password-protected key: not supported
    HPEM_FOREIGN,        // another kind of block (e.g. a CSR) in the field
};
HttpsPemCheck httpsCheckCertPem(const char* s, size_t len);
HttpsPemCheck httpsCheckKeyPem(const char* s, size_t len);

// Copies `in` without carriage returns and surrounding blank space (a
// browser textarea sends CRLF) and ends it with one newline; returns the
// length (0 = empty, or it does not fit `cap` incl. the terminating 0).
size_t httpsPemClean(const char* in, size_t len, char* out, size_t cap);

// ---- stored blob ----
// Writes [STORED_HTTPS_VERSION][u16 LE cert][u16 LE key][cert][key]; 0 = does not fit.
size_t httpsBlobEncode(const char* cert, size_t certLen, const char* key, size_t keyLen, uint8_t* out, size_t cap);
// The payload after the version byte (storedBlobLoad()): the two texts (not
// 0-terminated) or false if the lengths do not add up.
struct HttpsPemView { const char* cert = nullptr; size_t certLen = 0; const char* key = nullptr; size_t keyLen = 0; };
bool httpsBlobParse(const uint8_t* payload, size_t len, HttpsPemView& out);

// ---- port 80 while HTTPS runs (gCfg.httpPortMode) ----
enum HttpPortMode : uint8_t {
    HTTP_PORT_KEEP           = 0,   // both work
    HTTP_PORT_REDIRECT_PAGES = 1,   // a browser opening a page goes to https; API, scripts, POSTs stay on http
    HTTP_PORT_HTTPS_ONLY     = 2,   // every request goes to https (API and firmware upload included)
};
// A browser navigation: GET / HEAD that accepts text/html (what a browser
// sends for a page; curl, Home Assistant, Prometheus do not).
bool httpRequestIsBrowserPage(bool getOrHead, const char* acceptHeader);
// A plain request is redirected only when HTTPS really runs (otherwise port
// 80 serves everything - no lock-out), the mode says so for this request, and
// the client is not on the own access point (captive portal, phones without
// the node's certificate).
bool httpsShouldRedirect(bool httpsRunning, int mode, bool requestIsTls, bool onAp, bool browserPage);
// https://<host without port>[:port]<uri> (port 443 left out); false = no
// usable Host header (empty, or characters a header must not carry) - the
// request is then answered on HTTP.
bool httpsRedirectUrl(const char* host, const char* uri, uint16_t port, char* out, size_t cap);

// ---- names in a certificate (Subject Alternative Names) ----
constexpr int HTTPS_CERT_NAMES_MAX = 4;   // each kind; more are not listed
struct HttpsCertNames {
    char    dns[HTTPS_CERT_NAMES_MAX][64] = {};
    uint8_t dnsCount = 0;
    uint8_t ipv4[HTTPS_CERT_NAMES_MAX][4] = {};
    uint8_t ipv4Count = 0;
};
// The certificate names `dnsName` (case-insensitive) and - unless ipv4 is
// nullptr - the address ipv4.
bool httpsCertNamesCover(const HttpsCertNames& n, const char* dnsName, const uint8_t* ipv4);
bool httpsCertNamesHaveIpv4(const HttpsCertNames& n, const uint8_t* ipv4);
// "weathernode.local, 192.168.1.20" (cut to fit); returns the length.
size_t httpsCertNamesText(const HttpsCertNames& n, char* out, size_t cap);

// ---- the self-signed certificate the node generates ----
// EC P-256, CN = the device name, SAN DNS <hostname>.local + IP = the station
// address. 825 days: the longest validity Apple devices accept for a TLS
// server certificate, also for one the user trusts by hand.
constexpr int HTTPS_GENERATED_VALID_DAYS = 825;
// Name of a generated certificate. It is its own CA, so trust stores (Windows
// certificate manager, macOS keychain, Android / iOS "installed CAs") list it
// by this name: "Universal Weather Node <device name>", organisation
// "Universal Weather Node" - recognisable, and several nodes stay apart.
#define HTTPS_GENERATED_CN_PREFIX     "Universal Weather Node "
#define HTTPS_GENERATED_ORGANIZATION  "Universal Weather Node"
// at most 63 characters (X.509 allows 64; HttpsCertInfo::subject holds 63)
constexpr size_t HTTPS_GENERATED_CN_MAX = 63;
// The common name: prefix + device name (empty: the host name); the characters
// an X.509 name string would need escaped (, + = \ " < > ; #) and control
// characters become '-', cut at HTTPS_GENERATED_CN_MAX without splitting a
// UTF-8 character. false = does not fit `cap`.
bool httpsGeneratedCommonName(const char* deviceName, const char* hostname, char* out, size_t cap);
// "CN=<common name>,O=Universal Weather Node" for
// mbedtls_x509write_crt_set_subject_name(). false = does not fit `cap`.
bool httpsGeneratedSubject(const char* deviceName, const char* hostname, char* out, size_t cap);
// notBefore / notAfter as "YYYYMMDDhhmmss" (UTC): from a day before `now`
// (clock skew of the clients) for HTTPS_GENERATED_VALID_DAYS. Without a set
// clock (now before 2024) from HTTPS_GENERATED_UNSET_CLOCK_START instead.
constexpr int64_t HTTPS_GENERATED_UNSET_CLOCK_START = 1793491200;   // 2026-11-01 00:00 UTC
void httpsGeneratedValidity(int64_t now, char notBefore[15], char notAfter[15]);

// ---- which pair the start uses ----
enum HttpsPairPlan : uint8_t {
    HTTPS_PLAN_OFF = 0,               // HTTPS not enabled
    HTTPS_PLAN_UPLOADED,              // the admin's pair (parses, key matches)
    HTTPS_PLAN_STORED_GENERATED,      // the generated pair from an earlier start still fits
    HTTPS_PLAN_GENERATE,              // make a new pair (none, unusable, other names / address, expired)
    HTTPS_PLAN_WAIT_FOR_STATION_IP,   // a generated pair needs the station address: start once Wi-Fi is up
};
// storedGeneratedFits = parses, key matches, names cover <hostname>.local and
// the station address, its common name is httpsGeneratedCommonName() of the
// current device name, not expired (when the clock is set).
HttpsPairPlan httpsPlanPair(bool enabled, bool uploadedValid, bool stationIpKnown, bool storedGeneratedFits);

// ---- start ----
enum HttpsState : uint8_t {
    HTTPS_OFF = 0,                // not enabled (the default)
    HTTPS_RUNNING,                // listening on port 443
    HTTPS_WAITING_FOR_NETWORK,    // no uploaded pair: the generated one waits for the station address
    HTTPS_GENERATE_FAILED,        // generating or storing a certificate failed (detail: mbedTLS code)
    HTTPS_LOW_MEMORY,             // too little internal RAM for the second server task
    HTTPS_START_FAILED,           // esp_https_server refused (detail: esp_err_t)
};
const char* httpsStateName(HttpsState s);   // /api/info outputs.https

enum HttpsCertSource : uint8_t { HTTPS_SOURCE_NONE = 0, HTTPS_SOURCE_UPLOADED, HTTPS_SOURCE_GENERATED };
const char* httpsCertSourceName(HttpsCertSource s);   // /api/info outputs.httpsCertificate

// Internal RAM the HTTPS listener needs before it starts: its task stack
// (HTTPS_TASK_STACK) plus a reserve for the rest of the node. The TLS buffers
// themselves (~33 KB per connection) are in the PSRAM.
constexpr size_t HTTPS_TASK_STACK     = 12288;
constexpr size_t HTTPS_MIN_FREE_START = HTTPS_TASK_STACK + 40 * 1024;
// Enough internal RAM to start the listener.
inline bool httpsEnoughMemoryToStart(size_t internalFree) { return internalFree >= HTTPS_MIN_FREE_START; }

// Status for the status page and /api/info (https_esp.cpp; weak default off).
struct HttpsStatus {
    uint8_t  state = HTTPS_OFF;
    uint8_t  source = HTTPS_SOURCE_NONE;   // HttpsCertSource of the running certificate
    bool     uploadedRejected = false;     // an uploaded pair is stored but not usable (the generated one runs)
    int32_t  code = 0;           // mbedTLS / esp_err_t code of a failure
    uint16_t port = 0;
    char     subject[64] = "";   // CN of the running certificate
    char     notAfter[11] = "";  // YYYY-MM-DD
    HttpsCertNames names;        // its SANs
};
void httpsStatus(HttpsStatus& out);

// ---- device only (https_esp.cpp: NVS "config"/"https" and mbedTLS) ----

// The two stored pairs: the admin's ("config"/"https") and the generated one
// ("config"/"httpsgen"; never written over the admin's). Both are cleared by
// either reset (the "config" namespace).
enum HttpsPairSlot : uint8_t { HTTPS_PAIR_UPLOADED = 0, HTTPS_PAIR_GENERATED };

// A stored pair, both texts 0-terminated in one buffer (PSRAM).
struct HttpsPem {
    char*  buf = nullptr;
    const char* cert = "";
    size_t certLen = 0;
    const char* key = "";
    size_t keyLen = 0;
    HttpsPem() = default;
    HttpsPem(const HttpsPem&) = delete;
    HttpsPem& operator=(const HttpsPem&) = delete;
    ~HttpsPem();
    bool havePair() const { return certLen && keyLen; }
};
// false = nothing stored (or unreadable / a newer firmware's format): empty texts
bool httpsPemLoad(HttpsPem& out, HttpsPairSlot slot = HTTPS_PAIR_UPLOADED);
// Both empty: the slot's key is erased. false = NVS write failed (or sealed by a reset).
bool httpsPemSave(const char* cert, size_t certLen, const char* key, size_t keyLen, HttpsPairSlot slot = HTTPS_PAIR_UPLOADED);

// mbedTLS check of a pair (texts 0-terminated, lengths without the 0): the
// certificate chain and the key parse, and the key belongs to the first
// certificate. HV_OK fills `info` (may be nullptr).
enum HttpsVerify : uint8_t { HV_OK = 0, HV_CERT, HV_KEY, HV_MISMATCH, HV_NO_MEMORY };
struct HttpsCertInfo {
    char subject[64] = "";   // CN (or the whole subject without one)
    char notAfter[11] = "";  // YYYY-MM-DD
    char keyType[16] = "";   // "EC 256", "RSA 2048"
    bool expired = false;    // only when the clock is set
    HttpsCertNames names;    // DNS / IPv4 Subject Alternative Names
};
HttpsVerify httpsVerifyPair(const char* cert, size_t certLen, const char* key, size_t keyLen,
                            HttpsCertInfo* info = nullptr, int* mbedCode = nullptr);
// Certificate only (the settings page): false = does not parse.
bool httpsCertInfo(const char* cert, size_t certLen, HttpsCertInfo& info);

// A new self-signed pair (EC P-256, see HTTPS_GENERATED_VALID_DAYS) for
// `subject` ("CN=...", httpsGeneratedSubject()), SAN DNS `dnsName` and IPv4
// `ipv4`, random serial; stored in the slot HTTPS_PAIR_GENERATED and returned
// in `out`. 0 = done, else the mbedTLS code (or -1: no memory / not stored).
int httpsGenerate(const char* subject, const char* dnsName, const uint8_t ipv4[4], HttpsPem& out);

// web_api.cpp after each server start
void httpsSetStatus(const HttpsStatus& s);
