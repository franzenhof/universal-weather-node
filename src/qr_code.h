#pragma once
// QR code encoder (ISO/IEC 18004), own code: byte mode, error correction L
// or M, versions 1-10 (up to 271 bytes at L) - enough for a Wi-Fi join text
// (WIFI:T:WPA;S:...;P:...;;). Pure code, no allocation: the code and its work
// space live on the caller's stack (~1.3 KB). Host-tested in
// test/firmware_qr.cpp (Reed-Solomon / format / version vectors, a decode
// round trip of every version, PNG output for a real decoder).
#include <stddef.h>
#include <stdint.h>

enum QrEcc : uint8_t { QR_ECC_L = 0, QR_ECC_M = 1 };

constexpr int QR_VERSION_MAX = 10;
constexpr int QR_SIZE_MAX = 17 + 4 * QR_VERSION_MAX;   // 57 modules

struct QrCode {
    uint8_t version = 0;   // 0 = no code
    uint8_t size = 0;      // modules per side: 17 + 4 * version
    uint8_t mask = 0;
    uint8_t ecc = QR_ECC_L;
    uint8_t mod[(QR_SIZE_MAX * QR_SIZE_MAX + 7) / 8] = {};
    bool dark(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= size || y >= size) return false;
        const int i = y * size + x;
        return mod[i >> 3] >> (i & 7) & 1;
    }
};

// Encodes len bytes in byte mode with the smallest version 1..maxVersion
// that holds them at ecc. forceMask 0-7 = that mask (tests), -1 = the one
// with the lowest penalty. false = too long (out.version = 0).
bool qrEncode(const uint8_t* data, size_t len, QrEcc ecc, QrCode& out, int maxVersion = QR_VERSION_MAX, int forceMask = -1);

// Bytes that fit into a version at ecc (byte mode).
int qrCapacity(int version, QrEcc ecc);

// Building blocks, exposed for the tests.
// 15 format bits (ecc + mask, BCH-protected and XOR-masked).
uint16_t qrFormatBits(QrEcc ecc, int mask);
// 18 version bits (versions 7+).
uint32_t qrVersionBits(int version);
// Reed-Solomon remainder (GF(256), polynomial 0x11D): ecLen bytes into ec.
void qrReedSolomon(const uint8_t* data, int len, int ecLen, uint8_t* ec);
// true where version's function patterns are (finders, timing, alignment,
// format and version areas); grid as in QrCode::mod.
void qrFunctionMap(int version, uint8_t* grid);
// Total codewords of a version and its block layout at ecc.
int qrRawCodewords(int version);
int qrEccPerBlock(int version, QrEcc ecc);
int qrBlocks(int version, QrEcc ecc);
// mask condition (x = column, y = row)
bool qrMaskBit(int mask, int x, int y);
