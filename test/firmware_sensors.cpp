#include "heltec_board.h"
#include "sensors.h"
#include "globals.h"
#include "DallasTemperature.h"
#include "wind_window.h"
#include "pulse_counter.h"
extern uint32_t hostCounts[PULSE_COUNTER_COUNT];
#include <cassert>
#include <climits>
#include <cmath>

extern bool hostBmeOk;
extern float hostBmeT,hostBmeP,hostBmeH;
extern int hostBmeSda,hostBmeAddr;

int main()
{
    gCfg.pcbVersion=20; gBoard.pcb=true;   // PCB v2.0 (revision ID read)
    sensorsInit(getPins());
    hostProbeCount=3;
    startOneWireConversion();
    assert(digitalRead(PIN_1WIRE_HSS)==HIGH);
    hostNow+=999;
    assert(!pollOneWireConversion());
    hostNow++;
    assert(pollOneWireConversion());
    assert(digitalRead(PIN_1WIRE_HSS)==LOW);
    for(int i=0;i<3;i++) assert(bus1Temps[i]==DEVICE_DISCONNECTED_C);

    hostConversionComplete=true;
    startOneWireConversion();
    assert(pollOneWireConversion());
    assert(bus1Temps[0]==10.5f && bus1Temps[2]==14.5f);
    assert(digitalRead(PIN_1WIRE_HSS)==LOW);

    hostProbeCount=1;hostConversionComplete=false;
    startOneWireConversion();
    assert(bus1Temps[0]==10.5f && bus1Temps[2]==DEVICE_DISCONNECTED_C);
    hostNow+=1000;
    assert(pollOneWireConversion() && bus1Temps[0]==DEVICE_DISCONNECTED_C);

    hostProbeCount=0;
    startOneWireConversion();
    assert(pollOneWireConversion() && bus1Count==0);
    assert(digitalRead(PIN_1WIRE_HSS)==LOW);

    hostProbeCount=1;hostConversionComplete=false;
    hostNow=ULONG_MAX-500;
    startOneWireConversion();
    hostNow+=1000;
    assert(pollOneWireConversion());
    assert(bus1Temps[0]==DEVICE_DISCONNECTED_C);
    gBoard.pcb=false;                         // supply not switched (no HSS step)
    hostConversionComplete=true;
    startOneWireConversion();
    assert(pollOneWireConversion() && bus1Temps[0]==10.5f);
    gBoard.pcb=true;

    // ---- settling after the start: wind and rain pulses ignored for PULSE_SETTLE_MS ----
    gCfg.windEn=1;gCfg.rainEn=1;
    assert(pulseSettling());                  // before the end of setup()
    pulseSettleStart();
    hostCounts[PCNT_UNIT_WINDSPEED]=0;hostCounts[PCNT_UNIT_RAIN]=0;readPulseCounters();
    hostCounts[PCNT_UNIT_WINDSPEED]=63;hostCounts[PCNT_UNIT_RAIN]=5;hostNow+=1000;   // the burst: 63 pulses in 1 s = 226 km/h
    {
        const float before=rainMMSinceStart;
        PulseSample s0=readPulseCounters();
        assert(!s0.windValid && s0.rainMm==0.0f && rainMMSinceStart==before);
    }
    hostNow+=PULSE_SETTLE_MS;
    assert(!pulseSettling());
    sensorsInit(getPins());                   // counters back to 0 for the wrap tests below

    // ---- PCNT: accumulated counts across hardware overflows ----
    gCfg.windMsPerPps=0.001f;   // high-pulse anemometer
    hostCounts[PCNT_UNIT_WINDSPEED]=0;readPulseCounters();
    hostCounts[PCNT_UNIT_WINDSPEED]=2u*32767u+100u;hostNow+=60000;
    PulseSample ps=readPulseCounters();
    assert(ps.windValid && ps.windPulses==2u*32767u+100u);   // > 65535: no uint16 truncation
    // 32-bit count wrapping around between two reads
    hostCounts[PCNT_UNIT_WINDSPEED]=0xFFFFFFF0u;readPulseCounters();
    hostCounts[PCNT_UNIT_WINDSPEED]=20u;hostNow+=1000;
    ps=readPulseCounters();assert(ps.windPulses==36u);
    gCfg.windMsPerPps=1.00584f;
    // hardware wrapped, but the driver's overflow interrupt has not run yet
    // (count reads 32767 too low): counted once, not twice
    assert(pulseCounterMonotonic(32767u+10u, 32767u+5u)==32767u+10u);
    assert(pulseCounterMonotonic(50u, 32767u+5u)==32767u+50u);     // pending overflow added
    assert(pulseCounterMonotonic(32767u+60u, 32767u+50u)==32767u+60u);   // interrupt ran: exact again
    assert(pulseCounterMonotonic(3u, 0xFFFFFFF0u)==3u);           // 32-bit wrap is not a pending overflow

    // ---- BME280: out-of-range readings count as a failed read ----
    hostBmeOk=true;hostBmeT=21.5f;hostBmeP=95000.0f;hostBmeH=55.0f;
    assert(readBme280() && bme280Present && bme280TempC==21.5f && bme280PressAbs==950.0f);
    hostBmeH=150.0f;
    assert(!readBme280() && !bme280Present && bme280Humidity==55.0f);
    hostBmeH=55.0f;hostBmeT=-60.0f;assert(!readBme280());
    hostBmeT=21.5f;hostBmeP=20000.0f;assert(!readBme280());
    hostBmeP=95000.0f;assert(readBme280() && bme280Present);   // re-probed
    hostBmeOk=false;

    // ---- BME280 pins (B9): PCB 41/42; wireless 47/48, old wiring 41/42 only while GNSS is off ----
    {
        int sda,scl; bool old; uint8_t addr;
        sensorsBmePins(sda,scl,old,addr); assert(sda==41 && scl==42 && !old);   // PCB (above)
        const bool pcb=gBoard.pcb; const int pcbVer=gCfg.pcbVersion;
        gBoard.pcb=false; gCfg.pcbVersion=0; gCfg.gnssEn=0;
        hostBmeOk=true; hostBmeSda=41; hostBmeAddr=0x76;               // old wiring
        sensorsInit(getPins()); sensorsBmePins(sda,scl,old,addr);
        assert(bme280Present && sda==41 && scl==42 && old && addr==0x76);
        gCfg.gnssEn=1;                                                  // GNSS on: 41/42 never touched
        sensorsInit(getPins()); sensorsBmePins(sda,scl,old,addr);
        assert(!bme280Present && sda==47 && scl==48 && !old);
        hostBmeSda=47; hostBmeAddr=0x77;                                // moved to 47/48, SDO high
        assert(readBme280() && bme280Present);                          // hot-plug on 47/48, 0x77 found
        sensorsInit(getPins()); sensorsBmePins(sda,scl,old,addr);
        assert(bme280Present && sda==47 && scl==48 && !old && addr==0x77);
        gCfg.gnssEn=0; hostBmeOk=false;                                 // nothing anywhere: waits on 47/48
        sensorsInit(getPins()); sensorsBmePins(sda,scl,old,addr);
        assert(!bme280Present && sda==47 && scl==48 && !old);
        hostBmeSda=-1; hostBmeAddr=-1;
        gBoard.pcb=pcb; gCfg.pcbVersion=pcbVer;
        sensorsInit(getPins());
    }

    // ---- rain reset: the per-path windows keep counting ----
    rainMMSinceStart=5.0f;
    RainWindow w;w.reset(rainWindowTotalMm(),0);
    rainMMSinceStart+=0.4f;                  // rain before the reset
    resetRainAccumulator();
    assert(rainMMSinceStart==0.0f);
    rainMMSinceStart+=0.2f;                  // and after it
    assert(std::fabs(w.amount(rainWindowTotalMm())-0.6f)<0.001f);
}
