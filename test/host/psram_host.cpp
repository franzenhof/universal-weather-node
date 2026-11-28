// PSRAM / internal RAM (src/psram.h): the host has no PSRAM, big buffers come
// from malloc. Part of runtime.cpp; built alone by tests without the runtime.
#include "psram.h"
#include <stdlib.h>
bool   psramActive()     { return false; }
size_t psramTotal()      { return 0; }
size_t psramFree()       { return 0; }
size_t psramLargest()    { return 0; }
size_t internalFree()    { return 150000; }
size_t internalMinFree() { return 120000; }
size_t internalLargest() { return 90000; }
void*  bigAlloc(size_t n) { return malloc(n); }
size_t bigAllocLargest() { return 90000; }
void   psramLogStatus(int) {}

// psram_json.h: no PSRAM on the host - plain malloc
#include "psram_json.h"
namespace {
struct HostJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return malloc(n); }
    void  deallocate(void* p) override { free(p); }
    void* reallocate(void* p, size_t n) override { return realloc(p, n); }
};
}
ArduinoJson::Allocator* psramJsonAllocator()
{
    static HostJsonAllocator allocator;
    return &allocator;
}
