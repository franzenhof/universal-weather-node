#pragma once
// The node's virtual sensor settings (vsensor.h VsSettings): one copy in the
// PSRAM (~1.2 KB with 8 slots - kept out of Config, which lives in internal
// RAM and is copied whole by the settings POST and the portal), behind one
// lock. Loaded from / saved to the NVS blob "vsens" by config.cpp
// (vsSettingsLoad(), saveConfig()).
//
// Writers: the settings POST (web server task) and the boot load. Readers:
// the loop task (vsLoop(), one slot per call), the web task (settings page,
// status page, /api/info). Every access copies under the lock - a reader
// never sees a half-written slot.
#include "vsensor.h"
#include <memory>

void      vsSettingsRead(VsSettings& out);
void      vsSettingsWrite(const VsSettings& in);
VsSlotCfg vsSlotRead(int slot);          // a default (off) slot outside 0..VS_SLOTS-1
bool      vsSwitchedOn();                // VsSettings::enabled
int       vsTxPowerDbm();

// A heap copy for the web handlers (1.2 KB: not on their stacks), PSRAM
// when there is one. nullptr = no memory.
struct VsSettingsDeleter { void operator()(VsSettings* p) const; };
typedef std::unique_ptr<VsSettings, VsSettingsDeleter> VsSettingsCopy;
VsSettingsCopy vsSettingsCopy();         // filled with the current settings
