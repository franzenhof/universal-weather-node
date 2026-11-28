#pragma once
// PRG button decision (main.cpp handleButton), host-tested in test/firmware_button.cpp.
// The ISR records press / release edges with their times; the loop hands them
// over together with the current pin level, and step() says what to do.
//   Short press (< 3 s, on release) -> BTN_SHORT
//   Long press  (held >= 3 s)       -> BTN_LONG (config portal)
//   A press while the screensaver runs only ends it (BTN_SAVER_STOP); that
//   press gives neither a short nor a long press.
// A long press needs a press the loop has seen held ever since it started:
// if the pin reads LOW without a matching press event (an edge lost to
// contact bounce or the ISR's debounce), a new press starts now - an older
// press time is never reused.
#include <stdint.h>

constexpr uint32_t BTN_LONG_MS = 3000;

enum : uint8_t { BTN_NONE = 0, BTN_SAVER_STOP = 1, BTN_SHORT = 2, BTN_LONG = 4 };

struct ButtonLogic {
    uint32_t downAtMs = 0;    // start of the open press
    bool down    = false;     // a press is open (press seen, release not yet)
    bool lowSeen = false;     // every poll since downAtMs read the pin LOW
    bool spent   = false;     // the open press already acted (screensaver stop, long press)

    uint8_t step(bool pressEvent, uint32_t pressMs, bool releaseEvent, uint32_t releaseMs,
                 bool saverRunning, bool pinLow, uint32_t now)
    {
        uint8_t act = BTN_NONE;
        // Both pending: a release older than the press belongs to the previous press.
        const bool releaseFirst = pressEvent && releaseEvent && (int32_t)(releaseMs - pressMs) < 0;
        if (releaseFirst) act |= release(releaseMs);
        if (pressEvent) {
            down = true; lowSeen = true; downAtMs = pressMs;
            spent = saverRunning;
            if (saverRunning) act |= BTN_SAVER_STOP;
        }
        if (releaseEvent && !releaseFirst) act |= release(releaseMs);

        if (!pinLow) {
            lowSeen = false;
        } else {
            if (!down || !lowSeen) {   // LOW without a matching press (lost edge / bounce): a press from now -
                // a new one (a HIGH poll ended an open press), so not spent
                down = true; lowSeen = true; downAtMs = now; spent = false;
            }
            if (!spent && now - downAtMs >= BTN_LONG_MS) { spent = true; act |= BTN_LONG; }
        }
        return act;
    }

private:
    uint8_t release(uint32_t t)
    {
        const bool shortPress = down && !spent && t - downAtMs < BTN_LONG_MS;
        down = false; lowSeen = false;
        return shortPress ? BTN_SHORT : BTN_NONE;
    }
};
