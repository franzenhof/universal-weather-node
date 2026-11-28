#pragma once
// Sends a gzip-compressed embedded asset (tools/gz_embed.py): to clients that
// accept gzip as it is, with Content-Encoding: gzip and Content-Length of the
// compressed size; to others (curl without --compressed, old tools)
// decompressed on the fly with the ESP32-S3 ROM inflater (tinfl, ~43 KB heap
// only while sending - 406 when there is not that much free memory).
// The server must collect the "Accept-Encoding" header (collectHeaders()).
#include <stddef.h>
#include <stdint.h>

// Srv: WebServer (config portal) or HttpServer (web interface) - both
// instantiated in gz_send.cpp.
template <class Srv>
void sendGzipAsset(Srv& srv, const uint8_t* gz, size_t gzLen, size_t rawLen, const char* contentType,
                   const char* cacheControl = nullptr);
