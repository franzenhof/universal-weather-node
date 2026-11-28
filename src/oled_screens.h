#pragma once
// Weather screens for the Heltec's built-in 128x64 OLED (Settings → Display,
// docs/settings.md#display). Screen 0 is the classic status screen drawn by
// main.cpp; 1-5 are drawn here: temperature, wind, rain, pressure, status;
// 6-7 the custom screens, 8 the Wi-Fi QR code of the own access point.
#include <stdint.h>
#include "screensaver.h"

class OLEDDisplay;

// Text rows for ArialMT_Plain_10 (13 px glyph box, caps from +3 to +10,
// descenders to +12): six lines that all fit the 64 px. Up to rc.9 the rows
// were 0/11/22/33/44/55 and the sixth line was cut off at the bottom.
constexpr int16_t OLED_ROW[6] = { 0, 10, 20, 31, 41, 51 };

enum OledScreen : uint8_t { OS_CLASSIC = 0, OS_TEMP, OS_WIND, OS_RAIN, OS_PRESS, OS_STATUS,
                            OS_CUSTOM1, OS_CUSTOM2,   // OS_CUSTOM1 + OLED_CUSTOM_COUNT = OS_QR
                            OS_QR,                    // Wi-Fi QR code of the own access point (wifi_ap.h), only while it runs
                            OS_COUNT };

struct OledCustom;

// Draws screen s (1..OS_COUNT-1) into the display buffer (after clear()).
void oledDrawScreen(OLEDDisplay& d, uint8_t s);
// Next screen after cur that is enabled in mask (bit n = screen n); mask 0 = classic only.
uint8_t oledNextScreen(uint8_t cur, uint16_t mask);
// gCfg.dispScreens without the screens that have nothing to show now (the
// QR screen while the own access point is off); 0 = classic only.
uint16_t oledScreenMask();
// A custom screen as it would look now, drawn off-screen (the settings page's
// preview, web task; its own UTF-8 decoder state): 1024 bytes in SSD1306 order (byte x + 128 * page, bit
// = row in the page). False if the 2 KB of canvas memory are not there.
bool oledPreviewCustom(const OledCustom& c, uint8_t out[1024]);
// The settings page's list of placeholders: each name looked up as on a
// custom screen (device units, the display-only names). cb gets the value
// text (without unit) and its kind (placeholders.h PhKind) - only for
// names that have a value now.
extern const char* const OLED_PH_NAMES[];   // display-only names: clock, date, ip, u_temp ...
extern const int         OLED_PH_NAME_COUNT;
void oledPlaceholderValues(const char* const* names, int n,
                           void (*cb)(int i, const char* text, uint8_t kind, void* arg), void* arg);
// Screensaver (screensaver.h): the current values the figure reacts to (loop
// task, and the web task's preview: no shared mutable state), and the per-cycle
// note that remembers a frost for "thaw" (loop task only).
void oledSaverInput(SaverInput& in);
void oledSaverNoteCycle();
// Night-off hours [from, to) in local time; from == to = never.
bool oledNightNow(int fromHour, int toHour, int hourNow);
