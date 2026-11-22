#pragma once
// PSRAM (Heltec V4: ESP32-S3R2, 2 MB in the package) and internal-RAM figures.
//
// The Arduino-ESP32 2.x libraries (2.0.17) support PSRAM, but the core starts it
// only with -D BOARD_HAS_PSRAM (platformio.ini; without it esp32-hal-psram.h
// undefines CONFIG_SPIRAM - up to rc.8 no PSRAM was ever used). Now
// initArduino() tries on every board and adds a PSRAM to the heap (V3: none,
// the driver logs "PSRAM ID read error" on UART0 and the node goes on).
// Then malloc() of 4 KB and more prefers the PSRAM (CONFIG_SPIRAM_USE_MALLOC,
// ALWAYSINTERNAL 4096) - TLS record buffers, the radio trace, the gzip window
// - and Wi-Fi / lwIP may use it too. Task stacks, ISR data and small blocks
// stay in internal RAM, so every memory guard looks at internal RAM, plus
// the PSRAM where a large block is allocated.
#include <stddef.h>
#include <stdint.h>

bool   psramActive();      // found and added to the heap
size_t psramTotal();       // bytes in the heap, 0 without PSRAM
size_t psramFree();
size_t psramLargest();

// Internal RAM (8-bit capable): what Wi-Fi, lwIP, the tasks and the web
// pages need. Without PSRAM the same as the whole heap.
size_t internalFree();
size_t internalMinFree();
size_t internalLargest();

// Large, long-lived buffers (history rings, wind window): PSRAM if active,
// else internal RAM. nullptr only if neither has the room.
void*  bigAlloc(size_t bytes);
// Largest block bigAlloc() / malloc() of >= 4 KB can get right now.
size_t bigAllocLargest();

// Boot log: PSRAM size, or why there is none although the chip has some
// (the eFuse says so) - including the driver's own error line, which goes
// to UART0 only and so is never seen over the V4's USB log.
void   psramLogStatus(int efusePsramCap);
