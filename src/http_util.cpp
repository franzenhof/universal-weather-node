#include "http_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

// ---- URL / form ------------------------------------------------------------

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool httpAcceptsGzip(const char* ae)
{
    for (const char* p = ae ? ae : ""; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char* name = p;
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') p++;
        const bool gzip = p - name == 4 && !strncasecmp(name, "gzip", 4);
        double q = 1.0;
        while (*p && *p != ',') {   // parameters: ;q=0.5
            while (*p == ' ' || *p == '\t' || *p == ';') p++;
            if ((*p == 'q' || *p == 'Q') && p[1] == '=') q = strtod(p + 2, nullptr);
            while (*p && *p != ',' && *p != ';') p++;
        }
        if (gzip) return q > 0.0;
    }
    return false;
}

size_t httpUrlDecode(char* s, size_t len, bool plusIsSpace)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '+' && plusIsSpace) c = ' ';
        else if (c == '%' && i + 2 < len) {
            const int h = hexVal(s[i + 1]), l = hexVal(s[i + 2]);
            if (h >= 0 && l >= 0) { c = (char)(h << 4 | l); i += 2; }
        }
        s[o++] = c;
    }
    if (o < len) s[o] = '\0';
    return o;
}

void httpParseForm(char* s, size_t len, HttpFormCb cb, void* ctx)
{
    size_t i = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && s[end] != '&') end++;
        size_t eq = i;
        while (eq < end && s[eq] != '=') eq++;
        char* name = s + i;
        char* value = eq < end ? s + eq + 1 : s + end;
        const size_t nlen = eq - i, vlen = eq < end ? end - eq - 1 : 0;
        // terminate in place (the '=' and '&' / end become NULs); end == len writes at s[len], which callers reserve
        name[nlen] = '\0';
        value[vlen] = '\0';
        const size_t nl = httpUrlDecode(name, nlen), vl = httpUrlDecode(value, vlen);
        name[nl] = '\0';
        value[vl] = '\0';
        if (nl) cb(name, value, ctx);
        i = end + 1;
    }
}

// ---- request arguments -----------------------------------------------------

void httpWipe(void* p, size_t len)
{
    volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
    while (len--) *b++ = 0;
}

void HttpArgs::clear()
{
    if (arena_) { httpWipe(arena_, cap_); free(arena_); }
    free(starts_);
    arena_ = nullptr;
    starts_ = nullptr;
    used_ = cap_ = count_ = startsCap_ = 0;
    fieldOpen_ = overLimit_ = false;
}

bool HttpArgs::growBytes(size_t needed)
{
    if (needed <= cap_) return true;
    if (needed > maxBytes_) return refuse();
    size_t cap = cap_ ? cap_ * 2 : 256;
    if (cap < needed) cap = needed;
    if (cap > maxBytes_) cap = maxBytes_;
    // new block + copy + wipe of the old one (realloc would free it unwiped)
    char* a = static_cast<char*>(malloc(cap));
    if (!a) return refuse();
    if (arena_) { memcpy(a, arena_, used_); httpWipe(arena_, cap_); free(arena_); }
    arena_ = a;
    cap_ = cap;
    return true;
}

bool HttpArgs::growStarts()
{
    if (count_ < startsCap_) return true;
    if (count_ >= maxCount_) return refuse();
    size_t cap = startsCap_ ? startsCap_ * 2 : 32;
    if (cap > maxCount_) cap = maxCount_;
    uint32_t* s = static_cast<uint32_t*>(realloc(starts_, cap * sizeof(uint32_t)));
    if (!s) return refuse();
    starts_ = s;
    startsCap_ = cap;
    return true;
}

bool HttpArgs::beginField(const char* name)
{
    if (overLimit_) return false;
    fieldOpen_ = false;
    const size_t n = strlen(name) + 1;
    if (count_ >= maxCount_ || !growBytes(used_ + n + 1)) return refuse();   // + the value's terminator
    openField_ = used_;
    memcpy(arena_ + used_, name, n);
    used_ += n;
    fieldOpen_ = true;
    return true;
}

bool HttpArgs::appendField(const void* data, size_t len)
{
    if (overLimit_ || !fieldOpen_) return false;
    if (!growBytes(used_ + len + 1)) return false;
    memcpy(arena_ + used_, data, len);
    used_ += len;
    return true;
}

bool HttpArgs::endField()
{
    if (overLimit_ || !fieldOpen_) return false;
    if (!growBytes(used_ + 1) || !growStarts()) return false;
    arena_[used_++] = '\0';
    starts_[count_++] = (uint32_t)openField_;
    fieldOpen_ = false;
    return true;
}

bool HttpArgs::add(const char* name, const char* value)
{
    return beginField(name) && appendField(value, strlen(value)) && endField();
}

const char* HttpArgs::value(size_t i) const
{
    const char* n = name(i);
    return n ? n + strlen(n) + 1 : nullptr;
}

const char* HttpArgs::find(const char* name) const
{
    for (size_t i = 0; i < count_; i++)
        if (!strcmp(arena_ + starts_[i], name)) return value(i);
    return nullptr;
}

// ---- MD5 (RFC 1321), compact ----------------------------------------------

namespace {
struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t bytes = 0;
    uint8_t buf[64];
    size_t fill = 0;

    static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
    void block(const uint8_t* p)
    {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
        static const uint8_t S[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                       5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
        uint32_t m[16];
        for (int i = 0; i < 16; i++)
            m[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; i++) {
            uint32_t F;
            int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3 * i + 5) & 15; }
            else             { F = C ^ (B | ~D);       g = (7 * i) & 15; }
            const uint32_t t = D;
            D = C;
            C = B;
            B = B + rol(A + F + K[i] + m[g], S[i]);
            A = t;
        }
        a += A; b += B; c += C; d += D;
    }
    void update(const uint8_t* p, size_t n)
    {
        bytes += n;
        while (n) {
            const size_t k = (64 - fill) < n ? (64 - fill) : n;
            memcpy(buf + fill, p, k);
            fill += k; p += k; n -= k;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }
    void final(uint8_t out[16])
    {
        const uint64_t bits = bytes * 8;
        const uint8_t pad = 0x80, zero = 0;
        update(&pad, 1);
        while (fill != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (8 * i));
        update(len, 8);
        const uint32_t v[4] = { a, b, c, d };
        for (int i = 0; i < 16; i++) out[i] = (uint8_t)(v[i / 4] >> (8 * (i % 4)));
    }
};
} // namespace

void httpMd5Hex(const void* data, size_t len, char out[33])
{
    Md5 m;
    m.update((const uint8_t*)data, len);
    uint8_t h[16];
    m.final(h);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[2 * i] = hx[h[i] >> 4]; out[2 * i + 1] = hx[h[i] & 15]; }
    out[32] = '\0';
}

// ---- Digest ----------------------------------------------------------------

namespace {
// copies the value of key (quoted or token) from a Digest header; false if missing / too long
bool digestField(const char* h, const char* key, char* out, size_t cap)
{
    const size_t kl = strlen(key);
    for (const char* p = h; *p; p++) {
        if ((p == h || p[-1] == ' ' || p[-1] == ',') && !strncasecmp(p, key, kl) && p[kl] == '=') {
            const char* v = p + kl + 1;
            size_t n = 0;
            if (*v == '"') {
                v++;
                while (v[n] && v[n] != '"') n++;
                if (!v[n]) return false;
            } else {
                while (v[n] && v[n] != ',' && v[n] != ' ') n++;
            }
            if (n >= cap) return false;
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
    }
    return false;
}
} // namespace

bool httpParseDigest(const char* header, HttpDigest& d)
{
    if (!header || strncasecmp(header, "Digest ", 7)) return false;
    const char* h = header + 7;
    d = HttpDigest();
    if (!digestField(h, "username", d.username, sizeof(d.username))) return false;
    if (!digestField(h, "realm", d.realm, sizeof(d.realm))) return false;
    if (!digestField(h, "nonce", d.nonce, sizeof(d.nonce))) return false;
    if (!digestField(h, "uri", d.uri, sizeof(d.uri))) return false;
    if (!digestField(h, "response", d.response, sizeof(d.response))) return false;
    digestField(h, "opaque", d.opaque, sizeof(d.opaque));
    if (digestField(h, "qop", d.qop, sizeof(d.qop))) {
        if (!digestField(h, "nc", d.nc, sizeof(d.nc)) || !digestField(h, "cnonce", d.cnonce, sizeof(d.cnonce))) return false;
    }
    return true;
}

namespace {
// MD5 of the strings joined with ':' (no copy: the URI may be 1 KB)
void md5Joined(const char* const* parts, size_t count, char out[33])
{
    Md5 m;
    for (size_t i = 0; i < count; i++) {
        if (i) m.update((const uint8_t*)":", 1);
        m.update((const uint8_t*)parts[i], strlen(parts[i]));
    }
    uint8_t h[16];
    m.final(h);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[2 * i] = hx[h[i] >> 4]; out[2 * i + 1] = hx[h[i] & 15]; }
    out[32] = '\0';
}
} // namespace

void httpDigestResponse(const HttpDigest& d, const char* method, const char* password, char out[33])
{
    char ha1[33], ha2[33];
    const char* a1[] = { d.username, d.realm, password };
    md5Joined(a1, 3, ha1);
    const char* a2[] = { method, d.uri };
    md5Joined(a2, 2, ha2);
    if (d.qop[0]) {
        const char* r[] = { ha1, d.nonce, d.nc, d.cnonce, d.qop, ha2 };
        md5Joined(r, 6, out);
    } else {
        const char* r[] = { ha1, d.nonce, ha2 };
        md5Joined(r, 3, out);
    }
    httpWipe(ha1, sizeof(ha1));
}

bool httpParseNonceCount(const char* hex, uint32_t& out)
{
    const size_t n = strlen(hex);
    if (n < 1 || n > 8) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        const int h = hexVal(hex[i]);
        if (h < 0) return false;
        v = v << 4 | (uint32_t)h;
    }
    out = v;
    return v != 0;
}

bool httpNonceCountAccept(HttpNonceCount& c, uint32_t nc)
{
    if (nc == 0) return false;
    if (nc > c.highest) {
        const uint32_t shift = nc - c.highest;
        uint32_t seen = shift >= 32 ? 0 : c.seenBelow << shift;
        if (c.highest && shift <= 32) seen |= 1UL << (shift - 1);   // the old highest is now below
        c.seenBelow = seen;
        c.highest = nc;
        return true;
    }
    const uint32_t below = c.highest - nc;   // 0 = the highest itself
    if (below == 0 || below > 32) return false;
    const uint32_t bit = 1UL << (below - 1);
    if (c.seenBelow & bit) return false;
    c.seenBelow |= bit;
    return true;
}

// ---- login brake -------------------------------------------------------------

namespace {
uint32_t brakeWait(uint8_t fails)
{
    if (fails < HttpLoginBrake::FREE_FAILS) return 0;
    const unsigned doublings = fails - HttpLoginBrake::FREE_FAILS;
    const uint32_t w = doublings >= 6 ? HttpLoginBrake::MAX_WAIT_MS : 1000UL << doublings;
    return w < HttpLoginBrake::MAX_WAIT_MS ? w : HttpLoginBrake::MAX_WAIT_MS;
}
} // namespace

const HttpLoginBrake::Client* HttpLoginBrake::findLive(const uint8_t addr[16], uint32_t nowMs) const
{
    for (const Client& c : clients_)
        if (c.fails && nowMs - c.lastFailMs < FORGET_MS && !memcmp(c.addr, addr, 16)) return &c;
    return nullptr;
}

uint32_t HttpLoginBrake::waitMs(const uint8_t addr[16], uint32_t nowMs) const
{
    const Client* c = findLive(addr, nowMs);
    if (!c) return 0;
    const uint32_t since = nowMs - c->lastFailMs, wait = brakeWait(c->fails);
    return since < wait ? wait - since : 0;
}

void HttpLoginBrake::failed(const uint8_t addr[16], uint32_t nowMs)
{
    Client* c = const_cast<Client*>(findLive(addr, nowMs));
    if (!c) {   // a free (or forgotten) slot, else the one with the oldest wrong login
        for (Client& x : clients_)
            if (!x.fails || nowMs - x.lastFailMs >= FORGET_MS) { c = &x; break; }
        if (!c) {
            c = &clients_[0];
            for (Client& x : clients_) if (nowMs - x.lastFailMs > nowMs - c->lastFailMs) c = &x;
        }
        memcpy(c->addr, addr, 16);
        c->fails = 0;
    }
    if (c->fails < 255) c->fails++;
    c->lastFailMs = nowMs;
}

void HttpLoginBrake::succeeded(const uint8_t addr[16])
{
    for (Client& c : clients_)
        if (c.fails && !memcmp(c.addr, addr, 16)) c.fails = 0;
}

// ---- multipart/form-data ---------------------------------------------------

bool HttpMultipart::boundaryOf(const char* ct, char* out, size_t len)
{
    if (!ct || strncasecmp(ct, "multipart/form-data", 19)) return false;
    const char* b = strcasestr(ct, "boundary=");
    if (!b) return false;
    b += 9;
    if (*b == '"') b++;
    size_t n = 0;
    while (b[n] && b[n] != '"' && b[n] != ';' && b[n] != ' ') n++;
    if (!n || n >= len) return false;
    memcpy(out, b, n);
    out[n] = '\0';
    return true;
}

bool HttpMultipart::begin(const char* contentType, const Callbacks& cb)
{
    char b[72];
    if (!boundaryOf(contentType, b, sizeof(b))) { state_ = ERROR; return false; }
    snprintf(delim_, sizeof(delim_), "\r\n--%s", b);
    delimLen_ = strlen(delim_);
    cb_ = cb;
    // the body starts with "--boundary" (no CRLF before it): pretend one
    buf_[0] = '\r';
    buf_[1] = '\n';
    fill_ = 2;
    state_ = PREAMBLE;
    inPart_ = false;
    return true;
}

void HttpMultipart::flushData(size_t keep)
{
    // hand over everything but the last `keep` bytes (a delimiter may start there)
    if (fill_ <= keep) return;
    const size_t n = fill_ - keep;
    if (state_ == DATA && cb_.partData && n) cb_.partData(buf_, n, cb_.ctx);
    memmove(buf_, buf_ + n, keep);
    fill_ = keep;
}

bool HttpMultipart::headerLine()
{
    // buf_[0..fill_-2) = one header line without CRLF
    buf_[fill_ - 2] = '\0';
    const char* l = (const char*)buf_;
    if (!strncasecmp(l, "Content-Disposition:", 20)) {
        // key="value" as a parameter of its own (after ';' or white space,
        // so name= is not found inside filename=)
        auto field = [&](const char* key, char* out, size_t cap) {
            const char* p = l + 20;
            while ((p = strcasestr(p, key)) && p[-1] != ';' && p[-1] != ' ' && p[-1] != '\t') p++;
            out[0] = '\0';
            if (!p) return;
            p += strlen(key);
            size_t n = 0;
            while (p[n] && p[n] != '"' && n < cap - 1) { out[n] = p[n]; n++; }
            out[n] = '\0';
        };
        field("name=\"", name_, sizeof(name_));
        field("filename=\"", filename_, sizeof(filename_));
    } else if (!strncasecmp(l, "Content-Type:", 13)) {
        const char* p = l + 13;
        while (*p == ' ') p++;
        strncpy(type_, p, sizeof(type_) - 1);
        type_[sizeof(type_) - 1] = '\0';
    }
    return true;
}

bool HttpMultipart::feed(const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (state_ == ERROR) return false;
        if (state_ == DONE) return true;   // epilogue ignored
        if (fill_ >= sizeof(buf_)) {
            if (state_ != DATA && state_ != PREAMBLE) { state_ = ERROR; return false; }   // header line too long
            flushData(delimLen_);
        }
        buf_[fill_++] = data[i];
        switch (state_) {
            case PREAMBLE:
            case DATA:
                if (fill_ >= delimLen_ && !memcmp(buf_ + fill_ - delimLen_, delim_, delimLen_)) {
                    fill_ -= delimLen_;
                    flushData(0);   // the part's last data (PREAMBLE: dropped)
                    if (inPart_ && cb_.partEnd) cb_.partEnd(cb_.ctx);
                    inPart_ = false;
                    fill_ = 0;
                    state_ = AFTER_DELIM;
                }
                break;
            case AFTER_DELIM:   // "--" = the closing delimiter, CRLF = a part follows
                if (fill_ < 2) break;
                if (buf_[0] == '-' && buf_[1] == '-') state_ = DONE;
                else if (buf_[0] == '\r' && buf_[1] == '\n') { state_ = HEADERS; name_[0] = filename_[0] = type_[0] = '\0'; }
                else state_ = ERROR;
                fill_ = 0;
                break;
            case HEADERS:
                if (fill_ >= 2 && buf_[fill_ - 2] == '\r' && buf_[fill_ - 1] == '\n') {
                    if (fill_ == 2) {   // empty line: the data begins
                        inPart_ = true;
                        if (cb_.partBegin) cb_.partBegin(name_, filename_, type_, cb_.ctx);
                        state_ = DATA;
                    } else {
                        headerLine();
                    }
                    fill_ = 0;
                }
                break;
            default:
                break;
        }
    }
    return state_ != ERROR;
}
