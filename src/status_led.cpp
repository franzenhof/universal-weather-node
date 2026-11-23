#include "status_led.h"

static volatile uint32_t gPktMine = 0, gPktAny = 0;

void statusLedPacket(bool mine)
{
    gPktAny = gPktAny + 1;
    if (mine) gPktMine = gPktMine + 1;
}

bool statusLedStep(int mode, uint32_t now, uint32_t pktMine, uint32_t pktAny, StatusLedState& s)
{
    if (s.on && now - s.onMs >= LED_FLASH_MS) s.on = false;
    const bool newMine = pktMine != s.seenMine, newAny = pktAny != s.seenAny;
    s.seenMine = pktMine;
    s.seenAny = pktAny;   // packets while another mode was active are never flashed later
    switch (mode) {
        case LED_HEARTBEAT:
            if (!s.on && now - s.lastBeatMs >= LED_HEARTBEAT_MS) { s.on = true; s.onMs = s.lastBeatMs = now; }
            break;
        case LED_RX_MINE:
            if (newMine && !s.on) { s.on = true; s.onMs = now; }
            break;
        case LED_RX_ANY:
            if (newAny && !s.on) { s.on = true; s.onMs = now; }
            break;
        default:
            s.on = false;
            break;
    }
    return s.on;
}

#ifdef ARDUINO
#include <Arduino.h>
#include "globals.h"
#include "heltec_board.h"

static constexpr int PIN_STATUS_LED = 35;   // Heltec V3 / V4 white LED; the PCBs do not use GPIO35

void statusLedPoll()
{
    static bool pinSet = false, lit = false;
    static StatusLedState st;
    const int mode = boardIsR8() ? LED_OFF : gCfg.dispLed;
    const bool on = statusLedStep(mode, millis(), gPktMine, gPktAny, st);
    if (!pinSet) {
        if (!on) return;
        pinMode(PIN_STATUS_LED, OUTPUT);
        pinSet = true;
    }
    if (on != lit) { digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW); lit = on; }
}
#endif
