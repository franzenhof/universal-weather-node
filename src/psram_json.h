#pragma once
// ArduinoJson documents in the PSRAM (Heltec V4).
//
// ArduinoJson 7 grows a document in 1-KB slot pools plus one small block per
// copied string - all below the 4-KB limit from which malloc() prefers the
// PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), so a default JsonDocument
// lives in internal RAM: several KB per /api/data or /api/info document,
// split into many small blocks (fragmentation), and a static document keeps
// its last content there between requests. Every JsonDocument of the
// firmware is built with this allocator instead:
//
//     JsonDocument doc(psramJsonAllocator());
//
// PSRAM first, internal RAM only when the PSRAM is full or missing. Never
// for a document an ISR or a flash write (cache off) touches - none does.
#include <ArduinoJson.h>

ArduinoJson::Allocator* psramJsonAllocator();
