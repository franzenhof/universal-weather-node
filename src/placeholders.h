#pragma once
// Placeholder templates for the custom HTTP uploads and the alarm
// webhook / messages: "{name}" or "{name:suffix:suffix}" is replaced by a
// value; everything else is copied. Single pass, fixed output buffer.
//
// Suffixes (any order): a unit - temperature c / f, speed ms / kmh / mph /
// kn, rain and rain rate mm / in, pressure hpa / inhg - and ".N" decimals
// (0-4). Without a unit suffix: metric (°C, m/s, mm, mm/h, hPa); default
// decimals per kind. Unknown names and values that are not available
// render as nothing ("{x}" -> "").
// Encoding of the inserted values (not of the template text): none, URL
// (percent-encoding) or JSON string content (escaped quotes, backslashes,
// control characters). Host-tested (test/firmware_placeholders.cpp).
#include <stddef.h>
#include <stdint.h>

enum PhKind : uint8_t { PK_PLAIN = 0, PK_TEMP, PK_SPEED, PK_RAIN, PK_RATE, PK_PRESS, PK_TEXT };
enum PhEnc  : uint8_t { PE_NONE = 0, PE_URL, PE_JSON };

struct PhValue {
    PhKind      kind = PK_PLAIN;
    float       v = 0.0f;          // metric
    int8_t      decimals = -1;     // default decimals (-1: per kind)
    const char* text = nullptr;    // PK_TEXT
    const char* defUnit = nullptr; // unit suffix used when the placeholder names none (OLED: the device's units)
};

// name -> value; false = unknown / not available now
typedef bool (*PhLookup)(const char* name, PhValue& out, void* ctx);

// Renders tpl into out (always terminated, cut at len - 1). Returns the
// number of placeholders that rendered empty (unknown or unavailable).
int phRender(const char* tpl, char* out, size_t len, PhEnc enc, PhLookup lookup, void* ctx);

// A value with one placeholder's suffix applied ("f", "kmh:.0", ...) -> text.
void phFormat(const PhValue& v, const char* suffix, char* out, size_t len);
