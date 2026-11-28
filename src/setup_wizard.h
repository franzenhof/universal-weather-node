#pragma once
// Setup wizard (/setup, web_api.cpp): the first-run guide after the node
// joined the Wi-Fi. Steps and the settings fields each one posts to
// POST /api/settings (the normal settings handler validates them - there is
// no second validation path); host-tested (test/firmware_wizard.cpp,
// test/wizard.test.js checks every field against the settings handler).
#include "config.h"
#include <stddef.h>
#include <stdint.h>

constexpr int WIZ_STEPS = 7;   // 1 language, 2 wireless station, 3 location, 4 units, 5 password, 6 outputs, 7 summary

// Comma-separated field names step n (1..WIZ_STEPS) posts ("" = none);
// "name*" = every field starting with "name".
const char* wizardStepFields(int step);
// A field an open wizard POST of step n may carry: one of the step's fields,
// a secret field's "<name>_clear" box, or the form's own (form, wizard,
// wstep, wstay). An open POST with any other field is refused.
bool wizardFieldAllowed(int step, const char* name);
int wizardClampStep(int step);

// The dashboard sends the visitor to the wizard: not done yet and not
// already shown in this browser session (cookie wnSetup).
bool wizardAutoStart(const Config& c, bool shownThisSession);
// The cookie wnSetup holds an ID of this start (random per boot): after any
// restart (a reset, "Erase everything") the wizard is offered again, also to
// a browser that saw it before. wizardCookieShown(): the Cookie header holds
// exactly this start's value. wizardCookieValue(): "wnSetup=<8 hex digits>".
bool wizardCookieShown(const char* cookieHeader, uint32_t bootId);
void wizardCookieValue(uint32_t bootId, char* out, size_t len);
// A decimal-degree coordinate typed in a form (latitude: limit 90, longitude
// 180): "49.88", also with the decimal comma "49,88"; empty = not set. Writes
// the value with a point to out; false (out untouched) for anything else.
bool coordParse(const char* in, float limit, char* out, size_t outLen);
// The wizard (and the fields of the step it posts, wizardFieldAllowed())
// may be used without the admin password: only while none is set and the
// setup is not done - the first setup of a new node, like today's open
// settings portal. Otherwise the normal Digest login applies.
bool wizardOpenAccess(const Config& c);
