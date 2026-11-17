#pragma once
#include "wind_window.h"   // PathValues: wind/rain of the sending path

#include <CayenneLPP.h>
#include <stdint.h>
#include <stddef.h>
#include "payload.h"

// Shared with sendLoRa() (main.cpp), which picks buffer/length/FPort based
// on gCfg.payloadFormat.
extern CayenneLPP lpp;
extern uint8_t    binBuf[BIN_MAX_SIZE];
extern size_t     binLen;

// Both builders take the largest payload the current data rate allows
// (LoRaWANNode::getMaxPayloadLen()) and leave out the least important field
// groups until the packet fits - see dropNextGroup() in payload.h. The groups
// left out in the last uplink are in lastPayloadDropped ("" = complete).
extern char lastPayloadDropped[256];

// Populates `lpp` from the current sensor globals (see globals.h).
void buildLppPacket(size_t limit, const PathValues* pv = nullptr);

// Populates binBuf/binLen — compact binary alternative to CayenneLPP (layout
// in payload.h), see chirpstack/decoder.js (parseBin) for the decoder.
void buildBinaryPacket(size_t limit, const PathValues* pv = nullptr);
