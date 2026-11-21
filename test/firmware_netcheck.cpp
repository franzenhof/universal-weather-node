// Static IP form check (src/net_check.cpp): the "static IP (invalid address)"
// rejection a tester saw on every save; the Wi-Fi / web interface off rule.
#include "net_check.h"
#include <cassert>
#include <cstdio>

int main()
{
    assert(ipValid("192.168.1.10") && ipValid("0.0.0.0") && ipValid("255.255.255.0"));
    assert(!ipValid("") && !ipValid("192.168.1") && !ipValid("192.168.1.256") && !ipValid("a.b.c.d") && !ipValid("1.2.3.4 ") &&
           !ipValid("1.2.3.4.5") && !ipValid("1..2.3"));
    bool en;
    // off: never rejected, whatever the address fields hold
    assert(staticIpCheck(false, "garbage", "", "", "", true, en) == SIP_APPLY && !en);
    // on, but no address (e.g. stored like that by an old portal save): DHCP, not an error
    assert(staticIpCheck(true, "", "", "255.255.255.0", "", false, en) == SIP_APPLY && !en);
    // on and unchanged (another tab saved): kept, even if the stored gateway is incomplete
    assert(staticIpCheck(true, "192.168.1.10", "", "255.255.255.0", "", false, en) == SIP_KEEP && en);
    // on and changed: checked
    assert(staticIpCheck(true, "192.168.1.10", "", "255.255.255.0", "", true, en) == SIP_INVALID);
    assert(staticIpCheck(true, "192.168.1.10", "192.168.1.1", "255.255.255.0", "", true, en) == SIP_APPLY && en);
    assert(staticIpCheck(true, "192.168.1.10", "192.168.1.1", "255.255.255.0", "1.1.1", true, en) == SIP_INVALID);
    // Wi-Fi / web interface off from the web settings: only with LoRaWAN on
    // and joined (else the node would be unreachable) and the confirmation
    assert(radioOffCheck(false, false, false, false) == RADIO_OFF_NONE);
    assert(radioOffCheck(true, true, true, true) == RADIO_OFF_APPLY);
    assert(radioOffCheck(true, false, true, true) == RADIO_OFF_NOT_CONFIRMED);
    assert(radioOffCheck(true, true, true, false) == RADIO_OFF_NO_LORAWAN);    // not joined
    assert(radioOffCheck(true, true, false, true) == RADIO_OFF_NO_LORAWAN);    // LoRaWAN switched off in the same save
    assert(radioOffCheck(true, false, false, false) == RADIO_OFF_NO_LORAWAN);
    puts("netcheck ok");
    return 0;
}
