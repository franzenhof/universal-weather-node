// TF(): T() with placeholders (Arduino String; not part of the host tests,
// which use i18nFormatT() directly).
#include "i18n.h"

static String tfN(TextId id, const char* const* args, int n)
{
    String out;
    i18nFormatT(out, T(id), args, n);
    return out;
}

String TF(TextId id, const String& a0)
{
    const char* a[1] = { a0.c_str() };
    return tfN(id, a, 1);
}
String TF(TextId id, const String& a0, const String& a1)
{
    const char* a[2] = { a0.c_str(), a1.c_str() };
    return tfN(id, a, 2);
}
String TF(TextId id, const String& a0, const String& a1, const String& a2)
{
    const char* a[3] = { a0.c_str(), a1.c_str(), a2.c_str() };
    return tfN(id, a, 3);
}
String TF(TextId id, const String& a0, const String& a1, const String& a2, const String& a3)
{
    const char* a[4] = { a0.c_str(), a1.c_str(), a2.c_str(), a3.c_str() };
    return tfN(id, a, 4);
}
