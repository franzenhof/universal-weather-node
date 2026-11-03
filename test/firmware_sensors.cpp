#include "sensors.h"
#include "globals.h"
#include "DallasTemperature.h"
#include <cassert>
#include <climits>

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
}
