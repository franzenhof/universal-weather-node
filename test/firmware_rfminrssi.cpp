// Minimum signal per wireless sensor (src/rf_minrssi.cpp): parse, lookup, set / remove, range.
#include "Arduino.h"
#include "rf_minrssi.h"
#include "settings_backup.h"
#include "globals.h"
#include <cassert>
#include <cstring>
#include <string>

int main()
{
    int d;
    assert(rfMinRssiParse(" -95 ", d) && d == -95);
    assert(rfMinRssiParse("-100 dBm", d) && d == -100);
    assert(rfMinRssiParse("", d) && d == 0);
    assert(rfMinRssiParse("0", d) && d == 0);
    assert(!rfMinRssiParse("abc", d));
    assert(!rfMinRssiParse("-95x", d));
    assert(!rfMinRssiParse("-141", d));
    assert(!rfMinRssiParse("-19", d));
    assert(!rfMinRssiParse("95", d));
    assert(rfMinRssiParse("-140", d) && d == -140);
    assert(rfMinRssiParse("-20", d) && d == -20);

    char list[64] = "";
    assert(rfMinRssiSet(list, sizeof(list), 0x900181A2, -95));
    assert(rfMinRssiSet(list, sizeof(list), 0x300002B2, -100));
    assert(std::string(list) == "900181A2:-95|300002B2:-100");
    assert(rfMinRssiLookup(list, 0x900181A2) == -95);
    assert(rfMinRssiLookup(list, 0x300002B2) == -100);
    assert(rfMinRssiLookup(list, 0x12345678) == 0);
    // replace keeps the others; 0 removes
    assert(rfMinRssiSet(list, sizeof(list), 0x900181A2, -90));
    assert(std::string(list) == "300002B2:-100|900181A2:-90");
    assert(rfMinRssiSet(list, sizeof(list), 0x300002B2, 0));
    assert(std::string(list) == "900181A2:-90");
    assert(rfMinRssiSet(list, sizeof(list), 0x900181A2, 0) && list[0] == '\0');
    // out of range: refused, list unchanged
    assert(rfMinRssiSet(list, sizeof(list), 0xA, -80));
    assert(!rfMinRssiSet(list, sizeof(list), 0xB, -150));
    assert(!rfMinRssiSet(list, sizeof(list), 0xB, -10));
    assert(std::string(list) == "A:-80");
    // bad entries are dropped on the next change, never matched
    char bad[64] = "zz:-90|900181A2:abc|300002B2:-85|5:";
    assert(rfMinRssiLookup(bad, 0x900181A2) == 0);
    assert(rfMinRssiLookup(bad, 0x300002B2) == -85);
    assert(rfMinRssiSet(bad, sizeof(bad), 0x7, -70));
    assert(rfMinRssiLookup(bad, 0x300002B2) == -85 && rfMinRssiLookup(bad, 0x7) == -70);
    // no room: unchanged
    char small[10] = "";
    assert(rfMinRssiSet(small, sizeof(small), 0xA, -80));
    assert(!rfMinRssiSet(small, sizeof(small), 0xB, -80));
    assert(std::string(small) == "A:-80");
    assert(!backupIsSecret("rfminrssi"));
    puts("rfminrssi ok");
    return 0;
}
