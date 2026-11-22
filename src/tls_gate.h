#pragma once
// Memory gate for HTTPS requests (notifications, https weather networks,
// custom uploads, update check), host-tested (test/firmware_tlsgate.cpp).
//
// The prebuilt mbedTLS of our Arduino-ESP32 (2.0.11) has fixed 16 KB in and
// 16 KB out record buffers (CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN 16384, no
// dynamic / variable buffers), two ~16.7 KB blocks plus 10-20 KB for the
// handshake (ECDHE, certificate parsing even with setInsecure()) and the
// HTTP client: ~45-55 KB at the peak. A field node fell to 21 KiB free while
// a Pushover test ran next to a settings save. So a TLS request starts only
// with free heap >= TLS_MIN_FREE (~20 KiB left after the peak) and the
// largest free block >= TLS_MIN_LARGEST: the 16 KB record buffers are two
// separate ~16.7 KB allocations, everything else in the handshake comes in
// small pieces - a 24 KiB block is enough for them (a larger threshold would
// block needlessly on a node whose heap is merely fragmented, e.g. a 60 KiB
// largest block after days). Not while the web task renders a heavy page
// (settings, status, wizard, backup / restore, OTA) - it waits a few seconds.
// The web task in turn answers 503 for heavy pages while a TLS request runs
// and the heap is low. Plain HTTP and plain MQTT are not affected.
#include <stddef.h>
#include <stdint.h>

// With PSRAM (V4, psram.h) the two record buffers (calloc of > 4 KB) come
// from the PSRAM; internal RAM then only needs the small handshake pieces,
// Wi-Fi and lwIP: TLS_MIN_FREE_PSRAM. All figures are internal RAM unless
// named psram.
constexpr size_t   TLS_MIN_LARGEST   = 24 * 1024;
constexpr size_t   TLS_MIN_FREE      = 70 * 1024;
constexpr size_t   TLS_MIN_FREE_PSRAM = 32 * 1024;
constexpr size_t   WEB_LOW_HEAP      = 50 * 1024;   // heavy page refused (503) below this while TLS runs
constexpr uint32_t WEB_BUSY_MAX_MS   = 20000;       // a forgotten busy mark expires (no deadlock)

enum TlsDecision : uint8_t { TLS_GO = 0, TLS_WAIT_HEAP, TLS_WAIT_WEB };

TlsDecision tlsDecide(size_t internalFree, size_t internalLargest, size_t psramLargest, bool webBusy);
bool webShouldRefuse(bool tlsRunning, size_t freeHeap);

// ---- shared state (any task) ----
void webHeavyBegin(uint32_t nowMs);
void webHeavyEnd();
bool webHeavyActive(uint32_t nowMs);
void tlsBegin();
void tlsEnd();
bool tlsRunning();

// device: decision with the current heap; logs the heap at debug level
TlsDecision tlsMayStart(const char* what);
void tlsLogHeap(const char* what, const char* when);   // debug mode only

// RAII mark for the heavy web handlers
struct WebHeavyGuard {
    explicit WebHeavyGuard(uint32_t nowMs) { webHeavyBegin(nowMs); }
    ~WebHeavyGuard() { webHeavyEnd(); }
};
