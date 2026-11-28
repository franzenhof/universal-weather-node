#pragma once
// Applies one save's changes to the live settings without undoing what other
// tasks changed meanwhile. The settings POST collects the restart fields in
// a copy (`after`, made from gCfg at `before`'s instant); a LoRaWAN downlink
// or MQTT command may change gCfg (applyParam) while the POST is still
// validating. Copying `after` over gCfg would revert those writes - only the
// aligned 4-byte words the POST itself changed (after != before) are written
// to live, so one field never ends as a mix of two writers' bytes. The caller
// holds the config locks (wxUploadLock, loxoneLock).
// Host-tested (test/firmware_configmerge.cpp).
#include "config.h"

void configApplyChanges(Config& live, const Config& before, const Config& after);
