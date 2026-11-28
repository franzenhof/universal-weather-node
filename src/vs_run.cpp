#include "vs_run.h"
#include "vsensor.h"
#include "vs_store.h"
#include "config.h"
#include "globals.h"
#include "ph_live.h"
#include "sources.h"
#include "rf_sensors.h"
#include "davis_rx.h"
#include "ota_upload.h"
#include "lora_transport.h"
#include "serial_log.h"
#include "radio_board.h"
#include "psram.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <math.h>
#include <memory>
#include <new>
#include <optional>
#include <string.h>

namespace {
bool     armed[VS_SLOTS] = {};
uint16_t armedSig[VS_SLOTS] = {};
uint64_t dueMs[VS_SLOTS] = {};
uint64_t armedMs[VS_SLOTS] = {};   // enabled / changed / node started: IT+ pairing window (vsPairing())
int16_t  lastTxError = 0;
constexpr uint32_t FIRST_TX_MS = 10000, SLOT_STAGGER_MS = 3000;   // after enabling: slots not all at once

// Davis mode: one waiting frame per slot, handed from the loop task (builds
// it from the node's values) to the Davis receive task (owns the radio,
// sends it in the gap after a packet, vsDavisSendInGap()). The loop task
// collects the result (counters, airtime ledger). 8 x 72 bytes, in the PSRAM
// (bigAlloc) on the first frame queued in Davis mode, never freed.
enum PendState : uint8_t { PEND_EMPTY = 0, PEND_WAITING, PEND_SENDING, PEND_DONE };
struct Pending {
    PendState              state = PEND_EMPTY;
    int16_t                err = 0;          // PEND_DONE: RadioLib code
    uint32_t               airMs = 0;
    uint32_t               doneMs = 0;       // PEND_DONE: millis()
    uint64_t               queuedMs = 0;
    std::optional<VsFrame> frame;            // VsFrame is made only by vsBuildFrame()
};
Pending*     pend = nullptr;   // set once by the loop task before the first frame is queued
portMUX_TYPE pendMux = portMUX_INITIALIZER_UNLOCKED;

// Value names: the placeholders of the uploads (ph_live.h, metric), plus
// the board's DS18B20 probes and the station's running rain total.
bool lookup(const char* n, float& v, void* ctx)
{
    if (n[0] == 'd' && n[1] == 's' && n[2] >= '1' && n[2] <= '3' && !n[3]) {
        const int p = n[2] - '1';
        if (p >= bus1Count || !(bus1Temps[p] > -100.0f)) return false;   // DEVICE_DISCONNECTED_C = -127
        v = bus1Temps[p];
        return true;
    }
    if (!strcmp(n, "rain_total")) {
        if (!srcActive(SG_RAIN)) return false;
        v = rainMMSinceStart;
        return true;
    }
    PhValue pv;
    if (!phLiveLookup(n, pv, ctx) || pv.kind == PK_TEXT) return false;
    v = pv.v;
    return true;
}

// Davis reception owns the radio: frames go through the Davis task's gaps.
bool davisMode()
{
    return davisRadioOwned() || rfDavisMode(gCfg.rfEn, gCfg.rfProto);
}

// The FSK transmit itself (caller owns the radio): one frame, RadioLib code.
int16_t fskTransmit(const VsFrame& f)
{
    radio.clearPacketReceivedAction();   // the receivers' DIO1 handlers must not see TX done
    ConfigFSK_t c;
    c.frequency          = VS_FREQ_MHZ;
    c.bitRate            = f.kbps();         // 17.241 or (IT+ TX35 style) 9.579 kbit/s
    c.frequencyDeviation = VS_DEV_KHZ;
    c.receiverBandwidth  = 234.3f;
    c.power              = (int8_t)vsTxPowerDbm();   // radiated; BoardSX1262 subtracts the front end's PA gain
    c.preambleLength     = VS_PREAMBLE_BITS;
    int16_t st = radio.beginFSK(c);    // NRZ, no whitening, no shaping (RadioLib FSK defaults)
    if (st == RADIOLIB_ERR_NONE) st = radio.fixedPacketLengthMode((uint8_t)f.size());
    if (st == RADIOLIB_ERR_NONE) st = radio.setCRC(0);   // CRC-8 + sum are in the frame
    uint8_t sync[] = { 0x2D, 0xD4 };
    if (st == RADIOLIB_ERR_NONE) st = radio.setSyncWord(sync, 2);
    if (st == RADIOLIB_ERR_NONE) st = radio.transmit(f.data(), f.size());   // RF switch table: FEM to PA for TX
    radio.standby();
    return st;
}

// Loop task: results of the Davis task's sends into the counters and the
// ledger; waiting frames are dropped when Davis mode ended (keep = false).
void davisCollect(uint64_t now, bool keep)
{
    if (!pend) return;
    for (int i = 0; i < VS_SLOTS; i++) {
        Pending done;
        bool dropped = false;
        portENTER_CRITICAL(&pendMux);
        Pending& p = pend[i];
        if (p.state == PEND_DONE) { done = p; p.state = PEND_EMPTY; p.frame.reset(); }
        else if (p.state == PEND_WAITING && !keep) { dropped = true; p.state = PEND_EMPTY; p.frame.reset(); }
        portEXIT_CRITICAL(&pendMux);
        VsSlotStatus& st = gVsStatus[i];
        if (dropped) st.deferDavis++;
        if (done.state != PEND_DONE) continue;
        gVsBand.add(now, done.airMs);   // counted even when it failed half-way
        if (done.err == RADIOLIB_ERR_NONE) {
            st.sent++;
            st.lastTxMs = done.doneMs;
            st.lastError = 0;
            if (st.sent == 1) dbgSerial.printf("Virtual sensor %d: first frame sent in a Davis gap\n", i + 1);
        } else {
            st.skipBusy++;
            st.lastError = done.err;
            dbgSerial.printf("Virtual sensor %d: transmit in the Davis gap failed (%d)\n", i + 1, done.err);
        }
    }
}

// A slot is off: its waiting frame is dropped without counting.
void davisDrop(int i)
{
    if (!pend) return;
    portENTER_CRITICAL(&pendMux);
    if (pend[i].state == PEND_WAITING) { pend[i].state = PEND_EMPTY; pend[i].frame.reset(); }
    portEXIT_CRITICAL(&pendMux);
}

// Airtime of the frames waiting for (or in) a Davis gap: kept free in the budget.
uint32_t davisPendingAirMs()
{
    uint32_t ms = 0;
    if (!pend) return 0;
    portENTER_CRITICAL(&pendMux);
    for (int i = 0; i < VS_SLOTS; i++)
        if (pend[i].state == PEND_WAITING || pend[i].state == PEND_SENDING) ms += pend[i].airMs;
    portEXIT_CRITICAL(&pendMux);
    return ms;
}

// The slot's frame waits for the next Davis gap; a frame still waiting from
// the slot's last turn is replaced (newer values) and counted as deferred.
void davisQueue(int i, const VsFrame& f, uint32_t airMs, uint64_t now)
{
    if (!pend) {
        void* m = bigAlloc(sizeof(Pending) * VS_SLOTS);
        if (!m) { gVsStatus[i].skipBusy++; return; }
        Pending* p = static_cast<Pending*>(m);
        for (int k = 0; k < VS_SLOTS; k++) new (&p[k]) Pending();
        portENTER_CRITICAL(&pendMux);
        pend = p;
        portEXIT_CRITICAL(&pendMux);
    }
    bool replaced = false, busy = false;
    portENTER_CRITICAL(&pendMux);
    Pending& p = pend[i];
    if (p.state == PEND_SENDING || p.state == PEND_DONE) busy = true;   // on air right now: this turn's frame is not sent
    else {
        replaced = p.state == PEND_WAITING;
        p.frame = f;
        p.airMs = airMs;
        p.queuedMs = now;
        p.state = PEND_WAITING;
    }
    portEXIT_CRITICAL(&pendMux);
    if (replaced || busy) gVsStatus[i].deferDavis++;
}
} // namespace

int vsDavisSendInGap(const VsDavisGap& g)
{
    int n = 0;
    for (;;) {
        std::optional<VsFrame> f;
        int slot = -1;
        portENTER_CRITICAL(&pendMux);
        if (pend) {   // the pointer is set once, under this lock
            bool waiting[VS_SLOTS];
            uint64_t queued[VS_SLOTS];
            for (int i = 0; i < VS_SLOTS; i++) { waiting[i] = pend[i].state == PEND_WAITING && pend[i].frame; queued[i] = pend[i].queuedMs; }
            slot = vsDavisPick(waiting, queued);
            if (slot >= 0 && vsDavisGapAllows(g, esp_timer_get_time(), n, pend[slot].airMs)) {
                pend[slot].state = PEND_SENDING;
                f = pend[slot].frame;
            } else {
                slot = -1;
            }
        }
        portEXIT_CRITICAL(&pendMux);
        if (slot < 0) break;
        const int16_t st = fskTransmit(*f);
        portENTER_CRITICAL(&pendMux);
        pend[slot].state = PEND_DONE;
        pend[slot].err = st;
        pend[slot].doneMs = millis();
        portEXIT_CRITICAL(&pendMux);
        n++;
    }
    if (n) gVsDavisGapFrames = gVsDavisGapFrames + n;
    return n;
}

void vsDavisNoteGapUse(uint32_t us)
{
    if (us > gVsDavisGapMaxUs) gVsDavisGapMaxUs = us;
}

uint64_t vsNowMs() { return (uint64_t)esp_timer_get_time() / 1000; }

void vsNodeMac(uint8_t mac[6])
{
    const uint64_t m = ESP.getEfuseMac();   // byte 0 of the MAC in the lowest bits
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(m >> (8 * i));
}

bool vsIdReceived(const VsSlotCfg& c, int slot)
{
    uint8_t mac[6];
    vsNodeMac(mac);
    for (const RfSensorReading& r : gRfSensors) if (r.used && vsIdCollides(c, slot, mac, r.id)) return true;
    return false;
}

bool vsActive()
{
    if (!vsSwitchedOn()) return false;
    for (int i = 0; i < VS_SLOTS; i++) {
        const VsSlotCfg c = vsSlotRead(i);
        if (c.enabled && c.type != VS_OFF) return true;
    }
    return false;
}

void vsNoteLoraAirtime(uint32_t ms)
{
    gVsBand.add(vsNowMs(), ms);
}

// The only place in the firmware that transmits FSK. Takes a VsFrame (made
// by vsBuildFrame() from the node's own values) - never bytes.
bool vsRadioSend(const VsFrame& f)
{
    if (!f.size()) return false;
    loraRadioTaken();                    // wireless-sensor reception stops; rfSensorsLoop() restarts it
    const int16_t st = fskTransmit(f);
    if (!gCfg.rfEn) radio.sleep();       // nobody listens: SX1262 and front end off until the next user
    lastTxError = st;
    return st == RADIOLIB_ERR_NONE;
}

void vsLoop()
{
    const uint64_t now = vsNowMs();
    const bool davis = davisMode();
    davisCollect(now, davis);
    gVsBandLastHourMs = gVsBand.lastHourMs(now);
    if (!vsActive()) {
        for (int i = 0; i < VS_SLOTS; i++) { armed[i] = false; davisDrop(i); }
        return;
    }
    uint8_t mac[6];
    vsNodeMac(mac);
    std::unique_ptr<PhLive> live;
    for (int i = 0; i < VS_SLOTS; i++) {
        const VsSlotCfg c = vsSlotRead(i);
        if (!c.enabled || c.type == VS_OFF || c.type >= VS_TYPE_COUNT) { armed[i] = false; davisDrop(i); continue; }
        const uint16_t sig = (uint16_t)(c.type | c.channel << 8);
        if (!armed[i] || armedSig[i] != sig) {
            armed[i] = true; armedSig[i] = sig; armedMs[i] = now;
            dueMs[i] = now + FIRST_TX_MS + (uint64_t)i * SLOT_STAGGER_MS;
            continue;
        }
        if (now < dueMs[i]) continue;
        const bool busy = otaUploadActive();
        if (!busy && !davis && vsHoldForRx(now, dueMs[i], rfSensorsPacketDue())) continue;
        const VsTypeInfo& t = VS_TYPES[c.type];
        const int32_t jitter = (int32_t)(esp_random() % (2 * VS_JITTER_MS + 1)) - VS_JITTER_MS;
        const uint64_t next = vsNextDue(dueMs[i], now, t.intervalMs, jitter);
        dueMs[i] = next;
        VsSlotStatus& st = gVsStatus[i];
        if (busy) { st.skipBusy++; continue; }
        const uint32_t air = vsTypeAirtimeMs(c.type);
        if (!vsBudgetAllows(gVsBand, now, air + (davis ? davisPendingAirMs() : 0))) { st.skipBudget++; continue; }
        if (!live) {
            live.reset(new (std::nothrow) PhLive());
            if (!live) { st.skipMissing++; continue; }
            phLiveInit(*live, nullptr, /*liveScalars=*/true);   // the newest value per packet
        }
        VsValues v;
        if (!vsResolve(c, lookup, live.get(), v)) { st.skipMissing++; continue; }
        const VsFrame f = vsBuildFrame(c, i, mac, v, vsPairing(now, armedMs[i]));
        if (!f.size()) { st.skipMissing++; continue; }
        if (davis) { davisQueue(i, f, air, now); continue; }   // sent by the Davis task in the gap after a packet
        const bool ok = vsRadioSend(f);
        gVsBand.add(now, air);   // counted even when it failed half-way
        if (ok) {
            st.sent++;
            st.lastTxMs = millis();
            st.lastError = 0;
        } else {
            st.skipBusy++;
            st.lastError = lastTxError;
            dbgSerial.printf("Virtual sensor %d: transmit failed (%d)\n", i + 1, lastTxError);
        }
        if (ok && st.sent == 1)   // the first frame per slot since start; the counters show the rest
            dbgSerial.printf("Virtual sensor %d (%s, ID %08lX): sent %u bytes\n", i + 1, t.model, (unsigned long)f.id(), (unsigned)f.size());
    }
    gVsBandLastHourMs = gVsBand.lastHourMs(now);
}
