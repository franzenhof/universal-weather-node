#include "lora_payload.h"
#include "board_io.h"
#include "globals.h"
#include <cassert>

int main()
{
    gCfg.pcbVersion=20;
    gCfg.windEn=gCfg.rainEn=0;
    gIo.wind2SpeedMs=4.56f;gIo.wind2GustMs=9.87f;gIo.wind2DirDeg=271;
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
}
