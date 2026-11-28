#pragma once
// Pieces of HTTP handling that need no network. Host-tested
// (test/firmware_httputil.cpp).

// An Accept-Encoding header value lists gzip with a quality above 0
// ("gzip", "br, gzip;q=0.8"; not "gzip;q=0", not missing). Case-insensitive.
bool httpAcceptsGzip(const char* acceptEncoding);
