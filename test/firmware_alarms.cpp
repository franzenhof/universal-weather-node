// Alarm rules (src/alarms.cpp): hysteresis, minimum duration, repeat, all
// clear, unavailable data, templates, display units, storage, JSON.
#include "alarms.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>

Config gCfg;

int main()
{
    AlarmRule r;
    r.src = AS_TEMP; r.enabled = 1; r.above = 0; r.raise = 0.0f; r.clear = 1.0f;   // frost-like: < 0 °C, clear at >= 1 °C
    AlarmState s;
    // hysteresis
    assert(alarmStep(r, s, true, 2.0f, 100) == AE_NONE && !s.active);
    assert(alarmStep(r, s, true, -0.5f, 130) == AE_RAISE && s.active && s.since == 130);
    assert(alarmStep(r, s, true, 0.5f, 160) == AE_NONE && s.active);    // between thresholds: stays
    assert(alarmStep(r, s, true, 1.0f, 190) == AE_CLEAR && !s.active);  // clear at the clear threshold
    assert(alarmStep(r, s, true, 0.5f, 220) == AE_NONE && !s.active);   // between: does not raise again

    // minimum duration: pending, interrupted, then held long enough
    r.minDurationMin = 5;
    s = AlarmState();
    assert(alarmStep(r, s, true, -1.0f, 1000) == AE_NONE && s.pendingSince == 1000);
    assert(alarmStep(r, s, true, -1.0f, 1200) == AE_NONE);
    assert(alarmStep(r, s, true, 0.5f, 1230) == AE_NONE && s.pendingSince == 0);   // interrupted
    assert(alarmStep(r, s, true, -1.0f, 1260) == AE_NONE);
    assert(alarmStep(r, s, true, -1.0f, 1559) == AE_NONE);
    assert(alarmStep(r, s, true, -1.0f, 1560) == AE_RAISE);

    // repeat interval
    r.repeatMin = 10;
    assert(alarmStep(r, s, true, -2.0f, 1560 + 599) == AE_NONE);
    assert(alarmStep(r, s, true, -2.0f, 1560 + 600) == AE_REPEAT && s.lastNotify == 2160);
    assert(alarmStep(r, s, true, -2.0f, 2160 + 300) == AE_NONE);

    // unavailable (stale) data: neither raise nor clear, pending starts over
    assert(alarmStep(r, s, false, 5.0f, 3000) == AE_NONE && s.active && !s.valid);
    assert(alarmStep(r, s, true, NAN, 3030) == AE_NONE && s.active);
    AlarmState p;
    r.minDurationMin = 1;
    assert(alarmStep(r, p, true, -1.0f, 4000) == AE_NONE && p.pendingSince);
    assert(alarmStep(r, p, false, 0, 4030) == AE_NONE && !p.pendingSince);
    assert(alarmStep(r, p, true, -1.0f, 4060) == AE_NONE);   // the minute starts again
    assert(alarmStep(r, p, true, -1.0f, 4120) == AE_RAISE);

    // switching a raised rule off clears it once
    r.enabled = 0;
    assert(alarmStep(r, s, true, -5.0f, 5000) == AE_CLEAR && !s.active);
    assert(alarmStep(r, s, true, -5.0f, 5030) == AE_NONE);

    // sensor / measurement combinations: only what the sensor provides (silent / battery always)
    {
        const uint16_t wh51 = alarmCapMask(false, false, false, false, false, false, false, true, false, false);
        assert(alarmFieldAvailable(AF_MOISTURE, wh51) && alarmFieldAvailable(AF_SILENT_MIN, wh51) && alarmFieldAvailable(AF_BATTERY_LOW, wh51));
        assert(!alarmFieldAvailable(AF_TEMP, wh51) && !alarmFieldAvailable(AF_LEAK, wh51));
        const uint16_t ws90 = alarmCapMask(true, true, true, true, true, false, false, false, false, false);
        assert(alarmFieldAvailable(AF_GUST, ws90) && alarmFieldAvailable(AF_UV, ws90) && !alarmFieldAvailable(AF_CO2, ws90));
        assert(!alarmFieldAvailable(AF_COUNT, 0xFFFF));
    }

    // templates
    AlarmRule t;
    assert(alarmTemplate(0, t) && t.src == AS_FROST && t.above && t.raise == 0.5f && t.allClear);
    assert(alarmTemplate(3, t) && t.src == AS_PTEND && !t.above && t.raise == -6.0f && t.clear == -3.0f);
    assert(alarmTemplate(4, t) && t.src == AS_RAINRATE && t.raise > 0 && t.clear == 0.0f && !t.allClear);
    AlarmRule none;
    assert(!alarmTemplate(ALARM_TEMPLATES, none));
    // rain starts: raises above 0.1 mm/h, clears at 0
    AlarmState rs;
    assert(alarmStep(t, rs, true, 0.05f, 10) == AE_NONE);
    assert(alarmStep(t, rs, true, 1.2f, 40) == AE_RAISE);
    assert(alarmStep(t, rs, true, 0.0f, 70) == AE_CLEAR);

    // display units
    gCfg.unitSystem = 1;
    assert(std::fabs(alarmToDisplay(PK_TEMP, 0.0f, gCfg) - 32.0f) < 0.01f);
    assert(std::fabs(alarmFromDisplay(PK_TEMP, 50.0f, gCfg) - 10.0f) < 0.01f);
    assert(std::fabs(alarmFromDisplay(PK_PRESS, alarmToDisplay(PK_PRESS, 1013.0f, gCfg), gCfg) - 1013.0f) < 0.01f);
    assert(std::fabs(alarmFromDisplay(PK_SPEED, alarmToDisplay(PK_SPEED, 12.0f, gCfg), gCfg) - 12.0f) < 0.01f);
    gCfg.unitSystem = 0;
    AlarmRule leak; leak.src = AS_RF; leak.rfField = AF_LEAK;
    assert(alarmKind(leak) == PK_PLAIN && !strcmp(alarmUnitLabel(PK_PLAIN, AS_RF, AF_CO2, gCfg), "ppm"));

    // storage round trip; corrupt / foreign blob refused; enums sanitized
    AlarmRule rules[ALARM_RULES], back[ALARM_RULES];
    alarmTemplate(1, rules[0]);
    rules[5] = leak; rules[5].rfId = 0xABCD; strcpy(rules[5].name, "Leak");
    static uint8_t blob[ALARM_BLOB_SIZE];
    alarmsEncode(rules, blob);
    assert(alarmsDecode(blob, sizeof(blob), back) && back[0].src == AS_FOG && back[5].rfId == 0xABCD && !strcmp(back[5].name, "Leak"));
    blob[20] ^= 1;
    assert(!alarmsDecode(blob, sizeof(blob), back));
    assert(!alarmsDecode(blob, sizeof(blob) - 1, back));

    // JSON: no secrets, channels as booleans
    AlarmState st[ALARM_RULES];
    st[0].active = true; st[0].since = 100;
    AlarmChannels ch;
    strcpy(ch.tgToken, "123:SECRET"); strcpy(ch.tgChat, "42");
    strcpy(ch.whUrl, "https://h/?key=SECRET");
    JsonDocument doc;
    alarmsJson(doc, rules, st, ch, gCfg, 160);
    std::string js;
    serializeJson(doc, js);
    assert(js.find("SECRET") == std::string::npos);
    assert(doc["active"][0] == 1 && doc["rules"].size() == 2 && doc["rules"][0]["activeSec"] == 60);
    assert(doc["channels"]["telegram"] == true && doc["channels"]["webhook"] == true && doc["channels"]["ntfy"] == false);
    assert(doc["rules"][1]["sensorId"] == "0000ABCD" && doc["rules"][1]["sensorValue"] == "leak");
    puts("alarms ok");
    return 0;
}
