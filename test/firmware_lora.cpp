#include "lora_transport.h"
#include "globals.h"
#include <cassert>
#include <climits>

int main()
{
    gCfg.loraEn=0;loraPoll();
    assert(hostRadioBegins==0);
    gCfg.loraEn=1;hostRadioResult=-1;loraPoll();
    assert(hostRadioBegins==1 && !loraReady() && hostJoinCalls==0);
    loraPoll();assert(hostRadioBegins==1);
    hostNow+=60000;hostRadioResult=0;hostProvisionOk=false;loraPoll();
    assert(hostRadioBegins==2 && hostJoinCalls==0);
    hostNow+=60000;hostProvisionOk=true;loraPoll();
    assert(hostJoinCalls==1 && !loraReady() && hostNodes==0);
    for(int i=0;i<4;i++) { hostNow+=60000;loraPoll();assert(hostNodes==0); }
    hostJoinOk=true;hostNow+=60000;loraPoll();
    assert(loraReady() && hostNodes==1);
    int calls=hostJoinCalls;hostNow+=60000;loraPoll();
    assert(hostJoinCalls==calls);
    hostNow=ULONG_MAX-30000;loraReconnect();
    assert(!loraReady() && hostNodes==0 && hostSessionSaves==1);
    hostNow+=59999;loraPoll();assert(hostNodes==0);
    hostNow++;loraPoll();assert(loraReady());
    loraReconnect();assert(hostNodes==0);
}
