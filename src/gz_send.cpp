#include "gz_send.h"
#include "psram.h"
#include "serial_log.h"
#include "http_server.h"
#include "http_util.h"
#include <esp_heap_caps.h>
#include <miniz.h>   // ESP-IDF 5: esp_rom/include (ROM tinfl)
#include <stdlib.h>
#include <string.h>

namespace {
// gzip member: 10-byte header (gz_embed.py writes no optional fields), raw
// deflate data, 8-byte trailer (CRC32, size)
constexpr size_t GZ_HEADER = 10, GZ_TRAILER = 8;

template <class Srv>
bool acceptsGzip(Srv& srv) { return httpAcceptsGzip(srv.header("Accept-Encoding").c_str()); }

// One part of the body; false = the client is gone.
bool sendPart(HttpServer& srv, const char* p, size_t n) { return srv.sendContent(p, n); }
bool sendPart(WebServer& srv, const char* p, size_t n) { srv.sendContent(p, n); return srv.client().connected(); }
// A body cut short must not end as a complete response: closed instead.
void abortBody(HttpServer& srv) { srv.abortResponse(); }
void abortBody(WebServer& srv) { srv.client().stop(); }

// Fallback: inflate into a 32 KB wrapping window (the LZ dictionary) and send
// every filled part. Heap: tinfl state (~11 KB) + window, freed afterwards.
template <class Srv>
bool sendInflated(Srv& srv, const uint8_t* gz, size_t gzLen, size_t rawLen, const char* type)
{
    if (gzLen < GZ_HEADER + GZ_TRAILER || gz[0] != 0x1f || gz[1] != 0x8b || gz[3] != 0) return false;
    if (bigAllocLargest() < TINFL_LZ_DICT_SIZE + 1024) return false;   // > 4 KB: PSRAM if active
    tinfl_decompressor* d = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
    uint8_t* win = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
    if (!d || !win) { free(d); free(win); return false; }
    tinfl_init(d);
    srv.setContentLength(rawLen);
    srv.send(200, type, "");
    const uint8_t* in = gz + GZ_HEADER;
    size_t inLeft = gzLen - GZ_HEADER - GZ_TRAILER;
    size_t ofs = 0;
    tinfl_status st;
    bool sent = true;
    do {
        size_t inBytes = inLeft, outBytes = TINFL_LZ_DICT_SIZE - ofs;
        st = tinfl_decompress(d, in, &inBytes, win, win + ofs, &outBytes, 0);   // all input present, wrapping window
        in += inBytes; inLeft -= inBytes;
        if (outBytes) sent = sendPart(srv, (const char*)win + ofs, outBytes);
        ofs = (ofs + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
    } while (sent && st == TINFL_STATUS_HAS_MORE_OUTPUT);
    if (!sent || st != TINFL_STATUS_DONE) {
        if (sent) dbgSerial.printf("gzip asset: inflate failed (%d)\n", (int)st);
        abortBody(srv);
    }
    free(win);
    free(d);
    return true;
}
} // namespace

template <class Srv>
void sendGzipAsset(Srv& srv, const uint8_t* gz, size_t gzLen, size_t rawLen, const char* type, const char* cache)
{
    srv.sendHeader("Vary", "Accept-Encoding");
    if (cache) srv.sendHeader("Cache-Control", cache);
    if (acceptsGzip(srv)) {   // every browser: the bytes as they are (Content-Length = compressed size)
        srv.sendHeader("Content-Encoding", "gzip");
        srv.send_P(200, type, (const char*)gz, gzLen);
        return;
    }
    if (!sendInflated(srv, gz, gzLen, rawLen, type))
        srv.send(406, "text/plain; charset=utf-8",
                 "Not enough free memory to decompress: use a browser or curl --compressed.\n");
}

template void sendGzipAsset<WebServer>(WebServer&, const uint8_t*, size_t, size_t, const char*, const char*);
template void sendGzipAsset<HttpServer>(HttpServer&, const uint8_t*, size_t, size_t, const char*, const char*);
