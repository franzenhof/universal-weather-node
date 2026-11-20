#include "DavisHopper.h"

// EU 868 MHz channels in hop order. Measured from two EU transmitters with a
// TCXO-referenced SDR (kobuki/VPTools, 2026-10: spacing ~120.2 kHz) ...
static const float EU_MEASURED[5] = { 868.0589f, 868.2993f, 868.5396f, 868.1792f, 868.4196f };
// ... and the original DavisRFM69 table (DeKay, 2014), off by up to 12 kHz.
static const float EU_CLASSIC[5]  = { 868.0667f, 868.2971f, 868.5275f, 868.1819f, 868.4123f };
const DavisBand DAVIS_BAND_EU         = { EU_MEASURED, 5, "EU (measured)" };
const DavisBand DAVIS_BAND_EU_CLASSIC = { EU_CLASSIC, 5, "EU (classic)" };

const char* davisStateName(DavisHopState s)
{
    switch (s) {
        case DAVIS_ST_TRACK: return "TRACK";
        case DAVIS_ST_LOST:  return "LOST";
        case DAVIS_ST_RAW:   return "RAW";
        default:             return "ACQUIRE";
    }
}

const char* davisPktClassName(DavisPktClass c)
{
    switch (c) {
        case DAVIS_PKT_ACCEPTED:      return "ok";
        case DAVIS_PKT_CRC_FAIL:      return "CRC-FAIL";
        case DAVIS_PKT_OTHER_ID:      return "other-id";
        case DAVIS_PKT_REPEATER:      return "repeater";
        case DAVIS_PKT_BAD_TRAILER:   return "bad-trailer";
        case DAVIS_PKT_OUT_OF_WINDOW: return "out-of-window";
        case DAVIS_PKT_RAW_OK:        return "ok";
        default:                      return "?";
    }
}

float DavisHopper::channelMHz(uint8_t ch) const
{
    const DavisBand* b = cfg_.band ? cfg_.band : &DAVIS_BAND_EU;
    return b->mhz[ch % b->count];
}

float DavisHopper::tuneMHz() const
{
    return channelMHz(ch_) + tuneOffset_ / 1e6f;
}

uint16_t DavisHopper::scanCount() const
{
    if (!cfg_.scanSpanKhz) return 1;
    return (uint16_t)(1 + 2 * ((int32_t)cfg_.scanSpanKhz * 1000 / DAVIS_SCAN_STEP_HZ));
}

int32_t DavisHopper::scanOffset(uint16_t idx) const
{
    if (idx == 0) return scanCenter_;
    int32_t m = (idx + 1) / 2;
    return scanCenter_ + ((idx & 1) ? m : -m) * DAVIS_SCAN_STEP_HZ;
}

int64_t DavisHopper::dwellUs() const
{
    uint32_t t = davisIntervalUs(lockedId_ ? lockedId_ : 8);   // unknown ID: the longest period
    uint8_t n = cfg_.band ? cfg_.band->count : 5;
    return (int64_t)n * t + DAVIS_DWELL_EXTRA_US;
}

void DavisHopper::push(DavisEventKind k, uint8_t a, uint8_t b, int32_t off, int64_t at)
{
    const uint8_t cap = sizeof(ev_) / sizeof(ev_[0]);
    if (evCount_ == cap) { evHead_ = (uint8_t)((evHead_ + 1) % cap); evCount_--; }   // drop the oldest
    DavisEvent& e = ev_[(evHead_ + evCount_) % cap];
    e.kind = k; e.a = a; e.b = b; e.offsetHz = off; e.atUs = at;
    evCount_++;
}

void DavisHopper::configure(const DavisHopperConfig& c, int64_t nowUs)
{
    cfg_ = c;
    if (!cfg_.band || !cfg_.band->count) cfg_.band = &DAVIS_BAND_EU;
    if (cfg_.txId > 8) cfg_.txId = 0;
    if (!cfg_.lostAfter) cfg_.lostAfter = 1;
    accepted = crcErrors = otherPackets = misses = trackSlots = resyncs = scanSteps = 0;
    evHead_ = evCount_ = 0;
    lockedId_  = cfg_.txId;
    nominalUs_ = lockedId_ ? davisIntervalUs(lockedId_) : 0.0;
    periodUs_  = nominalUs_;
    learned_   = cfg_.offsetHz;
    lastOkUs_  = 0;
    probe_     = false;
    slotNo_    = 0;
    probeN_[0] = probeN_[1] = probeOk_[0] = probeOk_[1] = 0;
    missStreak_ = 0;
    nextUs_ = anchorUs_ = 0;
    if (cfg_.raw) {
        state_ = DAVIS_ST_RAW;
        ch_ = (uint8_t)(cfg_.rawChannel % cfg_.band->count);
        tuneOffset_ = cfg_.offsetHz;
        dwellStartUs_ = nowUs;
        return;
    }
    startAcquire(nowUs, 0, cfg_.offsetHz, false);
}

void DavisHopper::update(const DavisHopperConfig& c, int64_t nowUs)
{
    const DavisBand* band = c.band && c.band->count ? c.band : &DAVIS_BAND_EU;
    if (band != cfg_.band || c.txId != cfg_.txId || c.raw != cfg_.raw ||
        (c.raw && c.rawChannel != cfg_.rawChannel)) {
        configure(c, nowUs);
        return;
    }
    const int32_t oldBase = cfg_.offsetHz;
    cfg_.guardMs = c.guardMs;  cfg_.windowMs = c.windowMs;
    cfg_.lostAfter = c.lostAfter ? c.lostAfter : 1;
    cfg_.scanSpanKhz = c.scanSpanKhz;
    cfg_.afc = c.afc;
    cfg_.rawChannel = c.rawChannel;
    if (c.offsetHz != oldBase) {   // a new base offset replaces anything learned
        cfg_.offsetHz = c.offsetHz;
        learned_ = scanCenter_ = c.offsetHz;
        probeN_[0] = probeN_[1] = probeOk_[0] = probeOk_[1] = 0;
    }
    if (!cfg_.afc) probe_ = false;
    switch (state_) {
        case DAVIS_ST_RAW:   tuneOffset_ = cfg_.offsetHz; break;
        case DAVIS_ST_TRACK: tuneOffset_ = probe_ ? learned_ + probeSide_ * DAVIS_AFC_PROBE_HZ : learned_; break;
        default:
            if (scanIdx_ >= scanCount()) scanIdx_ = 0;
            tuneOffset_ = scanOffset(scanIdx_);
            break;
    }
}

void DavisHopper::startAcquire(int64_t nowUs, uint8_t parkCh, int32_t center, bool lost)
{
    state_ = lost ? DAVIS_ST_LOST : DAVIS_ST_ACQUIRE;
    ch_ = (uint8_t)(parkCh % cfg_.band->count);
    scanCenter_ = center;
    scanIdx_ = 0;
    tuneOffset_ = scanOffset(0);
    dwellStartUs_ = nowUs;
    dwellExtended_ = false;
    probe_ = false;
    missStreak_ = 0;
}

void DavisHopper::nextSlot(bool afterPacket)
{
    (void)afterPacket;
    ch_ = (uint8_t)((ch_ + 1) % cfg_.band->count);
    k_++;
    nextUs_ = anchorUs_ + (int64_t)(k_ * periodUs_ + 0.5);
    slotNo_++;
    probe_ = cfg_.afc && (slotNo_ % 4) == 0;
    if (probe_) {
        probeSide_ = (int8_t)-probeSide_;
        tuneOffset_ = learned_ + probeSide_ * DAVIS_AFC_PROBE_HZ;
    } else {
        tuneOffset_ = learned_;
    }
}

void DavisHopper::enterTrack(uint8_t rxCh, int64_t rxUs)
{
    state_ = DAVIS_ST_TRACK;
    ch_ = rxCh;
    anchorUs_ = rxUs;
    k_ = 0;
    missStreak_ = 0;
    nextSlot(true);
}

void DavisHopper::afcResult(bool ok)
{
    int i = probeSide_ > 0 ? 0 : 1;
    if (probeN_[i] < 255) probeN_[i]++;
    if (ok && probeOk_[i] < 255) probeOk_[i]++;
    if (probeN_[0] < 4 || probeN_[1] < 4) return;
    int d = (int)probeOk_[0] - (int)probeOk_[1];
    int32_t before = learned_;
    if (d >= 2)  learned_ += DAVIS_AFC_STEP_HZ;
    if (d <= -2) learned_ -= DAVIS_AFC_STEP_HZ;
    if (learned_ > cfg_.offsetHz + DAVIS_AFC_LIMIT_HZ) learned_ = cfg_.offsetHz + DAVIS_AFC_LIMIT_HZ;
    if (learned_ < cfg_.offsetHz - DAVIS_AFC_LIMIT_HZ) learned_ = cfg_.offsetHz - DAVIS_AFC_LIMIT_HZ;
    probeN_[0] = probeN_[1] = probeOk_[0] = probeOk_[1] = 0;
    if (learned_ != before) push(DAVIS_EV_AFC_STEP, ch_, 0, learned_, anchorUs_);
}

DavisEvent DavisHopper::poll(int64_t nowUs)
{
    if (!evCount_) {
        if (state_ == DAVIS_ST_ACQUIRE || state_ == DAVIS_ST_LOST) {
            if (nowUs - dwellStartUs_ >= dwellUs()) {
                scanSteps++;
                if (++scanIdx_ >= scanCount()) {
                    scanIdx_ = 0;
                    ch_ = (uint8_t)((ch_ + 1) % cfg_.band->count);
                }
                tuneOffset_ = scanOffset(scanIdx_);
                dwellStartUs_ = nowUs;
                dwellExtended_ = false;
                push(DAVIS_EV_SCAN_STEP, ch_, (uint8_t)scanIdx_, tuneOffset_, nowUs);
            }
        } else if (state_ == DAVIS_ST_TRACK) {
            if (nowUs > nextUs_ + (int64_t)cfg_.windowMs * 1000 + DAVIS_LATENCY_US) {
                const uint8_t missedCh = ch_;
                const int64_t at = nextUs_;
                if (probe_) {
                    push(DAVIS_EV_PROBE_MISS, missedCh, missStreak_, tuneOffset_, at);
                    afcResult(false);
                    nextSlot(false);
                } else {
                    misses++;
                    trackSlots++;
                    if (missStreak_ < 255) missStreak_++;
                    push(DAVIS_EV_MISS, missedCh, missStreak_, tuneOffset_, at);
                    if (missStreak_ >= cfg_.lostAfter) {
                        uint8_t park = (uint8_t)((missedCh + 1) % cfg_.band->count);
                        startAcquire(nowUs, park, learned_, true);
                        push(DAVIS_EV_LOST, ch_, 0, tuneOffset_, nowUs);
                    } else {
                        nextSlot(false);
                    }
                }
            }
        }
    }
    if (!evCount_) return DavisEvent();
    DavisEvent e = ev_[evHead_];
    evHead_ = (uint8_t)((evHead_ + 1) % (sizeof(ev_) / sizeof(ev_[0])));
    evCount_--;
    return e;
}

int64_t DavisHopper::nextDeadlineUs() const
{
    if (evCount_) return 0;
    switch (state_) {
        case DAVIS_ST_TRACK: return nextUs_ + (int64_t)cfg_.windowMs * 1000 + DAVIS_LATENCY_US + 1;
        case DAVIS_ST_RAW:   return INT64_MAX;
        default:             return dwellStartUs_ + dwellUs();
    }
}

DavisRxResult DavisHopper::onPacket(const uint8_t air[DAVIS_PACKET_LEN], int64_t rxUs, DavisModel model)
{
    DavisRxResult r;
    r.channel  = ch_;
    r.freqMHz  = tuneMHz();
    r.offsetHz = tuneOffset_;
    r.probe    = state_ == DAVIS_ST_TRACK && probe_;
    davisDecode(air, r.pkt, model);

    if (r.pkt.crc == DAVIS_CRC_FAIL) {
        crcErrors++;
        r.cls = DAVIS_PKT_CRC_FAIL;
        // Something passed the sync word while searching: maybe the
        // transmitter slightly off frequency - stay on this offset once more.
        if ((state_ == DAVIS_ST_ACQUIRE || state_ == DAVIS_ST_LOST) && !dwellExtended_) {
            dwellStartUs_ = rxUs;
            dwellExtended_ = true;
        }
    } else if (state_ == DAVIS_ST_RAW) {
        r.cls = DAVIS_PKT_RAW_OK;
        accepted++;
        lastOkUs_ = rxUs;
    } else if (r.pkt.crc == DAVIS_CRC_REPEATER) {
        r.cls = DAVIS_PKT_REPEATER;
        otherPackets++;
    } else if (davisTrailerZeroBits(r.pkt.bytes) > 2) {
        r.cls = DAVIS_PKT_BAD_TRAILER;
        otherPackets++;
    } else if (lockedId_ && r.pkt.txId != lockedId_) {
        r.cls = DAVIS_PKT_OTHER_ID;
        otherPackets++;
    } else if (state_ == DAVIS_ST_TRACK) {
        int64_t err = rxUs - nextUs_;
        r.hasTiming = true;
        r.timingErrUs = (int32_t)err;
        if (err < -(int64_t)cfg_.guardMs * 1000 || err > (int64_t)cfg_.windowMs * 1000 + DAVIS_LATENCY_US) {
            r.cls = DAVIS_PKT_OUT_OF_WINDOW;
            otherPackets++;
        } else {
            // PLL: the error over k_ periods corrects the period estimate
            periodUs_ += 0.25 * (double)err / (double)(k_ ? k_ : 1);
            const double lim = nominalUs_ * 200e-6;   // +-200 ppm
            if (periodUs_ > nominalUs_ + lim) periodUs_ = nominalUs_ + lim;
            if (periodUs_ < nominalUs_ - lim) periodUs_ = nominalUs_ - lim;
            if (probe_) afcResult(true);
            else        trackSlots++;
            accepted++;
            missStreak_ = 0;
            lastOkUs_ = rxUs;
            anchorUs_ = rxUs;
            k_ = 0;
            nextSlot(true);
            r.cls = DAVIS_PKT_ACCEPTED;
        }
    } else {   // ACQUIRE / LOST: found it
        if (!lockedId_) lockedId_ = r.pkt.txId;
        double nominal = davisIntervalUs(lockedId_);
        if (nominal != nominalUs_ || periodUs_ <= 0.0) { nominalUs_ = nominal; periodUs_ = nominal; }
        if (state_ == DAVIS_ST_LOST) resyncs++;
        learned_ = tuneOffset_;
        accepted++;
        lastOkUs_ = rxUs;
        r.locked = true;
        r.cls = DAVIS_PKT_ACCEPTED;
        enterTrack(ch_, rxUs);
    }
    r.state = state_;
    return r;
}
