// Loxone UDP output - network part (see loxone.h). The lines come from
// loxoneLines() (loxone_lines.cpp); here: host resolution and sending.
//
// Never blocks the main loop for long:
//   - an IP address as host is used directly; a host name is resolved with
//     lwIP's asynchronous dns_gethostbyname(), posted from loxonePoll() to
//     the lwIP thread with tcpip_callback() (no waiting; Arduino 2.0.17 has no
//     core lock for calling the raw API from the loop task),
//     the result is cached and refreshed every 10 min (the old address stays
//     in use meanwhile); a lookup without answer is given up after 5 s.
//   - WiFiUDP on a non-blocking socket: one sendto() per value (~0.1 ms);
//     a failed datagram (lwIP/Wi-Fi buffers full) is retried once after 2 ms,
//     at most 4 retries per set, and the set is cut off after 50 ms.
#include "loxone.h"
#include "out_status.h"
#include "globals.h"
#include "json_payload.h"
#include "serial_log.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <new>

namespace {

SemaphoreHandle_t loxMutex = nullptr;
LoxoneStatus      st;
WiFiUDP           udp;

constexpr unsigned long DNS_REFRESH_MS = 600000UL;   // re-resolve a host name every 10 min
constexpr unsigned long DNS_TIMEOUT_MS = 5000UL;
constexpr unsigned long DNS_RETRY_MS   = 30000UL;    // after a failed lookup
constexpr unsigned long SEND_BUDGET_MS = 50UL;

char          curHost[sizeof(Config::loxHost)] = "";  // host the cached address belongs to
bool          dnsPending  = false;
unsigned long dnsStartMs  = 0;
unsigned long resolvedMs  = 0;
unsigned long lastTryMs   = 0;
bool          triedOnce   = false;

// written by the lwIP callback (tcpip task)
volatile uint32_t dnsGen    = 0;
volatile bool     dnsDone   = false;
volatile uint32_t dnsResult = 0;   // IPv4, network byte order; 0 = not found

void dnsFound(const char*, const ip_addr_t* ip, void* arg)
{
    if ((uint32_t)(uintptr_t)arg != dnsGen) return;   // an abandoned lookup
    dnsResult = (ip && IP_IS_V4(ip)) ? ip4_addr_get_u32(ip_2_ip4(ip)) : 0;
    dnsDone   = true;
}

// One lookup request, owned by the lwIP thread once posted (freed there).
struct DnsRequest {
    uint32_t gen;
    char     host[sizeof(Config::loxHost)];
};

void dnsLookupInLwip(void* p)   // runs in the lwIP thread (tcpip_callback)
{
    DnsRequest* r = static_cast<DnsRequest*>(p);
    ip_addr_t addr;
    void* gen = (void*)(uintptr_t)r->gen;
    err_t err = dns_gethostbyname(r->host, &addr, dnsFound, gen);
    if (err == ERR_OK) dnsFound(r->host, &addr, gen);               // from the cache
    else if (err != ERR_INPROGRESS) dnsFound(r->host, nullptr, gen);  // failed at once
    delete r;
}

void setError(const char* e)
{
    outReport(OUT_LOXONE, false, 0, e);
    st.lastError   = e;
    st.lastErrorMs = millis();
}

void copyHost(char* out, size_t len)
{
    loxoneLock();
    strlcpy(out, (const char*)gCfg.loxHost, len);
    loxoneUnlock();
}

struct SendCtx {
    IPAddress     ip;
    uint16_t      port;
    unsigned long t0;
    int           ok = 0, failed = 0, retries = 0;
    bool          cut = false;
};

void sendLine(const char* line, size_t len, void* p)
{
    SendCtx& c = *static_cast<SendCtx*>(p);
    if (c.cut) { c.failed++; return; }
    for (int attempt = 0; attempt < 2; attempt++) {
        if (udp.beginPacket(c.ip, c.port) && udp.write((const uint8_t*)line, len) == len && udp.endPacket()) {
            c.ok++;
            return;
        }
        if (attempt > 0 || c.retries >= 4) break;
        c.retries++;
        delay(2);
    }
    c.failed++;
    if (millis() - c.t0 > SEND_BUDGET_MS) c.cut = true;
}

} // namespace

void loxoneLock()   { if (loxMutex) xSemaphoreTake(loxMutex, portMAX_DELAY); }
void loxoneUnlock() { if (loxMutex) xSemaphoreGive(loxMutex); }

const LoxoneStatus& loxoneStatus() { return st; }

void loxoneSetup()
{
    if (!loxMutex) loxMutex = xSemaphoreCreateMutex();
}

void loxonePoll()
{
    if (!gCfg.loxEn || !gCfg.wifiEn) return;
    const unsigned long now = millis();
    char host[sizeof(gCfg.loxHost)];
    copyHost(host, sizeof(host));

    if (strcmp(host, curHost) != 0) {   // host changed: forget the cached address
        strlcpy(curHost, host, sizeof(curHost));
        st.ipValid = false;
        dnsPending = false;
        triedOnce  = false;
        dnsGen = dnsGen + 1;
        IPAddress lit;
        if (host[0] && lit.fromString(host)) { st.ip = lit; st.ipValid = true; resolvedMs = now; }
    }
    if (!host[0]) return;
    IPAddress lit;
    if (lit.fromString(host)) return;   // literal address: nothing to resolve

    if (dnsPending) {
        if (dnsDone) {
            dnsPending = false;
            if (dnsResult) { st.ip = IPAddress(dnsResult); st.ipValid = true; resolvedMs = now; }
            else setError("host name not found");
        } else if (now - dnsStartMs > DNS_TIMEOUT_MS) {
            dnsPending = false;
            dnsGen = dnsGen + 1;    // a late answer is ignored
            setError("DNS timeout");
        }
        return;
    }
    bool stale = !st.ipValid || now - resolvedMs > DNS_REFRESH_MS;
    if (!stale || !WiFi.isConnected()) return;
    if (triedOnce && now - lastTryMs < (st.ipValid ? DNS_REFRESH_MS / 10 : DNS_RETRY_MS)) return;
    triedOnce = true;
    lastTryMs = now;

    dnsGen = dnsGen + 1;
    dnsDone = false;
    DnsRequest* r = new (std::nothrow) DnsRequest;
    if (!r) { setError("out of memory"); return; }
    r->gen = dnsGen;
    strlcpy(r->host, host, sizeof(r->host));
    if (tcpip_callback(dnsLookupInLwip, r) != ERR_OK) {
        delete r;
        setError("DNS lookup failed");
        return;
    }
    dnsPending = true;
    dnsStartMs = now;
}

void loxoneSendTelemetry(const PathValues* pv)
{
    if (!gCfg.loxEn || !gCfg.wifiEn) return;
    if (!WiFi.isConnected()) { st.errors++; setError("WiFi not connected, set skipped"); return; }
    if (!st.ipValid) {
        st.errors++;
        setError(curHost[0] ? (dnsPending ? "host name being resolved, set skipped" : "host not resolved, set skipped")
                            : "no host set");
        return;
    }
    char prefix[LOX_PREFIX_MAX + 1];
    loxoneLock();
    strlcpy(prefix, (const char*)gCfg.loxPrefix, sizeof(prefix));
    loxoneUnlock();

    JsonDocument doc;
    buildJsonPayload(doc, pv);
    if (doc.overflowed()) { st.errors++; setError("out of memory"); return; }

    SendCtx c;
    c.ip   = st.ip;
    c.port = (uint16_t)gCfg.loxPort;
    c.t0   = millis();
    loxoneLines(doc, prefix, sendLine, &c);

    st.everSent   = true;
    st.lastSendMs = millis();
    st.lastValues = c.ok;
    st.lastFailed = c.failed;
    st.sets++;
    if (!c.failed) outReport(OUT_LOXONE, true);
    if (c.failed) {
        st.errors += c.failed;
        setError(c.cut ? "send cut off (too slow), datagrams dropped" : "datagrams not sent (network buffers full)");
        if (!c.ok) udp.stop();   // fresh socket next time
    }
    dbgSerial.printf("Loxone UDP to %s:%d: %d values sent, %d failed (%lu ms)\n",
                     st.ip.toString().c_str(), gCfg.loxPort, c.ok, c.failed, millis() - c.t0);
}
