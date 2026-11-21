// Wireless sensor names (src/rf_names.cpp): set / replace / remove, cleaning,
// UTF-8-safe cut, room, and the settings-backup round trip of the text form.
#include "Arduino.h"
#include "rf_names.h"
#include "settings_backup.h"
#include "globals.h"
#include <cassert>
#include <cstring>
#include <string>

int main()
{
    char list[96] = "";
    char out[RF_NAME_MAX + 1];
    assert(rfNameSet(list, sizeof(list), 0x900181A2, "  Hochbeet "));
    assert(rfNameSet(list, sizeof(list), 0x300002B2, "Keller|Nord\n"));   // '|' and control characters removed
    assert(std::string(list) == "900181A2:Hochbeet|300002B2:KellerNord");
    assert(rfNameLookup(list, 0x900181A2, out, sizeof(out)) && std::string(out) == "Hochbeet");
    assert(!rfNameLookup(list, 0x12345678, out, sizeof(out)) && out[0] == '\0');
    // replace keeps the others, empty removes
    assert(rfNameSet(list, sizeof(list), 0x900181A2, "Gew\xC3\xA4" "chshaus"));
    assert(std::string(list) == "300002B2:KellerNord|900181A2:Gew\xC3\xA4" "chshaus");
    assert(rfNameSet(list, sizeof(list), 0x300002B2, ""));
    assert(std::string(list) == "900181A2:Gew\xC3\xA4" "chshaus");
    // at most 24 bytes, never inside a UTF-8 character
    char n[40] = "abcdefghijklmnopqrstuvw\xC3\xA4xyz";   // the 'ä' sits on bytes 23-24
    rfNameClean(n);
    assert(strlen(n) == 23 && std::string(n) == "abcdefghijklmnopqrstuvw");
    // no room: unchanged
    char small[24] = "";
    assert(rfNameSet(small, sizeof(small), 0xA, "abc"));
    assert(!rfNameSet(small, sizeof(small), 0xB, "a much longer name"));
    assert(std::string(small) == "A:abc");
    // the text field is a normal (non-secret) settings field: it survives a backup round trip
    assert(!backupIsSecret("rfnames"));
    puts("rfnames ok");
    return 0;
}
