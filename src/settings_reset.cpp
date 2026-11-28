// "Reset settings": what it keeps (settings_reset.h). No NVS here.
#include "settings_reset.h"
#include <string.h>

void settingsResetKeptInto(const Config& current, Config& fresh)
{
    fresh.provisioned = 1;   // no setup portal: the node stays on its network
    fresh.uiLang      = current.uiLang;
    memcpy(fresh.deviceName, current.deviceName, sizeof(fresh.deviceName));
    fresh.wifiEn      = current.wifiEn;
    fresh.webApiEn    = current.webApiEn;
    fresh.staticIpEn  = current.staticIpEn;
    memcpy(fresh.staticIp, current.staticIp, sizeof(fresh.staticIp));
    memcpy(fresh.staticGateway, current.staticGateway, sizeof(fresh.staticGateway));
    memcpy(fresh.staticSubnet, current.staticSubnet, sizeof(fresh.staticSubnet));
    memcpy(fresh.staticDns, current.staticDns, sizeof(fresh.staticDns));
    fresh.apMode      = current.apMode;
    memcpy(fresh.apSsid, current.apSsid, sizeof(fresh.apSsid));
    memcpy(fresh.apPass, current.apPass, sizeof(fresh.apPass));
    fresh.apChannel   = current.apChannel;
    memcpy(fresh.webAdminPass, current.webAdminPass, sizeof(fresh.webAdminPass));
    fresh.loraEn      = current.loraEn;
    fresh.joinEui     = current.joinEui;
    fresh.devEui      = current.devEui;
    memcpy(fresh.appKey, current.appKey, sizeof(fresh.appKey));
    memcpy(fresh.region, current.region, sizeof(fresh.region));
    // setupDone stays 0: the setup wizard is offered again (behind the kept admin password)
}
