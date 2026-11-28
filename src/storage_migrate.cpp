// The one place where stored formats of a newer firmware convert the
// formats of an older one (storage_layout.h, docs/design/storage-layout.md).
// 4.0.0 is the first frozen layout: nothing to convert yet - each hook below
// is where the first conversion of its format goes, as a case per version.
#include "storage_layout.h"
#include <string.h>

size_t storageMigrateBlob(StoredFormat f, uint8_t from, const uint8_t* in, size_t len, uint8_t* out, size_t outLen)
{
    (void)from; (void)in; (void)len; (void)out; (void)outLen;
    switch (f) {
        // e.g. case SF_NETWORK_SENSORS: if (from == 1) return netSensorsFromV1(in, len, out, outLen); break;
        default: break;
    }
    return 0;
}

void storageMigrateConfig(uint8_t from)
{
    (void)from;
    // e.g. if (from < 2) { rename "oldkey" to "newkey" in the "config" namespace }
}

const uint8_t* storedBlobPayload(const uint8_t* in, size_t len, uint8_t current, size_t payloadLen,
                                 StoredVersionAction* action)
{
    StoredVersionAction a = STORED_MIGRATE;   // empty / no blob: nothing usable
    const uint8_t* p = nullptr;
    if (in && len >= 1) {
        a = storedVersionAction(in[0], current);
        if (a == STORED_TAKE && (payloadLen == 0 || len == 1 + payloadLen)) p = in + 1;
        else if (a == STORED_TAKE) a = STORED_MIGRATE;   // right version, wrong size: damaged - not taken
    }
    if (action) *action = a;
    return p;
}

const uint8_t* storedBlobLoad(StoredFormat f, const uint8_t* in, size_t len, uint8_t current, size_t payloadLen,
                              uint8_t* scratch, size_t scratchLen, size_t* gotLen)
{
    StoredVersionAction a;
    const uint8_t* p = storedBlobPayload(in, len, current, payloadLen, &a);
    size_t n = p ? len - 1 : 0;
    if (!p && a == STORED_MIGRATE && in && len >= 1 && in[0] < current && scratch) {
        const size_t m = storageMigrateBlob(f, in[0], in, len, scratch, scratchLen);
        p = m ? storedBlobPayload(scratch, m, current, payloadLen) : nullptr;
        n = p ? m - 1 : 0;
    }
    if (gotLen) *gotLen = n;
    return p;
}
