#include "lora_payload.h"
#include "board_io.h"
#include "globals.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include "sources.h"
#include "sensors.h"

// Data of the LPP field (channel, type) in the current buffer, or nullptr.
static const uint8_t* lppFind(uint8_t ch, uint8_t type)
{
    const uint8_t* b=lpp.getBuffer();int n=lpp.getSize();
    for (int i=0;i+1<n;) {
        uint8_t c=b[i],t=b[i+1];
        int sz = (t==LPP_GENERIC_SENSOR||t==LPP_DISTANCE||t==LPP_ENERGY) ? 4
               : (t==LPP_DIGITAL_INPUT||t==LPP_DIGITAL_OUTPUT||t==LPP_RELATIVE_HUMIDITY||t==LPP_PERCENTAGE) ? 1 : 2;
        if (c==ch && t==type) return b+i+2;
        i+=2+sz;
    }
    return nullptr;
}

int main()
{
    gCfg.pcbVersion=20;
    gCfg.windEn=gCfg.rainEn=0;
    gIo.wind2Valid=true;gIo.wind2SpeedMs=4.56f;gIo.wind2GustMs=9.87f;gIo.wind2DirDeg=271;
    for (int mode=1;mode<=2;mode++) {
        gCfg.wind2Mode=mode;
        buildBinaryPacket(BIN_MAX_SIZE);
        assert(binBuf[0]==BIN_PROTO_VERSION);
        uint64_t mask=0;int k=1,sh=0;   // variable-length mask: 7 bits per byte
        do { mask|=(uint64_t)(binBuf[k]&0x7F)<<sh; sh+=7; } while(binBuf[k++]&0x80);
        assert(mask & (1ull<<BIN_WIND2_SPEED));
        assert(mask & (1ull<<BIN_WIND2_GUST));
        assert((bool)(mask & (1ull<<BIN_WIND2_DIR))==(mode==2));
        buildLppPacket(200);
        auto b=lpp.getBuffer();
        assert(b[0]==2 && b[1]==LPP_DIGITAL_INPUT);
        assert(b[3]==3 && b[4]==LPP_DIGITAL_INPUT);
        int pos=6;
        if(mode==2) { assert(b[pos]==2 && b[pos+1]==LPP_DIRECTION);pos+=4; }
        assert(b[pos]==6 && b[pos+1]==LPP_ANALOG_INPUT);pos+=4;
        assert(b[pos]==7 && b[pos+1]==LPP_ANALOG_INPUT);
    }
    gCfg.wind2Mode=0;

    // LPP Generic Sensor is exact above 2^24 (AUX pulse total)
    gCfg.auxPulseEn=1;gIo.auxPulsesCycle=7;gIo.auxPulsesTotal=4000000123u;
    buildLppPacket(200);
    const uint8_t* t=lppFind(11,LPP_GENERIC_SENSOR);
    assert(t && ((uint32_t)t[0]<<24|(uint32_t)t[1]<<16|(uint32_t)t[2]<<8|t[3])==4000000123u);
    gCfg.auxPulseEn=0;

    // NAN never goes on air as a number (LPP): a failed LiPo/VE reading is left out
    gCfg.lipoEn=1;batteryVolt=NAN;gCfg.veDirectEn=1;gIo.veValid=true;
    gIo.ve={NAN,-1.0f,NAN,NAN,NAN,NAN,NAN,3,0};
    buildLppPacket(200);
    assert(!lppFind(1,LPP_VOLTAGE) && !lppFind(30,LPP_VOLTAGE) && lppFind(30,LPP_ANALOG_INPUT));
    gCfg.lipoEn=0;gCfg.veDirectEn=0;gIo.veValid=false;batteryVolt=0.0f;

    // UV flags: bit 1 only when the index value is actually in the uplink
    gCfg.uvEn=1;gSrc.uvBoard=true;gIo.uvSensorVolts=1.0f;gIo.uvIndexCalibrated=true;gIo.uvIndex=-0.5f;
    buildLppPacket(200);
    const uint8_t* f=lppFind(14,LPP_DIGITAL_INPUT);
    assert(f && *f==0x01 && !lppFind(13,LPP_ANALOG_INPUT));
    gIo.uvIndex=2.0f;buildLppPacket(200);
    f=lppFind(14,LPP_DIGITAL_INPUT);
    assert(f && *f==0x03 && lppFind(13,LPP_ANALOG_INPUT));
    gCfg.uvEn=0;gSrc.uvBoard=false;

    // wind window without a valid sample: wind left out, not the previous values
    gCfg.windEn=1;gCfg.srcSel[SG_WIND]=SRC_AUTO;srcUpdateCycle();
    PathValues pv;pv.windValid=false;pv.windSpeedMs=0;
    buildLppPacket(200,&pv);
    assert(!lppFind(1,LPP_DIRECTION) && !lppFind(1,LPP_ANALOG_INPUT));
    pv.windValid=true;pv.windSpeedMs=5.0f;pv.windDirDeg=90;
    buildLppPacket(200,&pv);
    assert(lppFind(1,LPP_DIRECTION) && lppFind(1,LPP_ANALOG_INPUT));

    // "left out" list at a tiny limit: complete names, "..." when it is full
    gCfg.debugMode=1;gCfg.lipoEn=1;batteryVolt=3.9f;
    buildLppPacket(3,&pv);
    size_t n=strlen(lastPayloadDropped);
    assert(n>0 && n<sizeof(lastPayloadDropped));
    buildBinaryPacket(3,&pv);
    assert(strlen(lastPayloadDropped)<sizeof(lastPayloadDropped) && strstr(lastPayloadDropped,"debug"));
}
