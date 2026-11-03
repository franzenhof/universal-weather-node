// Host-side helper for vedirect.test.js: feeds the byte stream from stdin
// through the firmware's VE.Direct parser (src/vedirect.cpp) and prints the
// result as JSON.
#include <math.h>
#include <stdio.h>
#include "vedirect.h"

static void num(const char* key, float v, bool last = false)
{
    if (isnan(v)) printf("  \"%s\": null%s\n", key, last ? "" : ",");
    else          printf("  \"%s\": %.3f%s\n", key, v, last ? "" : ",");
}

int main()
{
    VeDirectParser p;
    int c, blocks = 0;
    while ((c = getchar()) != EOF)
        if (p.feed((uint8_t)c)) blocks++;
    const VeDirectValues& v = p.values();
    printf("{\n  \"blocks\": %d,\n  \"errors\": %u,\n", blocks, (unsigned)p.checksumErrors());
    num("battV", v.battV);  num("battA", v.battA);  num("pvV", v.pvV);  num("pvW", v.pvW);
    num("yield", v.yieldTodayKwh);  num("soc", v.socPct);  num("load", v.loadA);
    printf("  \"cs\": %d,\n  \"err\": %d\n}\n", v.chargeState, v.error);
    return 0;
}
