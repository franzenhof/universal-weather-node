#include "board_io.h"
#include "sensors.h"
#include "globals.h"
#include "params.h"
#include "LoRaWAN_ESP32.h"
#include "driver/pcnt.h"
#include <esp_timer.h>
#include <cassert>

extern Config hostSavedConfig;
int main()
{
    gCfg.pcbVersion=20;
    sensorsInit(getPins(20));
    ioInit();
    gCfg.wind2Mode=1;
    assert(ioAnyEnabled());
    ioPoll();
    hostCounts[2]=6;
    hostNow+=3000;
    ioPoll(); ioSample(); ioFinishCycle(3000);
    assert(gIo.wind2SpeedMs>0);
    gCfg.wind2Mode=0;

    gCfg.heaterMode=gCfg.fanMode=1;
    gCfg.heaterDutyPct=60;gCfg.fanDutyPct=75;
    ioPoll();
    assert(gIo.heaterDuty==60 && gIo.fanDuty==75);
    assert(!hostAttached[PIN_HEATER] && hostAttached[PIN_FAN]);
    assert(hostPwmFrequencies()[7]==100);
    assert(hostHeaterTimer().running && hostHeaterTimer().periodUs==100000);
    assert(hostPins[PIN_HEATER]==HIGH);
    unsigned long heaterStart=hostNow;
    hostNow=heaterStart+5999;hostRunTimers();
    assert(hostPins[PIN_HEATER]==HIGH);
    hostNow=heaterStart+6000;hostRunTimers();
    assert(hostPins[PIN_HEATER]==LOW);
    assert(gIo.heaterOn && gIo.heaterDuty==60); // the off window must not permit light sleep
    hostNow=heaterStart+9999;hostRunTimers();
    assert(hostPins[PIN_HEATER]==LOW);
    hostNow=heaterStart+10000;hostRunTimers();
    assert(hostPins[PIN_HEATER]==HIGH);
    // Duty changes retain the period; timer callbacks need no ioPoll().
    hostNow=heaterStart+12000;hostRunTimers();
    gCfg.heaterDutyPct=30;ioPoll();
    hostNow=heaterStart+13000;hostRunTimers();
    assert(hostPins[PIN_HEATER]==LOW);
    int onTicks=0;
    for (int tick=0;tick<100;++tick) {
        hostNow=heaterStart+20000+tick*100;
        hostRunTimers();
        if (hostPins[PIN_HEATER]==HIGH) ++onTicks;
    }
    assert(onTicks==30);
    hostNow=heaterStart+55000;hostRunTimers();
    assert(hostPins[PIN_HEATER]==LOW);
    gCfg.heaterDutyPct=100;ioPoll();
    assert(!hostHeaterTimer().running && hostPins[PIN_HEATER]==HIGH);
    gCfg.heaterMode=0;ioPoll();
    assert(!hostHeaterTimer().running && hostPins[PIN_HEATER]==LOW);
    gCfg.heaterMode=1;gCfg.heaterDutyPct=1;ioPoll();
    assert(hostPins[PIN_HEATER]==HIGH);
    hostNow+=100;hostRunTimers();
    assert(hostPins[PIN_HEATER]==LOW);
    gCfg.heaterDutyPct=99;ioPoll();
    heaterStart=hostNow-100;
    hostNow=heaterStart+9899;hostRunTimers();
    assert(hostPins[PIN_HEATER]==HIGH);
    hostNow=heaterStart+9900;hostRunTimers();
    assert(hostPins[PIN_HEATER]==LOW);
    gCfg.heaterMode=2;gCfg.heaterDutyPct=100;
    bme280Present=true;bme280TempC=-3;ioSample();
    assert(gIo.heaterDuty==55 && hostHeaterTimer().running);
    bme280TempC=5;ioSample();
    assert(!gIo.heaterOn && !hostHeaterTimer().running && hostPins[PIN_HEATER]==LOW);
    bme280TempC=NAN;ioSample();
    assert(!gIo.heaterOn && hostPins[PIN_HEATER]==LOW);
    gCfg.heaterMode=1;
    gCfg.heaterDutyPct=60;ioPoll();
    gCfg.outMinBattDv=124;
    ioPoll();
    assert(gIo.battLockout && !gIo.heaterOn && !gIo.fanOn);
    assert(!hostHeaterTimer().running && hostPins[PIN_HEATER]==LOW);
    hostHeaterTimer().args.callback(hostHeaterTimer().args.arg);
    assert(hostPins[PIN_HEATER]==LOW); // an already dispatched callback cannot re-enable heat
    hostRunTimers();
    assert(hostPins[PIN_HEATER]==LOW);
    gCfg.veDirectEn=1;
    ioPoll();
    assert(gIo.battLockout);
    auto block=[](const char* voltage,const char* power) {
        std::string s=std::string("\r\nV\t")+voltage+"\r\nPPV\t"+power+"\r\nChecksum\t";
        uint8_t sum=0; for(unsigned char c:s) sum+=c;
        s.push_back((char)(uint8_t)(-sum)); Serial1.input+=s; ioPoll();
    };
    block("12500","20");
    assert(gIo.battLockout); // release threshold is 12.6 V
    block("12700","20");
    assert(!gIo.battLockout && gIo.heaterOn);
    block("12300","20");
    assert(gIo.battLockout);
    gCfg.outMinBattDv=0;
    bme280Present=true;bme280TempC=25;
    gCfg.fanMode=2;gCfg.fanMinPvW=10;
    ioSample();
    block("12700","5");
    assert(!gIo.fanOn);
    block("12700","20");
    assert(gIo.fanOn);
    gIo.ve.pvW=NAN;ioPoll();
    assert(!gIo.fanOn);
    block("12700","20");
    assert(gIo.fanOn);
    hostNow+=10000;ioPoll();
    assert(!gIo.fanOn);
    gCfg.veDirectEn=0;ioPoll();
    assert(!gIo.fanOn);
    gCfg.fanMinPvW=0;ioPoll();
    assert(gIo.fanOn);
    gCfg.fanDutyPct=100;ioPoll();
    assert(!hostAttached[PIN_FAN] && hostPins[PIN_FAN]==HIGH);

    assert(applyParam(PARAM_LOW_POWER_EN,1));
    assert(applyParam(PARAM_VANE_POWER_SAVE,0));
    saveConfig(gCfg);
    gCfg=Config{};
    gCfg=hostSavedConfig;
    assert(gCfg.lowPowerEn==1 && gCfg.vanePowerSave==0);

    ioSuspendOutputs();
    assert(!gIo.heaterOn && !gIo.fanOn);
    assert(!hostAttached[PIN_HEATER] && !hostAttached[PIN_FAN]);
    assert(hostPins[PIN_HEATER]==LOW && hostPins[PIN_FAN]==LOW);
    assert(!hostHeaterTimer().running);
    hostRunTimers();
    assert(hostPins[PIN_HEATER]==LOW);
    gCfg.heaterMode=gCfg.fanMode=1;ioPoll();ioSample();
    assert(!gIo.heaterOn && !gIo.fanOn); // latched until restart
}
