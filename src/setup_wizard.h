#pragma once
// Setup wizard (/setup, web_api.cpp): the first-run guide after the node
// joined the Wi-Fi. Steps and the settings fields each one posts to
// POST /api/settings (the normal settings handler validates them - there is
// no second validation path); host-tested (test/firmware_wizard.cpp,
// test/wizard.test.js checks every field against the settings handler).
#include "config.h"

constexpr int WIZ_STEPS = 7;   // 1 language, 2 wireless station, 3 location, 4 units, 5 password, 6 outputs, 7 summary

// Comma-separated field names step n (1..WIZ_STEPS) posts ("" = none).
const char* wizardStepFields(int step);
int wizardClampStep(int step);

// The dashboard sends the visitor to the wizard: not done yet and not
// already shown in this browser session (cookie wnSetup).
bool wizardAutoStart(const Config& c, bool shownThisSession);
// The wizard (and the settings it posts) may be used without the admin
// password: only while none is set and the setup is not done - the first
// setup of a new node, like today's open settings portal. Otherwise the
// normal Digest login applies.
bool wizardOpenAccess(const Config& c);
