// Storage layout (src/storage_layout.h): the version rule every loader of a
// [version][payload] blob follows, and that 4.0.0's migration hook knows no
// older formats yet.
#include "storage_layout.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    assert(storedVersionAction(1, 1) == STORED_TAKE);
    assert(storedVersionAction(0, 1) == STORED_MIGRATE);
    assert(storedVersionAction(2, 1) == STORED_NEWER);

    const uint8_t blob[] = { 1, 0xAA, 0xBB, 0xCC };
    uint8_t scratch[16];
    size_t n = 99;
    StoredVersionAction a;
    // current version, any / exact payload length
    assert(storedBlobPayload(blob, sizeof(blob), 1, 0, &a) == blob + 1 && a == STORED_TAKE);
    assert(storedBlobLoad(SF_NETWORK_SENSORS, blob, sizeof(blob), 1, 3, scratch, sizeof(scratch), &n) == blob + 1 && n == 3);
    // right version, wrong size: damaged, not taken (and not "migrated")
    assert(!storedBlobPayload(blob, sizeof(blob), 1, 2, &a) && a == STORED_MIGRATE);
    assert(!storedBlobLoad(SF_NETWORK_SENSORS, blob, sizeof(blob), 1, 2, scratch, sizeof(scratch), &n) && n == 0);
    // a newer firmware's blob (downgrade): never read
    assert(!storedBlobPayload(blob, sizeof(blob), 0, 0, &a) && a == STORED_NEWER);
    assert(!storedBlobLoad(SF_NETWORK_SENSORS, blob, sizeof(blob), 0, 0, scratch, sizeof(scratch)));
    // an older blob goes to the hook; 4.0.0 has no older layouts: defaults
    for (int f = 0; f < SF_FORMAT_COUNT; f++)
        assert(storageMigrateBlob((StoredFormat)f, 1, blob, sizeof(blob), scratch, sizeof(scratch)) == 0);
    assert(!storedBlobLoad(SF_SERIES, blob, sizeof(blob), 2, 0, scratch, sizeof(scratch), &n) && n == 0);
    // a loader reads a stored blob longer than its current format (an older, longer
    // layout) up to the common ceiling, so the hook gets to see it
    assert(storedBlobLengthReadable(1) && storedBlobLengthReadable(STORED_BLOB_READ_MAX));
    assert(!storedBlobLengthReadable(0) && !storedBlobLengthReadable(STORED_BLOB_READ_MAX + 1));
    assert(storedBlobLengthReadable(STORED_HISTORY_TIER_READ_MAX, STORED_HISTORY_TIER_READ_MAX));
    {
        const uint8_t older[] = { 1, 1, 2, 3, 4, 5, 6, 7, 8, 9 };   // version 1, longer than the current payload of 3
        StoredVersionAction olderAction;
        assert(!storedBlobPayload(older, sizeof(older), 2, 3, &olderAction) && olderAction == STORED_MIGRATE);
    }
    // nothing stored
    assert(!storedBlobLoad(SF_SERIES, nullptr, 0, 1, 0, scratch, sizeof(scratch)));
    assert(!storedBlobPayload(blob, 0, 1, 0, &a) && a == STORED_MIGRATE);
    std::puts("storage ok");
    return 0;
}
