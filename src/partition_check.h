#pragma once

// Detects devices whose flash still has the old/stock partition table (no
// dedicated "cfgdata" NVS partition — see partitions.csv). This happens if a
// device was OTA-updated to a build that expects "cfgdata" without ever
// getting a full serial reflash first: OTA (WiFiManager's own /update page,
// or ArduinoOTA) only ever replaces the running app image, never the
// partition table.
//
// An in-place runtime patch of the partition table (rewriting just the
// unused "spiffs" entry into "cfgdata") was attempted here, but this
// Arduino-ESP32 SDK build is compiled with
// CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=y: esp_flash_erase_region()/
// esp_flash_write() call abort() unconditionally — not a returned error —
// the instant the target range overlaps the partition-table region
// (0x8000), specifically to stop firmware from bricking a device by
// accident. There is no supported way around that from application code in
// this SDK build (confirmed on real hardware: it crash-loops on every boot
// until reflashed).
//
// Call once, early in setup(), before loadConfig() — which would otherwise
// just silently fail to open "cfgdata", with no explanation, on every boot.
// Never touches flash. Returns true if "cfgdata" is missing: the caller must
// not proceed into normal operation or the Wi-Fi portal in that case (config
// can't be saved either way), and should instead show the problem on the
// display, since nothing useful can happen without a cable in reach. A
// device in this state needs a one-time full reflash via USB/serial (which
// rewrites the partition table itself, and gives a natural opportunity to
// re-check its configured parameters); see README.md.
bool cfgdataPartitionMissing();
