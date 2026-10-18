#pragma once

#include <CayenneLPP.h>
#include <stdint.h>
#include <stddef.h>
#include "payload.h"

// Shared with sendLoRa() (main.cpp), which picks buffer/length/FPort based
// on gCfg.payloadFormat.
extern CayenneLPP lpp;
extern uint8_t    binBuf[BIN_MAX_SIZE];
extern size_t     binLen;

// Populates `lpp` from the current sensor globals (see globals.h).
void buildLppPacket();

// Populates binBuf/binLen — compact binary alternative to CayenneLPP (layout
// in payload.h), see chirpstack/decoder.js (parseBin) for the decoder.
void buildBinaryPacket();
