#include "storage_write.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {
std::mutex              gateMutex;
std::condition_variable sessionClosed;
bool                    sealed = false;
int                     openSessions = 0;
} // namespace

StorageWrite::StorageWrite(const char* ns, const char* partition)
{
    std::lock_guard<std::mutex> g(gateMutex);   // check and open as one step: no session opens after the seal
    if (sealed) return;
    open_ = prefs_.begin(ns, false, partition);
    if (open_) openSessions++;
}

StorageWrite::~StorageWrite()
{
    if (!open_) return;
    prefs_.end();
    std::lock_guard<std::mutex> g(gateMutex);
    openSessions--;
    sessionClosed.notify_all();
}

bool storageSeal(unsigned waitMs)
{
    std::unique_lock<std::mutex> g(gateMutex);
    sealed = true;
    return sessionClosed.wait_for(g, std::chrono::milliseconds(waitMs), [] { return openSessions == 0; });
}

bool storageSealed()
{
    std::lock_guard<std::mutex> g(gateMutex);
    return sealed;
}
