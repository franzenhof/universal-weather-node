#include "reception_window.h"
#include "rf_schedule.h"   // RF_SCHED_MIN_GAP_MS

namespace {

void startWindow(ReceptionWindow& w, uint32_t nowMs)
{
    w = ReceptionWindow();
    w.newestBucketStartMs = nowMs;
    w.lastCountedMs = nowMs;
    w.packetsPerBucket[0] = 1;
    w.bucketsCovered = 1;
}

// Moves the ring forward to the bucket that holds nowMs; passed buckets start empty.
void advanceToNow(ReceptionWindow& w, uint32_t nowMs)
{
    const uint32_t sinceNewestStart = nowMs - w.newestBucketStartMs;
    const uint32_t bucketsPassed = sinceNewestStart / RECEPTION_BUCKET_MS;
    if (bucketsPassed == 0) return;
    const uint32_t bucketsToClear = bucketsPassed < (uint32_t)RECEPTION_BUCKETS ? bucketsPassed : RECEPTION_BUCKETS;
    for (uint32_t k = 0; k < bucketsToClear; k++) {
        w.newestBucket = (uint8_t)((w.newestBucket + 1) % RECEPTION_BUCKETS);
        w.packetsPerBucket[w.newestBucket] = 0;
        w.impossibleGapsPerBucket[w.newestBucket] = 0;
    }
    const uint32_t covered = w.bucketsCovered + bucketsToClear;
    w.bucketsCovered = (uint8_t)(covered < (uint32_t)RECEPTION_BUCKETS ? covered : RECEPTION_BUCKETS);
    w.newestBucketStartMs = nowMs - sinceNewestStart % RECEPTION_BUCKET_MS;   // stays on the 10-minute grid
}

// Sum of a ring over the buckets still in the window when the clock has moved
// bucketsPassed buckets past the newest one (stored bucket at age a is now at
// age a + bucketsPassed; the window holds ages 0..bucketsInWindow-1).
uint32_t sumOverWindow(const uint8_t* ring, uint8_t newestBucket, uint32_t bucketsPassed, uint32_t bucketsInWindow)
{
    uint32_t sum = 0;
    for (uint32_t age = 0; age + bucketsPassed < bucketsInWindow; age++)
        sum += ring[(newestBucket + RECEPTION_BUCKETS - age) % RECEPTION_BUCKETS];
    return sum;
}

bool idCollisionThresholdsReached(uint32_t impossibleGaps, uint32_t countedPackets)
{
    return impossibleGaps >= RECEPTION_ID_COLLISION_MIN_GAPS
        && (uint64_t)impossibleGaps * 100 >= (uint64_t)countedPackets * RECEPTION_ID_COLLISION_MIN_PERCENT;
}

} // namespace

void receptionCountPacket(ReceptionWindow& w, uint32_t nowMs, bool impossibleGap)
{
    if (w.bucketsCovered == 0 || nowMs - w.lastCountedMs >= RECEPTION_SILENCE_MS) { startWindow(w, nowMs); return; }
    advanceToNow(w, nowMs);
    if (impossibleGap && w.impossibleGapsPerBucket[w.newestBucket] < 255) w.impossibleGapsPerBucket[w.newestBucket]++;
    if (nowMs - w.lastCountedMs >= RF_SCHED_MIN_GAP_MS) {   // shorter: a repeat of the same transmission
        if (w.packetsPerBucket[w.newestBucket] < 255) w.packetsPerBucket[w.newestBucket]++;
        w.lastCountedMs = nowMs;
    }
    // the ID collision latch: set at the thresholds, released when no impossible gap is left
    const uint32_t impossibleGaps = sumOverWindow(w.impossibleGapsPerBucket, w.newestBucket, 0, w.bucketsCovered);
    if (impossibleGaps == 0) w.idCollisionLatched = false;
    else if (idCollisionThresholdsReached(impossibleGaps, sumOverWindow(w.packetsPerBucket, w.newestBucket, 0, w.bucketsCovered)))
        w.idCollisionLatched = true;
}

void receptionStopIfSilent(ReceptionWindow& w, uint32_t nowMs)
{
    if (w.bucketsCovered != 0 && nowMs - w.lastCountedMs >= RECEPTION_SILENCE_MS) receptionStop(w);
}

void receptionStop(ReceptionWindow& w)
{
    w.bucketsCovered = 0;
}

ReceptionVerdict receptionEvaluate(const ReceptionWindow& w, uint32_t periodMs, uint32_t nowMs)
{
    ReceptionVerdict v;
    if (w.bucketsCovered == 0) return v;
    if (nowMs - w.lastCountedMs >= RECEPTION_SILENCE_MS) return v;   // silent (not stopped yet)

    // Buckets the clock moved past since the last packet: read as empty, without writing.
    const uint32_t sinceNewestStart = nowMs - w.newestBucketStartMs;
    const uint32_t bucketsPassed = sinceNewestStart / RECEPTION_BUCKET_MS;
    if (bucketsPassed >= (uint32_t)RECEPTION_BUCKETS) return v;
    const uint32_t coveredNow = w.bucketsCovered + bucketsPassed;
    const uint32_t bucketsInWindow = coveredNow < (uint32_t)RECEPTION_BUCKETS ? coveredNow : RECEPTION_BUCKETS;
    const uint64_t spanMs = (uint64_t)(bucketsInWindow - 1) * RECEPTION_BUCKET_MS + sinceNewestStart % RECEPTION_BUCKET_MS;

    const uint32_t received = sumOverWindow(w.packetsPerBucket, w.newestBucket, bucketsPassed, bucketsInWindow);
    const uint32_t impossibleGaps = sumOverWindow(w.impossibleGapsPerBucket, w.newestBucket, bucketsPassed, bucketsInWindow);
    v.idCollision = impossibleGaps > 0 && (w.idCollisionLatched || idCollisionThresholdsReached(impossibleGaps, received));
    if (v.idCollision) return v;   // two transmitters: the counts mean nothing, the percentage stays unknown

    if (periodMs < RECEPTION_MIN_PERIOD_MS) return v;
    if (spanMs < RECEPTION_MIN_SPAN_MS || spanMs < (uint64_t)periodMs * RECEPTION_MIN_EXPECTED) return v;
    const uint64_t percent = (uint64_t)received * periodMs * 100 / spanMs;
    v.known = true;
    v.percent = (uint8_t)(percent > 100 ? 100 : percent);
    v.poor = spanMs >= RECEPTION_FULL_SPAN_MS && v.percent < RECEPTION_POOR_PERCENT;
    return v;
}
