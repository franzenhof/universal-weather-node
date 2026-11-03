#include "json_payload.h"
#include "globals.h"
#include "board_io.h"
#include <cassert>
#include <string>
#include <vector>

int main()
{
    gCfg.pcbVersion=20;
    gCfg.lipoEn=gCfg.debugMode=gCfg.uvEn=gCfg.uvIndexEn=gCfg.auxAinEn=gCfg.auxPulseEn=1;
    gCfg.heaterMode=gCfg.fanMode=2;gCfg.tachoEn=gCfg.veDirectEn=1;
    bme280Present=true;bus1Count=3;
    windSpeedMs=7.123456f;windGustMs=12.34567f;
    rainMM=.2f;rainRateMmH=2.412345f;rainMMSinceStart=8123.4f;
    windDirection=338;batteryVolt=4.12f;cpuTemperature=42.3f;
    bme280TempC=23.45678f;bme280PressAbs=950.1234f;bme280PressSea=1015.8123f;bme280Humidity=49.56789f;
    for(int i=0;i<3;i++) bus1Temps[i]=10.5f+2*i;
    gIo.uvSensorVolts=2.456f;gIo.uvIndex=5.67f;gIo.uvIndexCalibrated=true;
    gIo.auxAinV=4.321f;gIo.auxPulsesCycle=1234;gIo.auxPulsesTotal=12345678;
    gIo.heaterOn=gIo.fanOn=true;gIo.heaterDuty=60;gIo.fanDuty=100;
    gIo.fanRpm=2950;gIo.veValid=true;
    gIo.ve={13.28f,-1.23f,37.5f,45.0f,.12f,NAN,.31f,3,0};
    JsonDocument doc;buildJsonPayload(doc);
    size_t n=measureJson(doc);
    assert(n>1024);
    std::vector<char> buf(n+1);
    assert(serializeJson(doc,buf.data(),buf.size())==n);
    JsonDocument parsed;
    assert(!deserializeJson(parsed,buf.data(),n));
    assert(parsed["victron"]["batteryV"].as<float>()==gIo.ve.battV);
    assert(parsed["status"]["pcbRev"].as<int>()==20);
    assert(parsed["uvIndex"].as<float>()==gIo.uvIndex);
    gCfg.uvEn=gCfg.auxAinEn=gCfg.auxPulseEn=0;gCfg.wind2Mode=2;
    buildJsonPayload(doc);
    assert(!doc["wind2"].isNull() && doc["uvIndex"].isNull());
}
