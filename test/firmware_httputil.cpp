// HTTP helpers (src/http_util.cpp): the Accept-Encoding check.
#include "http_util.h"
#include <cassert>

int main()
{
    // Accept-Encoding: gzip with q > 0 only
    assert(httpAcceptsGzip("gzip") && httpAcceptsGzip("gzip, deflate, br") && httpAcceptsGzip("br;q=1.0, GZip;q=0.5"));
    assert(httpAcceptsGzip("deflate,gzip") && httpAcceptsGzip("gzip ; q=0.001"));
    assert(!httpAcceptsGzip("gzip;q=0") && !httpAcceptsGzip("gzip; q=0.000") && !httpAcceptsGzip("deflate, br"));
    assert(!httpAcceptsGzip("") && !httpAcceptsGzip(nullptr) && !httpAcceptsGzip("x-gzip2") && !httpAcceptsGzip("br;q=0.5;gzip"));
    assert(!httpAcceptsGzip("identity, gzip;q=0, br"));
    return 0;
}
