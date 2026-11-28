// InfluxDB backfill ring (src/influx_queue.cpp): queueing, order, overflow
// (drop oldest, counted), chunking, reply classes (4xx drop / 5xx retry with
// backoff), timestamps kept in the bodies, outage start / cleared events.
#include "influx_queue.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// A body like the builder's: one line, ns timestamp at the end.
static std::string body(uint32_t epoch, size_t pad = 0)
{
    char b[96];
    snprintf(b, sizeof b, "weather,host=n t=%u %u000000000\n", epoch, epoch);
    return std::string(b) + std::string(pad, 'x');
}

static void push(InfluxQueue& q, uint32_t epoch, size_t pad = 0)
{
    const std::string s = body(epoch, pad);
    q.push(epoch, s.data(), s.size());
}

int main()
{
    // ---- classification and backoff ----
    assert(influxClassify(204) == IR_OK && influxClassify(200) == IR_OK);
    assert(influxClassify(400) == IR_DROP && influxClassify(413) == IR_DROP && influxClassify(422) == IR_DROP);
    assert(influxClassify(401) == IR_RETRY && influxClassify(403) == IR_RETRY && influxClassify(404) == IR_RETRY);
    assert(influxClassify(408) == IR_RETRY && influxClassify(429) == IR_RETRY);
    assert(influxClassify(500) == IR_RETRY && influxClassify(503) == IR_RETRY);
    assert(influxClassify(-1) == IR_RETRY && influxClassify(-11) == IR_RETRY && influxClassify(0) == IR_RETRY);
    assert(influxRetryDelayMs(1) == 30000 && influxRetryDelayMs(2) == 60000 && influxRetryDelayMs(3) == 120000);
    assert(influxRetryDelayMs(6) == 600000 && influxRetryDelayMs(100) == 600000);

    std::vector<uint8_t> mem(4096);
    InfluxQueue q;
    assert(!q.attached() && !q.push(1, "a", 1));   // no buffer: nothing queued
    q.attach(mem.data(), mem.size());
    assert(q.attached() && q.count() == 0 && q.oldestEpoch() == 0 && !q.due(0));

    // ---- normal push: in, sent, out ----
    push(q, 1700000000);
    assert(q.count() == 1 && q.due(0) && q.oldestEpoch() == 1700000000);
    std::string out;
    size_t n = q.peek(out, INFLUX_CHUNK_BYTES);
    assert(n == 1 && out == body(1700000000));
    InfluxEvent ev = q.result(204, n, 1000);
    assert(ev.action == IR_OK && !ev.outageStarted && !ev.cleared && q.count() == 0 && q.used() == 0);

    // ---- outage: 5xx keeps the push, backoff, then the queued ones follow in order ----
    push(q, 100);
    out.clear(); n = q.peek(out, INFLUX_CHUNK_BYTES);
    ev = q.result(503, n, 10000);
    assert(ev.action == IR_RETRY && ev.outageStarted && q.count() == 1 && q.failsInRow() == 1 && q.inOutage());
    assert(!q.due(10000) && !q.due(39999) && q.due(40000));   // 30 s
    push(q, 110); push(q, 120);
    out.clear(); n = q.peek(out, INFLUX_CHUNK_BYTES);
    ev = q.result(-1, n, 40000);                               // timeout: no reply
    assert(ev.action == IR_RETRY && !ev.outageStarted && q.failsInRow() == 2 && !q.due(99999) && q.due(100000));   // 60 s
    out.clear(); n = q.peek(out, INFLUX_CHUNK_BYTES);
    assert(n == 3 && out == body(100) + body(110) + body(120));   // oldest first, timestamps as captured
    ev = q.result(204, n, 100000);
    assert(ev.action == IR_OK && ev.cleared && ev.resent == 3 && ev.dropped == 0 && q.count() == 0 && !q.inOutage());
    assert(q.failsInRow() == 0);

    // ---- chunking: <= maxBytes per request, at least one, order kept, rest stays ----
    for (uint32_t e = 200; e < 210; e++) push(q, e);           // 10 bodies of equal size
    const size_t one = body(200).size();
    out.clear(); n = q.peek(out, 3 * one + 1);
    assert(n == 3 && out == body(200) + body(201) + body(202) && q.count() == 10);   // peek removes nothing
    q.result(204, n, 0);
    assert(q.count() == 7 && q.oldestEpoch() == 203);
    out.clear(); n = q.peek(out, 1);                           // smaller than one body: still one
    assert(n == 1 && out == body(203));
    q.result(204, n, 0);
    out.clear(); n = q.peek(out, INFLUX_CHUNK_BYTES);
    assert(n == 6 && out.substr(0, one) == body(204) && out.substr(5 * one) == body(209));
    q.result(204, n, 0);
    assert(q.count() == 0);

    // ---- 4xx: the refused chunk is dropped (counted), the next one goes on at once ----
    push(q, 300); push(q, 301);
    out.clear(); n = q.peek(out, one);                         // only 300
    ev = q.result(400, n, 5000);
    assert(ev.action == IR_DROP && q.count() == 1 && q.droppedRejected() == 1 && q.oldestEpoch() == 301 && q.due(5000));
    out.clear(); n = q.peek(out, INFLUX_CHUNK_BYTES);
    assert(out == body(301));
    q.result(204, n, 5000);
    // 401 (token wrong) keeps the data until the setting is fixed
    push(q, 310);
    out.clear(); n = q.peek(out, INFLUX_CHUNK_BYTES);
    ev = q.result(401, n, 6000);
    assert(ev.action == IR_RETRY && q.count() == 1 && q.droppedRejected() == 1);
    out.clear(); n = q.peek(out, INFLUX_CHUNK_BYTES);
    ev = q.result(204, n, 40000);
    assert(ev.cleared && ev.resent == 1 && q.count() == 0);

    // ---- overflow: drop oldest, counted; the ring wraps and stays readable ----
    InfluxQueue r;
    std::vector<uint8_t> small(1000);
    r.attach(small.data(), small.size());
    const size_t rec = 8 + body(1000, 100).size();             // header + body
    const size_t fit = small.size() / rec;
    for (uint32_t e = 1000; e < 1000 + 3 * fit + 1; e++) push(r, e, 100);   // three rounds: wraps several times
    assert(r.count() == fit && r.used() == fit * rec && r.used() <= r.capacity());
    assert(r.droppedFull() == 2 * fit + 1);
    const uint32_t first = 1000 + 2 * fit + 1;
    assert(r.oldestEpoch() == first);
    out.clear(); n = r.peek(out, 1 << 20);
    std::string want;
    for (uint32_t e = first; e < first + fit; e++) want += body(e, 100);
    assert(n == fit && out == want);                           // order and contents across the wrap
    // a body larger than the whole ring: dropped itself, the queue is kept
    std::string huge(2000, 'h');
    assert(!r.push(5000, huge.data(), huge.size()) && r.count() == fit && r.droppedFull() == 2 * fit + 2);
    // dropped while in an outage: reported with `cleared`
    r.noteOffline();
    push(r, 6000, 100);                                        // drops one more
    out.clear(); n = r.peek(out, 1 << 20);
    ev = r.result(204, n, 0);
    assert(ev.cleared && ev.resent == fit && ev.dropped == 1);

    // ---- Wi-Fi down: noteOffline starts the outage once ----
    assert(q.noteOffline() && !q.noteOffline());
    push(q, 400);
    out.clear(); n = q.peek(out, INFLUX_CHUNK_BYTES);
    ev = q.result(204, n, 0);
    assert(ev.cleared && ev.resent == 1);

    // detach: empties, nothing queued afterwards
    push(q, 500);
    q.detach();
    assert(!q.attached() && q.count() == 0 && !q.due(0));

    printf("Firmware influx queue regressions passed.\n");
    return 0;
}
