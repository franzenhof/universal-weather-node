#pragma once
// Pieces of the HTTP server (http_server.h) that need no network: URL
// decoding, form bodies, a streaming multipart/form-data parser and the
// Digest login (RFC 7616, MD5, qop=auth). Host-tested
// (test/firmware_httputil.cpp).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// %xx and '+' (= space in a form value) decoded in place; returns the new length.
size_t httpUrlDecode(char* s, size_t len, bool plusIsSpace = true);

// "a=1&b=x%20y" -> cb(name, value) for each pair, decoded (in place: s is
// changed; s[len] is written too, so the buffer needs len + 1 bytes).
typedef void (*HttpFormCb)(const char* name, const char* value, void* ctx);
void httpParseForm(char* s, size_t len, HttpFormCb cb, void* ctx);

// An Accept-Encoding header value lists gzip with a quality above 0
// ("gzip", "br, gzip;q=0.8"; not "gzip;q=0", not missing). Case-insensitive.
bool httpAcceptsGzip(const char* acceptEncoding);

// MD5 of data as 32 lowercase hex digits + NUL (Digest login).
void httpMd5Hex(const void* data, size_t len, char out[33]);

// Overwrites len bytes with zeros in a way the compiler keeps (a buffer
// that held a password or key, before it is freed).
void httpWipe(void* p, size_t len);

// The arguments of one request (query and form fields, in order) in one
// growing buffer: a request can neither create more than maxCount
// arguments nor store more than maxBytes (names, values, terminators).
// Over a limit, add() / the field calls return false and overLimit() stays
// true - the server answers 413. Buffers >= 4 KB come from malloc, which
// puts them in the PSRAM on the device; clear() wipes them before freeing.
class HttpArgs {
public:
    HttpArgs(size_t maxCount, size_t maxBytes) : maxCount_(maxCount), maxBytes_(maxBytes) {}
    ~HttpArgs() { clear(); }
    HttpArgs(const HttpArgs&) = delete;
    HttpArgs& operator=(const HttpArgs&) = delete;

    void clear();
    bool add(const char* name, const char* value);
    // A value streamed in pieces (multipart field): beginField(name),
    // appendField() any number of times, endField() stores it.
    bool beginField(const char* name);
    bool appendField(const void* data, size_t len);
    bool endField();

    bool overLimit() const { return overLimit_; }
    size_t count() const { return count_; }
    const char* name(size_t i) const { return i < count_ ? arena_ + starts_[i] : nullptr; }
    const char* value(size_t i) const;
    const char* find(const char* name) const;   // value of the first argument of that name, nullptr if none

private:
    bool growBytes(size_t needed);
    bool growStarts();
    bool refuse() { overLimit_ = true; return false; }
    const size_t maxCount_, maxBytes_;
    char* arena_ = nullptr;
    size_t used_ = 0, cap_ = 0;
    uint32_t* starts_ = nullptr;   // offset of each argument's name; its value follows the name's terminator
    size_t count_ = 0, startsCap_ = 0;
    size_t openField_ = 0;         // offset of the name of a field begun, not yet ended
    bool fieldOpen_ = false, overLimit_ = false;
};

// Digest login: the fields of an "Authorization: Digest ..." header. uri
// takes what the server takes (CONFIG_HTTPD_MAX_URI_LEN 1024).
struct HttpDigest {
    char username[33] = "", realm[49] = "", nonce[49] = "", uri[1025] = "", response[33] = "";
    char qop[9] = "", nc[9] = "", cnonce[65] = "", opaque[49] = "";
};
// false = not a Digest header or a field too long / missing.
bool httpParseDigest(const char* header, HttpDigest& d);
// The response a client with this password would send (lowercase hex).
void httpDigestResponse(const HttpDigest& d, const char* method, const char* password, char out[33]);

// Replay guard for one nonce: every nonce count (Digest "nc", 8 hex digits)
// is accepted once. Browsers send parallel requests with increasing counts
// that may arrive out of order, so counts up to 32 below the highest one
// seen are still taken if new.
struct HttpNonceCount {
    uint32_t highest = 0;
    uint32_t seenBelow = 0;   // bit i: count highest - 1 - i was used
};
bool httpParseNonceCount(const char* hex, uint32_t& out);   // false unless 1-8 hex digits, not 0
bool httpNonceCountAccept(HttpNonceCount& c, uint32_t nc);   // false = used before / too old: a replay

// Brake on guessing the admin password, per client address (IPv6 or
// IPv4-mapped, 16 bytes): the first FREE_FAILS wrong logins cost nothing,
// then each further try must wait 1 s, 2 s, 4 s ... up to MAX_WAIT_MS after
// the last wrong one. The server answers a try that comes too early with
// 429 without checking it (and without sleeping). A client is forgotten
// FORGET_MS after its last wrong login, or for a newer one once CLIENTS
// addresses are tracked (the oldest goes).
class HttpLoginBrake {
public:
    static constexpr int      CLIENTS     = 8;
    static constexpr uint8_t  FREE_FAILS  = 5;
    static constexpr uint32_t MAX_WAIT_MS = 60000;
    static constexpr uint32_t FORGET_MS   = 15UL * 60000UL;
    uint32_t waitMs(const uint8_t addr[16], uint32_t nowMs) const;   // 0 = may try now
    void failed(const uint8_t addr[16], uint32_t nowMs);
    void succeeded(const uint8_t addr[16]);

private:
    struct Client { uint8_t addr[16]; uint8_t fails; uint32_t lastFailMs; };
    Client clients_[CLIENTS] = {};
    const Client* findLive(const uint8_t addr[16], uint32_t nowMs) const;
};

// Streaming multipart/form-data parser: feed() the body in pieces of any
// size; the callbacks get each part's headers once, then its data (in
// pieces), then its end. Small fields and file uploads alike.
class HttpMultipart {
public:
    struct Callbacks {
        void (*partBegin)(const char* name, const char* filename, const char* contentType, void* ctx);
        void (*partData)(const uint8_t* data, size_t len, void* ctx);
        void (*partEnd)(void* ctx);
        void* ctx;
    };
    // boundary: the value from the Content-Type header (without "--").
    bool begin(const char* contentType, const Callbacks& cb);
    // false once the body is malformed (stop feeding).
    bool feed(const uint8_t* data, size_t len);
    // true if the closing boundary was seen.
    bool done() const { return state_ == DONE; }
    static bool boundaryOf(const char* contentType, char* out, size_t len);

private:
    enum State : uint8_t { PREAMBLE, AFTER_DELIM, HEADERS, DATA, DONE, ERROR };
    void flushData(size_t keep);
    bool headerLine();
    Callbacks cb_ = {};
    char delim_[80] = "";      // "\r\n--" + boundary
    size_t delimLen_ = 0;
    State state_ = ERROR;
    uint8_t buf_[512];         // look-ahead for the delimiter / one header line
    size_t fill_ = 0;
    char name_[64] = "", filename_[128] = "", type_[64] = "";
    bool inPart_ = false;
};
