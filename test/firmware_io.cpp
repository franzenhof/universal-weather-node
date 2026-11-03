#include "board_io.h"
#include "sensors.h"
#include "globals.h"
#include "params.h"
#include "LoRaWAN_ESP32.h"
#include "driver/pcnt.h"
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
    assert(hostAttached[PIN_HEATER] && hostAttached[PIN_FAN]);
    gCfg.outMinBattDv=124;
    ioPoll();
    assert(gIo.battLockout && !gIo.heaterOn && !gIo.fanOn);
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
    gCfg.heaterMode=gCfg.fanMode=1;ioPoll();ioSample();
    assert(!gIo.heaterOn && !gIo.fanOn); // latched until restart
}
