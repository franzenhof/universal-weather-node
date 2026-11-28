// PRG button decision (src/button_logic.h): short / long press, screensaver
// press, late release, and no long press from a stale press time (B13b D2).
#include "button_logic.h"
#include <cassert>
#include <cstdio>

int main()
{
    // short press: press, then release in a later loop
    {
        ButtonLogic b;
        assert(b.step(true, 1000, false, 0, false, true, 1005) == BTN_NONE);
        assert(b.step(false, 0, true, 1120, false, false, 1125) == BTN_SHORT);
        assert(b.step(false, 0, false, 0, false, false, 1200) == BTN_NONE);
    }
    // short press: both edges taken in one loop (loop was blocked)
    {
        ButtonLogic b;
        assert(b.step(true, 1000, true, 1300, false, false, 4000) == BTN_SHORT);
    }
    // long press: fires once after 3 s held, the release gives no short press
    {
        ButtonLogic b;
        assert(b.step(true, 1000, false, 0, false, true, 1001) == BTN_NONE);
        assert(b.step(false, 0, false, 0, false, true, 3999) == BTN_NONE);
        assert(b.step(false, 0, false, 0, false, true, 4000) == BTN_LONG);
        assert(b.step(false, 0, false, 0, false, true, 9000) == BTN_NONE);
        assert(b.step(false, 0, true, 9100, false, false, 9101) == BTN_NONE);
    }
    // a press held 4 s while the loop was blocked: long press as soon as the loop sees it
    {
        ButtonLogic b;
        assert(b.step(true, 1000, false, 0, false, true, 5000) == BTN_LONG);
    }
    // late release: the previous press's release is older than the new press
    {
        ButtonLogic b;
        assert(b.step(true, 1000, false, 0, false, true, 1001) == BTN_NONE);
        assert(b.step(true, 1500, true, 1200, false, true, 1501) == BTN_SHORT);   // short for the first press
        assert(b.step(false, 0, true, 1600, false, false, 1601) == BTN_SHORT);    // and for the second
    }
    // screensaver: the press only stops it, neither its release nor holding it acts
    {
        ButtonLogic b;
        assert(b.step(true, 1000, false, 0, true, true, 1001) == BTN_SAVER_STOP);
        assert(b.step(false, 0, false, 0, false, true, 4500) == BTN_NONE);
        assert(b.step(false, 0, true, 4600, false, false, 4601) == BTN_NONE);
        assert(b.step(true, 1000, true, 1100, true, false, 1101) == BTN_SAVER_STOP);   // both edges in one loop
        // the next press acts normally
        assert(b.step(true, 6000, true, 6100, false, false, 6101) == BTN_SHORT);
    }
    // D2: earlier tap, later the press edge is misread as a release (bounce) and the
    // stable LOW edge is dropped by the debounce: no portal from the old press time
    {
        ButtonLogic b;
        assert(b.step(true, 1000, true, 1120, false, false, 1125) == BTN_SHORT);
        assert(b.step(false, 0, true, 20000, false, true, 20010) == BTN_NONE);   // bogus release, pin LOW
        assert(b.step(false, 0, false, 0, false, true, 22000) == BTN_NONE);
        assert(b.step(false, 0, true, 22100, false, false, 22101) == BTN_SHORT);  // released: a short press
        // the same lost edge, held on: a long press 3 s after the loop first saw it
        assert(b.step(false, 0, false, 0, false, true, 30000) == BTN_NONE);
        assert(b.step(false, 0, false, 0, false, true, 32999) == BTN_NONE);
        assert(b.step(false, 0, false, 0, false, true, 33000) == BTN_LONG);
    }
    // a release lost to the debounce (tap < 50 ms): the next LOW starts a new press
    {
        ButtonLogic b;
        assert(b.step(true, 1000, false, 0, false, false, 1030) == BTN_NONE);   // released, no release event
        assert(b.step(false, 0, false, 0, false, true, 60000) == BTN_NONE);     // pin LOW, press edge lost
        assert(b.step(false, 0, false, 0, false, true, 62999) == BTN_NONE);
        assert(b.step(false, 0, false, 0, false, true, 63000) == BTN_LONG);
    }
    // two lost edges after a screensaver-ending press: its release and the next
    // press edge are lost - the next press still acts (long, and short)
    {
        ButtonLogic b;
        assert(b.step(true, 1000, false, 0, true, true, 1010) == BTN_SAVER_STOP);   // press ends the screensaver
        assert(b.step(false, 0, false, 0, false, false, 1100) == BTN_NONE);         // released, release edge lost
        assert(b.step(false, 0, false, 0, false, true, 6000) == BTN_NONE);          // new press, press edge lost
        assert(b.step(false, 0, false, 0, false, true, 8999) == BTN_NONE);
        assert(b.step(false, 0, false, 0, false, true, 9000) == BTN_LONG);
        ButtonLogic s;
        assert(s.step(true, 1000, false, 0, true, true, 1010) == BTN_SAVER_STOP);
        assert(s.step(false, 0, false, 0, false, false, 1100) == BTN_NONE);
        assert(s.step(false, 0, false, 0, false, true, 6000) == BTN_NONE);
        assert(s.step(false, 0, true, 6200, false, false, 6201) == BTN_SHORT);
    }
        // a lone release (its press never seen) does nothing
    {
        ButtonLogic b;
        assert(b.step(false, 0, true, 5000, false, false, 5001) == BTN_NONE);
    }
    // millis() wrap
    {
        ButtonLogic b;
        assert(b.step(true, 0xFFFFFF00u, false, 0, false, true, 0xFFFFFF01u) == BTN_NONE);
        assert(b.step(false, 0, true, 0x40u, false, false, 0x41u) == BTN_SHORT);
        assert(b.step(true, 0xFFFFFF00u, false, 0, false, true, 0xFFFFFF01u) == BTN_NONE);
        assert(b.step(false, 0, false, 0, false, true, 0xFFFFFF00u + 3000) == BTN_LONG);
    }
    puts("Firmware button regressions passed.");
    return 0;
}
