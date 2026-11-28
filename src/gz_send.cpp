#include "gz_send.h"
#include "psram.h"
#include "serial_log.h"
#include "http_util.h"
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <esp32s3/rom/miniz.h>
#include <stdlib.h>
#include <string.h>

namespace {
// gzip member: 10-byte header (gz_embed.py writes no optional fields), raw
// deflate data, 8-byte trailer (CRC32, size)
constexpr size_t GZ_HEADER = 10, GZ_TRAILER = 8;

bool acceptsGzip(WebServer& srv) { return httpAcceptsGzip(srv.header("Accept-Encoding").c_str()); }

// Fallback: inflate into a 32 KB wrapping window (the LZ dictionary) and send
// every filled part. Heap: tinfl state (~11 KB) + window, freed afterwards.
bool sendInflated(WebServer& srv, const uint8_t* gz, size_t gzLen, size_t rawLen, const char* type)
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
    bool sent = true;   // false = the client is gone
    do {
        size_t inBytes = inLeft, outBytes = TINFL_LZ_DICT_SIZE - ofs;
        st = tinfl_decompress(d, in, &inBytes, win, win + ofs, &outBytes, 0);   // all input present, wrapping window
        in += inBytes; inLeft -= inBytes;
        if (outBytes) { srv.sendContent((const char*)win + ofs, outBytes); sent = srv.client().connected(); }
        ofs = (ofs + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
    } while (sent && st == TINFL_STATUS_HAS_MORE_OUTPUT);
    if (!sent || st != TINFL_STATUS_DONE) {   // a body cut short must not end as a complete response: closed
        if (sent) dbgSerial.printf("gzip asset: inflate failed (%d)\n", (int)st);
        srv.client().stop();
    }
    free(win);
    free(d);
    return true;
}
} // namespace

void sendGzipAsset(WebServer& srv, const uint8_t* gz, size_t gzLen, size_t rawLen, const char* type, const char* cache)
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
