#include "setup_wizard.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {
const char* const FIELDS[WIZ_STEPS] = {
    "uilang",                                                   // 1 language
    "rfen,rfproto,rfnm_*",                                      // 2 wireless station (states: /api/rf/sensor)
    "devname,lat,lon,altitude,tz",                              // 3 location
    "unitsys,windunit",                                         // 4 units
    "webadminpw",                                               // 5 admin password
    "mqtten,mqtthost,mqttport,mqttuser,mqttpass,mqtttls,"       // 6 outputs
    "awkuser,awkpass,wuid,wukey,wcid,wckey,wnpid,wnpkey,windyid,windypw,osemid,osemtok,osemmap,"
    "loxen,loxhost,loxport",
    "",                                                         // 7 summary
};
}

int wizardClampStep(int step) { return step < 1 ? 1 : step > WIZ_STEPS ? WIZ_STEPS : step; }

const char* wizardStepFields(int step)
{
    return step >= 1 && step <= WIZ_STEPS ? FIELDS[step - 1] : "";
}

bool wizardFieldAllowed(int step, const char* name)
{
    static const char* const META[] = { "form", "wizard", "wstep", "wstay" };
    for (const char* m : META) if (!strcmp(name, m)) return true;
    const size_t nameLen = strlen(name);
    for (const char* f = wizardStepFields(step); *f;) {
        const char* end = strchr(f, ',');
        const size_t len = end ? (size_t)(end - f) : strlen(f);
        if (len && f[len - 1] == '*') {   // a prefix
            if (nameLen >= len - 1 && !strncmp(name, f, len - 1)) return true;
        } else if (!strncmp(name, f, len) && (!name[len] || !strcmp(name + len, "_clear"))) {
            return true;
        }
        if (!end) break;
        f = end + 1;
    }
    return false;
}

bool wizardAutoStart(const Config& c, bool shownThisSession) { return !c.setupDone && !shownThisSession; }

void wizardCookieValue(uint32_t bootId, char* out, size_t len)
{
    snprintf(out, len, "wnSetup=%08lx", (unsigned long)bootId);
}

bool wizardCookieShown(const char* h, uint32_t bootId)
{
    if (!h) return false;
    char want[20];
    wizardCookieValue(bootId, want, sizeof(want));
    const size_t n = strlen(want);
    for (const char* p = h; *p;) {
        while (*p == ' ' || *p == ';') p++;   // "a=1; b=2"
        const char* end = strchr(p, ';');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !strncmp(p, want, n)) return true;
        if (!end) break;
        p = end;
    }
    return false;
}

bool wizardOpenAccess(const Config& c) { return !c.webAdminPass[0] && !c.setupDone; }

bool coordParse(const char* in, float limit, char* out, size_t outLen)
{
    char v[24];
    const size_t n = strlen(in);
    if (n >= sizeof(v) || n >= outLen) return false;
    memcpy(v, in, n + 1);
    char* comma = strchr(v, ',');
    if (comma) {   // the decimal comma: one, and not together with a point
        if (strchr(comma + 1, ',') || strchr(v, '.')) return false;
        *comma = '.';
    }
    if (v[0]) {
        char* end = nullptr;
        const float f = strtof(v, &end);
        if (*end != '\0' || !isfinite(f) || f < -limit || f > limit) return false;
    }
    memcpy(out, v, n + 1);
    return true;
}
