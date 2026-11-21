// Setup wizard (src/setup_wizard.cpp): steps, auto start, open access while
// no password is set, parameter 58, the stored-flag default for existing nodes.
#include "setup_wizard.h"
#include "params.h"
#include "globals.h"
#include <cassert>
#include <cstring>

int main()
{
    // steps 1..7, clamped
    assert(wizardClampStep(0) == 1 && wizardClampStep(-5) == 1 && wizardClampStep(3) == 3 && wizardClampStep(8) == WIZ_STEPS);
    assert(!strcmp(wizardStepFields(1), "uilang") && !strcmp(wizardStepFields(5), "webadminpw"));
    assert(!strcmp(wizardStepFields(WIZ_STEPS), "") && !strcmp(wizardStepFields(0), "") && !strcmp(wizardStepFields(99), ""));
    assert(strstr(wizardStepFields(3), "altitude") && strstr(wizardStepFields(6), "mqtthost") && strstr(wizardStepFields(6), "loxport"));

    // a new node: wizard once per browser session, open while no password
    Config c;
    assert(c.setupDone == 0 && !c.webAdminPass[0]);
    assert(wizardAutoStart(c, false) && !wizardAutoStart(c, true));
    assert(wizardOpenAccess(c));
    // a password closes the open access (Digest from then on), the wizard is still offered
    strcpy(c.webAdminPass, "secret");
    assert(!wizardOpenAccess(c) && wizardAutoStart(c, false));
    // done (finished or dismissed): never again by itself, settings locked as before without a password
    c.webAdminPass[0] = '\0';
    c.setupDone = 1;
    assert(!wizardAutoStart(c, false) && !wizardOpenAccess(c));

    // parameter 58: offer the wizard again / mark it done
    gCfg.setupDone = 1;
    assert(applyParam(PARAM_SETUP_DONE, 0) && gCfg.setupDone == 0);
    assert(!applyParam(PARAM_SETUP_DONE, 2) && gCfg.setupDone == 0);
    assert(applyParam(PARAM_SETUP_DONE, 1) && gCfg.setupDone == 1);
    return 0;
}
