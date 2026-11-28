#include "davis_rx.h"
#include "status_led.h"
#include "rf_trace.h"
#include "davis_diag.h"
#include "globals.h"
#include "rf_sensors.h"
#include "serial_log.h"
#include "sources.h"
#include "system_restart.h"
#include "radio_board.h"   // the shared SX1262 (Heltec V4 front end handled there)
#include "vs_run.h"        // virtual sensors: frames in the gap after a packet
#include <DavisDecoder.h>
#include <DavisHopper.h>
#include <DavisRadio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <math.h>
#include <new>

namespace {

// ---- handover between the main loop and the receive task ----

struct RxSettings {
    DavisHopperConfig hop;
    DavisModel model = DAVIS_VP2;
    int rxBw = 938;   // kHz x 10
};

bool sameSettings(const RxSettings& a, const RxSettings& b)
{
    const DavisHopperConfig &x = a.hop, &y = b.hop;
    return x.band == y.band && x.txId == y.txId && x.offsetHz == y.offsetHz && x.guardMs == y.guardMs &&
           x.windowMs == y.windowMs && x.lostAfter == y.lostAfter && x.scanSpanKhz == y.scanSpanKhz &&
           x.afc == y.afc && x.raw == y.raw && x.rawChannel == y.rawChannel && a.model == b.model && a.rxBw == b.rxBw;
}

RxSettings settingsFromConfig()
{
    RxSettings s;
    s.hop.band        = gCfg.davisTable ? &DAVIS_BAND_EU_CLASSIC : &DAVIS_BAND_EU;
    s.hop.txId        = (uint8_t)gCfg.davisTxId;
    s.hop.offsetHz    = gCfg.davisOffsetHz;
    s.hop.guardMs     = (uint16_t)gCfg.davisGuardMs;
    s.hop.windowMs    = (uint16_t)gCfg.davisWindowMs;
    s.hop.lostAfter   = 12;
    s.hop.scanSpanKhz = (uint16_t)gCfg.davisScanKhz;
    s.hop.afc         = gCfg.davisAfc != 0;
    s.hop.raw         = gCfg.davisRaw != 0;
    s.hop.rawChannel  = (uint8_t)gCfg.davisRawCh;
    s.model           = gCfg.davisModel ? DAVIS_VUE : DAVIS_VP2;
    s.rxBw            = gCfg.davisRxBw;
    return s;
}

// Receive task -> main loop
enum RecKind : uint8_t { REC_PACKET, REC_EVENT, REC_RADIO_ERROR };
struct Rec {
    RecKind       kind;
    int64_t       atUs;
    float         rssi;
    uint8_t       air[DAVIS_PACKET_LEN];
    DavisRxResult r;
    DavisEvent    ev;
    int16_t       err;
};

// Receive task -> main loop: state snapshot
struct TaskStatus {
    uint8_t  state = 0, txId = 0, channel = 0;
    float    freqMHz = 0.0f;
    double   periodUs = 0.0;
    int32_t  learned = 0;
    uint32_t accepted = 0, crcErrors = 0, other = 0, misses = 0, trackSlots = 0, resyncs = 0;
};

TaskHandle_t      task = nullptr;
QueueHandle_t     queue = nullptr;
SemaphoreHandle_t lock = nullptr;            // pending settings + status
portMUX_TYPE      mux = portMUX_INITIALIZER_UNLOCKED;
RxSettings        pending, applied;
volatile bool     settingsChanged = false;
volatile bool     stopRequest = false;
volatile bool     taskRunning = false;
volatile bool     owned = false;
TaskStatus        status;
volatile uint32_t queueDrops = 0;

void IRAM_ATTR packetHook()
{
    BaseType_t woken = pdFALSE;
    if (task) vTaskNotifyGiveFromISR(task, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void send(const Rec& r)
{
    if (xQueueSend(queue, &r, 0) != pdTRUE) queueDrops = queueDrops + 1;
}

void receiveTask(void*)
{
    DavisHopper hop;
    DavisRadio  dr;
    RxSettings  s;
    xSemaphoreTake(lock, portMAX_DELAY);
    s = pending;
    settingsChanged = false;
    xSemaphoreGive(lock);
    int64_t now = esp_timer_get_time();
    hop.configure(s.hop, now);
    int16_t st = dr.begin(hop.tuneMHz(), s.rxBw / 10.0f);
    float tuned = st == RADIOLIB_ERR_NONE ? hop.tuneMHz() : NAN;
    Rec rec;
    if (st != RADIOLIB_ERR_NONE) { rec.kind = REC_RADIO_ERROR; rec.err = st; rec.atUs = now; send(rec); }

    while (!stopRequest) {
        if (settingsChanged) {
            RxSettings n;
            xSemaphoreTake(lock, portMAX_DELAY);
            n = pending;
            settingsChanged = false;
            xSemaphoreGive(lock);
            now = esp_timer_get_time();
            if (n.rxBw != s.rxBw || st != RADIOLIB_ERR_NONE) {
                st = dr.begin(hop.tuneMHz(), n.rxBw / 10.0f);
                tuned = NAN;
            }
            s = n;
            hop.update(s.hop, now);
        }
        if (st != RADIOLIB_ERR_NONE) {          // radio failed: retry every 5 s
            rec.kind = REC_RADIO_ERROR; rec.err = st; rec.atUs = esp_timer_get_time(); send(rec);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            if (stopRequest) break;
            st = dr.begin(hop.tuneMHz(), s.rxBw / 10.0f);
            tuned = st == RADIOLIB_ERR_NONE ? hop.tuneMHz() : NAN;
            continue;
        }
        bool readOne = false;
        if (dr.pending()) {   // packets first: their ISR time decides, not the wake-up
            uint8_t b[DAVIS_PACKET_LEN];
            float rssi;
            int64_t t;
            readOne = true;
            if (dr.read(b, rssi, t)) {
                rec.kind = REC_PACKET;
                rec.atUs = t;
                rec.rssi = rssi;
                memcpy(rec.air, b, sizeof(b));
                rec.r = hop.onPacket(b, t, s.model);
                send(rec);
                // Virtual sensors (C5): only after a packet that confirmed the
                // hop prediction - never after a miss, while searching or in raw
                // capture. The hopper already points at the next channel.
                if (rec.r.cls == DAVIS_PKT_ACCEPTED && rec.r.hasTiming && hop.state() == DAVIS_ST_TRACK) {
                    VsDavisGap g;
                    g.confirmed = true;
                    g.packetUs  = t;
                    g.nextUs    = hop.nextExpectedUs();
                    g.guardUs   = (uint32_t)s.hop.guardMs * 1000;
                    const int64_t t0 = esp_timer_get_time();
                    if (vsDavisSendInGap(g) > 0) {   // the radio left the Davis setup
                        st = dr.begin(hop.tuneMHz(), s.rxBw / 10.0f);
                        tuned = st == RADIOLIB_ERR_NONE ? hop.tuneMHz() : NAN;
                        vsDavisNoteGapUse((uint32_t)(esp_timer_get_time() - t0));
                        readOne = false;            // receiving again on the next channel
                        if (st != RADIOLIB_ERR_NONE) continue;   // retried by the radio-error path
                    }
                }
            }
        }
        now = esp_timer_get_time();
        for (DavisEvent e = hop.poll(now); e.kind != DAVIS_EV_NONE; e = hop.poll(now)) {
            rec.kind = REC_EVENT; rec.atUs = e.atUs; rec.ev = e;
            send(rec);
        }
        float f = hop.tuneMHz();
        if (f != tuned) { st = dr.tune(f); tuned = st == RADIOLIB_ERR_NONE ? f : NAN; }
        else if (readOne) dr.restart();

        portENTER_CRITICAL(&mux);
        status.state = hop.state();  status.txId = hop.lockedId();  status.channel = hop.channel();
        status.freqMHz = f;  status.periodUs = hop.periodUs();  status.learned = hop.learnedOffsetHz();
        status.accepted = hop.accepted;  status.crcErrors = hop.crcErrors;  status.other = hop.otherPackets;
        status.misses = hop.misses;  status.trackSlots = hop.trackSlots;  status.resyncs = hop.resyncs;
        portEXIT_CRITICAL(&mux);

        int64_t waitUs = hop.nextDeadlineUs() - esp_timer_get_time();
        if (waitUs < 1000) waitUs = 1000;
        if (waitUs > 100000) waitUs = 100000;   // settings / stop stay responsive
        if (!dr.pending()) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((waitUs + 999) / 1000));
    }
    dr.standby();
    DavisRadio::setPacketHook(nullptr);
    taskRunning = false;
    vTaskDelete(nullptr);
}

// ---- sniffer capture: the radio trace (rf_trace.h) ----
// Davis mode starts an open-ended trace (all packets) unless one is running;
// every packet / event line goes there (and, budgeted, to the web log).
bool traceByDavis = false;

// ---- main-loop side ----

constexpr int  MAX_LOG_LINES_PER_MIN = 30;   // per-packet lines to the web log; the capture keeps all
unsigned long  minuteStartMs = 0;
int            linesThisMinute = 0;
uint32_t       suppressedLines = 0;
DavisDiag      atMinuteStart;               // counters for the summary
unsigned long  lastSummaryMs = 0;
uint8_t        lastState = 0xFF;
DavisRainFilter rainCounter;
float          rainTotalMm = 0.0f;
uint32_t       lastQueueDrops = 0;

// One log line: capture always, the web log (dbgSerial) if wanted and the
// per-minute budget allows (else counted and reported in the summary).
void logLine(const char* line, bool toWebLog, bool budgeted, RfTraceResult tr = RF_TR_EVENT, bool hasId = false,
             uint32_t id = 0)
{
    rfTraceText(RF_TP_DAVIS, line, tr, hasId, id);
    if (!toWebLog) return;
    if (budgeted) {
        if (linesThisMinute >= MAX_LOG_LINES_PER_MIN) { suppressedLines++; return; }
        linesThisMinute++;
    }
    dbgSerial.print(line);
}

void timeText(char* out, size_t n, int64_t us)
{
    long long ms = us / 1000;
    snprintf(out, n, "%lld.%03lld", ms / 1000, ms % 1000);
}

void valueText(const DavisPacket& p, char* out, size_t n, float mmPerTip)
{
    switch (p.type) {
        case DAVIS_MSG_TEMP:
            if (p.hasTemp)         snprintf(out, n, "%.1f F / %.1f C", p.tempF, p.tempC);
            else if (p.tempAnalog) snprintf(out, n, "analog sensor (not decoded)");
            else                   snprintf(out, n, "no sensor");
            break;
        case DAVIS_MSG_HUMIDITY:
            if (p.hasHumidity) snprintf(out, n, "%.1f %%", p.humidity); else snprintf(out, n, "no sensor");
            break;
        case DAVIS_MSG_RAIN:
            if (p.hasRainCount) snprintf(out, n, "tip counter %u", p.rainCount); else snprintf(out, n, "no collector");
            break;
        case DAVIS_MSG_RAINRATE:
            if (!p.hasRainRate)          snprintf(out, n, "invalid");
            else if (p.rainTipSec <= 0)  snprintf(out, n, "no rain");
            else snprintf(out, n, "%.1f s/tip = %.1f mm/h", p.rainTipSec, davisRainRateMmH(p.rainTipSec, mmPerTip));
            break;
        case DAVIS_MSG_UV:
            if (p.hasUv) snprintf(out, n, "UV index %.1f", p.uvIndex); else snprintf(out, n, "no sensor");
            break;
        case DAVIS_MSG_SOLAR:
            if (p.hasSolar) snprintf(out, n, "%.0f W/m2", p.solarWm2); else snprintf(out, n, "no sensor");
            break;
        case DAVIS_MSG_GUST:
            snprintf(out, n, "10-min gust %u mph (index %u)", p.gustMph, p.gustIndex);
            break;
        case DAVIS_MSG_SUPERCAP:
            if (p.hasSupercap) snprintf(out, n, "%.2f V? (raw %u)", p.supercapV, p.raw10); else snprintf(out, n, "raw %u", p.raw10);
            break;
        case DAVIS_MSG_SOLARCELL:
            if (p.hasSolarCell) snprintf(out, n, "%.2f V? (raw %u)", p.solarCellV, p.raw10); else snprintf(out, n, "raw %u", p.raw10);
            break;
        default:
            snprintf(out, n, "raw10 %u, bytes 3-5 %02X %02X %02X", p.raw10, p.bytes[3], p.bytes[4], p.bytes[5]);
            break;
    }
}

float bucketMm() { return gCfg.davisBucket ? 0.254f : 0.2f; }

void storeValues(const DavisPacket& p, float rssi)
{
    const uint32_t id = RF_ID_DAVIS | p.txId;
    unsigned long now = millis();
    if (rfStateOf(id) == RF_STATE_HIDDEN) { rfNoteHidden(id, nullptr, rssi, now); return; }
    int idx = rfSlotFor(id, now);
    if (idx < 0) return;
    RfSensorReading& r = gRfSensors[idx];
    r.used      = true;
    r.id        = id;
    r.type      = 1;                       // weather station (Bresser numbering, like the Fine Offset arrays)
    r.decoder   = 0;
    r.proto     = RF_PROTO_DAVIS;
    r.model     = gCfg.davisModel ? "Vantage Vue" : "Vantage Pro2";
    r.channel   = p.txId;
    r.rssi      = rssi;
    r.batteryOk = !p.batteryLow;
    r.packets++;
    statusLedPacket(true);              // Davis mode: the ISS is the station

    RfValues v;
    v.ok = RF_VAL_WIND;                    // every packet: speed (integer mph) and direction
    v.windAvgMs = v.windGustMs = p.windMph * 0.44704f;   // one sample per packet: the gust is the highest of them
    v.windDirDeg = p.hasWindDir ? p.windDirDeg : (r.hasWind ? r.windDirDeg : 0.0f);
    if (p.hasTemp)     { v.ok |= RF_VAL_TEMP; v.tempC = p.tempC; }
    if (p.hasHumidity) { v.ok |= RF_VAL_HUM;  v.humidity = (uint8_t)lroundf(p.humidity); }
    if (p.hasRainCount) {
        rainCounter.configure(gCfg.davisRainFilter, (uint8_t)gCfg.davisRainMaxPkt, (uint8_t)gCfg.davisRainMaxMin);
        const DavisRainStep st = rainCounter.update(p.rainCount, now);
        if (st.event >= DRE_HELD) {   // phantom-rain filter: always to the log and the radio trace
            static const char* const what[] = { "", "", "held for confirmation", "confirmed as rain", "counter reset, not counted", "corrupt value dropped" };
            char line[112];
            snprintf(line, sizeof(line), "Davis rain counter %u: jump of %u tips (%.1f mm) %s", p.rainCount & 0x7F, st.jump,
                     st.jump * bucketMm(), what[st.event]);
            logLine(line, true, false, RF_TR_EVENT, true, id);
        }
        rainTotalMm += st.tips * bucketMm();
        v.ok |= RF_VAL_RAIN;
        v.rainTotalMm = rainTotalMm;
    }
    if (p.hasUv)    { v.ok |= RF_VAL_UV; v.uvIndex = p.uvIndex; }
    if (p.hasSolar) {
        v.ok |= RF_VAL_LIGHT;
        v.lightLux = p.solarWm2 * RF_DAVIS_LUX_PER_WM2;
        r.hasSolar = true;
        r.solarWm2 = p.solarWm2;
    }
    if (p.hasSupercap) { r.hasSupercap = true; r.supercapV = p.supercapV; }
    if (p.hasRainRate) { gDavisDiag.hasRainRate = true; gDavisDiag.rainRateMmH = davisRainRateMmH(p.rainTipSec, bucketMm()); }
    if (p.hasGust)     { gDavisDiag.hasGust = true; gDavisDiag.gust10MinMs = p.gustMph * 0.44704f; }
    unsigned long rainGapMs;
    float rainDelta = rfStoreValues(r, v, now, rainGapMs);
    srcRfPacket(idx, true, rainDelta, (v.ok & RF_VAL_LIGHT) != 0, rainGapMs);
}

void handlePacket(const Rec& rec)
{
    const DavisRxResult& r = rec.r;
    const DavisPacket& p = r.pkt;
    const bool valid = r.cls == DAVIS_PKT_ACCEPTED || r.cls == DAVIS_PKT_RAW_OK;
    DavisDiag& d = gDavisDiag;
    char val[48] = "";
    if (p.crc != DAVIS_CRC_FAIL) valueText(p, val, sizeof(val), bucketMm());
    if (valid) {
        d.havePacket = true;
        d.lastPacketMs = millis();
        d.lastRssi = rec.rssi;
        DavisTypeDiag& t = d.types[p.type & 0x0F];
        t.count++;
        t.lastMs = d.lastPacketMs;
        memcpy(t.bytes, p.bytes, sizeof(t.bytes));
        strlcpy(t.text, val, sizeof(t.text));
    }
    if (r.hasTiming && r.cls == DAVIS_PKT_ACCEPTED) { d.haveTiming = true; d.lastTimingErrUs = r.timingErrUs; }
    if (r.cls == DAVIS_PKT_ACCEPTED) storeValues(p, rec.rssi);

    const bool sniff = gCfg.davisSniffer || r.state == DAVIS_ST_RAW;
    char line[300], ts[20];
    timeText(ts, sizeof(ts), rec.atUs);
    int n = snprintf(line, sizeof(line), "DV %s ch%u %.4f MHz %+.1fk %.0f dBm %s", ts, r.channel, r.freqMHz,
                     r.offsetHz / 1000.0f, rec.rssi, davisPktClassName(r.cls));
    if (p.crc != DAVIS_CRC_FAIL)
        n += snprintf(line + n, sizeof(line) - n, " id%u%s %s", p.txId, p.batteryLow ? " BATT-LOW" : "", davisTypeName(p.type));
    n += snprintf(line + n, sizeof(line) - n, " |");
    for (size_t i = 0; i < DAVIS_PACKET_LEN; i++) n += snprintf(line + n, sizeof(line) - n, " %02X", p.bytes[i]);
    n += snprintf(line + n, sizeof(line) - n, " | air");
    for (size_t i = 0; i < DAVIS_PACKET_LEN; i++) n += snprintf(line + n, sizeof(line) - n, " %02X", rec.air[i]);
    if (p.crc != DAVIS_CRC_FAIL)
        n += snprintf(line + n, sizeof(line) - n, " | wind %u mph %.0f deg | %s", p.windMph, p.windDirDeg, val);
    if (r.hasTiming) n += snprintf(line + n, sizeof(line) - n, " | dt %+.1f ms", r.timingErrUs / 1000.0f);
    if (r.probe)     n += snprintf(line + n, sizeof(line) - n, " (AFC probe)");
    snprintf(line + n, sizeof(line) - n, " | %s\n", davisStateName(r.state));
    const RfTraceResult tr = valid ? RF_TR_OK : p.crc == DAVIS_CRC_FAIL ? RF_TR_FAILED : RF_TR_IGNORED;
    logLine(line, sniff, true, tr, p.crc != DAVIS_CRC_FAIL, RF_ID_DAVIS | p.txId);

    if (r.locked) {
        snprintf(line, sizeof(line), "Davis: locked on transmitter ID %u, ch%u %.4f MHz (offset %+.1f kHz), RSSI %.0f dBm.\n",
                 p.txId, r.channel, r.freqMHz, r.offsetHz / 1000.0f, rec.rssi);
        logLine(line, true, false);
    }
}

void handleEvent(const Rec& rec)
{
    const DavisEvent& e = rec.ev;
    char line[160], ts[20];
    timeText(ts, sizeof(ts), rec.atUs);
    const bool sniff = gCfg.davisSniffer != 0;
    switch (e.kind) {
        case DAVIS_EV_MISS:
            snprintf(line, sizeof(line), "DV %s ch%u MISS (%u in a row)\n", ts, e.a, e.b);
            logLine(line, sniff, true);
            break;
        case DAVIS_EV_PROBE_MISS:
            snprintf(line, sizeof(line), "DV %s ch%u AFC probe %+.1fk: nothing\n", ts, e.a, e.offsetHz / 1000.0f);
            logLine(line, sniff, true);
            break;
        case DAVIS_EV_LOST:
            snprintf(line, sizeof(line), "Davis: transmitter lost (%s) - searching again on ch%u, offset %+.1f kHz.\n",
                     ts, e.a, e.offsetHz / 1000.0f);
            logLine(line, true, false);
            break;
        case DAVIS_EV_SCAN_STEP:
            snprintf(line, sizeof(line), "DV %s search: ch%u, offset %+.1f kHz\n", ts, e.a, e.offsetHz / 1000.0f);
            logLine(line, sniff, true);
            break;
        case DAVIS_EV_AFC_STEP:
            snprintf(line, sizeof(line), "Davis: AFC offset now %+.1f kHz.\n", e.offsetHz / 1000.0f);
            logLine(line, true, false);
            break;
        default:
            break;
    }
}

void summary(unsigned long now, bool force)
{
    const unsigned long every = gCfg.davisSniffer || gCfg.davisRaw ? 60000UL : 600000UL;
    if (now - minuteStartMs >= 60000UL) { minuteStartMs = now; linesThisMinute = 0; }
    if (!force && now - lastSummaryMs < every) return;
    const DavisDiag& d = gDavisDiag;
    const DavisDiag& a = atMinuteStart;
    uint32_t slots = d.trackSlots - a.trackSlots, miss = d.misses - a.misses;
    char sync[16] = "-";
    if (slots) snprintf(sync, sizeof(sync), "%.1f %%", 100.0f * (slots - miss) / slots);
    char line[300];
    int n = snprintf(line, sizeof(line),
                     "Davis summary %lu s: %lu ok, %lu CRC fail, %lu other, %lu missed, sync %s | %s id%u ch%u %.4f MHz, "
                     "period %.2f ms, offset %+.1f kHz (set %+.1f)",
                     (now - lastSummaryMs) / 1000, (unsigned long)(d.packets - a.packets),
                     (unsigned long)(d.crcErrors - a.crcErrors), (unsigned long)(d.otherPackets - a.otherPackets),
                     (unsigned long)miss, sync, davisDiagStateName(d.state), d.txId, d.channel, d.freqMHz, d.periodMs,
                     d.learnedOffsetHz / 1000.0f, d.offsetHz / 1000.0f);
    if (d.havePacket) n += snprintf(line + n, sizeof(line) - n, ", last RSSI %.0f dBm", d.lastRssi);
    if (suppressedLines) n += snprintf(line + n, sizeof(line) - n, " (%lu lines only in /api/davis/log)", (unsigned long)suppressedLines);
    if (queueDrops != lastQueueDrops) n += snprintf(line + n, sizeof(line) - n, " (%lu records dropped)", (unsigned long)(queueDrops - lastQueueDrops));
    snprintf(line + n, sizeof(line) - n, "\n");
    logLine(line, true, false);
    suppressedLines = 0;
    lastQueueDrops = queueDrops;
    atMinuteStart = d;
    lastSummaryMs = now;
}

void start()
{
    if (!lock)    lock = xSemaphoreCreateMutex();
    if (!queue)   queue = xQueueCreate(16, sizeof(Rec));
    if (!rfTraceRunning()) {   // the sniffer capture: an open-ended radio trace (all packets)
        char err[120];
        traceByDavis = rfTraceStart(0, RF_TRACE_ALL, 0, true, err, sizeof(err));
        if (!traceByDavis) dbgSerial.printf("Davis: no packet capture (%s).\n", err);
    }
    // Exclusive: the table holds only the Davis transmitter.
    rfSensorsClear();
    rainCounter.reset();
    rainTotalMm = 0.0f;
    gDavisDiag = DavisDiag();
    gDavisDiag.running = true;
    atMinuteStart = gDavisDiag;
    minuteStartMs = lastSummaryMs = millis();
    linesThisMinute = 0; suppressedLines = 0; lastState = 0xFF;
    applied = pending = settingsFromConfig();
    settingsChanged = false;
    stopRequest = false;
    owned = true;          // before the task touches the radio
    taskRunning = true;
    DavisRadio::setRadio(&radio);
    DavisRadio::setPacketHook(packetHook);
    if (xTaskCreatePinnedToCore(receiveTask, "davis", 4096, nullptr, 5, &task, 1) != pdPASS) {
        task = nullptr;
        taskRunning = false;
        owned = false;
        gDavisDiag.running = false;
        dbgSerial.println("Davis: could not start the receive task (out of memory?) - retrying.");
        return;
    }
    char line[200];
    snprintf(line, sizeof(line), "Davis: exclusive mode on - LoRaWAN paused, no other wireless sensors. %s, ID %s, "
             "offset %+.1f kHz, scan +-%d kHz, %s%s%s.\n",
             applied.hop.band->name, gCfg.davisTxId ? String(gCfg.davisTxId).c_str() : "first heard",
             gCfg.davisOffsetHz / 1000.0f, gCfg.davisScanKhz, gCfg.davisSniffer ? "sniffer on" : "sniffer off",
             gCfg.davisRaw ? ", RAW CAPTURE ch" : "", gCfg.davisRaw ? String(gCfg.davisRawCh).c_str() : "");
    logLine(line, true, false);
}

} // namespace

bool davisRadioOwned() { return owned; }
bool davisLoraPaused() { return owned || rfDavisMode(gCfg.rfEn, gCfg.rfProto); }

void davisRxLoop()
{
    if (!owned) { start(); if (!owned) return; }
    // settings (live)
    RxSettings s = settingsFromConfig();
    if (!sameSettings(s, applied)) {
        bool restart = s.hop.band != applied.hop.band || s.hop.txId != applied.hop.txId || s.hop.raw != applied.hop.raw;
        applied = s;
        xSemaphoreTake(lock, portMAX_DELAY);
        pending = s;
        settingsChanged = true;
        xSemaphoreGive(lock);
        if (task) xTaskNotifyGive(task);
        char line[160];
        snprintf(line, sizeof(line), "Davis: settings applied%s (ID %d, offset %+.1f kHz, window %d / guard %d ms, %s).\n",
                 restart ? " - searching again" : "", gCfg.davisTxId, gCfg.davisOffsetHz / 1000.0f, gCfg.davisWindowMs,
                 gCfg.davisGuardMs, gCfg.davisRaw ? "raw capture" : "hopping");
        logLine(line, true, false);
        if (restart) { rainCounter.reset(); }
    }
    // received packets and events
    Rec rec;
    while (xQueueReceive(queue, &rec, 0) == pdTRUE) {
        if (rec.kind == REC_PACKET)     handlePacket(rec);
        else if (rec.kind == REC_EVENT) handleEvent(rec);
        else {
            if (gDavisDiag.radioError != rec.err)
                dbgSerial.printf("Davis: radio error %d (retrying every 5 s).\n", rec.err);
            gDavisDiag.radioError = rec.err;
        }
    }
    // state snapshot of the task
    TaskStatus t;
    portENTER_CRITICAL(&mux);
    t = status;
    portEXIT_CRITICAL(&mux);
    DavisDiag& d = gDavisDiag;
    d.state = t.state;  d.txId = t.txId;  d.channel = t.channel;  d.freqMHz = t.freqMHz;
    d.periodMs = (float)(t.periodUs / 1000.0);
    d.offsetHz = gCfg.davisOffsetHz;  d.learnedOffsetHz = t.learned;
    d.packets = t.accepted;  d.crcErrors = t.crcErrors;  d.otherPackets = t.other;
    d.misses = t.misses;  d.trackSlots = t.trackSlots;  d.resyncs = t.resyncs;
    if (t.accepted && d.radioError) d.radioError = 0;
    if (t.state != lastState) {
        if (lastState != 0xFF && t.state == DAVIS_DIAG_TRACK)
            dbgSerial.printf("Davis: tracking transmitter ID %u, period %.2f ms.\n", t.txId, d.periodMs);
        lastState = t.state;
    }
    summary(millis(), false);
}

void davisRxStop()
{
    if (!owned) return;
    // Cooperative stop: the task checks stopRequest at least every 100 ms and
    // after each radio call; a radio call on a hung chip can take a few
    // seconds (RadioLib BUSY / SPI timeouts, several per begin()). Deleting it
    // from here could leave the SPI bus or `lock` taken for good, so a task
    // that does not end within DAVIS_STOP_WAIT_MS restarts the node instead.
    constexpr uint32_t DAVIS_STOP_WAIT_MS = 10000;
    stopRequest = true;
    if (task) xTaskNotifyGive(task);
    for (uint32_t waitedMs = 0; waitedMs < DAVIS_STOP_WAIT_MS && taskRunning; waitedMs += 10) vTaskDelay(pdMS_TO_TICKS(10));
    if (taskRunning) systemRestart("Davis task did not stop");
    task = nullptr;
    DavisRadio::setPacketHook(nullptr);
    if (queue) xQueueReset(queue);
    summary(millis(), true);
    owned = false;
    gDavisDiag.running = false;
    if (traceByDavis && rfTraceStatusNow().autoStarted) rfTraceStop("Davis mode left");   // kept for download
    traceByDavis = false;
    rfSensorsClear();
    dbgSerial.println("Davis: exclusive mode off - radio released (LoRaWAN / other wireless sensors resume).");
}
