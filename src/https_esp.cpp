// HTTPS on the device (https_cert.h): the certificate and key in NVS
// ("config"/"https" uploaded, "config"/"httpsgen" generated - either reset
// clears both with the settings), the mbedTLS checks before a pair is stored
// or started, the generation of the self-signed pair, and the status.
#include "http_util.h"   // httpWipe
#include "https_cert.h"
#include "storage_layout.h"
#include "storage_write.h"
#include "serial_log.h"
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <mbedtls/asn1.h>
#include <mbedtls/ecp.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <memory>

namespace {
constexpr const char* NS = "config";
const char* slotKey(HttpsPairSlot slot) { return slot == HTTPS_PAIR_GENERATED ? "httpsgen" : "https"; }

void* psramAlloc(size_t n)
{
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

int fillRandom(void*, unsigned char* out, size_t len)
{
    esp_fill_random(out, len);
    return 0;
}

// the DNS and IPv4 entries of the Subject Alternative Names (raw entries:
// context-specific tag 2 = dNSName, 7 = iPAddress)
void certNamesFrom(const mbedtls_x509_crt& crt, HttpsCertNames& names)
{
    for (const mbedtls_x509_sequence* s = &crt.subject_alt_names; s && s->buf.p; s = s->next) {
        const int type = s->buf.tag & MBEDTLS_ASN1_TAG_VALUE_MASK;
        if (type == MBEDTLS_X509_SAN_DNS_NAME && names.dnsCount < HTTPS_CERT_NAMES_MAX) {
            const size_t n = s->buf.len < sizeof(names.dns[0]) ? s->buf.len : sizeof(names.dns[0]) - 1;
            memcpy(names.dns[names.dnsCount], s->buf.p, n);
            names.dns[names.dnsCount][n] = '\0';
            names.dnsCount++;
        } else if (type == MBEDTLS_X509_SAN_IP_ADDRESS && s->buf.len == 4 && names.ipv4Count < HTTPS_CERT_NAMES_MAX) {
            memcpy(names.ipv4[names.ipv4Count++], s->buf.p, 4);
        }
    }
}

void certInfoFrom(const mbedtls_x509_crt& crt, HttpsCertInfo& info)
{
    char dn[160];
    if (mbedtls_x509_dn_gets(dn, sizeof(dn), &crt.subject) > 0) {
        const char* cn = strstr(dn, "CN=");
        const char* v = cn ? cn + 3 : dn;
        size_t n = cn ? strcspn(v, ",") : strlen(v);
        if (n >= sizeof(info.subject)) n = sizeof(info.subject) - 1;
        memcpy(info.subject, v, n);
        info.subject[n] = '\0';
    }
    snprintf(info.notAfter, sizeof(info.notAfter), "%04d-%02d-%02d",
             crt.valid_to.year % 10000, crt.valid_to.mon % 100, crt.valid_to.day % 100);
    // only with the clock set (NTP, GNSS ...); mbedTLS is built without
    // MBEDTLS_HAVE_TIME_DATE, so the comparison is done here (UTC, to the second)
    const time_t now = time(nullptr);
    struct tm t;
    if (now > 1704067200 && gmtime_r(&now, &t)) {
        const mbedtls_x509_time& v = crt.valid_to;
        const long long a = ((((long long)(t.tm_year + 1900) * 13 + t.tm_mon + 1) * 32 + t.tm_mday) * 24 + t.tm_hour) * 3600LL + t.tm_min * 60 + t.tm_sec;
        const long long b = ((((long long)v.year * 13 + v.mon) * 32 + v.day) * 24 + v.hour) * 3600LL + v.min * 60 + v.sec;
        info.expired = a > b;
    }
    certNamesFrom(crt, info.names);
}

portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
HttpsStatus  gStatus;
} // namespace

namespace {
// the stored blob, the migration scratch and the pair hold the private key:
// zeroed before they are freed
struct WipedFree {
    size_t len;
    void operator()(void* p) const { if (p) { httpWipe(p, len); free(p); } }
};
void freePem(HttpsPem& pem)
{
    if (pem.buf) WipedFree{ pem.certLen + pem.keyLen + 2 }(pem.buf);
    pem.buf = nullptr;
}
} // namespace

HttpsPem::~HttpsPem() { freePem(*this); }

bool httpsPemLoad(HttpsPem& out, HttpsPairSlot slot)
{
    const char* nvsKey = slotKey(slot);
    freePem(out);
    out.cert = out.key = "";
    out.certLen = out.keyLen = 0;
    Preferences prefs;
    if (!prefs.begin(NS, true, "cfgdata")) return false;
    static_assert(HTTPS_BLOB_MAX <= STORED_BLOB_READ_MAX, "https beyond the loaders' read ceiling");
    const size_t n = prefs.isKey(nvsKey) ? prefs.getBytesLength(nvsKey) : 0;
    if (!storedBlobLengthReadable(n)) { prefs.end(); return false; }   // also longer than today's: an older layout for the hook
    std::unique_ptr<uint8_t, WipedFree> b((uint8_t*)psramAlloc(n), WipedFree{ n });
    if (!b || prefs.getBytes(nvsKey, b.get(), n) != n) { prefs.end(); return false; }
    prefs.end();
    size_t len = 0;
    // the migration scratch only for an older version (none exists in 4.0.0)
    const bool older = storedVersionAction(b.get()[0], STORED_HTTPS_VERSION) == STORED_MIGRATE;
    std::unique_ptr<uint8_t, WipedFree> scratch(older ? (uint8_t*)psramAlloc(HTTPS_BLOB_MAX) : nullptr, WipedFree{ HTTPS_BLOB_MAX });
    const uint8_t* p = storedBlobLoad(SF_HTTPS, b.get(), n, STORED_HTTPS_VERSION, 0, scratch.get(), scratch ? HTTPS_BLOB_MAX : 0, &len);
    HttpsPemView v;
    if (!p || !httpsBlobParse(p, len, v)) return false;
    out.buf = (char*)psramAlloc(v.certLen + v.keyLen + 2);
    if (!out.buf) return false;
    memcpy(out.buf, v.cert, v.certLen);
    out.buf[v.certLen] = '\0';
    memcpy(out.buf + v.certLen + 1, v.key, v.keyLen);
    out.buf[v.certLen + 1 + v.keyLen] = '\0';
    out.cert = out.buf;
    out.certLen = v.certLen;
    out.key = out.buf + v.certLen + 1;
    out.keyLen = v.keyLen;
    return true;
}

bool httpsPemSave(const char* cert, size_t certLen, const char* key, size_t keyLen, HttpsPairSlot slot)
{
    const char* nvsKey = slotKey(slot);
    StorageWrite w(NS);   // storage_write.h: refused once a reset sealed the storage
    if (!w.ok()) return false;
    Preferences& prefs = w.prefs();
    bool ok;
    if (!certLen && !keyLen) {
        ok = !prefs.isKey(nvsKey) || prefs.remove(nvsKey);
    } else {
        const size_t cap = 1 + 4 + certLen + keyLen;
        std::unique_ptr<uint8_t, void (*)(void*)> b((uint8_t*)psramAlloc(cap), free);
        const size_t n = b ? httpsBlobEncode(cert, certLen, key, keyLen, b.get(), cap) : 0;
        ok = n && prefs.putBytes(nvsKey, b.get(), n) == n;
    }
    if (!ok) dbgSerial.println("HTTPS: FAILED to store the certificate / key (NVS full or error?)");
    return ok;
}

HttpsVerify httpsVerifyPair(const char* cert, size_t certLen, const char* key, size_t keyLen,
                            HttpsCertInfo* info, int* mbedCode)
{
    // the mbedTLS contexts are ~1 KB: on the heap (PSRAM), not the handler's stack
    struct Ctx { mbedtls_x509_crt crt; mbedtls_pk_context pk; };
    std::unique_ptr<Ctx, void (*)(void*)> c((Ctx*)psramAlloc(sizeof(Ctx)), free);
    if (!c) return HV_NO_MEMORY;
    mbedtls_x509_crt_init(&c->crt);
    mbedtls_pk_init(&c->pk);
    HttpsVerify r = HV_OK;
    // PEM: the length includes the terminating 0
    int e = mbedtls_x509_crt_parse(&c->crt, (const unsigned char*)cert, certLen + 1);
    if (e != 0) r = HV_CERT;   // > 0: some certificates of the chain did not parse
    if (r == HV_OK && (e = mbedtls_pk_parse_key(&c->pk, (const unsigned char*)key, keyLen + 1, nullptr, 0, fillRandom, nullptr)) != 0)
        r = HV_KEY;
    if (r == HV_OK && (e = mbedtls_pk_check_pair(&c->crt.pk, &c->pk, fillRandom, nullptr)) != 0)
        r = HV_MISMATCH;
    if (r == HV_OK && info) {
        *info = HttpsCertInfo();
        certInfoFrom(c->crt, *info);
        snprintf(info->keyType, sizeof(info->keyType), "%s %u", mbedtls_pk_get_name(&c->pk), (unsigned)mbedtls_pk_get_bitlen(&c->pk));
    }
    if (mbedCode) *mbedCode = e;
    mbedtls_pk_free(&c->pk);
    mbedtls_x509_crt_free(&c->crt);
    return r;
}

bool httpsCertInfo(const char* cert, size_t certLen, HttpsCertInfo& info)
{
    info = HttpsCertInfo();
    std::unique_ptr<mbedtls_x509_crt, void (*)(void*)> crt((mbedtls_x509_crt*)psramAlloc(sizeof(mbedtls_x509_crt)), free);
    if (!crt || !certLen) return false;
    mbedtls_x509_crt_init(crt.get());
    const bool ok = mbedtls_x509_crt_parse(crt.get(), (const unsigned char*)cert, certLen + 1) == 0;
    if (ok) {
        certInfoFrom(*crt, info);
        snprintf(info.keyType, sizeof(info.keyType), "%s %u", mbedtls_pk_get_name(&crt->pk), (unsigned)mbedtls_pk_get_bitlen(&crt->pk));
    }
    mbedtls_x509_crt_free(crt.get());
    return ok;
}

int httpsGenerate(const char* subject, const char* dnsName, const uint8_t ipv4[4], HttpsPem& out)
{
    // contexts and the PEM output on the heap (PSRAM): the caller's stack is
    // the web task's 8 KB; mbedTLS allocates its own numbers in the PSRAM too
    // (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC)
    constexpr size_t CERT_CAP = 1024, KEY_CAP = 512;   // EC P-256: ~700 B certificate, ~230 B key
    struct Ctx {
        mbedtls_pk_context pk;
        mbedtls_x509write_cert crt;
        unsigned char certPem[CERT_CAP];
        unsigned char keyPem[KEY_CAP];
    };
    std::unique_ptr<Ctx, WipedFree> c((Ctx*)psramAlloc(sizeof(Ctx)), WipedFree{ sizeof(Ctx) });
    if (!c) return -1;
    mbedtls_pk_init(&c->pk);
    mbedtls_x509write_crt_init(&c->crt);
    char notBefore[15], notAfter[15];
    httpsGeneratedValidity((int64_t)time(nullptr), notBefore, notAfter);
    uint8_t serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7F;   // a positive INTEGER
    serial[0] |= 0x40;   // and no leading zero byte
    // SAN: <hostname>.local and the station address
    mbedtls_x509_san_list sanIp = {}, sanDns = {};
    sanDns.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    sanDns.node.san.unstructured_name.p = (unsigned char*)dnsName;
    sanDns.node.san.unstructured_name.len = strlen(dnsName);
    sanDns.next = &sanIp;
    sanIp.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
    sanIp.node.san.unstructured_name.p = (unsigned char*)ipv4;
    sanIp.node.san.unstructured_name.len = 4;
    // extended key usage: TLS server (Apple requires it)
    mbedtls_asn1_sequence eku = {};
    eku.buf.tag = MBEDTLS_ASN1_OID;
    eku.buf.p = (unsigned char*)MBEDTLS_OID_SERVER_AUTH;
    eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);

    int e = mbedtls_pk_setup(&c->pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (!e) e = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(c->pk), fillRandom, nullptr);
    if (!e) e = mbedtls_pk_write_key_pem(&c->pk, c->keyPem, sizeof(c->keyPem));
    if (!e) {
        mbedtls_x509write_crt_set_version(&c->crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&c->crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&c->crt, &c->pk);
        mbedtls_x509write_crt_set_issuer_key(&c->crt, &c->pk);
        e = mbedtls_x509write_crt_set_subject_name(&c->crt, subject);
    }
    if (!e) e = mbedtls_x509write_crt_set_issuer_name(&c->crt, subject);
    if (!e) e = mbedtls_x509write_crt_set_serial_raw(&c->crt, serial, sizeof(serial));
    if (!e) e = mbedtls_x509write_crt_set_validity(&c->crt, notBefore, notAfter);
    // its own CA (pathlen 0): Android installs only CA certificates as
    // trusted, iOS offers full trust only for them
    if (!e) e = mbedtls_x509write_crt_set_basic_constraints(&c->crt, 1, 0);
    if (!e) e = mbedtls_x509write_crt_set_key_usage(&c->crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_CERT_SIGN);
    if (!e) e = mbedtls_x509write_crt_set_ext_key_usage(&c->crt, &eku);
    if (!e) e = mbedtls_x509write_crt_set_subject_alternative_name(&c->crt, &sanDns);
    if (!e) e = mbedtls_x509write_crt_set_subject_key_identifier(&c->crt);
    if (!e) e = mbedtls_x509write_crt_set_authority_key_identifier(&c->crt);
    if (!e) e = mbedtls_x509write_crt_pem(&c->crt, c->certPem, sizeof(c->certPem), fillRandom, nullptr);
    mbedtls_x509write_crt_free(&c->crt);
    mbedtls_pk_free(&c->pk);
    if (e) return e;
    const size_t certLen = strlen((const char*)c->certPem), keyLen = strlen((const char*)c->keyPem);
    if (!httpsPemSave((const char*)c->certPem, certLen, (const char*)c->keyPem, keyLen, HTTPS_PAIR_GENERATED)) return -1;
    freePem(out);
    out.buf = (char*)psramAlloc(certLen + keyLen + 2);
    if (!out.buf) { out.cert = out.key = ""; out.certLen = out.keyLen = 0; return -1; }
    memcpy(out.buf, c->certPem, certLen + 1);
    memcpy(out.buf + certLen + 1, c->keyPem, keyLen + 1);
    out.cert = out.buf;
    out.certLen = certLen;
    out.key = out.buf + certLen + 1;
    out.keyLen = keyLen;
    return 0;
}

void httpsSetStatus(const HttpsStatus& s)
{
    portENTER_CRITICAL(&statusMux);
    gStatus = s;
    portEXIT_CRITICAL(&statusMux);
}

void httpsStatus(HttpsStatus& out)
{
    portENTER_CRITICAL(&statusMux);
    out = gStatus;
    portEXIT_CRITICAL(&statusMux);
}
