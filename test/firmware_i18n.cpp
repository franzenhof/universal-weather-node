// Web interface languages (src/i18n.cpp): Accept-Language parsing, the
// cookie / setting / browser order, lookup with English fallback, placeholders,
// parameter 57.
#include "i18n.h"
#include "i18n_table.h"
#include "params.h"
#include "globals.h"
#include <cassert>
#include <cstring>
#include <string>

int main()
{
    // ---- Accept-Language: highest q among the supported languages ----
    assert(i18nParseAcceptLanguage("de-AT,de;q=0.9,en;q=0.8") == LANG_DE);   // region tag counts for its language
    assert(i18nParseAcceptLanguage("en-US,en;q=0.9,de;q=0.8") == LANG_EN);
    assert(i18nParseAcceptLanguage("nl-NL,nl;q=0.9,fr;q=0.7,de;q=0.5") == LANG_FR);   // unsupported skipped
    assert(i18nParseAcceptLanguage("pt-BR") == LANG_PT && i18nParseAcceptLanguage("es_ES") == LANG_ES);
    assert(i18nParseAcceptLanguage("de;q=0.3, fr;q=0.8") == LANG_FR);         // order does not matter, q does
    assert(i18nParseAcceptLanguage("fr;q=0, de") == LANG_DE);                   // q=0 = not acceptable
    assert(i18nParseAcceptLanguage("de, fr") == LANG_DE);                       // equal q: the first
    assert(i18nParseAcceptLanguage("*") == LANG_EN && i18nParseAcceptLanguage("") == LANG_EN && i18nParseAcceptLanguage(nullptr) == LANG_EN);
    assert(i18nParseAcceptLanguage("deu, english") == LANG_EN);                 // not two-letter tags
    assert(i18nParseAcceptLanguage("DE-de") == LANG_DE);                        // case-insensitive
    assert(langFromCode("Pt", 2) == LANG_PT && langFromCode("xx", 2) == -1 && langFromCode("e", 1) == -1);
    assert(!strcmp(langCode(LANG_FR), "fr") && !strcmp(langCode(99), "en"));

    // ---- order: per-browser cookie, then a fixed setting, then the browser ----
    assert(i18nLangFor(UI_LANG_AUTO, "", "fr-CH") == LANG_FR);
    assert(i18nLangFor(2, "", "fr-CH") == LANG_DE);                              // setting 2 = de
    assert(i18nLangFor(2, "a=1; wnLang=es; b=2", "fr") == LANG_ES);              // cookie wins
    assert(i18nLangFor(2, "wnLang=", "fr") == LANG_DE);                          // empty cookie = device setting
    assert(i18nLangFor(0, "xwnLang=pt", "de") == LANG_DE);                       // another cookie's name
    assert(i18nLangFor(0, "wnLang=xx", "es") == LANG_ES);                        // unsupported value ignored
    assert(i18nLangFor(1, nullptr, nullptr) == LANG_EN && i18nLangFor(5, nullptr, nullptr) == LANG_PT);
    assert(uiLangValid(0) && uiLangValid(5) && !uiLangValid(6) && !uiLangValid(-1));

    // ---- lookup: every language has every text (the generator falls back to English) ----
    for (int l = 0; l < LANG_COUNT; l++)
        for (int k = 0; k < T_COUNT_; k++) assert(TL((uint8_t)l, (TextId)k) && TL((uint8_t)l, (TextId)k)[0]);
    i18nSet(LANG_DE);
    assert(i18nCurrent() == LANG_DE && T(T_c_settings) == I18N_TEXT[LANG_DE][T_c_settings]);
    i18nSet(42);
    assert(i18nCurrent() == LANG_EN && !strcmp(T(T_c_settings), "Settings"));
    assert(!strcmp(TL(LANG_EN, (TextId)T_COUNT_), ""));

    // ---- placeholders ----
    std::string out;
    const char* args[2] = { "42", "7" };
    i18nFormatT(out, "vor {0} s, {1} Werte, {2} bleibt", args, 2);
    assert(out == "vor 42 s, 7 Werte, {2} bleibt");
    out.clear();
    i18nFormatT(out, TL(LANG_EN, T_c_ago_s), args, 1);
    assert(out == "42 s ago");

    // ---- parameter 57 ----
    assert(applyParam(PARAM_UI_LANG, 2) && gCfg.uiLang == 2);
    assert(!applyParam(PARAM_UI_LANG, 6) && !applyParam(PARAM_UI_LANG, -1) && gCfg.uiLang == 2);
    assert(applyParam(PARAM_UI_LANG, 0) && gCfg.uiLang == UI_LANG_AUTO);
    return 0;
}
