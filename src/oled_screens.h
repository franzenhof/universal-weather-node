#pragma once
// Weather screens for the Heltec's built-in 128x64 OLED (Settings → Display,
// docs/settings.md#display). Screen 0 is the classic status screen drawn by
// main.cpp; 1-5 are drawn here: temperature, wind, rain, pressure, status.
#include <stdint.h>

class OLEDDisplay;

enum OledScreen : uint8_t { OS_CLASSIC = 0, OS_TEMP, OS_WIND, OS_RAIN, OS_PRESS, OS_STATUS, OS_COUNT };

// Draws screen s (1..OS_COUNT-1) into the display buffer (after clear()).
void oledDrawScreen(OLEDDisplay& d, uint8_t s);
// Next screen after cur that is enabled in mask (bit n = screen n); mask 0 = classic only.
uint8_t oledNextScreen(uint8_t cur, uint8_t mask);
// Night-off hours [from, to) in local time; from == to = never.
bool oledNightNow(int fromHour, int toHour, int hourNow);
