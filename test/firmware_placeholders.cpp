// Placeholder renderer (src/placeholders.cpp): units, decimals, encodings,
// unknown / unavailable names, JSON braces left alone, buffer limits.
#include "placeholders.h"
#include <cassert>
#include <cstring>
#include <string>

static bool lookup(const char* n, PhValue& o, void*)
{
    if (!strcmp(n, "temp")) { o.kind = PK_TEMP;  o.v = 21.37f; return true; }
    if (!strcmp(n, "cold")) { o.kind = PK_TEMP;  o.v = -0.02f; return true; }
    if (!strcmp(n, "wind")) { o.kind = PK_SPEED; o.v = 5.0f;   return true; }
    if (!strcmp(n, "rain_day")) { o.kind = PK_RAIN; o.v = 12.7f; return true; }
    if (!strcmp(n, "rain_rate")) { o.kind = PK_RATE; o.v = 2.54f; return true; }
    if (!strcmp(n, "qnh"))  { o.kind = PK_PRESS; o.v = 1013.25f; return true; }
    if (!strcmp(n, "hum"))  { o.kind = PK_PLAIN; o.v = 63.4f;  return true; }
    if (!strcmp(n, "uv"))   { o.kind = PK_PLAIN; o.v = 3.25f; o.decimals = 1; return true; }
    if (!strcmp(n, "name")) { o.kind = PK_TEXT;  o.text = "Frost & \"ice\"/1"; return true; }
    return false;   // unknown, or "missing" (not available now)
}

static std::string r(const char* tpl, PhEnc enc = PE_NONE, int* empty = nullptr, size_t len = 256)
{
    char out[256];
    int e = phRender(tpl, out, len, enc, lookup, nullptr);
    if (empty) *empty = e;
    return out;
}

int main()
{
    // default metric units and decimals
    assert(r("{temp} {wind} {rain_day} {rain_rate} {qnh} {hum} {uv}") == "21.4 5.0 12.7 2.5 1013.2 63 3.2");
    // units
    assert(r("{temp:f} {temp:c}") == "70.5 21.4");
    assert(r("{wind:kmh} {wind:mph} {wind:kn} {wind:ms}") == "18.0 11.2 9.7 5.0");
    assert(r("{rain_day:in} {rain_rate:in} {qnh:inhg} {rain_day:mm}") == "0.50 0.10 29.92 12.7");
    // decimals, alone and with a unit, either order
    assert(r("{temp:.0} {temp:.3} {wind:kmh:.0} {wind:.2:mph} {qnh:inhg:.3}") == "21 21.370 18 11.18 29.921");
    assert(r("{cold}") == "0.0");   // no "-0.0"
    // unknown / unavailable: empty, counted
    int empty = 0;
    assert(r("a{nope}b{missing:f}c", PE_NONE, &empty) == "abc" && empty == 2);
    // not placeholders: JSON braces, empty braces, spaces, unterminated
    assert(r("{\"t\":{temp},\"x\":{}}") == "{\"t\":21.4,\"x\":{}}");
    assert(r("{ temp } {temp") == "{ temp } {temp");
    // encodings: only inserted values are encoded
    assert(r("https://x/?n={name}&t={temp}", PE_URL) == "https://x/?n=Frost%20%26%20%22ice%22%2F1&t=21.4");
    assert(r("{\"n\":\"{name}\"}", PE_JSON) == "{\"n\":\"Frost & \\\"ice\\\"/1\"}");
    // output always terminated, cut at the buffer
    assert(r("{temp}{temp}{temp}", PE_NONE, nullptr, 8) == "21.421.");
    char tiny[1];
    phRender("{temp}", tiny, sizeof(tiny), PE_NONE, lookup, nullptr);
    assert(tiny[0] == '\0');
    puts("placeholders ok");
    return 0;
}
