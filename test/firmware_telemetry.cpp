#include "json_payload.h"
#include "globals.h"
#include "board_io.h"
#include "rf_sensors.h"
#include "sources.h"
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
    srcUpdateCycle();
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
    assert(parsed["cpuTemp"].as<float>()==42.3f && parsed["resetReason"].as<std::string>()=="unknown");
    gCfg.debugMode=0;buildJsonPayload(doc);   // CPU temperature and restart cause also without debug mode
    assert(doc["cpuTemp"].as<float>()==42.3f && !doc["resetReason"].isNull() && doc["freeHeapKiB"].isNull());
    gCfg.debugMode=1;
    // wireless sensors: only with rfEn, only the groups the sensor sent
    buildJsonPayload(doc);
    assert(doc["rfSensors"].isNull());
    gCfg.rfEn=1;
    RfSensorReading& r=gRfSensors[0];
    r.used=true;r.id=0x2A5B;r.type=1;r.rssi=-71.4f;r.lastMs=millis();r.batteryOk=false;r.tempMs=r.humMs=r.windMs=r.rainMs=r.uvMs=r.lightMs=millis();
    r.hasTemp=r.hasHumidity=r.hasWind=r.hasRain=r.hasUv=r.hasLight=true;
    r.tempC=4.5f;r.humidity=88;r.windAvgMs=3.1f;r.windGustMs=6.2f;r.windDirDeg=247.4f;r.rainTotalMm=123.4f;r.uvIndex=1.2f;r.lightLux=5321.6f;
    buildJsonPayload(doc);
    JsonObject rf=doc["rfSensors"][0];
    assert(rf["id"].as<std::string>()=="00002A5B" && rf["rssi"].as<int>()==-71 && rf["batteryOk"].as<bool>()==false);
    assert(rf["temp"].as<float>()==4.5f && rf["humidityPct"].as<int>()==88 && rf["windDirDeg"].as<int>()==247);
    assert(rf["lightLux"].as<long>()==5322 && !rf["rainTotal"].isNull() && doc["rfSensors"].size()==1);
    // value sources: Auto keeps the board BME280, explicit wireless 1 takes the sensor
    srcUpdateCycle();buildJsonPayload(doc);
    assert(doc["sources"]["tempHum"].as<std::string>()=="board" && doc["sources"]["light"].as<std::string>()=="radio:00002A5B");
    assert(doc["lightLux"].as<float>()==5321.6f && doc["radioBatteryLow"].as<bool>());
    gCfg.srcSel[SG_AIR]=SRC_RF1;srcUpdateCycle();buildJsonPayload(doc);
    assert(doc["temp"].as<float>()==4.5f && doc["humidityPct"].as<float>()==88 && !doc["pressureQnh"].isNull());
    gCfg.srcSel[SG_AIR]=SRC_OFF;srcUpdateCycle();buildJsonPayload(doc);
    assert(doc["temp"].isNull() && doc["sources"]["tempHum"].as<std::string>()=="none");
    // rain: only increases of the selected source reach the running total
    gCfg.rainEn=1;gCfg.srcSel[SG_RAIN]=SRC_AUTO;float before=rainMMSinceStart;
    srcBoardRain(0.2f);srcRfPacket(0,false,1.0f);
    assert(rainMMSinceStart-before>0.19f && rainMMSinceStart-before<0.21f);
    gCfg.srcSel[SG_RAIN]=SRC_RF1;before=rainMMSinceStart;
    srcBoardRain(0.2f);srcRfPacket(0,false,1.0f);
    assert(rainMMSinceStart-before>0.99f && rainMMSinceStart-before<1.01f);
    gCfg.srcSel[SG_AIR]=gCfg.srcSel[SG_RAIN]=SRC_AUTO;
    gRfSensors[1].used=true;gRfSensors[1].id=0x99;   // a thermo-only sensor: no weather groups
    buildJsonPayload(doc);
    assert(doc["rfSensors"].size()==2 && doc["rfSensors"][1]["windSpeed"].isNull());
    // Fine Offset extras: model, WH31 channel, soil moisture, lightning
    assert(doc["rfSensors"][0]["model"].isNull() && doc["rfSensors"][0]["channel"].isNull());
    assert(doc["rfSensors"][0]["supercapV"].isNull() && doc["rfSensors"][1]["supercapV"].isNull());   // only WS85/WS90
    gRfSensors[1].proto=RF_PROTO_FINEOFFSET;gRfSensors[1].model="WH31";gRfSensors[1].channel=3;
    gRfSensors[1].hasMoisture=true;gRfSensors[1].moisturePct=36;
    gRfSensors[1].hasLightning=true;gRfSensors[1].lightningCount=7;gRfSensors[1].lightningKm=0;
    buildJsonPayload(doc);
    {
        JsonObject fo=doc["rfSensors"][1];
        assert(fo["protocol"].as<std::string>()=="Fine Offset" && fo["model"].as<std::string>()=="WH31" && fo["channel"].as<int>()==3);
        assert(fo["soilMoisturePct"].as<int>()==36 && fo["lightningCount"].as<int>()==7 && fo["lightningDistKm"].isNull());
        assert(fo["supercapV"].isNull());
        gRfSensors[1].hasSupercap=true;gRfSensors[1].supercapV=3.84f;buildJsonPayload(doc);
        assert(fabs(doc["rfSensors"][1]["supercapV"].as<float>()-3.8f)<0.01f);
        gRfSensors[1].hasSupercap=false;
        gRfSensors[1].lightningKm=12;buildJsonPayload(doc);
        assert(doc["rfSensors"][1]["lightningDistKm"].as<int>()==12);
    }
    // per-value freshness: the station keeps sending wind, the temperature/
    // humidity part fails -> after 5 min only temperature/humidity drop out
    gCfg.srcSel[SG_AIR]=SRC_RF1;gCfg.srcSel[SG_WIND]=SRC_RF1;gCfg.windEn=0;
    srcUpdateCycle();assert(gSrc.tempValid && gSrc.humValid && srcIsRadio(SG_WIND));
    delay(SRC_RF_FRESH_MS+1000);
    r.windMs=r.lastMs=millis();                       // only wind packets arrived meanwhile
    srcUpdateCycle();buildJsonPayload(doc);
    assert(!gSrc.tempValid && !gSrc.humValid && srcIsRadio(SG_WIND) && !srcActive(SG_AIR));
    assert(doc["temp"].isNull() && doc["sources"]["tempHum"].as<std::string>()=="none");
    JsonArray st=doc["rfSensors"][0]["stale"];
    assert(!st.isNull() && st[0].as<std::string>()=="temp" && st[1].as<std::string>()=="humidity");
    gCfg.srcSel[SG_AIR]=SRC_AUTO;srcUpdateCycle();   // Auto falls back to the board BME280
    assert(gSrc.tempValid && srcResolve(SG_AIR)==SRC_IS_BOARD);
    gCfg.srcSel[SG_WIND]=SRC_AUTO;
    gCfg.rfEn=0;
    gCfg.uvEn=gCfg.auxAinEn=gCfg.auxPulseEn=0;gCfg.wind2Mode=2;
    srcUpdateCycle();buildJsonPayload(doc);
    assert(!doc["wind2"].isNull() && doc["uvIndex"].isNull());
}
