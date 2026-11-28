#pragma once
// Custom OLED screens (Settings → Display, docs/settings.md#custom-screens):
// two screens whose lines are placeholder templates ("{temp} °C",
// placeholders.h). This part has no display or firmware dependencies
// (host-tested, test/firmware_oledcustom.cpp): the data, how it is stored
// in one NVS string per screen, and the input clean-up. Drawn by
// oled_screens.cpp (oledDrawScreen / oledPreviewCustom).
#include <stddef.h>
#include <stdint.h>

constexpr int OLED_CUSTOM_COUNT = 2;   // more later if needed: config, enum OledScreen, docs
constexpr int OLED_CUSTOM_LINES = 5;
constexpr int OLED_CUSTOM_TITLE = 24;   // bytes incl. the terminator
constexpr int OLED_CUSTOM_LINE  = 64;   // template bytes incl. the terminator

enum OledCustomLayout : uint8_t {
    OCL_SMALL = 0,   // title + 5 small lines
    OCL_BIG   = 1,   // title + line 1 in large type + lines 2-3 small (lines 4-5 unused)
};

struct OledCustom {
    uint8_t layout = OCL_SMALL;
    char    title[OLED_CUSTOM_TITLE] = "";
    char    line[OLED_CUSTOM_LINES][OLED_CUSTOM_LINE] = {};
};

// Stored form: "<layout>\n<title>\n<line 1>\n...\n<line 5>" (one NVS string).
// Returns the length written (out always terminated).
size_t oledCustomPack(const OledCustom& c, char* out, size_t len);
// Unknown / damaged text gives an empty screen; missing lines stay empty.
void   oledCustomUnpack(const char* s, OledCustom& c);
// Copies a form value into a field: control characters (newline, tab ...)
// dropped, cut at len - 1 bytes without splitting a UTF-8 character.
void   oledCustomClean(const char* in, char* out, size_t len);
