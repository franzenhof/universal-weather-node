#include "oled_screens.h"
#include "globals.h"
#include "sources.h"
#include "sensors.h"
#include "units.h"
#include "derived_now.h"
#include "rf_sensors.h"
#include "i18n.h"
#include "alarms.h"
#include "api_info.h"
#include "wifi_mqtt.h"
#include "oled_custom.h"
#include "ph_live.h"
#include "time_sync.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayFonts.h>
#include <WiFi.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

namespace {
uint8_t lang() { return gCfg.uiLang >= 1 && gCfg.uiLang <= LANG_COUNT ? gCfg.uiLang - 1 : LANG_EN; }
const char* tx(TextId id) { return TL(lang(), id); }

// title line, big value (24 px), one or two small lines below
void layout(OLEDDisplay& d, const char* title, const char* big, const char* line1, const char* line2 = nullptr)
{
    d.setTextAlignment(TEXT_ALIGN_LEFT);
    d.setFont(ArialMT_Plain_10);
    d.drawString(0, 0, title);
    d.drawHorizontalLine(0, 12, 128);
    d.setFont(ArialMT_Plain_24);
    d.drawString(0, 13, big);
    d.setFont(ArialMT_Plain_10);
    if (line1) d.drawString(0, line2 ? 40 : 46, line1);
    if (line2) d.drawString(0, OLED_ROW[5], line2);
}

const char* compass(int deg)
{
    static char pt[4];
    const char* list = tx(T_c_compass);   // 16 points, comma-separated
    int idx = ((deg % 360) * 16 + 180) / 360 % 16;
    for (int i = 0; i < idx && list; i++) { list = strchr(list, ','); if (list) list++; }
    if (!list) return "";
    size_t n = 0;
    while (list[n] && list[n] != ',' && n < sizeof(pt) - 1) { pt[n] = list[n]; n++; }
    pt[n] = '\0';
    return pt;
}

const char* trendWord(const char* en)
{
    static const struct { const char* en; TextId t; } m[] = {
        { "storm warning", T_oled_wt_storm }, { "worsening", T_oled_wt_worse }, { "improving", T_oled_wt_better },
        { "settled", T_oled_wt_settled }, { "unsettled", T_oled_wt_unsettled }, { "no change", T_oled_wt_same } };
    for (const auto& e : m) if (en && !strcmp(en, e.en)) return tx(e.t);
    return "";
}
// ---- custom screens ----------------------------------------------------
// Placeholders as for the uploads (ph_live.h), but in the device's units
// unless a placeholder names one, plus a few display-only names. A value
// that is not there (or a misspelt name) shows as "--".
struct DispCtx {
    PhLive live;
    char   clock[8] = "", date[12] = "", dateFull[12] = "", ip[16] = "";
};

bool dispLookup(const char* n, PhValue& o, void* ctx)
{
    DispCtx& c = *(DispCtx*)ctx;
    auto txt = [&](const char* s) { o.kind = PK_TEXT; o.text = s && *s ? s : "--"; return true; };
    if (!strcmp(n, "clock"))   return txt(c.clock);
    if (!strcmp(n, "date"))      return txt(c.date);
    if (!strcmp(n, "date_full")) return txt(c.dateFull);
    if (!strcmp(n, "ip"))      return txt(c.ip);
    if (!strcmp(n, "u_temp"))  { static char u[4]; snprintf(u, sizeof(u), "\xC2\xB0%s", temperatureUnitLabel(gCfg)); return txt(u); }
    if (!strcmp(n, "u_wind"))  return txt(windSpeedUnitLabel(gCfg));
    if (!strcmp(n, "u_rain"))  return txt(rainUnitLabel(gCfg));
    if (!strcmp(n, "u_rate"))  return txt(rainRateUnitLabel(gCfg));
    if (!strcmp(n, "u_press")) return txt(pressureUnitLabel(gCfg));
    if (!phLiveLookup(n, o, &c.live)) return txt(nullptr);
    const bool imp = gCfg.unitSystem == 1;
    switch (o.kind) {
        case PK_TEMP:  o.defUnit = imp ? "f" : nullptr; break;
        case PK_SPEED: o.defUnit = imp ? "mph" : gCfg.windUnitMs ? nullptr : "kmh"; break;
        case PK_RAIN: case PK_RATE: o.defUnit = imp ? "in" : nullptr; break;
        case PK_PRESS: o.defUnit = imp ? "inhg" : nullptr; break;
        default: break;
    }
    return true;
}

// ~300 bytes on the heap (off the web task's stack), filled for now; nullptr if no memory
DispCtx* newDispCtx()
{
    DispCtx* ctx = new (std::nothrow) DispCtx();
    if (!ctx) return nullptr;
    phLiveInit(ctx->live, nullptr);
    struct tm lt;
    if (localTimeNow(lt)) {
        strftime(ctx->clock, sizeof(ctx->clock), "%H:%M", &lt);
        strftime(ctx->date, sizeof(ctx->date), gCfg.unitSystem == 1 ? "%m/%d" : "%d.%m.", &lt);
        strftime(ctx->dateFull, sizeof(ctx->dateFull), gCfg.unitSystem == 1 ? "%m/%d/%Y" : "%d.%m.%Y", &lt);
    }
    if (gCfg.wifiEn && wifiIsConnected()) strlcpy(ctx->ip, WiFi.localIP().toString().c_str(), sizeof(ctx->ip));
    return ctx;
}

void drawCustom(OLEDDisplay& d, const OledCustom& c)
{
    DispCtx* ctx = newDispCtx();
    if (!ctx) return;
    char title[64], l[OLED_CUSTOM_LINES][64];
    phRender(c.title, title, sizeof(title), PE_NONE, dispLookup, ctx);
    for (int i = 0; i < OLED_CUSTOM_LINES; i++) phRender(c.line[i], l[i], sizeof(l[i]), PE_NONE, dispLookup, ctx);
    delete ctx;
    if (c.layout == OCL_BIG) {
        layout(d, title, l[0], l[1][0] ? l[1] : nullptr, l[2][0] ? l[2] : nullptr);
        return;
    }
    d.setTextAlignment(TEXT_ALIGN_LEFT);
    d.setFont(ArialMT_Plain_10);
    int row = 0;
    if (title[0]) { d.drawString(0, OLED_ROW[0], title); d.drawHorizontalLine(0, 11, 128); row = 1; }
    for (int i = 0; i < OLED_CUSTOM_LINES && row < 6; i++, row++) d.drawString(0, OLED_ROW[row], l[i]);
}

// Off-screen canvas for the preview: the display library's drawing, no I2C.
class OledCanvas : public OLEDDisplay {
public:
    OledCanvas() { setGeometry(GEOMETRY_128_64); }
    void display() override {}
    const uint8_t* pixels() const { return buffer; }
protected:
    int  getBufferOffset() override { return 0; }
    bool connect() override { return true; }
};

} // namespace

const char* const OLED_PH_NAMES[] = { "clock", "date", "date_full", "ip", "u_temp", "u_wind", "u_rain", "u_rate", "u_press" };
const int OLED_PH_NAME_COUNT = sizeof(OLED_PH_NAMES) / sizeof(OLED_PH_NAMES[0]);

void oledPlaceholderValues(const char* const* names, int n,
                           void (*cb)(int i, const char* text, uint8_t kind, void* arg), void* arg)
{
    DispCtx* ctx = newDispCtx();
    if (!ctx) return;
    for (int i = 0; i < n; i++) {
        PhValue v;
        if (!dispLookup(names[i], v, ctx)) continue;
        if (v.kind == PK_TEXT && v.text && !strcmp(v.text, "--")) continue;   // not available now
        char t[48];
        phFormat(v, nullptr, t, sizeof(t));
        cb(i, t, v.kind, arg);
    }
    delete ctx;
}

static_assert(OS_CUSTOM1 + OLED_CUSTOM_COUNT == OS_COUNT, "one OledScreen per custom screen");

bool oledPreviewCustom(const OledCustom& c, uint8_t out[1024])
{
    OledCanvas cv;
    // init(), not allocateBuffer() alone: only init() sets the library's
    // BufferOffset - without it the buffers land at a random offset and
    // the heap is corrupted (seen as a crash loop on the first previews)
    if (!cv.init()) return false;
    cv.clear();
    drawCustom(cv, c);
    memcpy(out, cv.pixels(), 1024);
    return true;   // the canvas frees its buffers
}

uint8_t oledNextScreen(uint8_t cur, uint16_t mask)
{
    mask &= (1 << OS_COUNT) - 1;
    if (!mask) return OS_CLASSIC;
    for (uint8_t k = 1; k <= OS_COUNT; k++) {
        const uint8_t s = (uint8_t)((cur + k) % OS_COUNT);
        if (mask & (1 << s)) return s;
    }
    return OS_CLASSIC;
}

bool oledNightNow(int from, int to, int h)
{
    if (from == to) return false;
    return from < to ? (h >= from && h < to) : (h >= from || h < to);
}

void oledDrawScreen(OLEDDisplay& d, uint8_t s)
{
    char big[24], l1[48], l2[48] = "";
    static DerivedNow dn;   // ~150 bytes, not on the loop task's stack
    derivedNow(dn, windSpeedMs, windDirection);
    const char* none = tx(T_oled_nodata);
    if (s >= OS_CUSTOM1 && s < OS_COUNT) { drawCustom(d, gCfg.dispCustom[s - OS_CUSTOM1]); return; }
    switch (s) {
        case OS_TEMP:
            if (gSrc.tempValid) snprintf(big, sizeof(big), "%.1f\xC2\xB0%s", temperatureForOutput(gSrc.tempC, gCfg), temperatureUnitLabel(gCfg));
            else strlcpy(big, none, sizeof(big));
            l1[0] = '\0';
            if (gSrc.tempValid && gSrc.humValid) {
                int n = snprintf(l1, sizeof(l1), "%.0f %%", gSrc.humidity);
                if (dn.dewValid) snprintf(l1 + n, sizeof(l1) - n, "  %s %.1f\xC2\xB0", tx(T_oled_dew), temperatureForOutput(dn.dewPointC, gCfg));
            }
            if (dn.feelsValid) snprintf(l2, sizeof(l2), "%s %.1f\xC2\xB0", tx(T_oled_feels), temperatureForOutput(dn.feelsLikeC, gCfg));
            layout(d, tx(T_oled_temp), big, l1, l2[0] ? l2 : nullptr);
            break;
        case OS_WIND:
            if (srcActive(SG_WIND) && windCycleValid) {
                snprintf(big, sizeof(big), "%.1f %s", windSpeedForOutput(windSpeedMs, gCfg), windSpeedUnitLabel(gCfg));
                snprintf(l1, sizeof(l1), "%s %.1f   %s %d\xC2\xB0", tx(T_oled_gust), windSpeedForOutput(windGustMs, gCfg),
                         compass(windDirection), windDirection);
            } else { strlcpy(big, none, sizeof(big)); l1[0] = '\0'; }
            layout(d, tx(T_oled_wind), big, l1);
            break;
        case OS_RAIN: {
            const bool rain = srcActive(SG_RAIN);
            const int dec = gCfg.unitSystem == 1 ? 2 : 1;
            if (rain && dn.daily.valid) snprintf(big, sizeof(big), "%.*f %s", dec, rainForOutput(dn.daily.rainTodayMm, gCfg), rainUnitLabel(gCfg));
            else strlcpy(big, rain ? "--" : none, sizeof(big));
            l1[0] = '\0';
            if (rain) snprintf(l1, sizeof(l1), "%s %.*f  %.*f %s", tx(T_oled_1h), dec, rainForOutput(dn.rain1hMm, gCfg),
                               dec, rainRateForOutput(rainRateMmH, gCfg), rainRateUnitLabel(gCfg));
            char title[32];
            snprintf(title, sizeof(title), "%s %s", tx(T_oled_rain), tx(T_oled_today));
            layout(d, title, big, l1);
            break;
        }
        case OS_PRESS:
            if (gSrc.pressValid) snprintf(big, sizeof(big), "%.*f %s", gCfg.unitSystem == 1 ? 2 : 1,
                                          pressureForOutput(gSrc.pressSeaHpa, gCfg), pressureUnitLabel(gCfg));
            else strlcpy(big, none, sizeof(big));
            l1[0] = '\0';
            if (dn.pressTrendValid) {
                const float dp = dn.pressDelta3hHpa;
                const char* arrow = dp >= 0.1f ? "/" : dp <= -0.1f ? "\\" : "-";
                snprintf(l1, sizeof(l1), "%s %+.*f / 3 h  %s", arrow, gCfg.unitSystem == 1 ? 2 : 1, pressureForOutput(dp, gCfg),
                         trendWord(dn.weatherTrend));
            }
            layout(d, tx(T_oled_press), big, l1);
            break;
        default: {   // status
            d.setFont(ArialMT_Plain_10);
            d.setTextAlignment(TEXT_ALIGN_LEFT);
            d.drawString(0, 0, tx(T_oled_status));
            d.drawHorizontalLine(0, 12, 128);
            if (!gCfg.wifiEn) snprintf(l1, sizeof(l1), "WiFi off");
            else if (wifiIsConnected()) snprintf(l1, sizeof(l1), "%s  %d dBm", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            else snprintf(l1, sizeof(l1), "WiFi ...");
            d.drawString(0, 14, l1);
            char host[40];
            deviceHostname(host, sizeof(host));
            snprintf(l1, sizeof(l1), "%s.local", host);
            d.drawString(0, 26, l1);
            int n = 0;
            for (int i = 0; i < RF_MAX_SENSORS; i++) n += gRfSensors[i].used;
            snprintf(l1, sizeof(l1), "%d %s   %d %s", gCfg.rfEn ? n : 0, tx(T_oled_sensors), alarmsActiveCount(), tx(T_oled_alarms));
            d.drawString(0, 38, l1);
            break;
        }
    }
}
