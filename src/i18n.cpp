#include "i18n.h"
#include "i18n_table.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {
uint8_t current = LANG_EN;
const char* const CODES[LANG_COUNT] = { "en", "de", "fr", "es", "pt" };
}

const char* langCode(uint8_t lang) { return lang < LANG_COUNT ? CODES[lang] : "en"; }

int langFromCode(const char* code, size_t len)
{
    if (!code || len < 2 || (len > 2 && isalpha((unsigned char)code[2]))) return -1;
    char c[3] = { (char)tolower((unsigned char)code[0]), (char)tolower((unsigned char)code[1]), 0 };
    for (int i = 0; i < LANG_COUNT; i++) if (!strcmp(c, CODES[i])) return i;
    return -1;
}

uint8_t i18nParseAcceptLanguage(const char* h)
{
    int best = -1;
    float bestQ = 0.0f;
    while (h && *h) {
        while (*h == ' ' || *h == ',') h++;
        const char* tag = h;
        while (*h && *h != ',' && *h != ';' && *h != ' ') h++;
        size_t tagLen = (size_t)(h - tag);
        float q = 1.0f;
        while (*h && *h != ',') {   // parameters: ";q=0.8"
            if (*h == 'q' && h[1] == '=') q = (float)atof(h + 2);
            h++;
        }
        // language part of "de-AT" / "de_AT" / "de"
        size_t primary = 0;
        while (primary < tagLen && tag[primary] != '-' && tag[primary] != '_') primary++;
        int lang = primary == 2 ? langFromCode(tag, 2) : -1;
        if (lang >= 0 && q > 0.0f && q > bestQ) { best = lang; bestQ = q; }
    }
    return best >= 0 ? (uint8_t)best : (uint8_t)LANG_EN;
}

uint8_t i18nLangFor(int setting, const char* cookie, const char* accept)
{
    // cookie "wnLang=de" (anywhere in "a=1; wnLang=de; b=2")
    for (const char* p = cookie; p && (p = strstr(p, "wnLang=")) != nullptr; p += 7) {
        if (p != cookie && p[-1] != ' ' && p[-1] != ';') continue;
        const char* v = p + 7;
        size_t n = 0;
        while (v[n] && v[n] != ';' && v[n] != ' ') n++;
        int lang = n == 2 ? langFromCode(v, 2) : -1;
        if (lang >= 0) return (uint8_t)lang;
    }
    if (setting > 0 && setting <= LANG_COUNT) return (uint8_t)(setting - 1);
    return i18nParseAcceptLanguage(accept);
}

void    i18nSet(uint8_t lang) { current = lang < LANG_COUNT ? lang : LANG_EN; }
uint8_t i18nCurrent()         { return current; }

const char* TL(uint8_t lang, TextId id)
{
    if (id >= T_COUNT_) return "";
    return I18N_TEXT[lang < LANG_COUNT ? lang : LANG_EN][id];
}

const char* T(TextId id) { return TL(current, id); }

