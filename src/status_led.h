#pragma once
// Status LED (Settings → Display): the Heltec's white LED on GPIO35.
// 0 = off, 1 = heartbeat (40 ms every 2 s while the main loop runs),
// 2 = a flash for every valid packet of a "mine" (★) wireless sensor,
// 3 = a flash for every valid wireless packet (also neighbours').
// Stored in the setting "displed" (formerly a heartbeat on/off box: 1 keeps working).
#include <stdint.h>

enum StatusLedMode : int { LED_OFF = 0, LED_HEARTBEAT = 1, LED_RX_MINE = 2, LED_RX_ANY = 3 };
constexpr int LED_MODE_MAX = LED_RX_ANY;
constexpr uint32_t LED_FLASH_MS = 40;
constexpr uint32_t LED_HEARTBEAT_MS = 2000;

// A valid wireless packet was taken (checksum ok, sensor not hidden) - any task.
void statusLedPacket(bool mine);

// Pure step (host-tested): returns whether the LED should be on now.
struct StatusLedState {
    bool     on = false;
    uint32_t onMs = 0;         // when the current flash started
    uint32_t lastBeatMs = 0;   // heartbeat
    uint32_t seenMine = 0, seenAny = 0;   // packet counters already shown
};
bool statusLedStep(int mode, uint32_t nowMs, uint32_t pktMine, uint32_t pktAny, StatusLedState& s);

// Main loop: drives GPIO35 (not on a V4-R8, whose octal PSRAM uses it).
void statusLedPoll();
