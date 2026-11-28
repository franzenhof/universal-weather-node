#include "vs_store.h"
#include "psram.h"
#include <mutex>
#include <new>
#include <stdlib.h>

namespace {
std::mutex  storeMutex;
VsSettings* store = nullptr;

// Created on first use (under the lock), in the PSRAM on the V4.
VsSettings& storeLocked()
{
    if (!store) {
        void* p = bigAlloc(sizeof(VsSettings));
        if (!p) abort();   // at boot: nothing sensible works without ~1 KB
        store = new (p) VsSettings();
    }
    return *store;
}
} // namespace

void vsSettingsRead(VsSettings& out)
{
    std::lock_guard<std::mutex> g(storeMutex);
    out = storeLocked();
}

void vsSettingsWrite(const VsSettings& in)
{
    std::lock_guard<std::mutex> g(storeMutex);
    storeLocked() = in;
}

VsSlotCfg vsSlotRead(int slot)
{
    if (slot < 0 || slot >= VS_SLOTS) return VsSlotCfg();
    std::lock_guard<std::mutex> g(storeMutex);
    return storeLocked().slot[slot];
}

bool vsSwitchedOn()
{
    std::lock_guard<std::mutex> g(storeMutex);
    return storeLocked().enabled != 0;
}

int vsTxPowerDbm()
{
    std::lock_guard<std::mutex> g(storeMutex);
    return storeLocked().txPowerDbm;
}

void VsSettingsDeleter::operator()(VsSettings* p) const
{
    if (!p) return;
    p->~VsSettings();
    free(p);
}

VsSettingsCopy vsSettingsCopy()
{
    void* p = bigAlloc(sizeof(VsSettings));
    if (!p) return VsSettingsCopy();
    VsSettingsCopy c(new (p) VsSettings());
    vsSettingsRead(*c);
    return c;
}
