#include "sensors.h"
#include "globals.h"
#include "DallasTemperature.h"
#include "wind_window.h"
#include "driver/pcnt.h"
#include <cassert>
#include <climits>
#include <cmath>

extern bool hostBmeOk;
extern float hostBmeT,hostBmeP,hostBmeH;

int main()
{
    gCfg.pcbVersion=20;
    sensorsInit(getPins(20));
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
    gCfg.pcbVersion=12;
    hostConversionComplete=true;
    startOneWireConversion();
    assert(pollOneWireConversion() && bus1Temps[0]==10.5f);

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
    sensorsInit(getPins(20));                 // counters back to 0 for the wrap tests below

    // ---- PCNT: any number of counter wraps between two reads ----
    gCfg.windMsPerPps=0.001f;   // high-pulse anemometer
    hostCounts[PCNT_UNIT_WINDSPEED]=0;readPulseCounters();
    hostPcntIsr[PCNT_UNIT_WINDSPEED](hostPcntArg[PCNT_UNIT_WINDSPEED]);
    hostPcntIsr[PCNT_UNIT_WINDSPEED](hostPcntArg[PCNT_UNIT_WINDSPEED]);
    hostCounts[PCNT_UNIT_WINDSPEED]=100;hostNow+=60000;
    PulseSample ps=readPulseCounters();
    assert(ps.windValid && ps.windPulses==2u*32767u+100u);   // > 65535: no uint16 truncation
    // wrapped, but the interrupt has not run yet: counted once, not twice
    hostCounts[PCNT_UNIT_WINDSPEED]=50;hostNow+=1000;
    ps=readPulseCounters();assert(ps.windPulses==32767u-50u);
    hostPcntIsr[PCNT_UNIT_WINDSPEED](hostPcntArg[PCNT_UNIT_WINDSPEED]);
    hostCounts[PCNT_UNIT_WINDSPEED]=60;hostNow+=1000;
    ps=readPulseCounters();assert(ps.windPulses==10u);
    gCfg.windMsPerPps=1.00584f;

    // ---- BME280: out-of-range readings count as a failed read ----
    hostBmeOk=true;hostBmeT=21.5f;hostBmeP=95000.0f;hostBmeH=55.0f;
    assert(readBme280() && bme280Present && bme280TempC==21.5f && bme280PressAbs==950.0f);
    hostBmeH=150.0f;
    assert(!readBme280() && !bme280Present && bme280Humidity==55.0f);
    hostBmeH=55.0f;hostBmeT=-60.0f;assert(!readBme280());
    hostBmeT=21.5f;hostBmeP=20000.0f;assert(!readBme280());
    hostBmeP=95000.0f;assert(readBme280() && bme280Present);   // re-probed
    hostBmeOk=false;

    // ---- rain reset: the per-path windows keep counting ----
    rainMMSinceStart=5.0f;
    RainWindow w;w.reset(rainWindowTotalMm(),0);
    rainMMSinceStart+=0.4f;                  // rain before the reset
    resetRainAccumulator();
    assert(rainMMSinceStart==0.0f);
    rainMMSinceStart+=0.2f;                  // and after it
    assert(std::fabs(w.amount(rainWindowTotalMm())-0.6f)<0.001f);
}
