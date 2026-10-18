#include "partition_check.h"
#include "serial_log.h"

#include <Arduino.h>
#include <esp_partition.h>

bool cfgdataPartitionMissing()
{
    // Present? This is the expected, permanent path for every device that's
    // had a full reflash since cfgdata was introduced.
    if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "cfgdata") != nullptr) {
        return false;
    }

    // NOTE: an in-place runtime patch of the partition table used to be
    // attempted here, but this Arduino-ESP32 SDK build is compiled with
    // CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=y: esp_flash_erase_region()/
    // esp_flash_write() call abort() unconditionally — not a returned error
    // — the instant the target range overlaps the partition-table region
    // (0x8000), specifically to stop firmware from bricking a device by
    // accident. There is no supported way around that from application code
    // in this SDK build, so this device needs a one-time full serial reflash
    // (which rewrites the partition table itself) — see README.md.
    dbgSerial.println("Partition repair: 'cfgdata' not found. This SDK build cannot safely "
                       "patch the partition table at runtime (protected region) — a one-time "
                       "full reflash via USB/serial is required. See README.md.");
    return true;
}
