#include "config_merge.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Config is plain data: char arrays and uint8_t (any offset), uint16_t and
// 4-byte int / float (naturally aligned), two uint64_t (EUIs, form only). So
// every field of more than one byte lies within one aligned 4-byte word, or
// (uint64_t) covers whole words - merging whole words never mixes the bytes
// of one field from two writers.
static_assert(alignof(Config) >= 4 && sizeof(Config) % 4 == 0, "Config: whole aligned 4-byte words");

void configApplyChanges(Config& live, const Config& before, const Config& after)
{
    // word by word: a word the POST changed is taken from it as a whole (it
    // wins for that word - a field another task changed meanwhile in the same
    // word can only be a neighbouring byte field); a word it did not change
    // stays as live has it. One aligned 32-bit store per word: a task reading
    // live without the lock sees each field either old or new.
    unsigned char* l = reinterpret_cast<unsigned char*>(&live);
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&before);
    const unsigned char* a = reinterpret_cast<const unsigned char*>(&after);
    for (size_t i = 0; i < sizeof(Config); i += 4) {
        uint32_t wa, wb;
        memcpy(&wa, a + i, 4);
        memcpy(&wb, b + i, 4);
        if (wa != wb) memcpy(l + i, &wa, 4);
    }
}
