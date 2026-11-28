// Custom OLED screens (src/oled_custom.cpp): stored form round trip,
// damaged input, field clean-up (control characters, UTF-8 cut).
#include "oled_custom.h"
#include <cassert>
#include <cstring>
#include <string>

int main()
{
    OledCustom c;
    c.layout = OCL_BIG;
    strcpy(c.title, "Garten");
    strcpy(c.line[0], "{rf2_temp:.1}\xC2\xB0");
    strcpy(c.line[1], "Feuchte {rf2_hum} %");
    char s[400];
    oledCustomPack(c, s, sizeof(s));
    assert(std::string(s) == "1\nGarten\n{rf2_temp:.1}\xC2\xB0\nFeuchte {rf2_hum} %\n\n\n");
    OledCustom u;
    oledCustomUnpack(s, u);
    assert(u.layout == OCL_BIG && !strcmp(u.title, "Garten") && !strcmp(u.line[0], c.line[0]) &&
           !strcmp(u.line[1], c.line[1]) && !u.line[4][0]);

    // empty / foreign / damaged text: an empty screen
    oledCustomUnpack("", u);           assert(u.layout == OCL_SMALL && !u.title[0]);
    oledCustomUnpack(nullptr, u);      assert(!u.title[0]);
    oledCustomUnpack("7\nX", u);       assert(!u.title[0]);
    oledCustomUnpack("0\nT\na", u);    assert(!strcmp(u.title, "T") && !strcmp(u.line[0], "a") && !u.line[1][0]);
    // extra lines are ignored, over-long fields cut
    oledCustomUnpack("0\nT\n1\n2\n3\n4\n5\n6\n7", u);
    assert(!strcmp(u.line[4], "5"));
    std::string longLine(100, 'x');
    oledCustomUnpack(("0\n" + longLine + "\n" + longLine).c_str(), u);
    assert(strlen(u.title) == OLED_CUSTOM_TITLE - 1 && strlen(u.line[0]) == OLED_CUSTOM_LINE - 1);

    // clean: control characters dropped, a UTF-8 character never split
    char out[8];
    oledCustomClean("a\nb\tc\x7F" "d", out, sizeof(out));   assert(!strcmp(out, "abcd"));
    oledCustomClean("abcde\xC2\xB0", out, sizeof(out));     assert(!strcmp(out, "abcde\xC2\xB0"));   // 7 bytes fit
    oledCustomClean("abcdef\xC2\xB0", out, sizeof(out));    assert(!strcmp(out, "abcdef"));          // 2-byte char would not
    oledCustomClean("ab\xC2", out, sizeof(out));            assert(!strcmp(out, "ab"));              // broken sequence
    oledCustomClean(nullptr, out, sizeof(out));             assert(!out[0]);
    oledCustomClean("a\x80" "b\xFF" "c\xF8", out, sizeof(out)); assert(!strcmp(out, "abc"));          // stray continuation, never-valid bytes
    oledCustomClean("\xE2\x82\xAC\xBF", out, sizeof(out));  assert(!strcmp(out, "\xE2\x82\xAC"));  // a valid sequence kept
    // pack never overruns
    char tiny[6];
    oledCustomPack(c, tiny, sizeof(tiny));
    assert(strlen(tiny) == 5);
    return 0;
}
