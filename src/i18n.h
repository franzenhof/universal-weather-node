#pragma once
// Web interface languages (settings, status, log, firmware update pages and
// the setup portal labels). The strings live in i18n/<lang>.json, compiled
// into flash by tools/i18n_gen.py (include/i18n_keys.h: TextId T_<key>,
// include/i18n_table.h: the table). English is the fallback.
//
// The page handlers call i18nSelect() first; T() / TF() then return the
// texts of that language. One language at a time: the web server and the
// portal each handle one request after the other on their task.
#include <stddef.h>
#include <stdint.h>
#include <Arduino.h>
#include "i18n_keys.h"

enum Lang : uint8_t { LANG_EN = 0, LANG_DE, LANG_FR, LANG_ES, LANG_PT, LANG_COUNT };

// Setting gCfg.uiLang / parameter 57: 0 = Auto (browser), 1..5 = en, de, fr, es, pt.
constexpr int UI_LANG_AUTO = 0;
inline bool uiLangValid(int v) { return v >= 0 && v <= LANG_COUNT; }

const char* langCode(uint8_t lang);              // "en", "de", ...
int  langFromCode(const char* code, size_t len); // two-letter code (any case) -> Lang, -1 = unsupported

// Accept-Language: the supported language with the highest q-value
// ("de-AT,de;q=0.9,en;q=0.8" -> de; region tags count for their language;
// q=0 excluded; equal q: the first). LANG_EN when none is supported.
uint8_t i18nParseAcceptLanguage(const char* header);

// The language of a request: the per-browser choice (cookie "wnLang=xx",
// set by the dashboard / the page language menu) first, then a fixed
// setting, then the browser's Accept-Language.
uint8_t i18nLangFor(int setting, const char* cookieHeader, const char* acceptLanguage);

void    i18nSet(uint8_t lang);    // current language for T() / TF()
uint8_t i18nCurrent();

const char* T(TextId id);
const char* TL(uint8_t lang, TextId id);
// fmt with {0} .. {n-1} replaced by args, appended to out (Out: String or
// std::string - templated so the host tests need no Arduino String)
template <class Out>
void i18nFormatT(Out& out, const char* fmt, const char* const* args, int n)
{
    for (const char* s = fmt; *s; s++) {
        if (s[0] == '{' && s[1] >= '0' && s[1] < '0' + n && s[2] == '}') { out += args[s[1] - '0']; s += 2; }
        else out += *s;
    }
}
// T() with {0} .. {3} replaced by the arguments
String TF(TextId id, const String& a0);
String TF(TextId id, const String& a0, const String& a1);
String TF(TextId id, const String& a0, const String& a1, const String& a2);
String TF(TextId id, const String& a0, const String& a1, const String& a2, const String& a3);
