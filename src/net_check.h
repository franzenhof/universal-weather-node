#pragma once
// Static IP fields of the settings form (web_api.cpp), host-tested
// (test/firmware_netcheck.cpp).
enum StaticIpOutcome { SIP_KEEP = 0, SIP_APPLY, SIP_INVALID };

bool ipValid(const char* s);   // dotted quad a.b.c.d, each 0-255

// en / ip / gw / sn / dns: the values after the form (the stored ones for
// fields not in the request); changed: any of them differs from the stored
// setting. Off: never rejected. On with an empty address: DHCP, like the
// Wi-Fi code does (effectiveEn = false). On and unchanged: kept as stored -
// a save of other settings is never refused because of it. Else checked.
StaticIpOutcome staticIpCheck(bool en, const char* ip, const char* gw, const char* sn, const char* dns, bool changed,
                              bool& effectiveEn);

// Switching Wi-Fi (wifien) or the web interface (webapien) off from the web
// settings: afterwards the node is reachable only via the config portal
// (PRG 3 s) or a LoRaWAN downlink. offRequested: either goes from on to off;
// confirmed: the confirmation checkbox; loraEnAfter: LoRaWAN on after this
// save; loraJoined: the node has a LoRaWAN session now.
enum RadioOffOutcome { RADIO_OFF_NONE = 0, RADIO_OFF_APPLY, RADIO_OFF_NOT_CONFIRMED, RADIO_OFF_NO_LORAWAN };
RadioOffOutcome radioOffCheck(bool offRequested, bool confirmed, bool loraEnAfter, bool loraJoined);
