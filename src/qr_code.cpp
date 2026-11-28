#include "qr_code.h"
#include <string.h>

// Written from the standard (ISO/IEC 18004:2015): data bits -> codewords ->
// Reed-Solomon blocks, interleaved -> zigzag placement -> mask with the
// lowest penalty -> format / version bits.

namespace {

constexpr int GRID_BYTES = (QR_SIZE_MAX * QR_SIZE_MAX + 7) / 8;

// [ecc][version], index 0 unused
const int8_t ECC_PER_BLOCK[2][QR_VERSION_MAX + 1] = {
    { -1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18 },    // L
    { -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 },   // M
};
const int8_t BLOCKS[2][QR_VERSION_MAX + 1] = {
    { -1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4 },
    { -1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 },
};

struct Grid {
    int size;
    uint8_t* bits;
    bool get(int x, int y) const { const int i = y * size + x; return bits[i >> 3] >> (i & 7) & 1; }
    void set(int x, int y, bool v)
    {
        const int i = y * size + x;
        if (v) bits[i >> 3] |= (uint8_t)(1 << (i & 7));
        else   bits[i >> 3] &= (uint8_t)~(1 << (i & 7));
    }
};

uint8_t gfMul(uint8_t x, uint8_t y)
{
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

int alignmentPositions(int version, int* pos)
{
    if (version == 1) return 0;
    const int n = version / 7 + 2;
    const int size = 17 + 4 * version;
    const int step = (version * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
    pos[0] = 6;
    for (int i = n - 1, p = size - 7; i >= 1; i--, p -= step) pos[i] = p;
    return n;
}

// Function patterns into m (values) and f (mask of function modules).
void drawFunction(int version, Grid& m, Grid& f)
{
    const int size = m.size;
    auto put = [&](int x, int y, bool v) { m.set(x, y, v); f.set(x, y, true); };
    for (int i = 0; i < size; i++) { put(6, i, i % 2 == 0); put(i, 6, i % 2 == 0); }   // timing
    const int fc[3][2] = { { 3, 3 }, { size - 4, 3 }, { 3, size - 4 } };
    for (const auto& c : fc)   // finders with their light separators
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                const int x = c[0] + dx, y = c[1] + dy;
                if (x < 0 || y < 0 || x >= size || y >= size) continue;
                const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
                const int d = ax > ay ? ax : ay;
                put(x, y, d != 2 && d != 4);
            }
    int ap[7];
    const int na = alignmentPositions(version, ap);
    for (int i = 0; i < na; i++)
        for (int j = 0; j < na; j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0)) continue;   // finder corners
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) {
                    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
                    put(ap[i] + dx, ap[j] + dy, (ax > ay ? ax : ay) != 1);
                }
        }
    // format areas (values written later) and the dark module
    for (int i = 0; i < 9; i++) { f.set(8, i, true); f.set(i, 8, true); }
    for (int i = 0; i < 8; i++) { f.set(size - 1 - i, 8, true); f.set(8, size - 1 - i, true); }
    put(8, size - 8, true);
    if (version >= 7) {
        const uint32_t vb = qrVersionBits(version);
        for (int i = 0; i < 18; i++) {
            const bool b = vb >> i & 1;
            const int a = size - 11 + i % 3, c = i / 3;
            put(a, c, b);
            put(c, a, b);
        }
    }
}

void drawFormat(Grid& m, QrEcc ecc, int mask)
{
    const int size = m.size;
    const uint16_t b = qrFormatBits(ecc, mask);
    auto bit = [b](int i) { return (b >> i & 1) != 0; };
    for (int i = 0; i <= 5; i++) m.set(8, i, bit(i));
    m.set(8, 7, bit(6));
    m.set(8, 8, bit(7));
    m.set(7, 8, bit(8));
    for (int i = 9; i < 15; i++) m.set(14 - i, 8, bit(i));
    for (int i = 0; i < 8; i++) m.set(size - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; i++) m.set(8, size - 15 + i, bit(i));
    m.set(8, size - 8, true);
}

void applyMask(Grid& m, const Grid& f, int mask)
{
    for (int y = 0; y < m.size; y++)
        for (int x = 0; x < m.size; x++)
            if (!f.get(x, y) && qrMaskBit(mask, x, y)) m.set(x, y, !m.get(x, y));
}

// Penalty rules N1-N4 of the standard (finder-like patterns counted inside
// the symbol only - affects the choice of the mask, never the validity).
long penalty(const Grid& m)
{
    const int n = m.size;
    long p = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int a = 0; a < n; a++) {
            int run = 0;
            bool last = false;
            uint16_t win = 0;   // last 11 modules of this line
            for (int b = 0; b < n; b++) {
                const bool v = pass ? m.get(a, b) : m.get(b, a);
                if (b && v == last) run++;
                else { if (run >= 5) p += 3 + (run - 5); run = 1; last = v; }
                win = (uint16_t)(((win << 1) | v) & 0x7FF);
                if (b >= 10 && (win == 0x5D0 || win == 0x05D)) p += 40;   // 10111010000 / 00001011101
            }
            if (run >= 5) p += 3 + (run - 5);
        }
    for (int y = 0; y + 1 < n; y++)
        for (int x = 0; x + 1 < n; x++) {
            const bool v = m.get(x, y);
            if (v == m.get(x + 1, y) && v == m.get(x, y + 1) && v == m.get(x + 1, y + 1)) p += 3;
        }
    long dark = 0;
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) dark += m.get(x, y);
    const long total = (long)n * n;
    long d = dark * 20 - total * 10;   // deviation from 50 % dark, in 5 % steps
    if (d < 0) d = -d;
    const long k = (d + total - 1) / total - 1;
    return p + (k > 0 ? k : 0) * 10;
}

} // namespace

int qrRawCodewords(int version)
{
    int r = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int na = version / 7 + 2;
        r -= (25 * na - 10) * na - 55;
        if (version >= 7) r -= 36;
    }
    return r / 8;
}

int qrEccPerBlock(int version, QrEcc ecc) { return ECC_PER_BLOCK[ecc][version]; }
int qrBlocks(int version, QrEcc ecc) { return BLOCKS[ecc][version]; }

int qrCapacity(int version, QrEcc ecc)
{
    if (version < 1 || version > QR_VERSION_MAX) return 0;
    const int dataBits = (qrRawCodewords(version) - qrEccPerBlock(version, ecc) * qrBlocks(version, ecc)) * 8;
    const int head = 4 + (version <= 9 ? 8 : 16);
    return (dataBits - head) / 8;
}

uint16_t qrFormatBits(QrEcc ecc, int mask)
{
    const int data = (ecc == QR_ECC_L ? 1 : 0) << 3 | (mask & 7);
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    return (uint16_t)(((data << 10) | (rem & 0x3FF)) ^ 0x5412);
}

uint32_t qrVersionBits(int version)
{
    uint32_t rem = (uint32_t)version;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    return (uint32_t)version << 12 | (rem & 0xFFF);
}

void qrReedSolomon(const uint8_t* data, int len, int ecLen, uint8_t* ec)
{
    uint8_t gen[32];
    memset(gen, 0, sizeof(gen));
    gen[ecLen - 1] = 1;   // monic generator, leading term implicit: prod (x - a^i), i < ecLen
    uint8_t root = 1;
    for (int i = 0; i < ecLen; i++) {
        for (int j = 0; j < ecLen; j++) {
            gen[j] = gfMul(gen[j], root);
            if (j + 1 < ecLen) gen[j] ^= gen[j + 1];
        }
        root = gfMul(root, 0x02);
    }
    memset(ec, 0, (size_t)ecLen);
    for (int i = 0; i < len; i++) {
        const uint8_t factor = data[i] ^ ec[0];
        memmove(ec, ec + 1, (size_t)ecLen - 1);
        ec[ecLen - 1] = 0;
        for (int j = 0; j < ecLen; j++) ec[j] ^= gfMul(gen[j], factor);
    }
}

void qrFunctionMap(int version, uint8_t* grid)
{
    uint8_t vals[GRID_BYTES];
    memset(vals, 0, sizeof(vals));
    memset(grid, 0, GRID_BYTES);
    Grid m{ 17 + 4 * version, vals }, f{ 17 + 4 * version, grid };
    drawFunction(version, m, f);
}

bool qrMaskBit(int mask, int x, int y)
{
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

bool qrEncode(const uint8_t* data, size_t len, QrEcc ecc, QrCode& out, int maxVersion, int forceMask)
{
    out = QrCode();
    if (maxVersion > QR_VERSION_MAX) maxVersion = QR_VERSION_MAX;
    int version = 1;
    while (version <= maxVersion && (int)len > qrCapacity(version, ecc)) version++;
    if (version > maxVersion) return false;

    const int raw = qrRawCodewords(version);
    const int ecLen = qrEccPerBlock(version, ecc), nBlocks = qrBlocks(version, ecc);
    const int dataLen = raw - ecLen * nBlocks;

    // data codewords: mode 0100, count, bytes, terminator, padding
    uint8_t cw[400];
    memset(cw, 0, sizeof(cw));
    int bit = 0;
    auto put = [&](uint32_t v, int n) {
        for (int i = n - 1; i >= 0; i--, bit++)
            if (v >> i & 1) cw[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
    };
    put(4, 4);
    put((uint32_t)len, version <= 9 ? 8 : 16);
    for (size_t i = 0; i < len; i++) put(data[i], 8);
    const int cap = dataLen * 8;
    put(0, cap - bit < 4 ? cap - bit : 4);
    bit = (bit + 7) & ~7;
    uint8_t pad = 0xEC;
    while (bit < cap) { put(pad, 8); pad ^= 0xEC ^ 0x11; }   // put() advances bit

    // blocks: the short ones first, the long ones have one data byte more
    uint8_t all[400];
    const int shortLen = raw / nBlocks, nShort = nBlocks - raw % nBlocks;
    const int shortData = shortLen - ecLen;
    uint8_t ec[5][32];
    int off = 0;
    for (int b = 0; b < nBlocks; b++) {
        const int dl = shortData + (b >= nShort);
        qrReedSolomon(cw + off, dl, ecLen, ec[b]);
        off += dl;
    }
    int k = 0;
    for (int i = 0; i <= shortData; i++) {
        off = 0;
        for (int b = 0; b < nBlocks; b++) {
            const int dl = shortData + (b >= nShort);
            if (i < dl) all[k++] = cw[off + i];
            off += dl;
        }
    }
    for (int i = 0; i < ecLen; i++)
        for (int b = 0; b < nBlocks; b++) all[k++] = ec[b][i];

    out.version = (uint8_t)version;
    out.size = (uint8_t)(17 + 4 * version);
    out.ecc = ecc;
    uint8_t fbits[GRID_BYTES];
    memset(fbits, 0, sizeof(fbits));
    Grid m{ out.size, out.mod }, f{ out.size, fbits };
    drawFunction(version, m, f);

    // zigzag placement, two columns at a time from the right, skipping the vertical timing column
    const int size = out.size;
    int i = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; vert++)
            for (int j = 0; j < 2; j++) {
                const int x = right - j;
                const bool up = ((right + 1) & 2) == 0;
                const int y = up ? size - 1 - vert : vert;
                if (f.get(x, y) || i >= raw * 8) continue;
                m.set(x, y, all[i >> 3] >> (7 - (i & 7)) & 1);
                i++;
            }
    }

    int best = forceMask;
    if (best < 0 || best > 7) {
        long bestP = 0;
        for (int mk = 0; mk < 8; mk++) {
            applyMask(m, f, mk);
            drawFormat(m, ecc, mk);
            const long p = penalty(m);
            if (mk == 0 || p < bestP) { bestP = p; best = mk; }
            applyMask(m, f, mk);   // XOR again: undone
        }
    }
    applyMask(m, f, best);
    drawFormat(m, ecc, best);
    out.mask = (uint8_t)best;
    return true;
}
