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

struct Out {
    char* p; size_t len, n;
    void put(char c) { if (n + 1 < len) p[n++] = c; }
    void puts(const char* s) { while (*s) put(*s++); }
};

void putEncoded(Out& o, const char* s, PhEnc enc)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (enc == PE_URL) {
            if (alnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.put((char)c);
            else { o.put('%'); o.put(hex[c >> 4]); o.put(hex[c & 15]); }
        } else if (enc == PE_JSON) {
            if (c == '"' || c == '\\') { o.put('\\'); o.put((char)c); }
            else if (c == '\n') o.puts("\\n");
            else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o.puts(b); }
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
    // suffix tokens separated by ':'
    char buf[24];
    snprintf(buf, sizeof(buf), "%s", suffix ? suffix : "");
    for (char* tok = buf; tok && *tok;) {
        char* next = strchr(tok, ':');
        if (next) *next++ = '\0';
        if (tok[0] == '.' && tok[1] >= '0' && tok[1] <= '4' && !tok[2]) dec = tok[1] - '0';
        else if (!unitSet) {
            unitSet = true;
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
        }
        tok = next;
    }
    if (dec < 0) dec = v.kind == PK_PLAIN ? 0 : 1;
    if (dec > 4) dec = 4;
    snprintf(out, len, "%.*f", dec, (double)x);
    if (!strncmp(out, "-0", 2) && strtod(out, nullptr) == 0.0) memmove(out, out + 1, strlen(out));   // "-0.0" -> "0.0"
}

int phRender(const char* tpl, char* out, size_t len, PhEnc enc, PhLookup lookup, void* ctx)
{
    if (!len) return 0;
    Out o{ out, len, 0 };
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
            char val[96];
            phFormat(v, suffix, val, sizeof(val));
            putEncoded(o, val, enc);
        } else {
            empty++;
        }
        s = close;
    }
    out[o.n] = '\0';
    return empty;
}
