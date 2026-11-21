#include "partition_check.h"
#include "serial_log.h"

#include <Arduino.h>
#include <esp_partition.h>
#include <nvs_flash.h>

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

bool cfgdataPrepare()
{
    esp_err_t e = nvs_flash_init_partition("cfgdata");
    if (e != ESP_ERR_NVS_NO_FREE_PAGES && e != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (e != ESP_OK) dbgSerial.printf("cfgdata: NVS init failed (%s).\n", esp_err_to_name(e));
        return false;
    }
    dbgSerial.printf("cfgdata: not usable as settings storage (%s) - data of another firmware? Erasing it.\n",
                     esp_err_to_name(e));
    e = nvs_flash_erase_partition("cfgdata");
    if (e == ESP_OK) e = nvs_flash_init_partition("cfgdata");
    dbgSerial.printf("cfgdata: %s.\n", e == ESP_OK ? "erased and formatted - starting with default settings" : esp_err_to_name(e));
    return e == ESP_OK;
}
