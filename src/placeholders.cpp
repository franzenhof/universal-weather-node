#include "placeholders.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
bool alnum(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// "{name}" / "{name:sfx}" with name [A-Za-z0-9_]+ and the suffix [A-Za-z0-9.:]*
// - anything else (JSON braces of a body template) is copied as text
bool validPlaceholder(const char* s, const char* close)
{
    const char* p = s + 1;
    if (p == close) return false;
    for (; p < close && *p != ':'; p++) if (!alnum((unsigned char)*p) && *p != '_') return false;
    for (; p < close; p++) if (!alnum((unsigned char)*p) && *p != '.' && *p != ':') return false;
    return true;
}

// The output buffer: a piece (one character, or its whole escape sequence)
// goes in only if it fits completely - a cut output never ends in the middle
// of a "%XX" or "\uXXXX" - and the first one that does not fit marks it cut.
struct Out {
    char* p; size_t len, n;
    bool cut;
    void put(const char* s, size_t k)
    {
        if (cut || n + k + 1 > len) { cut = true; return; }
        memcpy(p + n, s, k);
        n += k;
    }
    void put(char c) { put(&c, 1); }
};

void putEncoded(Out& o, const char* s, PhEnc enc)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s && !o.cut; s++) {
        const unsigned char c = (unsigned char)*s;
        char b[8];
        if (enc == PE_URL) {
            if (alnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.put((char)c);
            else { b[0] = '%'; b[1] = hex[c >> 4]; b[2] = hex[c & 15]; o.put(b, 3); }
        } else if (enc == PE_JSON) {
            if (c == '"' || c == '\\') { b[0] = '\\'; b[1] = (char)c; o.put(b, 2); }
            else if (c == '\n') o.put("\\n", 2);
            else if (c < 0x20) { snprintf(b, sizeof(b), "\\u%04x", c); o.put(b, 6); }
            else o.put((char)c);
        } else {
            o.put((char)c);
        }
    }
}

} // namespace

void phFormat(const PhValue& v, const char* suffix, char* out, size_t len)
{
    if (!len) return;
    if (v.kind == PK_TEXT) { snprintf(out, len, "%s", v.text ? v.text : ""); return; }
    float x = v.v;
    int dec = v.decimals;
    bool unitSet = false;
    auto unit = [&](const char* tok) {
        switch (v.kind) {
            case PK_TEMP:  if (!strcmp(tok, "f")) { x = x * 1.8f + 32.0f; } break;
            case PK_SPEED:
                if      (!strcmp(tok, "kmh")) { x *= 3.6f; }
                else if (!strcmp(tok, "mph")) { x *= 2.236936f; }
                else if (!strcmp(tok, "kn"))  { x *= 1.943844f; }
                break;
            case PK_RAIN: case PK_RATE:
                if (!strcmp(tok, "in")) { x /= 25.4f; if (dec < 0) dec = 2; }
                break;
            case PK_PRESS: if (!strcmp(tok, "inhg")) { x /= 33.8639f; if (dec < 0) dec = 2; } break;
            default: break;
        }
    };
    // suffix tokens separated by ':'
    char buf[24];
    snprintf(buf, sizeof(buf), "%s", suffix ? suffix : "");
    for (char* tok = buf; tok && *tok;) {
        char* next = strchr(tok, ':');
        if (next) *next++ = '\0';
        if (tok[0] == '.' && tok[1] >= '0' && tok[1] <= '4' && !tok[2]) dec = tok[1] - '0';
        else if (!unitSet) { unitSet = true; unit(tok); }
        tok = next;
    }
    if (!unitSet && v.defUnit) unit(v.defUnit);
    if (dec < 0) dec = v.kind == PK_PLAIN ? 0 : 1;
    if (dec > 4) dec = 4;
    snprintf(out, len, "%.*f", dec, (double)x);
    if (!strncmp(out, "-0", 2) && strtod(out, nullptr) == 0.0) memmove(out, out + 1, strlen(out));   // "-0.0" -> "0.0"
}

int phRender(const char* tpl, char* out, size_t len, PhEnc enc, PhLookup lookup, void* ctx, bool* cut)
{
    if (cut) *cut = false;
    if (!len) return 0;
    Out o{ out, len, 0, false };
    int empty = 0;
    for (const char* s = tpl ? tpl : ""; *s; s++) {
        const char* close = *s == '{' ? strchr(s, '}') : nullptr;
        if (!close || close - s > 40 || !validPlaceholder(s, close)) { o.put(*s); continue; }
        char name[41];
        memcpy(name, s + 1, (size_t)(close - s - 1));
        name[close - s - 1] = '\0';
        char* suffix = strchr(name, ':');
        if (suffix) *suffix++ = '\0';
        PhValue v;
        if (lookup && lookup(name, v, ctx)) {
            if (v.kind == PK_TEXT) {   // straight from the source: a text (an alarm message) may be long
                putEncoded(o, v.text ? v.text : "", enc);
            } else {
                char val[64];   // a number: up to 39 integer digits (float) + decimals
                phFormat(v, suffix, val, sizeof(val));
                putEncoded(o, val, enc);
            }
        } else {
            empty++;
        }
        s = close;
    }
    out[o.n] = '\0';
    if (cut) *cut = o.cut;
    return empty;
}
