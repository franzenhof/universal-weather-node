#include "influx_queue.h"
#include <cstring>

InfluxReply influxClassify(int code)
{
    if (code >= 200 && code < 300) return IR_OK;
    if (code == 401 || code == 403 || code == 404 || code == 408 || code == 429) return IR_RETRY;
    if (code >= 400 && code < 500) return IR_DROP;
    return IR_RETRY;   // <= 0 (no reply), 1xx/3xx (unexpected), 5xx
}

uint32_t influxRetryDelayMs(uint32_t failsInRow)
{
    uint32_t d = INFLUX_RETRY_MIN_MS;
    for (uint32_t i = 1; i < failsInRow && d < INFLUX_RETRY_MAX_MS; i++) d *= 2;
    return d < INFLUX_RETRY_MAX_MS ? d : INFLUX_RETRY_MAX_MS;
}

void InfluxQueue::attach(uint8_t* buf, size_t cap)
{
    buf_ = buf;
    cap_ = buf ? cap : 0;
    head_ = used_ = count_ = 0;
    fails_ = 0;
    waiting_ = false;
    outage_ = false;
    outageResent_ = 0;
}

void InfluxQueue::readAt(size_t off, void* dst, size_t n) const
{
    off %= cap_;
    const size_t first = n < cap_ - off ? n : cap_ - off;
    memcpy(dst, buf_ + off, first);
    if (n > first) memcpy((uint8_t*)dst + first, buf_, n - first);
}

void InfluxQueue::writeAt(size_t off, const void* src, size_t n)
{
    off %= cap_;
    const size_t first = n < cap_ - off ? n : cap_ - off;
    memcpy(buf_ + off, src, first);
    if (n > first) memcpy(buf_, (const uint8_t*)src + first, n - first);
}

uint32_t InfluxQueue::lenAt(size_t off) const
{
    uint32_t len;
    readAt(off + 4, &len, 4);
    return len;
}

uint32_t InfluxQueue::oldestEpoch() const
{
    if (!count_) return 0;
    uint32_t e;
    readAt(head_, &e, 4);
    return e;
}

void InfluxQueue::dropOldest()
{
    if (!count_) return;
    const size_t n = HDR + lenAt(head_);
    head_ = (head_ + n) % cap_;
    used_ -= n;
    count_--;
    if (!count_) head_ = used_ = 0;
}

bool InfluxQueue::push(uint32_t epochSec, const char* body, size_t len)
{
    if (!buf_ || !body || !len) return false;
    const size_t need = HDR + len;
    if (need > cap_) { droppedFull_++; return false; }
    while (cap_ - used_ < need) { dropOldest(); droppedFull_++; }
    const size_t at = head_ + used_;
    const uint32_t hdr[2] = { epochSec, (uint32_t)len };
    writeAt(at, hdr, HDR);
    writeAt(at + HDR, body, len);
    used_ += need;
    count_++;
    return true;
}

size_t InfluxQueue::peek(std::string& out, size_t maxBytes) const
{
    size_t n = 0, off = head_, total = 0;
    while (n < count_) {
        const uint32_t len = lenAt(off);
        if (n && total + len > maxBytes) break;
        const size_t was = out.size();
        out.resize(was + len);
        readAt(off + HDR, &out[was], len);
        total += len;
        off += HDR + len;
        n++;
    }
    return n;
}

bool InfluxQueue::noteOffline()
{
    if (outage_) return false;
    outage_ = true;
    outageResent_ = 0;
    outageDropped0_ = droppedFull_ + droppedRejected_;
    return true;
}

bool InfluxQueue::due(uint32_t nowMs) const
{
    if (!count_) return false;
    return !waiting_ || (int32_t)(nowMs - nextTryMs_) >= 0;
}

InfluxEvent InfluxQueue::result(int code, size_t n, uint32_t nowMs)
{
    InfluxEvent ev;
    ev.action = influxClassify(code);
    if (n > count_) n = count_;
    if (ev.action == IR_RETRY) {
        ev.outageStarted = noteOffline();
        if (fails_ < UINT32_MAX) fails_++;
        waiting_ = true;
        nextTryMs_ = nowMs + influxRetryDelayMs(fails_);
        return ev;
    }
    // reachable: OK or refused - either way these pushes leave the ring
    for (size_t i = 0; i < n; i++) dropOldest();
    if (ev.action == IR_DROP) droppedRejected_ += (uint32_t)n;
    else if (outage_)         outageResent_ += (uint32_t)n;
    fails_ = 0;
    waiting_ = false;
    if (outage_ && !count_) {
        ev.cleared = true;
        ev.resent = outageResent_;
        ev.dropped = droppedFull_ + droppedRejected_ - outageDropped0_;
        outage_ = false;
    }
    return ev;
}
