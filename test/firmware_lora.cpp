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
    // failed joins back off: next attempt after 2, then 4 min
    hostNow+=60000;loraPoll();assert(hostJoinCalls==1);
    hostNow+=60000;loraPoll();assert(hostJoinCalls==2 && hostNodes==0);
    hostNow+=4*60000-1;loraPoll();assert(hostJoinCalls==2);
    hostNow++;loraPoll();assert(hostJoinCalls==3 && hostNodes==0);
    hostJoinOk=true;hostNow+=8*60000;loraPoll();
    assert(loraReady() && hostNodes==1);
    assert(loRaNode->dutyCycle);   // regional duty-cycle limit enforced after the join
    int calls=hostJoinCalls;hostNow+=60000;loraPoll();
    assert(hostJoinCalls==calls);
    hostNow=ULONG_MAX-30000;loraReconnect();
    assert(!loraReady() && hostNodes==0 && hostSessionSaves==1);
    hostNow+=59999;loraPoll();assert(hostNodes==0);
    hostNow++;loraPoll();assert(loraReady());
    loraReconnect();assert(hostNodes==0);
    // a long outage: the retry interval doubles and is capped at 60 min
    hostJoinOk=false;
    unsigned long wait=60000;
    for(int i=0;i<8;i++) {
        int before=hostJoinCalls;
        hostNow+=wait-1;loraPoll();assert(hostJoinCalls==before);
        hostNow++;loraPoll();assert(hostJoinCalls==before+1 && !loraReady());
        wait = wait*2 > 3600000UL ? 3600000UL : wait*2;
    }
    assert(wait==3600000UL);
    // success resets the interval: after the next reconnect a retry follows in 60 s
    hostJoinOk=true;hostNow+=wait;loraPoll();assert(loraReady());
    loraReconnect();hostJoinOk=false;
    hostNow+=60000;loraPoll();assert(!loraReady());
    int c=hostJoinCalls;hostNow+=120000;loraPoll();assert(hostJoinCalls==c+1);
}
