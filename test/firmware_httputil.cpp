// HTTP server helpers (src/http_util.cpp): URL / form decoding, MD5,
// Digest login (RFC 2617 example, long URIs, nonce-count replay guard),
// streaming multipart in any piece size, the bounded argument store and
// the per-client login brake.
#include "http_util.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

struct Kv { std::string k, v; };
static void collect(const char* n, const char* v, void* c) { static_cast<std::vector<Kv>*>(c)->push_back({n, v}); }

struct Part { std::string name, file, type, data; bool ended; };
static void pBegin(const char* n, const char* f, const char* t, void* c)
{ auto& v = *static_cast<std::vector<Part>*>(c); v.push_back({n, f, t, "", false}); }
static void pData(const uint8_t* d, size_t n, void* c) { static_cast<std::vector<Part>*>(c)->back().data.append((const char*)d, n); }
static void pEnd(void* c) { static_cast<std::vector<Part>*>(c)->back().ended = true; }

static std::vector<Part> multipart(const std::string& body, const char* ct, size_t piece, bool* ok = nullptr)
{
    std::vector<Part> parts;
    HttpMultipart m;
    HttpMultipart::Callbacks cb = { pBegin, pData, pEnd, &parts };
    bool good = m.begin(ct, cb);
    for (size_t o = 0; good && o < body.size(); o += piece)
        good = m.feed((const uint8_t*)body.data() + o, body.size() - o < piece ? body.size() - o : piece);
    if (ok) *ok = good && m.done();
    return parts;
}

static void addToArgs(const char* n, const char* v, void* c) { static_cast<HttpArgs*>(c)->add(n, v); }

static void testArgs()
{
    // order, lookup, empty values, the first of a repeated name
    HttpArgs a(8, 256);
    char q[] = "x=1&y=&x=2&z=%41";
    httpParseForm(q, strlen(q), addToArgs, &a);
    assert(!a.overLimit() && a.count() == 4);
    assert(!strcmp(a.name(0), "x") && !strcmp(a.value(0), "1") && !strcmp(a.value(1), "") && !strcmp(a.value(3), "A"));
    assert(!strcmp(a.find("x"), "1") && !a.find("w") && !a.name(4) && !a.value(4));
    // a field streamed in pieces (multipart)
    assert(a.beginField("f") && a.appendField("ab", 2) && a.appendField("cd", 2) && a.endField());
    assert(!strcmp(a.find("f"), "abcd") && a.count() == 5);
    a.clear();
    assert(a.count() == 0 && !a.find("x") && !a.overLimit());

    // count limit: the unauthenticated "a&a&a&..." body of 128 KiB
    std::string flood;
    for (int i = 0; i < 65536; i++) flood += "a&";
    HttpArgs counted(2048, 128 * 1024 + 4096);
    std::vector<char> body(flood.begin(), flood.end());
    body.push_back('\0');
    httpParseForm(body.data(), flood.size(), addToArgs, &counted);
    assert(counted.overLimit() && counted.count() == 2048);
    assert(!counted.add("late", "1") && counted.count() == 2048);   // stays refused

    // byte limit: few, but large values
    HttpArgs sized(2048, 1000);
    std::string big(600, 'v');
    assert(sized.add("one", big.c_str()));
    assert(!sized.add("two", big.c_str()) && sized.overLimit() && sized.count() == 1);
    // a streamed field over the byte limit
    HttpArgs streamed(2048, 1000);
    assert(streamed.beginField("file"));
    bool fits = true;
    for (int i = 0; i < 100 && fits; i++) fits = streamed.appendField(big.data(), 50);
    assert(!fits && streamed.overLimit() && !streamed.endField() && streamed.count() == 0);
    // appendField / endField without beginField
    HttpArgs bare(4, 100);
    assert(!bare.appendField("x", 1) && !bare.endField());
}

static void testDigestLongUri()
{
    // a protected URL as long as the server takes it (1024)
    std::string uri = "/api/settings?" + std::string(1000, 'q');
    std::string hdr = "Digest username=\"admin\", realm=\"r\", nonce=\"n1\", uri=\"" + uri +
                      "\", qop=auth, nc=00000002, cnonce=\"c\", response=\"00\"";
    HttpDigest d;
    assert(httpParseDigest(hdr.c_str(), d) && uri == d.uri);
    char ha1[33], ha2[33], expect[33], got[33];
    std::string a1 = "admin:r:pw", a2 = "POST:" + uri;
    httpMd5Hex(a1.data(), a1.size(), ha1);
    httpMd5Hex(a2.data(), a2.size(), ha2);
    std::string r = std::string(ha1) + ":n1:00000002:c:auth:" + ha2;
    httpMd5Hex(r.data(), r.size(), expect);
    httpDigestResponse(d, "POST", "pw", got);
    assert(!strcmp(expect, got));
    // longer than the server's URI limit: refused, not cut
    std::string tooLong = "Digest username=\"a\", realm=\"r\", nonce=\"n\", uri=\"/" + std::string(1100, 'q') + "\", response=\"0\"";
    assert(!httpParseDigest(tooLong.c_str(), d));
}

static void testNonceCount()
{
    uint32_t v;
    assert(httpParseNonceCount("00000001", v) && v == 1);
    assert(httpParseNonceCount("0000001a", v) && v == 26);
    assert(!httpParseNonceCount("00000000", v) && !httpParseNonceCount("", v) && !httpParseNonceCount("123456789", v) && !httpParseNonceCount("0000000g", v));

    HttpNonceCount c;
    assert(httpNonceCountAccept(c, 1) && !httpNonceCountAccept(c, 1));   // a replay of the same request
    assert(httpNonceCountAccept(c, 2) && httpNonceCountAccept(c, 4));
    assert(httpNonceCountAccept(c, 3));                                  // parallel requests out of order
    assert(!httpNonceCountAccept(c, 3) && !httpNonceCountAccept(c, 2) && !httpNonceCountAccept(c, 4));
    assert(!httpNonceCountAccept(c, 0));
    assert(httpNonceCountAccept(c, 40));      // a jump: the window moves
    assert(!httpNonceCountAccept(c, 4));      // 36 below: too old
    assert(httpNonceCountAccept(c, 8) && !httpNonceCountAccept(c, 8));   // 32 below, new: still taken once
    assert(httpNonceCountAccept(c, 39) && !httpNonceCountAccept(c, 39));
    HttpNonceCount far;
    assert(httpNonceCountAccept(far, 5) && httpNonceCountAccept(far, 37));   // exactly 32 ahead: 5 is the oldest in the window
    assert(!httpNonceCountAccept(far, 5) && httpNonceCountAccept(far, 6));
}

static void testLoginBrake()
{
    HttpLoginBrake b;
    uint8_t a[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 172, 24, 10, 5};
    uint8_t other[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 172, 24, 10, 6};
    uint32_t t = 1000;
    for (int i = 0; i < HttpLoginBrake::FREE_FAILS; i++) { assert(b.waitMs(a, t) == 0); b.failed(a, t); t += 10; }
    t -= 10;
    // the 6th try: 1 s after the 5th wrong one; nobody else waits
    assert(b.waitMs(a, t) == 1000 && b.waitMs(a, t + 400) == 600 && b.waitMs(a, t + 1000) == 0);
    assert(b.waitMs(other, t) == 0);
    // doubling up to a minute
    uint32_t expect = 2000;
    for (int i = 0; i < 10; i++) {
        t += 60000;
        b.failed(a, t);
        assert(b.waitMs(a, t) == (expect < HttpLoginBrake::MAX_WAIT_MS ? expect : HttpLoginBrake::MAX_WAIT_MS));
        expect *= 2;
    }
    // the right password clears it
    b.succeeded(a);
    assert(b.waitMs(a, t) == 0);
    // forgotten FORGET_MS after the last wrong login
    for (int i = 0; i < 6; i++) b.failed(other, t);
    assert(b.waitMs(other, t) > 0 && b.waitMs(other, t + HttpLoginBrake::FORGET_MS) == 0);
    // more clients than slots: the one with the oldest wrong login goes, the newest stay
    HttpLoginBrake many;
    uint8_t c[16] = {};
    for (int k = 0; k <= HttpLoginBrake::CLIENTS; k++) {
        c[15] = (uint8_t)k;
        for (int i = 0; i < 6; i++) many.failed(c, 5000 + k);
    }
    c[15] = 0;
    assert(many.waitMs(c, 5100) == 0);
    c[15] = HttpLoginBrake::CLIENTS;
    assert(many.waitMs(c, 5100) > 0);
    // millis() wrap
    HttpLoginBrake wrap;
    for (int i = 0; i < 6; i++) wrap.failed(a, 0xFFFFFF00u);
    assert(wrap.waitMs(a, 0x00000100u) == 2000 - 0x200);   // 6 wrong: 2 s
}

int main()
{
    // URL decoding
    char a[] = "a%20b+c%2Bd%zz%4";
    size_t n = httpUrlDecode(a, strlen(a));
    assert(std::string(a, n) == "a b c+d%zz%4");
    char p[] = "x+y%2F";
    assert(std::string(p, httpUrlDecode(p, strlen(p), false)) == "x+y/");

    // form body
    char form[64] = "devname=G%C3%A4rten+1&empty=&flag&x=%3D%26";
    std::vector<Kv> kv;
    httpParseForm(form, strlen(form), collect, &kv);
    assert(kv.size() == 4);
    assert(kv[0].k == "devname" && kv[0].v == "G\xC3\xA4rten 1");
    assert(kv[1].k == "empty" && kv[1].v.empty() && kv[2].k == "flag" && kv[2].v.empty());
    assert(kv[3].k == "x" && kv[3].v == "=&");

    // MD5
    char h[33];
    httpMd5Hex("", 0, h);    assert(!strcmp(h, "d41d8cd98f00b204e9800998ecf8427e"));
    httpMd5Hex("abc", 3, h); assert(!strcmp(h, "900150983cd24fb0d6963f7d28e17f72"));
    std::string longer(1000, 'a');
    httpMd5Hex(longer.data(), longer.size(), h); assert(!strcmp(h, "cabe45dcc9ae5b66ba86600cca6b8ba8"));

    // Digest: the RFC 2617 example (user Mufasa, password "Circle Of Life")
    const char* hdr = "Digest username=\"Mufasa\", realm=\"testrealm@host.com\", nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", "
                      "uri=\"/dir/index.html\", qop=auth, nc=00000001, cnonce=\"0a4f113b\", "
                      "response=\"6629fae49393a05397450978507c4ef1\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"";
    HttpDigest d;
    assert(httpParseDigest(hdr, d));
    assert(!strcmp(d.username, "Mufasa") && !strcmp(d.nc, "00000001") && !strcmp(d.qop, "auth") && !strcmp(d.uri, "/dir/index.html"));
    char resp[33];
    httpDigestResponse(d, "GET", "Circle Of Life", resp);
    assert(!strcmp(resp, d.response));
    httpDigestResponse(d, "GET", "wrong", resp);
    assert(strcmp(resp, d.response));
    assert(!httpParseDigest("Basic YWRtaW46eA==", d));
    assert(!httpParseDigest("Digest username=\"a\", realm=\"r\"", d));   // fields missing

    // multipart: a field and a file, in every piece size
    const char* ct = "multipart/form-data; boundary=----WebKitFormBoundaryX7";
    std::string file;
    for (int i = 0; i < 3000; i++) file += (char)(i * 7);   // binary, includes CR / LF / '-'
    file += "\r\n------WebKitFormBoundaryX";               // almost a delimiter
    std::string body = "------WebKitFormBoundaryX7\r\nContent-Disposition: form-data; name=\"form\"\r\n\r\n1\r\n"
                       "------WebKitFormBoundaryX7\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"uwn.bin\"\r\n"
                       "Content-Type: application/octet-stream\r\n\r\n" + file +
                       "\r\n------WebKitFormBoundaryX7--\r\n";
    for (size_t piece : { (size_t)1, (size_t)7, (size_t)64, (size_t)511, (size_t)4096 }) {
        bool ok;
        auto parts = multipart(body, ct, piece, &ok);
        assert(ok && parts.size() == 2);
        assert(parts[0].name == "form" && parts[0].file.empty() && parts[0].data == "1" && parts[0].ended);
        assert(parts[1].name == "firmware" && parts[1].file == "uwn.bin" && parts[1].type == "application/octet-stream");
        assert(parts[1].data == file && parts[1].ended);
    }
    // boundary in quotes; malformed body
    char b[80];
    assert(HttpMultipart::boundaryOf("multipart/form-data; boundary=\"abc\"", b, sizeof(b)) && !strcmp(b, "abc"));
    assert(!HttpMultipart::boundaryOf("application/x-www-form-urlencoded", b, sizeof(b)));
    bool ok;
    multipart("--abc\r\nX", "multipart/form-data; boundary=abc", 3, &ok);
    assert(!ok);   // never closed
    // name= without a space after ';' (RFC 7578 allows it), not found inside filename=
    auto tight = multipart("--abc\r\nContent-Disposition: form-data;filename=\"f.bin\";name=\"fw\"\r\n\r\nx\r\n--abc--",
                           "multipart/form-data; boundary=abc", 5, &ok);
    assert(ok && tight.size() == 1 && tight[0].name == "fw" && tight[0].file == "f.bin");

    // Accept-Encoding: gzip with q > 0 only
    assert(httpAcceptsGzip("gzip") && httpAcceptsGzip("gzip, deflate, br") && httpAcceptsGzip("br;q=1.0, GZip;q=0.5"));
    assert(httpAcceptsGzip("deflate,gzip") && httpAcceptsGzip("gzip ; q=0.001"));
    assert(!httpAcceptsGzip("gzip;q=0") && !httpAcceptsGzip("gzip; q=0.000") && !httpAcceptsGzip("deflate, br"));
    assert(!httpAcceptsGzip("") && !httpAcceptsGzip(nullptr) && !httpAcceptsGzip("x-gzip2") && !httpAcceptsGzip("br;q=0.5;gzip"));
    assert(!httpAcceptsGzip("identity, gzip;q=0, br"));

    testArgs();
    testDigestLongUri();
    testNonceCount();
    testLoginBrake();
    return 0;
}
