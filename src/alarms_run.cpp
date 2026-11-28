// Alarm rules: device part (see alarms.h, docs/alarms.md).
#include "alarms.h"
#include "alarm_queue.h"
#include "psram_json.h"
#include "lsensors.h"
#include "notify.h"
#include "tls_gate.h"
#include "ph_live.h"
#include "out_status.h"
#include "globals.h"
#include "i18n.h"
#include "rf_sensors.h"
#include "sources.h"
#include "time_sync.h"
#include "serial_log.h"
#include "storage_layout.h"
#include "storage_write.h"
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <memory>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace {
AlarmRule         rules[ALARM_RULES];
AlarmState        states[ALARM_RULES];
AlarmChannels     channels;
SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
unsigned long     lastPollMs = 0;
constexpr unsigned long POLL_MS = 30000UL;

// ---- notification queue (alarm events / tests -> upload task) ----
struct QMsg {
    uint8_t  rule;            // 0..ALARM_RULES-1, 0xFF = test
    uint8_t  event;           // AlarmEvent
    uint8_t  mask;            // channels still to send (AC_*, never AC_MQTT)
    uint8_t  priority;
    uint8_t  attempts;
    char     title[24];
    char     text[200];
    char     value[16], threshold[16], unit[8];
    // AS_RF rule: its logical sensor at the time of the event ({sensor}; the
    // sender reads no rule, which the web task may edit meanwhile)
    char     sensorInternalId[sizeof(AlarmRule::sensorInternalId)];
};
constexpr int QLEN = 6;
AlarmQueue<QMsg, QLEN> queue;   // under mtx
unsigned long nextTryMs = 0, backoffMs = 0;

// ---- MQTT events (<prefix>/alarm) ----
struct MEvent { uint8_t rule, event; char value[16]; };
MEvent        mq[4];
int           mqCount = 0;
uint16_t      mqttDirty = 0xFFFF;   // rules whose retained state is to be (re)published
volatile bool configChanged = false;

uint8_t lang() { return gCfg.uiLang >= 1 && gCfg.uiLang <= LANG_COUNT ? gCfg.uiLang - 1 : LANG_EN; }

bool rfValue(const AlarmRule& r, float& v)
{
    LogicalSensor e;   // the logical sensor's current radio ID (lsensors.h; copied under its lock)
    const uint32_t rid = lsCopyByInternalId(r.sensorInternalId, e) ? e.radioId : 0;
    for (int i = 0; i < RF_MAX_SENSORS && rid; i++) {
        const RfSensorReading& s = gRfSensors[i];
        if (!s.used || s.id != rid) continue;
        const unsigned long age = millis() - s.lastMs;
        if (r.rfField == AF_SILENT_MIN) { v = (s.staleMask & RF_VAL_ANY) ? 1e6f : age / 60000.0f; return true; }
        const bool alive = !(s.staleMask & RF_VAL_ANY) && age <= rfFreshMs(s);
        switch (r.rfField) {
            case AF_TEMP:  v = s.tempC;     return s.hasTemp && srcRfValueFresh(i, "temp");
            case AF_HUM:   v = s.humidity;  return s.hasHumidity && srcRfValueFresh(i, "humidity");
            case AF_WIND:  v = s.windAvgMs; return s.hasWind && srcRfValueFresh(i, "wind");
            case AF_GUST:  v = s.windGustMs; return s.hasWind && srcRfValueFresh(i, "wind");
            case AF_UV:    v = s.uvIndex;   return s.hasUv && srcRfValueFresh(i, "uv");
            case AF_LUX:   v = s.lightLux;  return s.hasLight && srcRfValueFresh(i, "light");
            case AF_LEAK:  v = s.leak ? 1 : 0; return s.hasLeak && alive;
            case AF_LIGHTNING_KM: v = s.lightningKm ? s.lightningKm : 999; return s.hasLightning && alive;
            case AF_MOISTURE: v = s.moisturePct; return s.hasMoisture && alive;
            case AF_CO2:   v = s.co2Ppm;    return s.hasCo2 && alive;
            case AF_PM25:  v = s.pm25;      return s.hasPm25 && alive;
            case AF_BATTERY_LOW: v = s.batteryOk ? 0 : 1; return alive;
            case AF_RECEPTION_PCT: {   // unknown (no interval yet, silent) never raises or clears
                const ReceptionVerdict rx = rfReceptionOf(s);
                v = rx.percent;
                return rx.known;
            }
            default: return false;
        }
    }
    // not in the table: "silent" is the only value that still means something
    if (r.rfField == AF_SILENT_MIN) { v = 1e6f; return true; }
    return false;
}

bool ruleValue(const AlarmRule& r, PhLive& l, float& v)
{
    static const char* const names[AS_COUNT] = { nullptr, "temp", "feels", "hum", "dew", "qnh", "ptend3h", "wind", "gust",
        "rain_rate", "rain_day", "rain_1h", "uv", nullptr, nullptr, nullptr, nullptr };
    switch (r.src) {
        case AS_FROST: case AS_FOG: case AS_SULTRY: {
            if (!l.d.warnValid) return false;
            const uint8_t bit = r.src == AS_FROST ? WARN_FROST : r.src == AS_FOG ? WARN_FOG : WARN_SULTRY;
            v = (l.d.warnings & bit) ? 1.0f : 0.0f;
            return true;
        }
        case AS_RF: return rfTableOn() && rfValue(r, v);
        default: {
            if (r.src >= AS_COUNT || !names[r.src]) return false;
            PhValue pv;
            if (!phLiveLookup(names[r.src], pv, &l)) return false;
            v = pv.v;
            return true;
        }
    }
}

// placeholders of a message: the rule's own plus the live values. The
// rendering task's own buffer for {sensor} (enqueue: loop / web task, the
// webhook: upload task).
struct MsgCtx { const QMsg* m; PhLive* live; char sensorLabel[LS_DISPLAY_NAME_MAX + 1]; };
bool msgLookup(const char* n, PhValue& o, void* ctx)
{
    MsgCtx& c = *static_cast<MsgCtx*>(ctx);
    auto txt = [&o](const char* s) { o.kind = PK_TEXT; o.text = s; return true; };
    if (!strcmp(n, "name"))      return txt(c.m->title);
    if (!strcmp(n, "value"))     return txt(c.m->value);
    if (!strcmp(n, "unit"))      return txt(c.m->unit);
    if (!strcmp(n, "threshold")) return txt(c.m->threshold);
    if (!strcmp(n, "message"))   return txt(c.m->text);
    if (!strcmp(n, "state"))     return txt(c.m->rule == 0xFF ? "test" : c.m->event == AE_CLEAR ? "cleared" : "raised");
    if (!strcmp(n, "priority"))  { o.kind = PK_PLAIN; o.v = c.m->priority; return true; }
    if (!strcmp(n, "rule"))      { if (c.m->rule == 0xFF) return false; o.kind = PK_PLAIN; o.v = c.m->rule + 1; return true; }
    if (!strcmp(n, "time"))      return phLiveLookup("time_local", o, c.live);
    if (!strcmp(n, "sensor")) {
        if (!c.m->sensorInternalId[0]) return false;   // not a wireless-sensor rule, or the test
        // the logical sensor's name, else its custom ID (A5c D7)
        LogicalSensor e;
        if (!lsCopyByInternalId(c.m->sensorInternalId, e)) return false;
        snprintf(c.sensorLabel, sizeof(c.sensorLabel), "%s", e.displayName[0] ? e.displayName : e.customId);
        return txt(c.sensorLabel);
    }
    return phLiveLookup(n, o, c.live);
}

// caller holds mtx
void enqueue(uint8_t rule, AlarmEvent ev, const AlarmRule* r, float value, PhLive& live)
{
    // the channels this queue sends (MQTT goes its own way: mq[]); none = nothing
    // queued - and no pending message dropped for it
    const uint8_t mask = r ? (uint8_t)(r->channels & ~(1 << AC_MQTT)) : (uint8_t)(1 << (int)value);   // test: value = channel
    if (!mask) return;
    bool dropped = false;
    QMsg& m = queue.push(dropped);
    if (dropped) dbgSerial.println("Alarms: queue full, oldest message dropped.");
    m.rule = rule; m.event = ev; m.attempts = 0; m.mask = mask;
    if (r) {
        const PhKind k = alarmKind(*r);
        m.priority = r->priority;
        if (r->src == AS_RF) strlcpy(m.sensorInternalId, r->sensorInternalId, sizeof(m.sensorInternalId));
        strlcpy(m.title, r->name[0] ? r->name : alarmSrcKey(r->src), sizeof(m.title));
        const int dec = k == PK_PLAIN ? (r->src == AS_UV ? 1 : 0) : 1;
        snprintf(m.value, sizeof(m.value), "%.*f", dec, (double)alarmToDisplay(k, value, gCfg));
        snprintf(m.threshold, sizeof(m.threshold), "%.*f", dec,
                 (double)alarmToDisplay(k, ev == AE_CLEAR ? r->clear : r->raise, gCfg));
        strlcpy(m.unit, alarmUnitLabel(k, r->src, r->rfField, gCfg), sizeof(m.unit));
        const char* tpl = ev == AE_CLEAR ? (r->clearMsg[0] ? r->clearMsg : TL(lang(), T_al_msg_clear))
                                         : (r->msg[0] ? r->msg : TL(lang(), T_al_msg_raise));
        MsgCtx c{ &m, &live, "" };
        phRender(tpl, m.text, sizeof(m.text), PE_NONE, msgLookup, &c);
    } else {   // test
        m.priority = 3;
        strlcpy(m.title, gCfg.deviceName[0] ? gCfg.deviceName : "Weather node", sizeof(m.title));
        MsgCtx c{ &m, &live, "" };
        phRender(TL(lang(), T_al_msg_test), m.text, sizeof(m.text), PE_NONE, msgLookup, &c);
    }
}

void load()
{
    Preferences prefs;
    if (!prefs.begin("alarms", true, "cfgdata")) return;   // never written
    uint8_t* blob = (uint8_t*)malloc(ALARM_BLOB_SIZE);   // ~2 KB, only at start
    if (blob && prefs.getBytesLength("rules") == ALARM_BLOB_SIZE && prefs.getBytes("rules", blob, ALARM_BLOB_SIZE) == ALARM_BLOB_SIZE)
        alarmsDecode(blob, ALARM_BLOB_SIZE, rules);
    free(blob);
    // [version][AlarmChannels] (storage_layout.h)
    // the stored blob (also longer than today's: an older layout for the hook) + the migration scratch, only at start
    static_assert(1 + sizeof(AlarmChannels) <= STORED_BLOB_READ_MAX, "alarm channels beyond the loaders' read ceiling");
    const size_t chanLen = prefs.isKey("chan") ? prefs.getBytesLength("chan") : 0;
    uint8_t* chanBlob = storedBlobLengthReadable(chanLen) ? (uint8_t*)malloc(chanLen) : nullptr;
    uint8_t* chanScratch = chanBlob ? (uint8_t*)malloc(1 + sizeof(AlarmChannels)) : nullptr;
    const uint8_t* chanPayload = chanScratch && prefs.getBytes("chan", chanBlob, chanLen) == chanLen
        ? storedBlobLoad(SF_ALARM_CHANNELS, chanBlob, chanLen, STORED_ALARM_CHANNELS_VERSION, sizeof(AlarmChannels),
                         chanScratch, 1 + sizeof(AlarmChannels))
        : nullptr;
    if (chanPayload) {
        memcpy(&channels, chanPayload, sizeof(channels));
        const AlarmChannels& c = channels;
        // never trust stored strings
        channels.ntfyUrl[sizeof(c.ntfyUrl) - 1] = channels.ntfyTopic[sizeof(c.ntfyTopic) - 1] = '\0';
        channels.ntfyToken[sizeof(c.ntfyToken) - 1] = channels.ntfyTags[sizeof(c.ntfyTags) - 1] = '\0';
        channels.poToken[sizeof(c.poToken) - 1] = channels.poUser[sizeof(c.poUser) - 1] = '\0';
        channels.tgToken[sizeof(c.tgToken) - 1] = channels.tgChat[sizeof(c.tgChat) - 1] = '\0';
        channels.whUrl[sizeof(c.whUrl) - 1] = channels.whBody[sizeof(c.whBody) - 1] = '\0';
    }
    free(chanScratch);
    free(chanBlob);
    prefs.end();
}
} // namespace

void alarmsSetup()   { xSemaphoreTake(mtx, portMAX_DELAY); load(); xSemaphoreGive(mtx); }
void alarmsLock()    { xSemaphoreTake(mtx, portMAX_DELAY); }
void alarmsUnlock()  { xSemaphoreGive(mtx); }
AlarmRule* alarmsRules()          { return rules; }
AlarmChannels& alarmsChannels()   { return channels; }
void alarmsResetState(int i)      { if (i >= 0 && i < ALARM_RULES) { states[i] = AlarmState(); mqttDirty |= 1 << i; } }

void alarmsSave()
{
    uint8_t* blob = (uint8_t*)malloc(ALARM_BLOB_SIZE);   // ~2 KB while saving
    if (!blob) { dbgSerial.println("Alarms: no memory to save."); return; }
    alarmsEncode(rules, blob);
    StorageWrite w("alarms");   // storage_write.h: refused once a reset sealed the storage
    if (!w.ok()) { free(blob); return; }
    Preferences& prefs = w.prefs();
    bool ok = prefs.putBytes("rules", blob, ALARM_BLOB_SIZE) == ALARM_BLOB_SIZE;
    free(blob);
    {
        uint8_t* chanBlob = (uint8_t*)malloc(1 + sizeof(channels));
        if (chanBlob) {
            chanBlob[0] = STORED_ALARM_CHANNELS_VERSION;   // storage_layout.h
            memcpy(chanBlob + 1, &channels, sizeof(channels));
            ok &= prefs.putBytes("chan", chanBlob, 1 + sizeof(channels)) == 1 + sizeof(channels);
            free(chanBlob);
        } else ok = false;
    }
    if (!ok) dbgSerial.println("Alarms: NVS write failed.");
    mqttDirty = 0xFFFF;
    configChanged = true;
}

void alarmChannelsConfigured(bool* c)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    c[0] = channels.ntfyTopic[0] != 0;
    c[1] = channels.poToken[0] && channels.poUser[0];
    c[2] = channels.tgToken[0] && channels.tgChat[0];
    c[3] = channels.whUrl[0] != 0;
    xSemaphoreGive(mtx);
}

bool alarmsConfigChanged() { const bool c = configChanged; configChanged = false; return c; }

void alarmsSnapshot(AlarmRule* r, AlarmState* s)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (r) memcpy(r, rules, sizeof(rules));
    if (s) memcpy(s, states, sizeof(states));
    xSemaphoreGive(mtx);
}

uint16_t alarmsEnabledMask()
{
    uint16_t m = 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < ALARM_RULES; i++) if (rules[i].enabled && rules[i].src != AS_OFF) m |= 1 << i;
    xSemaphoreGive(mtx);
    return m;
}

bool alarmsSetEnabledMask(uint16_t mask)
{
    if (mask >> ALARM_RULES) return false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < ALARM_RULES; i++) rules[i].enabled = (mask >> i) & 1;
    alarmsSave();
    xSemaphoreGive(mtx);
    return true;
}

void alarmsPoll()
{
    if (lastPollMs && millis() - lastPollMs < POLL_MS) return;
    lastPollMs = millis();
    static PhLive live;   // ~200 bytes, not on the loop task's stack
    phLiveInit(live, nullptr);
    const uint32_t now = alarmClockSec();
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < ALARM_RULES; i++) {
        const AlarmRule& r = rules[i];
        float v = 0.0f;
        const bool valid = r.src != AS_OFF && r.enabled && ruleValue(r, live, v);
        const AlarmEvent ev = alarmStep(r, states[i], valid, v, now);
        if (ev == AE_NONE) continue;
        dbgSerial.printf("Alarm %d '%s': %s\n", i + 1, r.name, ev == AE_CLEAR ? "cleared" : ev == AE_RAISE ? "raised" : "repeat");
        mqttDirty |= 1 << i;
        if (r.channels & (1 << AC_MQTT) && mqCount < 4) {
            MEvent& e = mq[mqCount++];
            e.rule = (uint8_t)i; e.event = ev;
            snprintf(e.value, sizeof(e.value), "%.2f", (double)alarmToDisplay(alarmKind(r), v, gCfg));
        }
        if (ev == AE_CLEAR && (!r.allClear || r.src == AS_OFF || !r.enabled)) continue;
        enqueue((uint8_t)i, ev, &r, v, live);
    }
    xSemaphoreGive(mtx);
}

bool alarmsTest(uint8_t ch)
{
    if (ch >= AC_COUNT) return false;
    static PhLive live;
    phLiveInit(live, nullptr);
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (ch == AC_MQTT) {
        if (mqCount < 4) { MEvent& e = mq[mqCount++]; e.rule = 0xFF; e.event = AE_RAISE; strcpy(e.value, ""); }
    } else {
        enqueue(0xFF, AE_RAISE, nullptr, ch, live);
    }
    xSemaphoreGive(mtx);
    return true;
}

int alarmsActiveCount()
{
    int n = 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (const AlarmState& s : states) n += s.active;
    xSemaphoreGive(mtx);
    return n;
}

void alarmsActiveJson(JsonArray a)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    for (int i = 0; i < ALARM_RULES; i++) {
        if (!states[i].active) continue;
        JsonObject o = a.add<JsonObject>();
        o["n"] = i + 1;
        o["name"] = rules[i].name[0] ? rules[i].name : alarmSrcKey(rules[i].src);
        o["priority"] = rules[i].priority;
    }
    xSemaphoreGive(mtx);
}

void alarmsNotifyService(AlarmHttpFn http)
{
    if (nextTryMs && (long)(millis() - nextTryMs) < 0) return;
    static QMsg m;          // upload task only: a copy of the head, sent without the lock
    uint32_t sendingId = 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    const bool have = queue.front(m, sendingId);
    xSemaphoreGive(mtx);
    if (!have) return;
    // the lowest channel still open, one request per call
    uint8_t ch = 0;
    while (ch < AC_COUNT && !(m.mask & (1 << ch))) ch++;
    std::unique_ptr<NotifyReq> reqBuf(new (std::nothrow) NotifyReq());   // ~1 KB while sending
    if (!reqBuf) return;
    NotifyReq& req = *reqBuf;
    static PhLive live;
    bool built = false;
    const NotifyMsg nm{ m.title, m.text, m.priority };
    xSemaphoreTake(mtx, portMAX_DELAY);
    const AlarmChannels c = channels;   // copy: the web task may edit
    xSemaphoreGive(mtx);
    switch (ch) {
        case AC_NTFY:     built = notifyNtfy(c, nm, req); break;
        case AC_PUSHOVER: built = notifyPushover(c, nm, req); break;
        case AC_TELEGRAM: built = notifyTelegram(c, nm, req); break;
        case AC_WEBHOOK: {
            phLiveInit(live, nullptr);
            MsgCtx ctx{ &m, &live, "" };
            built = notifyWebhook(c, msgLookup, &ctx, req);
            break;
        }
        default: break;
    }
    const uint8_t outId = OUT_NOTIFY_FIRST + (ch == AC_WEBHOOK ? 3 : ch);
    bool done = true;
    if (built && req.truncated) {   // a cut URL / JSON body is never sent (permanent: not retried)
        dbgSerial.printf("Alarm notification %s: URL or body too long with the values filled in - not sent\n", alarmChannelKey(ch));
        outReport(outId, false, 0, "too long, not sent");
    } else if (built) {
        // HTTPS only with enough memory and no heavy web page being built (tls_gate.h)
        if (!strncmp(req.url, "https://", 8)) {
            const TlsDecision g = tlsMayStart(alarmChannelKey(ch));
            if (g == TLS_WAIT_WEB) { nextTryMs = millis() + 5000UL; return; }   // a few seconds
            if (g == TLS_WAIT_HEAP) {
                backoffMs = backoffMs ? (backoffMs * 2 > 300000UL ? 300000UL : backoffMs * 2) : 10000UL;
                nextTryMs = millis() + backoffMs;
                return;
            }
        }
        char reply[48] = "";
        const int code = http(req, reply, sizeof(reply));
        const bool ok = notifyOk(ch, code);
        // never log the URL (tokens)
        dbgSerial.printf("Alarm notification %s: HTTP %d -> %s\n", alarmChannelKey(ch), code, ok ? "OK" : "FAILED");
        outReport(outId, ok, code, ok ? nullptr : reply);
        if (!ok && ++m.attempts < 4) {
            done = false;
            backoffMs = backoffMs ? (backoffMs * 2 > 300000UL ? 300000UL : backoffMs * 2) : 15000UL;
            nextTryMs = millis() + backoffMs;
        }
    } else {
        outReport(outId, false, 0, "not configured");
    }
    if (done) { backoffMs = 0; nextTryMs = 0; m.attempts = 0; m.mask &= (uint8_t)~(1 << ch); }
    xSemaphoreTake(mtx, portMAX_DELAY);
    // only onto the message that was sent: dropped meanwhile (queue full) = nothing to write back
    if (QMsg* head = queue.frontIfStill(sendingId)) {
        head->mask = m.mask;
        head->attempts = m.attempts;
        if (!m.mask) queue.popFront();
    }
    xSemaphoreGive(mtx);
}

uint32_t alarmClockSec() { return alarmClockSecFrom(esp_timer_get_time()); }

void alarmsMqttService(AlarmPublishFn pub, const char* prefix, bool all)
{
    // Collected under the lock, published after it: a publish is network I/O
    // that can block for seconds on a stalled broker, and alarmsPoll(),
    // /api/data and /api/alarms wait for this lock.
    struct Batch {
        uint16_t dirty = 0, active = 0, deleted = 0;
        int      events = 0;
        char     eventPayload[4][200];
    };
    static_assert(sizeof(mq) / sizeof(mq[0]) == 4, "one payload per queued MQTT event");
    std::unique_ptr<Batch> b(new (std::nothrow) Batch());   // ~820 bytes while publishing
    if (!b) return;   // tried again on the next call (nothing taken)
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (all) mqttDirty = 0xFFFF;
    b->dirty = mqttDirty;
    for (int i = 0; i < ALARM_RULES; i++) {
        if (states[i].active)       b->active  |= 1 << i;
        if (rules[i].src == AS_OFF) b->deleted |= 1 << i;
    }
    mqttDirty = 0;
    for (int k = 0; k < mqCount; k++) {
        const MEvent& e = mq[k];
        char* payload = b->eventPayload[b->events++];
        if (e.rule == 0xFF) {
            snprintf(payload, sizeof(b->eventPayload[0]), "{\"rule\":0,\"name\":\"test\",\"state\":\"test\"}");
        } else {
            JsonDocument d(psramJsonAllocator());
            d["rule"] = e.rule + 1;
            d["name"] = rules[e.rule].name;
            d["state"] = e.event == AE_CLEAR ? "cleared" : e.event == AE_RAISE ? "raised" : "repeat";
            d["value"] = serialized(e.value);
            d["unit"] = alarmUnitLabel(alarmKind(rules[e.rule]), rules[e.rule].src, rules[e.rule].rfField, gCfg);
            d["priority"] = rules[e.rule].priority;
            serializeJson(d, payload, sizeof(b->eventPayload[0]));
        }
    }
    mqCount = 0;
    xSemaphoreGive(mtx);

    char topic[96];
    for (int i = 0; i < ALARM_RULES; i++) {
        if (!(b->dirty & (1 << i))) continue;
        snprintf(topic, sizeof(topic), "%s/alarm/%d", prefix, i + 1);
        if (b->deleted & (1 << i)) pub(topic, "", true);   // a deleted rule: retained state removed
        else pub(topic, (b->active & (1 << i)) ? "ON" : "OFF", true);
    }
    snprintf(topic, sizeof(topic), "%s/alarm", prefix);
    for (int k = 0; k < b->events; k++) pub(topic, b->eventPayload[k], false);
}
