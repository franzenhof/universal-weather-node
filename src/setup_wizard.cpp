#include "setup_wizard.h"

namespace {
const char* const FIELDS[WIZ_STEPS] = {
    "uilang",                                                   // 1 language
    "rfen",                                                     // 2 wireless station (states: /api/rf/sensor)
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

bool wizardAutoStart(const Config& c, bool shownThisSession) { return !c.setupDone && !shownThisSession; }

bool wizardOpenAccess(const Config& c) { return !c.webAdminPass[0] && !c.setupDone; }
