// QR encoder (src/qr_code.cpp) and the QR screen (src/wifi_ap_logic.cpp
// qrOledRender): known vectors (Reed-Solomon of the ISO / thonky.com
// "HELLO WORLD" 1-M example, format and version bits from the standard's
// tables), a decode round trip of every version and both ECC levels with a
// small decoder written here, the OLED layout. With B8_QR_DIR set, PNGs of
// the OLED screen (as the display shows it, x4) and of a few codes are
// written there - for a real QR decoder.
#include "qr_code.h"
#include "wifi_ap_logic.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- PNG (grey, stored deflate blocks) ---------------------------------------
static uint32_t crcTab[256];
static uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu)
{
    for (size_t i = 0; i < n; i++) c = crcTab[(c ^ d[i]) & 255] ^ (c >> 8);
    return c;
}
static void be32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s)); }
static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> o; be32(o, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), data.begin(), data.end());
    o.insert(o.end(), td.begin(), td.end());
    be32(o, crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
    fwrite(o.data(), 1, o.size(), f);
}
static void writePng(const std::string& path, const std::vector<uint8_t>& grey, int w, int h)
{
    for (int i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcTab[i] = c; }
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), grey.begin() + y * w, grey.begin() + (y + 1) * w); }
    std::vector<uint8_t> z = { 0x78, 0x01 };
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw.size(); pos += 65535) {
        const size_t len = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
        z.push_back(pos + len >= raw.size() ? 1 : 0);
        z.push_back((uint8_t)len); z.push_back((uint8_t)(len >> 8)); z.push_back((uint8_t)~len); z.push_back((uint8_t)(~len >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
    }
    be32(z, (b << 16) | a);
    FILE* f = fopen(path.c_str(), "wb");
    assert(f);
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ih; be32(ih, (uint32_t)w); be32(ih, (uint32_t)h); ih.push_back(8); ih.push_back(0); ih.push_back(0); ih.push_back(0); ih.push_back(0);
    chunk(f, "IHDR", ih); chunk(f, "IDAT", z); chunk(f, "IEND", {});
    fclose(f);
}

// ---- a small decoder: format bits, unmask, codewords, blocks, RS check, byte mode ----
static bool decode(const QrCode& q, std::string& text)
{
    const int size = q.size, version = (size - 17) / 4;
    // format bits, first copy (around the top-left finder)
    int fb = 0;
    for (int i = 0; i <= 5; i++) fb |= q.dark(8, i) << i;
    fb |= q.dark(8, 7) << 6; fb |= q.dark(8, 8) << 7; fb |= q.dark(7, 8) << 8;
    for (int i = 9; i < 15; i++) fb |= q.dark(14 - i, 8) << i;
    int fb2 = 0;   // second copy must agree
    for (int i = 0; i < 8; i++) fb2 |= q.dark(size - 1 - i, 8) << i;
    for (int i = 8; i < 15; i++) fb2 |= q.dark(8, size - 15 + i) << i;
    if (fb != fb2 || !q.dark(8, size - 8)) return false;
    int ecc = -1, mask = -1;
    for (int e = 0; e < 2; e++) for (int m = 0; m < 8; m++) if (qrFormatBits((QrEcc)e, m) == fb) { ecc = e; mask = m; }
    if (ecc < 0) return false;
    uint8_t fn[(QR_SIZE_MAX * QR_SIZE_MAX + 7) / 8];
    qrFunctionMap(version, fn);
    auto isFn = [&](int x, int y) { const int i = y * size + x; return (fn[i >> 3] >> (i & 7) & 1) != 0; };
    const int raw = qrRawCodewords(version);
    std::vector<uint8_t> all(raw, 0);
    int i = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; vert++)
            for (int j = 0; j < 2; j++) {
                const int x = right - j;
                const int y = ((right + 1) & 2) == 0 ? size - 1 - vert : vert;
                if (isFn(x, y) || i >= raw * 8) continue;
                if (q.dark(x, y) != qrMaskBit(mask, x, y)) all[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
                i++;
            }
    }
    const int ecLen = qrEccPerBlock(version, (QrEcc)ecc), nb = qrBlocks(version, (QrEcc)ecc);
    const int shortLen = raw / nb, nShort = nb - raw % nb, shortData = shortLen - ecLen;
    std::vector<std::vector<uint8_t>> blk(nb);
    int k = 0;
    for (int c = 0; c <= shortData; c++)
        for (int b = 0; b < nb; b++) if (c < shortData + (b >= nShort)) blk[b].push_back(all[k++]);
    std::vector<uint8_t> data;
    for (int b = 0; b < nb; b++) {
        std::vector<uint8_t> ec(ecLen);
        for (int c = 0; c < ecLen; c++) ec[c] = all[k + c * nb + b];
        uint8_t calc[32];
        qrReedSolomon(blk[b].data(), (int)blk[b].size(), ecLen, calc);
        if (memcmp(calc, ec.data(), ecLen)) return false;
        data.insert(data.end(), blk[b].begin(), blk[b].end());
    }
    int bit = 0;
    auto get = [&](int n) { int v = 0; for (int z = 0; z < n; z++, bit++) v = v << 1 | (data[bit >> 3] >> (7 - (bit & 7)) & 1); return v; };
    if (get(4) != 4) return false;
    const int len = get(version <= 9 ? 8 : 16);
    text.clear();
    for (int z = 0; z < len; z++) text += (char)get(8);
    return true;
}

static void oledToGrey(const uint8_t* buf, std::vector<uint8_t>& g, int& w, int& h, int scale, int border)
{
    w = 128 * scale + 2 * border; h = 64 * scale + 2 * border;
    g.assign((size_t)w * h, 0);   // the display's surroundings are dark
    for (int y = 0; y < 64 * scale; y++)
        for (int x = 0; x < 128 * scale; x++) {
            const int px = x / scale, py = y / scale;
            g[(size_t)(y + border) * w + x + border] = (buf[px + 128 * (py >> 3)] >> (py & 7) & 1) ? 230 : 15;
        }
}

static void codeToGrey(const QrCode& q, std::vector<uint8_t>& g, int& w, int scale)
{
    w = (q.size + 8) * scale;
    g.assign((size_t)w * w, 255);
    for (int y = 0; y < w; y++) for (int x = 0; x < w; x++)
        if (q.dark(x / scale - 4, y / scale - 4)) g[(size_t)y * w + x] = 0;
}

int main()
{
    // ---- known vectors ----
    {   // "HELLO WORLD", version 1-M, alphanumeric: data and EC codewords (thonky.com QR tutorial)
        const uint8_t d[16] = { 32, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236, 17, 236, 17 };
        const uint8_t want[10] = { 196, 35, 39, 119, 235, 215, 231, 226, 93, 23 };
        uint8_t ec[10];
        qrReedSolomon(d, 16, 10, ec);
        assert(!memcmp(ec, want, 10));
    }
    assert(qrFormatBits(QR_ECC_L, 0) == 0x77C4);   // 111011111000100
    assert(qrFormatBits(QR_ECC_L, 4) == 0x662F);   // 110011000101111
    assert(qrFormatBits(QR_ECC_M, 0) == 0x5412);   // 101010000010010
    assert(qrVersionBits(7) == 0x07C94);           // 000111110010010100
    assert(qrVersionBits(10) == 0x0A4D3);          // 001010010011010011
    // capacities (byte mode) from the standard's table
    const int capL[11] = { 0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271 };
    const int capM[11] = { 0, 14, 26, 42, 62, 84, 106, 122, 152, 180, 213 };
    for (int v = 1; v <= 10; v++) { assert(qrCapacity(v, QR_ECC_L) == capL[v]); assert(qrCapacity(v, QR_ECC_M) == capM[v]); }

    // ---- round trip: every version, both levels, several masks ----
    for (int e = 0; e < 2; e++)
        for (int v = 1; v <= 10; v++) {
            const int len = qrCapacity(v, (QrEcc)e);
            std::string s;
            for (int i = 0; i < len; i++) s += (char)('!' + (i * 7 + v) % 90);
            for (int m = -1; m < 8; m += (v == 7 ? 1 : 3)) {
                QrCode q;
                assert(qrEncode((const uint8_t*)s.data(), s.size(), (QrEcc)e, q, QR_VERSION_MAX, m));
                assert(q.version == v);   // full capacity: exactly this version
                assert(q.size == 17 + 4 * v);
                if (m >= 0) assert(q.mask == m);
                std::string back;
                assert(decode(q, back));
                assert(back == s);
            }
            QrCode q;   // one byte more: the next version (or too long)
            s += 'x';
            const bool ok = qrEncode((const uint8_t*)s.data(), s.size(), (QrEcc)e, q);
            assert(ok == (v < 10));
            if (ok) assert(q.version == v + 1);
            else assert(q.version == 0);
        }
    {   // the finder patterns are where they belong
        QrCode q;
        assert(qrEncode((const uint8_t*)"x", 1, QR_ECC_L, q));
        assert(q.version == 1 && q.size == 21);
        for (int i = 0; i < 7; i++) { assert(q.dark(i, 0) && q.dark(0, i) && q.dark(20 - i, 0) && q.dark(i, 20)); }
        assert(!q.dark(1, 1) && q.dark(2, 2) && q.dark(4, 4) && !q.dark(5, 5) && !q.dark(7, 7));
    }

    // ---- Wi-Fi text and the OLED screen ----
    char t[256];
    assert(wifiQrText("WeatherNode-A1B2", "flugplatz2026", t, sizeof(t)) == strlen(t));
    assert(!strcmp(t, "WIFI:T:WPA;S:WeatherNode-A1B2;P:flugplatz2026;;"));
    QrCode q;
    assert(qrEncode((const uint8_t*)t, strlen(t), QR_ECC_L, q));
    assert(q.version == 3);   // the default SSID with a 13-character password: version 3 -> scale 2 on the OLED
    uint8_t buf[1024 + 2];
    memset(buf, 0, sizeof(buf));
    buf[1024] = 0xA5; buf[1025] = 0x5A;
    assert(qrOledRender(q, buf) == 2);
    assert(buf[1024] == 0xA5 && buf[1025] == 0x5A);
    for (int x = 64; x < 128; x++) for (int p = 0; p < 8; p++) assert(buf[x + 128 * p] == 0);   // the right half untouched
    assert(buf[0] & 1);   // corner: quiet zone, lit
    // the first finder's corner module: (off, off) with off = (64 - 58) / 2 = 3, dark
    assert(!(buf[3 + 128 * 0] >> 3 & 1) && !(buf[4] >> 4 & 1));
    {   // long SSID + password: version 4+ -> scale 1, still inside the 64 x 64 box
        char t2[256];
        wifiQrText("Modellflugverein Hintertupfing", "sehr-langes-passwort-123", t2, sizeof(t2));
        QrCode q2;
        assert(qrEncode((const uint8_t*)t2, strlen(t2), QR_ECC_L, q2));
        assert(q2.version >= 4);
        uint8_t b2[1024] = {};
        assert(qrOledRender(q2, b2) == 1);
        std::string back;
        assert(decode(q2, back) && back == t2);
    }

    if (const char* dir = getenv("B8_QR_DIR")) {
        std::vector<uint8_t> g;
        int w, h;
        oledToGrey(buf, g, w, h, 4, 40);
        writePng(std::string(dir) + "/oled-qr-v3.png", g, w, h);
        char t2[256];
        wifiQrText("Modellflugverein Hintertupfing", "sehr-langes-passwort-123", t2, sizeof(t2));
        QrCode q2;
        qrEncode((const uint8_t*)t2, strlen(t2), QR_ECC_L, q2);
        uint8_t b2[1024] = {};
        qrOledRender(q2, b2);
        oledToGrey(b2, g, w, h, 4, 40);
        writePng(std::string(dir) + "/oled-qr-v" + std::to_string(q2.version) + ".png", g, w, h);
        for (int v : { 1, 5, 7, 10 }) {
            std::string s;
            for (int i = 0; i < qrCapacity(v, QR_ECC_M); i++) s += (char)('A' + i % 26);
            QrCode q3;
            qrEncode((const uint8_t*)s.data(), s.size(), QR_ECC_M, q3);
            codeToGrey(q3, g, w, 6);
            writePng(std::string(dir) + "/code-v" + std::to_string(v) + "-M.png", g, w, w);
        }
        printf("QR PNGs written to %s\n", dir);
    }
    puts("qr ok");
    return 0;
}
