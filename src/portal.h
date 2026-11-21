#pragma once

#include "config.h"
#include <SSD1306Wire.h>

// heltec_unofficial.h defines (not just declares) global objects like
// `display` directly in the header, so it can only be #included from a
// single translation unit (main.cpp). This extern gives portal.cpp access
// to the same object for status messages during the portal session.
extern SSD1306Wire display;

// Blocking: runs the WiFiManager AP config portal (SSID "WeatherNode") until
// the user exits it, then writes the updated settings into cfg (gCfg; no
// Config copy on the stack, see portal.cpp). Also serves /decoder,
// /help(.md), and /calibrate* (see include/*_page.h for the static content).
void runConfigPortal(Config& cfg);
