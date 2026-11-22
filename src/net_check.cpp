#include "net_check.h"
#include <stdlib.h>

bool ipValid(const char* s)
{
    if (!s || !*s) return false;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return false;
        char* end = nullptr;
        const long v = strtol(s, &end, 10);
        if (v > 255 || end - s > 3) return false;
        s = end;
        if (part < 3) { if (*s != '.') return false; s++; }
    }
    return *s == '\0';
}

StaticIpOutcome staticIpCheck(bool en, const char* ip, const char* gw, const char* sn, const char* dns, bool changed,
                              bool& effectiveEn, StaticIpField* bad)
{
    effectiveEn = en;
    if (bad) *bad = SIPF_NONE;
    if (!en) return SIP_APPLY;
    if (!ip || !*ip) { effectiveEn = false; return SIP_APPLY; }
    if (!changed) return SIP_KEEP;
    const StaticIpField f = !ipValid(ip) ? SIPF_IP : !ipValid(gw) ? SIPF_GATEWAY : !ipValid(sn) ? SIPF_SUBNET
                          : (dns && *dns && !ipValid(dns)) ? SIPF_DNS : SIPF_NONE;
    if (bad) *bad = f;
    return f == SIPF_NONE ? SIP_APPLY : SIP_INVALID;
}

RadioOffOutcome radioOffCheck(bool offRequested, bool confirmed, bool loraEnAfter, bool loraJoined)
{
    if (!offRequested) return RADIO_OFF_NONE;
    // LoRaWAN first: without it the node would be unreachable, ticked or not
    if (!loraEnAfter || !loraJoined) return RADIO_OFF_NO_LORAWAN;
    if (!confirmed) return RADIO_OFF_NOT_CONFIRMED;
    return RADIO_OFF_APPLY;
}
