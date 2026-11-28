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

    // an open wizard POST: only its step's fields (secret "_clear" boxes, prefixes) and the form's own
    assert(wizardFieldAllowed(1, "uilang") && wizardFieldAllowed(1, "form") && wizardFieldAllowed(1, "wizard"));
    assert(wizardFieldAllowed(1, "wstep") && wizardFieldAllowed(2, "wstay"));
    assert(!wizardFieldAllowed(1, "devname") && !wizardFieldAllowed(1, "updateurl") && !wizardFieldAllowed(1, "uilan"));
    assert(!wizardFieldAllowed(1, "uilangx") && !wizardFieldAllowed(1, ""));
    assert(wizardFieldAllowed(2, "rfname_0A1B2C3D") && wizardFieldAllowed(2, "rfproto") && !wizardFieldAllowed(2, "rfnames"));
    assert(wizardFieldAllowed(6, "mqttpass") && wizardFieldAllowed(6, "mqttpass_clear") && wizardFieldAllowed(6, "loxport"));
    assert(!wizardFieldAllowed(6, "mqttcacert") && !wizardFieldAllowed(6, "webadminpw") && !wizardFieldAllowed(6, "mqttpass_x"));
    assert(!wizardFieldAllowed(WIZ_STEPS, "uilang") && !wizardFieldAllowed(0, "uilang") && wizardFieldAllowed(0, "form"));

    // a new node: wizard once per browser session, open while no password
    Config c;
    assert(c.setupDone == 0 && !c.webAdminPass[0]);
    assert(wizardAutoStart(c, false) && !wizardAutoStart(c, true));
    assert(!wizardAutoStart(c, false, true));   // a phone on the own access point: the dashboard
    assert(wizardOpenAccess(c));
    // a password closes the open access (Digest from then on), the wizard is still offered
    strcpy(c.webAdminPass, "secret");
    assert(!wizardOpenAccess(c) && wizardAutoStart(c, false));
    // done (finished or dismissed): never again by itself, settings locked as before without a password
    c.webAdminPass[0] = '\0';
    c.setupDone = 1;
    assert(!wizardAutoStart(c, false) && !wizardOpenAccess(c));

    // the wizard cookie belongs to one start: another start's value (also the
    // old fixed "wnSetup=1") does not count, the exact cookie among others does
    {
        char v[20];
        wizardCookieValue(0x1a2b3c4dU, v, sizeof(v));
        assert(!strcmp(v, "wnSetup=1a2b3c4d"));
        assert(wizardCookieShown("wnSetup=1a2b3c4d", 0x1a2b3c4dU));
        assert(wizardCookieShown("wnLang=de; wnSetup=1a2b3c4d; wnDev=fr", 0x1a2b3c4dU));
        assert(wizardCookieShown("wnLang=de;wnSetup=1a2b3c4d", 0x1a2b3c4dU));
        assert(!wizardCookieShown("wnSetup=1a2b3c4d", 0x1a2b3c4eU));   // a restart since
        assert(!wizardCookieShown("wnSetup=1", 1));                     // the old fixed value
        assert(!wizardCookieShown("wnSetup=1a2b3c4dx", 0x1a2b3c4dU));
        assert(!wizardCookieShown("xwnSetup=1a2b3c4d", 0x1a2b3c4dU));
        assert(!wizardCookieShown("", 0x1a2b3c4dU) && !wizardCookieShown(nullptr, 0x1a2b3c4dU));
        assert(wizardCookieShown("wnSetup=00000001", 1));
    }

    // parameter 58: offer the wizard again / mark it done
    gCfg.setupDone = 1;
    assert(applyParam(PARAM_SETUP_DONE, 0) && gCfg.setupDone == 0);
    assert(!applyParam(PARAM_SETUP_DONE, 2) && gCfg.setupDone == 0);
    assert(applyParam(PARAM_SETUP_DONE, 1) && gCfg.setupDone == 1);
    return 0;
}
