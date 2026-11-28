// Screensaver (src/screensaver.cpp): condition selection from a plain input
// struct, every animation renders inside the 1024-byte buffer, the figure
// moves its lit pixels, the other modes behave. With B6_FRAMES_DIR set, contact
// sheets of frames are written there as PNG (x4) for looking at them.
#include "screensaver.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int lit(const uint8_t* b) { int n = 0; for (int i = 0; i < 1024; i++) for (int k = 0; k < 8; k++) n += b[i] >> k & 1; return n; }

// ---- PNG (stored deflate blocks, no zlib needed) ------------------------------
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
static void writePng(const std::string& path, const std::vector<const uint8_t*>& frames, int cols, int scale)
{
    for (int i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcTab[i] = c; }
    const int gap = 6, n = (int)frames.size(), rows = (n + cols - 1) / cols;
    const int w = cols * (128 * scale + gap) + gap, h = rows * (64 * scale + gap) + gap;
    std::vector<uint8_t> raw;   // grey 8 bit; the gaps are mid grey so the frame edges show
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) {
            const int cx = (x - gap) / (128 * scale + gap), cy = (y - gap) / (64 * scale + gap);
            const int fx = (x - gap) - cx * (128 * scale + gap), fy = (y - gap) - cy * (64 * scale + gap);
            uint8_t v = 90;
            if (x >= gap && y >= gap && cx < cols && fx < 128 * scale && fy < 64 * scale && cy * cols + cx < n) {
                const int px = fx / scale, py = fy / scale;
                v = (frames[cy * cols + cx][px + 128 * (py >> 3)] >> (py & 7)) & 1 ? 255 : 0;
            }
            raw.push_back(v);
        }
    }
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

// ---- rendering with guard bytes around the buffer ---------------------------------
static uint8_t guarded[64 + 1024 + 64];
static const uint8_t* render(const SaverScene& s, uint32_t t, uint32_t dur)
{
    memset(guarded, 0xA5, sizeof(guarded));
    saverRender(s, t, dur, guarded + 64);
    for (int i = 0; i < 64; i++) { assert(guarded[i] == 0xA5); assert(guarded[64 + 1024 + i] == 0xA5); }
    return guarded + 64;
}

static const char* const CN[SC_COUNT] = { "frost", "thaw", "mild", "hot", "uv", "rain", "wind", "lightning", "fog", "air", "soil", "night" };

int main()
{
    // ---- condition selection -------------------------------------------------------
    SaverInput in;
    in.tempC = 15;
    assert(saverConditions(in) == (1u << SC_MILD));
    assert(saverPickCondition(in, 0) == SC_MILD);
    SaverInput e;   // nothing known: mild
    assert(saverPickCondition(e, 5) == SC_MILD);
    in.tempC = -2; assert(saverConditions(in) == (1u << SC_FROST));
    in.tempC = 0.5f; in.thawed = true; assert(saverConditions(in) == ((1u << SC_THAW) | (1u << SC_MILD)));
    assert(saverPickCondition(in, 3) == SC_THAW);
    in.thawed = false; in.tempC = 12; in.thawed = true; assert(!(saverConditions(in) & (1u << SC_THAW)));   // too warm to be "thaw"
    in.thawed = false;
    in.tempC = 30; assert((saverConditions(in) & (1u << SC_HOT)) && !(saverConditions(in) & (1u << SC_MILD)));
    in.tempC = 15; in.uvIndex = 7; assert(saverConditions(in) & (1u << SC_UV)); in.uvIndex = 3; assert(!(saverConditions(in) & (1u << SC_UV))); in.uvIndex = NAN;
    in.rainRateMmH = 0.4f; assert(saverConditions(in) & (1u << SC_RAIN)); in.rainRateMmH = 0; assert(!(saverConditions(in) & (1u << SC_RAIN)));
    in.rain1hMm = 1.0f; assert(saverConditions(in) & (1u << SC_RAIN)); in.rain1hMm = NAN;
    in.gustMs = 12; assert(saverConditions(in) & (1u << SC_WIND)); in.gustMs = 5; assert(!(saverConditions(in) & (1u << SC_WIND)));
    in.windMs = 8; assert(saverConditions(in) & (1u << SC_WIND)); in.windMs = NAN; in.gustMs = NAN;
    in.humidity = 96; assert(saverConditions(in) & (1u << SC_FOG)); in.humidity = 70; assert(!(saverConditions(in) & (1u << SC_FOG)));
    in.pm25 = 60; assert(saverConditions(in) & (1u << SC_AIR)); in.pm25 = 10; assert(!(saverConditions(in) & (1u << SC_AIR)));
    in.co2Ppm = 2000; assert(saverConditions(in) & (1u << SC_AIR)); in.co2Ppm = 600; assert(!(saverConditions(in) & (1u << SC_AIR)));
    in.soilPct = 12; assert(saverConditions(in) & (1u << SC_SOIL)); in.soilPct = 60; assert(!(saverConditions(in) & (1u << SC_SOIL))); in.soilPct = -1;
    in.night = true; assert(saverConditions(in) & (1u << SC_NIGHT)); assert(saverPickCondition(in, 1) == SC_NIGHT);   // night when nothing else
    in.rainRateMmH = 1; assert(saverPickCondition(in, 1) == SC_RAIN);   // weather wins over night
    in.lightning = true; assert(saverPickCondition(in, 1) == SC_LIGHTNING && saverPickCondition(in, 2) == SC_LIGHTNING);
    // several active: every one of them gets picked over the random numbers, never mild / night
    SaverInput m; m.tempC = -3; m.gustMs = 15; m.night = true; m.rainRateMmH = 2;
    bool seen[SC_COUNT] = {};
    for (uint32_t r = 0; r < 30; r++) seen[saverPickCondition(m, r)] = true;
    assert(seen[SC_FROST] && seen[SC_WIND] && seen[SC_RAIN] && !seen[SC_NIGHT] && !seen[SC_MILD]);

    // ---- pools: at least 2 animations for the common conditions, 1 for the others ------
    for (int c = 0; c < SC_COUNT; c++) assert(saverAnimCount((uint8_t)c) >= 1);
    for (int c : { SC_FROST, SC_MILD, SC_HOT, SC_RAIN, SC_WIND, SC_NIGHT }) assert(saverAnimCount((uint8_t)c) >= 2);
    assert(saverSurpriseCount() >= 1);

    // saverStart: valid scene for any random number; surprises are rare but happen
    int surprises = 0;
    for (uint32_t r = 0; r < 4000; r++) {
        SaverScene s = saverStart(SV_FIGURE, m, r * 2654435761u + 7);
        assert(s.mode == SV_FIGURE && s.cond < SC_COUNT);
        assert(s.anim < (s.surprise ? saverSurpriseCount() : saverAnimCount(s.cond)));
        surprises += s.surprise;
    }
    assert(surprises > 100 && surprises < 800);

    // ---- every animation of every condition + the surprises, many frames in the buffer -------
    const char* dir = getenv("B6_FRAMES_DIR");
    const uint32_t durs[] = { 3000, 8000, 30000 };
    for (int sur = 0; sur < 2; sur++)
        for (int c = 0; c < (sur ? 1 : SC_COUNT); c++) {
            const int n = sur ? saverSurpriseCount() : saverAnimCount((uint8_t)c);
            for (int a = 0; a < n; a++) {
                SaverScene s; s.mode = SV_FIGURE; s.cond = (uint8_t)c; s.anim = (uint8_t)a; s.surprise = sur; s.seed = 12345u + a;
                int mx = 0, mn = 1 << 30, cxMin = 1, cxMax = 0;
                for (uint32_t dur : durs)
                    for (uint32_t t = 0; t <= dur; t += dur / 100) {
                        const uint8_t* f = render(s, t, dur);
                        const int l = lit(f);
                        if (dur == 8000) { mx = l > mx ? l : mx; mn = l < mn ? l : mn; }
                        if (dur == 8000 && l) {   // centroid of the lit pixels: the figure crosses the screen, so it travels
                            long sx = 0; for (int i = 0; i < 1024; i++) for (int k = 0; k < 8; k++) if (f[i] >> k & 1) sx += i % 128;
                            const int cxm = (int)(sx / l);
                            if (cxMin > cxMax) cxMin = cxMax = cxm; else { if (cxm < cxMin) cxMin = cxm; if (cxm > cxMax) cxMax = cxm; }
                        }
                    }
                assert(mx > 40);   // something is drawn
                assert(c == SC_LIGHTNING || mx < 1024 * 8 * 3 / 5);   // not a mostly lit frame
                if (cxMax - cxMin <= 12) printf("static: %s %d\n", saverAnimName((uint8_t)c, (uint8_t)a, sur), cxMax - cxMin);
                (void)mn;
                if (dir) {
                    uint32_t ps[6] = { 100, 250, 450, 550, 750, 920 };
                    if (!sur && c == SC_LIGHTNING) { ps[1] = 163; ps[2] = 345; ps[3] = 500; ps[4] = 625; ps[5] = 830; }   // the flashes of the bolt animation (1300 ms cycle)
                    std::vector<uint8_t> store(6 * 1024); std::vector<const uint8_t*> fr;
                    for (int i = 0; i < 6; i++) { memcpy(&store[i * 1024], render(s, ps[i] * 8, 8000), 1024); fr.push_back(&store[i * 1024]); }
                    writePng(std::string(dir) + "/" + (sur ? "surprise" : CN[c]) + "_" + saverAnimName((uint8_t)c, (uint8_t)a, sur) + ".png", fr, 3, 4);
                }
            }
        }

    // ---- the other modes ---------------------------------------------------------------
    SaverScene b; b.mode = SV_BLANK;
    for (uint32_t t = 0; t <= 8000; t += 500) assert(lit(render(b, t, 8000)) == 0);
    SaverScene d; d.mode = SV_DOT;
    int firstX = -1; bool dotMoves = false;
    for (uint32_t t = 0; t <= 8000; t += 250) {
        const uint8_t* f = render(d, t, 8000);
        assert(lit(f) == 16);   // one 4x4 dot, always completely inside
        int x = 0; while (!(f[x] | f[x + 128] | f[x + 256] | f[x + 384] | f[x + 512] | f[x + 640] | f[x + 768] | f[x + 896])) x++;
        if (firstX < 0) firstX = x; else if (x != firstX) dotMoves = true;
    }
    assert(dotMoves);
    SaverScene ic; ic.mode = SV_ICONS;
    {   // sun -> cloud -> rain: three different pictures, every frame drawn
        uint8_t third[3][1024];
        const uint32_t at[3] = { 1000, 4500, 8000 };
        for (int i = 0; i < 3; i++) memcpy(third[i], render(ic, at[i], 9000), 1024);
        assert(memcmp(third[0], third[1], 1024) && memcmp(third[1], third[2], 1024) && memcmp(third[0], third[2], 1024));
        for (uint32_t t = 0; t <= 9000; t += 300) assert(lit(render(ic, t, 9000)) > 100);
    }
    SaverScene off; off.mode = SV_OFF; assert(lit(render(off, 100, 8000)) == 0);
    // out-of-range elapsed / zero duration must not crash or draw outside
    render(ic, 999999, 8000); render(ic, 5, 0); render(d, 0xFFFFFFFFu, 1);
    if (dir) {
        std::vector<uint8_t> store(18 * 1024); std::vector<const uint8_t*> fr;
        int i = 0;
        for (uint32_t t : { 0u, 2000u, 4000u, 6000u }) { memcpy(&store[i * 1024], render(d, t, 8000), 1024); fr.push_back(&store[i * 1024]); i++; }
        writePng(std::string(dir) + "/mode_dot.png", fr, 4, 4);
        fr.clear(); i = 0;
        for (uint32_t t : { 0u, 1500u, 3000u, 4500u, 6000u, 8500u }) { memcpy(&store[i * 1024], render(ic, t, 9000), 1024); fr.push_back(&store[i * 1024]); i++; }
        writePng(std::string(dir) + "/mode_icons.png", fr, 3, 4);
        fr.clear(); memcpy(&store[0], render(b, 100, 8000), 1024); fr.push_back(&store[0]);
        writePng(std::string(dir) + "/mode_blank.png", fr, 1, 4);
    }
    puts("screensaver ok");
    return 0;
}
