// The two resets (src/settings_reset.h): what "Reset settings" keeps, and the
// storage seal (src/storage_write.h) that stops every writer before a reset
// clears or erases.
#include "settings_reset.h"
#include "storage_write.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

int main()
{
    {   // Reset settings: network access, admin password, language, LoRaWAN keys kept - the rest defaults
        Config cur;
        cur.provisioned = 1; cur.uiLang = 2; strcpy(cur.deviceName, "garten");
        cur.wifiEn = 1; cur.webApiEn = 1; cur.staticIpEn = 1;
        strcpy(cur.staticIp, "172.24.10.107"); strcpy(cur.staticGateway, "172.24.10.1");
        strcpy(cur.staticSubnet, "255.255.255.0"); strcpy(cur.staticDns, "1.1.1.1");
        cur.apMode = 2; strcpy(cur.apSsid, "Station"); strcpy(cur.apPass, "secret123"); cur.apChannel = 6;
        strcpy(cur.webAdminPass, "admin-pw");
        cur.loraEn = 1; cur.joinEui = 0x1122334455667788ULL; cur.devEui = 0x0102030405060708ULL;
        for (int i = 0; i < 16; i++) cur.appKey[i] = (uint8_t)i;
        strcpy(cur.region, "US915");
        // settings that must go back to their defaults
        cur.mqttEn = 1; strcpy(cur.mqttHost, "broker"); strcpy(cur.mqttPass, "mqtt-pw");
        cur.httpsEn = 1; cur.altitudeM = 1234; cur.setupDone = 1; cur.wuIntervalSec = 60; strcpy(cur.wuStationKey, "wu-key");
        cur.calTempDc = -5; strcpy(cur.rfNames, "17000008:Garten");

        Config fresh;
        const Config d;
        settingsResetKeptInto(cur, fresh);
        assert(fresh.provisioned == 1 && fresh.uiLang == 2 && !strcmp(fresh.deviceName, "garten"));
        assert(fresh.wifiEn == 1 && fresh.webApiEn == 1 && fresh.staticIpEn == 1);
        assert(!strcmp(fresh.staticIp, "172.24.10.107") && !strcmp(fresh.staticGateway, "172.24.10.1"));
        assert(!strcmp(fresh.staticSubnet, "255.255.255.0") && !strcmp(fresh.staticDns, "1.1.1.1"));
        assert(fresh.apMode == 2 && !strcmp(fresh.apSsid, "Station") && !strcmp(fresh.apPass, "secret123") && fresh.apChannel == 6);
        assert(!strcmp(fresh.webAdminPass, "admin-pw"));
        assert(fresh.loraEn == 1 && fresh.joinEui == cur.joinEui && fresh.devEui == cur.devEui);
        assert(!memcmp(fresh.appKey, cur.appKey, 16) && !strcmp(fresh.region, "US915"));
        assert(fresh.mqttEn == d.mqttEn && !fresh.mqttHost[0] && !fresh.mqttPass[0]);
        assert(fresh.httpsEn == d.httpsEn && fresh.altitudeM == d.altitudeM && fresh.setupDone == 0);
        assert(fresh.wuIntervalSec == d.wuIntervalSec && !fresh.wuStationKey[0]);
        assert(fresh.calTempDc == d.calTempDc && !fresh.rfNames[0]);
        // a never-provisioned node: the reset makes it a provisioned one (no portal)
        Config blank, fresh2;
        settingsResetKeptInto(blank, fresh2);
        assert(fresh2.provisioned == 1);
    }
    {   // the seal: waits for a session already open, refuses new ones; "Reset settings"
        // seals again until true and clears only then - after the open session's last write
        std::atomic<bool> opened(false), release(false), wrote(false);
        std::thread writer([&] {
            StorageWrite w("config");
            assert(w.ok());
            opened = true;
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            wrote = true;   // a key written after the first seal timed out
        });
        while (!opened) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(!storageSealed());
        assert(!storageSeal(20));   // the writer's session is still open: timed out
        assert(storageSealed());
        {
            StorageWrite late("records");
            assert(!late.ok());       // no new session after the seal
        }
        std::thread releaser([&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); release = true; });
        assert(storageSeal(5000));    // returns once the open session closed
        assert(wrote);                // so the clear that follows comes after its write
        writer.join();
        releaser.join();
        StorageWrite again("cupload", "cfgdata");
        assert(!again.ok());
    }
    std::puts("settings reset ok");
    return 0;
}
