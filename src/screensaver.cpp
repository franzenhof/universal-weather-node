#include "screensaver.h"
#include <string.h>

namespace {

constexpr int W = 128, H = 64, GY = 62;   // GY: the row the figure's feet stand on (63 = ground line)

// ---- canvas: 1024 bytes, SSD1306 order --------------------------------------
struct Cv {
    uint8_t* b;
    void set(int x, int y, bool v = true) const {
        if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
        if (v) b[x + W * (y >> 3)] |= (uint8_t)(1u << (y & 7)); else b[x + W * (y >> 3)] &= (uint8_t)~(1u << (y & 7));
    }
    void line(int x0, int y0, int x1, int y1, bool v = true) const {
        int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
        int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
        for (int n = 0; n < 400; n++) {
            set(x0, y0, v);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * e;
            if (e2 >= dy) { e += dy; x0 += sx; }
            if (e2 <= dx) { e += dx; y0 += sy; }
        }
    }
    void rect(int x0, int y0, int x1, int y1, bool v = true) const {   // filled, inclusive
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) set(x, y, v);
    }
    void disc(int cx, int cy, int r, bool v = true) const {
        for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) if (x * x + y * y <= r * r + r / 2) set(cx + x, cy + y, v);
    }
    void ring(int cx, int cy, int r) const {
        for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (d <= r * r + r && d > (r - 1) * (r - 1) + (r - 1)) set(cx + x, cy + y);
        }
    }
    void invert() const { for (int i = 0; i < W * H / 8; i++) b[i] = (uint8_t)~b[i]; }
};

uint32_t hs(uint32_t a, uint32_t k)
{
    uint32_t h = a * 2654435761u ^ (k + 0x9e3779b9u) * 40503u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return h;
}
int sw(uint32_t t, int period, int amp) { return (int)lroundf(sinf((float)(t % (uint32_t)period) * 6.2831853f / (float)period) * (float)amp); }

// ---- the figure ---------------------------------------------------------------
struct Fig {
    int  x = 0, dy = 0, step = 0, lean = 0;
    bool walk = true, autoArms = true;
    int  hl[2] = {0, 0}, hr[2] = {0, 0};   // hand offsets from the shoulder (autoArms off): left / right arm
    int  hx[2] = {0, 0}, hy[2] = {0, 0};   // out: hand positions
    int  headX = 0, headY = 0, shY = 0;    // out: head centre (radius 4), shoulder row
};
const int8_t SW[8] = { 0, 2, 4, 2, 0, -2, -4, -2 };

// 33 px tall: legs 12, torso 12 (3 wide), head r4; x = centre of the torso
void figure(const Cv& c, Fig& f)
{
    const int g = GY + f.dy, hip = g - 12, sh = g - 24, sx = f.x + f.lean;
    const int ph = f.step & 7, s = f.walk ? SW[ph] : 0;
    const int liftA = f.walk && ph >= 1 && ph <= 3, liftB = f.walk && ph >= 5;
    const int sp = f.walk ? 0 : 3;
    for (int w = 0; w < 2; w++) {   // legs, 2 px wide, with a foot
        c.line(f.x - 1 + w, hip, f.x - 1 + w + s - sp, g - liftA);
        c.line(f.x + w, hip, f.x + w - s + sp, g - liftB);
    }
    c.rect(f.x - 1 + s - sp + 1, g - liftA, f.x + 2 + s - sp, g - liftA);
    c.rect(f.x + 1 - s + sp, g - liftB, f.x + 3 - s + sp, g - liftB);
    for (int w = -1; w <= 1; w++) c.line(f.x + w, hip, sx + w, sh);   // torso
    c.line(sx - 3, sh + 1, sx + 3, sh + 1);   // shoulders
    f.headX = sx; f.headY = g - 29; f.shY = sh;
    c.disc(f.headX, f.headY, 4);
    c.set(f.headX + 2, f.headY - 1, false);   // the eye: looks to the right
    int ax[2], ay[2];
    if (f.autoArms) { ax[0] = -s; ay[0] = 8; ax[1] = s; ay[1] = 8; if (!f.walk) { ax[0] = -1; ax[1] = 1; } }
    else { ax[0] = f.hl[0]; ay[0] = f.hl[1]; ax[1] = f.hr[0]; ay[1] = f.hr[1]; }
    for (int i = 0; i < 2; i++) {
        f.hx[i] = sx + (i ? 3 : -3) + ax[i]; f.hy[i] = sh + 1 + ay[i];
        c.line(sx + (i ? 3 : -3), sh + 1, f.hx[i], f.hy[i]);
        c.rect(f.hx[i], f.hy[i], f.hx[i] + 1, f.hy[i] + 1);   // the hand
    }
}

// ---- props ---------------------------------------------------------------------
void snow(const Cv& c, uint32_t t, int n, uint32_t seed)
{
    for (int i = 0; i < n; i++) {
        int x = (int)(hs(seed, i) % W) + sw(t + i * 97, 2000, 3);
        int y = (int)((hs(seed, i + 99) % 62 + t / (28 + (i % 3) * 12)) % 62);
        c.set(x, y);
        if (i % 3 == 0) { c.set(x + 1, y); c.set(x - 1, y); c.set(x, y + 1); c.set(x, y - 1); }
    }
}
void rain(const Cv& c, uint32_t t, int n, uint32_t seed, int slant)
{
    for (int i = 0; i < n; i++) {
        int y = (int)((hs(seed, i + 7) % 64 + t / 5) % 64);
        int x = (int)(hs(seed, i) % (W + 16)) - 8 - (int)(y * slant) / 16;
        c.line(x, y, x - slant, y + 3);
    }
}
void sun(const Cv& c, int cx, int cy, int r, uint32_t t, int len)
{
    c.disc(cx, cy, r);
    for (int k = 0; k < 8; k++) {
        float a = (float)k * 0.785398f + (float)t / 900.0f;
        int l = (k & 1) ? len : len - 2;
        c.line(cx + (int)lroundf(cosf(a) * (r + 3)), cy + (int)lroundf(sinf(a) * (r + 3)),
               cx + (int)lroundf(cosf(a) * (r + 3 + l)), cy + (int)lroundf(sinf(a) * (r + 3 + l)));
    }
}
void cloud(const Cv& c, int cx, int cy, bool v = true)
{
    c.disc(cx - 9, cy + 3, 5, v); c.disc(cx, cy - 1, 8, v); c.disc(cx + 9, cy + 3, 5, v); c.rect(cx - 9, cy + 3, cx + 9, cy + 8, v);
}
void note(const Cv& c, int x, int y) { c.rect(x, y, x + 1, y + 1); c.line(x + 1, y, x + 1, y - 4); c.set(x + 2, y - 4); }
void zed(const Cv& c, int x, int y, int s) { c.line(x, y, x + s, y); c.line(x + s, y, x, y + s); c.line(x, y + s, x + s, y + s); }
void star(const Cv& c, int x, int y, bool big)
{
    c.set(x, y);
    if (big) { c.set(x - 1, y); c.set(x + 1, y); c.set(x, y - 1); c.set(x, y + 1); }
}
void ground(const Cv& c) { c.line(0, 63, 127, 63); }
void dither(const Cv& c, int x0, int y0, int x1, int y1, int ph)   // fog: every 4th pixel, shifted
{
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) if ((x * 3 + y * 5 + ph) % 7 == 0 && ((x + y) & 1) == 0) c.set(x, y);   // sparse, drifting texture
}
void puff(const Cv& c, int x, int y, int r) { c.ring(x, y, r); }
void drops(const Cv& c, int x, int y, uint32_t t, int n)   // falling sweat / water drops from (x, y)
{
    for (int i = 0; i < n; i++) {
        int k = (int)((t / 60 + i * 5) % 12);
        c.set(x + (i % 2 ? 3 : -2), y + k); c.set(x + (i % 2 ? 3 : -2), y + k + 1);
    }
}
void sunglassesOn(const Cv& c, const Fig& f) { c.line(f.headX - 3, f.headY - 1, f.headX + 4, f.headY - 1, false); c.line(f.headX - 3, f.headY, f.headX + 4, f.headY, false); c.line(f.headX - 5, f.headY - 1, f.headX - 3, f.headY - 1); }

// ---- animations ----------------------------------------------------------------
struct Ctx {
    const Cv* c;
    int x, step; bool walking;
    int a;            // progress of the act, 0..255; -1 outside it
    int p;            // progress of the whole takeover, 0..1000
    uint32_t t;       // ms
    uint32_t seed;
};
typedef void (*AnimFn)(const Ctx&);
enum : uint8_t { AF_STOP = 1 };   // the figure stops (at stopX) to act; otherwise it walks through, a = p
struct AnimDef { AnimFn fn; uint8_t flags; int8_t stopX; const char* name; };

Fig mk(const Ctx& k) { Fig f; f.x = k.x; f.step = k.step; f.walk = k.walking; return f; }
int act(const Ctx& k) { return k.a < 0 ? 0 : k.a; }

// -- frost
void frostScarf(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); snow(c, k.t, 24, k.seed);
    Fig f = mk(k);
    if (k.a >= 0) { f.x += (k.t / 70) & 1 ? 1 : -1; f.autoArms = false; f.hl[0] = 3; f.hl[1] = 4; f.hr[0] = -1; f.hr[1] = 4; }
    figure(c, f);
    const int sy = f.shY - 1;   // scarf: collar + a tail that flutters
    c.line(f.x - 3, sy, f.x + 3, sy); c.line(f.x - 3, sy - 1, f.x + 3, sy - 1);
    c.line(f.x - 3, sy + 1, f.x - 11, sy + 4 + sw(k.t, 400, 2)); c.line(f.x - 3, sy + 2, f.x - 11, sy + 6 + sw(k.t, 400, 2));
    for (int i = 0; i < 3; i++) {   // breath
        int q = (int)((k.t / 130 + i * 4) % 12);
        if (q < 8) puff(c, f.headX + 6 + q, f.headY + 2 - q / 3, 1 + q / 4);
    }
}
void frostSnowball(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); snow(c, k.t, 14, k.seed);
    c.disc(k.x + 14, GY + 2, 5);   // the snow heap
    Fig f = mk(k);
    const int a = act(k);
    int bx = -1, by = 0;
    if (k.a >= 0) {
        f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.walk = false;
        if (a < 80) { f.hr[0] = 5 + (a / 16); f.hr[1] = 11; f.lean = 2; f.dy = a / 40; }                    // bends down, scoops
        else if (a < 150) { f.hr[0] = -6; f.hr[1] = -8; f.lean = -2; bx = f.x - 7; by = GY - 34; }            // winds up
        else { f.hr[0] = 7; f.hr[1] = -6; const int q = a - 150; bx = f.x + 8 + q * 70 / 105; by = GY - 30 + (q * (q - 105)) / 70; }   // throws, arc
    } else {
        f.autoArms = true;
    }
    figure(c, f);
    if (k.a >= 0 && a >= 80) c.disc(bx, by, 2);
}
// -- thaw
void thawSlush(const Ctx& k)
{
    const Cv& c = *k.c; ground(c);
    drops(c, 20, 0, k.t, 3); drops(c, 70, 3, k.t + 300, 3); drops(c, 110, 0, k.t + 700, 3);
    c.line(k.x - 14, GY + 1, k.x + 16, GY + 1); c.line(k.x - 10, 63, k.x + 12, 63);
    Fig f = mk(k);
    const int a = act(k);
    int jump = 0;
    if (k.a >= 0) { f.walk = false; jump = (int)(fabsf(sinf((float)a * 6.2831853f * 1.5f / 255.0f)) * 9.0f); f.dy = -jump; f.autoArms = false; f.hl[0] = -4; f.hl[1] = -4; f.hr[0] = 5; f.hr[1] = -4; }
    figure(c, f);
    if (k.a >= 0 && jump < 3) for (int i = 0; i < 6; i++) { int d = (int)((k.t / 40 + i * 3) % 9); c.set(f.x + 1 + (i - 3) * (2 + d / 3), GY - 1 - (d < 5 ? d : 8 - d)); }
}
void thawSnowman(const Ctx& k)
{
    const Cv& c = *k.c; ground(c);
    const int m = 1000 - k.p, cx = 108;   // melts away over the takeover
    const int r1 = 2 + 6 * m / 1000, r2 = 1 + 4 * m / 1000, r3 = 1 + 3 * m / 1000;
    c.disc(cx, GY - r1, r1);
    if (m > 150) c.disc(cx, GY - 2 * r1 - r2 + 1, r2);
    if (m > 300) { c.disc(cx, GY - 2 * r1 - 2 * r2 - r3 + 2, r3); c.set(cx + 1, GY - 2 * r1 - 2 * r2 - r3 + 1, false); }
    c.line(cx - 3 - (1000 - m) / 90, GY + 1, cx + 3 + (1000 - m) / 90, GY + 1);
    drops(c, cx, GY - 2 * r1 - 2 * r2, k.t, 2);
    Fig f = mk(k);
    if (k.a >= 0) { f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.hr[0] = 4; f.hr[1] = -6 + sw(k.t, 400, 2); }
    figure(c, f);
}
// -- mild
void mildWhistle(const Ctx& k)
{
    const Cv& c = *k.c; ground(c);
    cloud(c, 24 + (int)(k.t / 160) % 150 - 20, 12);
    Fig f = mk(k); figure(c, f);
    for (int i = 0; i < 2; i++) { int q = (int)((k.t / 100 + i * 7) % 14); note(c, f.headX + 5 + q, f.headY - 2 - q / 2 - i); }
}
void mildWave(const Ctx& k)
{
    const Cv& c = *k.c; ground(c);
    c.line(k.x + 12, GY, k.x + 12, GY - 6); c.disc(k.x + 12, GY - 8, 1); c.set(k.x + 12, GY - 10); c.set(k.x + 12, GY - 6 - 8 + 1);   // flower
    c.set(k.x + 10, GY - 8); c.set(k.x + 14, GY - 8);
    Fig f = mk(k);
    if (k.a >= 0) { f.walk = false; f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.hr[0] = 5; f.hr[1] = -9 + sw(k.t, 300, 3); }
    figure(c, f);
    if (k.a >= 0) { c.set(f.headX + 3, f.headY + 2, false); c.set(f.headX + 2, f.headY + 3, false); }
}
// -- hot
void hotFan(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); sun(c, 112, 13, 6, k.t, 5);
    Fig f = mk(k);
    if (k.a >= 0) { f.walk = false; f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.hr[0] = 6; f.hr[1] = -9 + sw(k.t, 240, 3); }
    figure(c, f);
    drops(c, f.headX, f.headY + 3, k.t, 3);
}
void hotCocktail(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); sun(c, 14, 13, 6, k.t, 5);
    Fig f = mk(k); f.autoArms = false;
    const int a = act(k);
    f.hl[0] = -1; f.hl[1] = 8;
    int up = k.a < 0 ? 0 : (a < 60 ? a * 100 / 60 : a > 200 ? (255 - a) * 100 / 55 : 100);   // glass raised 0..100 %
    f.hr[0] = 4; f.hr[1] = 8 - up * 12 / 100;
    if (k.walking) { f.hr[1] = 6; }
    figure(c, f);
    sunglassesOn(c, f);
    const int gx = f.hx[1], gy = f.hy[1];   // the glass: a trapezoid with a straw and a slice
    c.line(gx - 2, gy - 4, gx + 2, gy - 4); c.line(gx - 2, gy - 4, gx - 1, gy); c.line(gx + 2, gy - 4, gx + 1, gy); c.set(gx - 1, gy - 2); c.set(gx + 1, gy - 2);
    c.line(gx + 1, gy - 4, gx + 3, gy - 8);
    c.disc(gx + 3, gy - 5, 1);
}
// -- uv
void uvSunscreen(const Ctx& k)
{
    const Cv& c = *k.c; ground(c);
    sun(c, 108, 14, 8, k.t, 8);
    Fig f = mk(k);
    if (k.a >= 0) { f.walk = false; f.autoArms = false; f.hr[0] = 9; f.hr[1] = 4; f.hl[0] = 4 + sw(k.t, 500, 3) + 3; f.hl[1] = 5; }
    figure(c, f);
    c.line(f.headX - 6, f.headY - 3, f.headX + 7, f.headY - 3);   // hat brim
    c.rect(f.headX - 3, f.headY - 7, f.headX + 3, f.headY - 4);
}
// -- rain
void rainUmbrella(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); rain(c, k.t, 34, k.seed, 1);
    c.line(14, GY, 30, GY); c.line(80, GY, 96, GY);   // puddles
    Fig f = mk(k); f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.hr[0] = 4; f.hr[1] = -4;
    figure(c, f);
    const int ux = f.hx[1], uy = f.hy[1] - 9;   // canopy: a filled half disc on a handle
    c.line(ux, f.hy[1], ux, uy);
    for (int y = -9; y <= 0; y++) for (int x = -10; x <= 10; x++) if (x * x + y * y * 2 <= 100) c.set(ux + x, uy + y);
    for (int x = -8; x <= 8; x += 4) c.set(ux + x, uy + 1);   // scalloped edge
    c.set(ux, uy - 10);
}
void rainPalm(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); rain(c, k.t, 30, k.seed, 1);
    const int rip = (int)(k.t / 120 % 4);
    c.line(k.x - 6 - rip, GY + 1, k.x + 8 + rip, GY + 1);
    Fig f = mk(k);
    if (k.a >= 0) { f.walk = false; f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.hr[0] = 8; f.hr[1] = -2; }
    figure(c, f);
    if (k.a >= 0) {
        c.set(f.headX + 3, f.headY + 2, false); c.set(f.headX + 3, f.headY + 3, false);   // mouth open to catch drops
        c.line(f.hx[1] - 1, f.hy[1] - 1, f.hx[1] + 2, f.hy[1] - 1);                      // the palm
        for (int i = 0; i < 3; i++) { int q = (int)((k.t / 50 + i * 4) % 10); c.set(f.hx[1] + i - 1, f.hy[1] - 2 - q); }
    }
}
// -- wind
void streaks(const Cv& c, uint32_t t, uint32_t seed, int n, int dir)   // dir +1: right, -1: left
{
    for (int i = 0; i < n; i++) {
        int len = 6 + (int)(hs(seed, i + 40) % 10), y = 3 + (int)(hs(seed, i) % 52);
        int x = (int)((hs(seed, i + 20) % (W + 30) + (dir > 0 ? t / 3 : (uint32_t)(W + 30) * 100 - t / 3)) % (W + 30)) - 15;
        c.line(x, y, x + len, y);
    }
}
void leaf(const Cv& c, int x, int y, uint32_t t) { c.set(x, y); c.set(x + 1, y + ((t / 90) & 1)); c.set(x + 2, y); }
void windLean(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); streaks(c, k.t, k.seed, 8, -1);
    for (int i = 0; i < 3; i++) leaf(c, (int)((hs(k.seed, i + 60) % 128 + 100000 - k.t / 4 * (1 + i % 2)) % 140) - 6, 8 + i * 17 + sw(k.t + i * 200, 500, 3), k.t);
    Fig f = mk(k); f.lean = 2; figure(c, f);
    const int sy = GY - 18;   // the scarf streams out behind, to the right of... the walker: blown backwards
    c.line(f.x + 2, sy, f.x + 12, sy - 1 + sw(k.t, 200, 1)); c.line(f.x + 2, sy + 1, f.x + 13, sy + 2 + sw(k.t + 90, 200, 2));
}
void windHat(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); streaks(c, k.t, k.seed, 9, 1);
    Fig f = mk(k);
    const int a = act(k);
    if (k.a >= 0) { f.walk = false; f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.hr[0] = 3; f.hr[1] = a < 60 ? -11 : -12 + sw(k.t, 300, 2); f.lean = a < 60 ? 0 : 1; }
    figure(c, f);
    int hatX = f.headX, hatY = f.headY - 5;
    if (k.a >= 0 && a >= 60) { const int q = a - 60; hatX += q * 5 / 10; hatY -= q / 5; }
    if (k.a < 0 && k.p > 650) { hatX = f.x + 150; }
    if (hatX < W + 8) {
        const bool tilt = k.a >= 60 && ((k.t / 100) & 1);
        if (tilt) c.line(hatX - 4, hatY + 2, hatX + 4, hatY - 2); else c.line(hatX - 4, hatY, hatX + 4, hatY);
        c.rect(hatX - 2, hatY - 3, hatX + 2, hatY - 1);
    }
}
// -- lightning
void lightningBolt(const Ctx& k)
{
    const Cv& c = *k.c; ground(c);
    cloud(c, 22, 8); cloud(c, 64, 6); cloud(c, 106, 9);
    rain(c, k.t, 18, k.seed, 2);
    const uint32_t cyc = k.t % 1300, n = k.t / 1300;
    const bool flash = cyc < 200;
    Fig f = mk(k); f.step = (int)(k.t / 70);
    figure(c, f);
    if (flash) {
        const int bx = 12 + (int)(hs(k.seed, n) % 100);
        const int px[6] = { bx, bx - 5, bx + 3, bx - 5, bx + 2, bx - 1 }, py[6] = { 17, 26, 31, 42, 50, GY };
        for (int i = 0; i < 5; i++) { c.line(px[i], py[i], px[i + 1], py[i + 1]); c.line(px[i] + 1, py[i], px[i + 1] + 1, py[i + 1]); }
        for (int i = -2; i <= 2; i++) c.line(f.headX + i, f.headY - 5, f.headX + i * 2, f.headY - 9);   // hair on end
        if (cyc < 70) c.invert();
    }
}
// -- fog
void fogFlashlight(const Ctx& k)
{
    const Cv& c = *k.c; ground(c);
    dither(c, 0, 3, 127, 14, (int)(k.t / 150)); dither(c, 0, 18, 127, 30, (int)(k.t / 210) * 3); dither(c, 0, 57, 127, 62, (int)(k.t / 120));
    Fig f = mk(k); f.step = (int)(k.t / 190); f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.hr[0] = 7; f.hr[1] = 0;
    figure(c, f);
    const int bx = f.hx[1], by = f.hy[1];   // torch and a dotted beam
    c.rect(bx, by - 1, bx + 2, by + 1);
    for (int d = 3; d <= 34; d++) {
        const int half = d / 3;
        c.set(bx + d, by - half); c.set(bx + d, by + half);
        if (d % 3 == 0) for (int y = -half + 2; y < half; y += 3) c.set(bx + d + ((d / 3) & 1), by + y);
    }
}
// -- bad air
void smog(const Cv& c, uint32_t t) { dither(c, 0, 0, 127, 7, (int)(t / 200)); dither(c, 0, 6, 127, 11, (int)(t / 130) * 2); }
void airCough(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); smog(c, k.t);
    Fig f = mk(k);
    const int a = act(k);
    if (k.a >= 0) { f.walk = false; f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.hr[0] = 3; f.hr[1] = -9; f.dy = (a / 20) % 2 ? 0 : -1; f.lean = (a / 20) % 2 ? 1 : 0; }
    figure(c, f);
    if (k.a >= 0 && (a / 20) % 2 == 0) for (int i = 0; i < 4; i++) { int q = (int)((k.t / 60) % 8) + i; c.ring(f.headX + 6 + q * 2, f.headY + 2 + (i - 2) * 2, 1 + (q > 6)); }
}
void airMask(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); smog(c, k.t);
    Fig f = mk(k);
    const int a = act(k);
    const bool on = (k.a >= 100) || (k.a < 0 && k.p > 650);
    if (k.a >= 0) {
        f.walk = false; f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8;
        f.hr[0] = 3; f.hr[1] = a < 90 ? 8 - a / 6 : a < 150 ? -7 : 8 - (255 - a) / 7;   // hand up to the face, then down again
    }
    figure(c, f);
    if (on) {   // the mask: a pad over nose and mouth, ear loops
        c.rect(f.headX - 2, f.headY + 1, f.headX + 5, f.headY + 4); c.line(f.headX - 4, f.headY, f.headX - 2, f.headY + 1);
        c.line(f.headX + 1, f.headY + 2, f.headX + 4, f.headY + 2, false);   // pleat
    }
}
// -- dry soil
void soilWater(const Ctx& k)
{
    const Cv& c = *k.c; ground(c);
    const int a = act(k);
    const int perk = k.a >= 0 ? (a > 60 ? (a - 60) * 100 / 195 : 0) : (k.p > 650 ? 100 : 0);
    const int px = 106, droop = 6 - perk * 6 / 100;
    c.line(px, GY, px, GY - 9);
    c.line(px, GY - 6, px - 5, GY - 7 + droop); c.line(px, GY - 8, px + 5, GY - 8 + droop);
    if (perk >= 100) { c.disc(px, GY - 12, 2); c.set(px, GY - 12, false); }
    for (int x = 92; x < 124; x += 8) { c.set(x, 63, false); }   // cracks in dry ground
    Fig f = mk(k);
    f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.hr[0] = 8; f.hr[1] = 0;
    if (k.walking) { f.autoArms = true; }
    figure(c, f);
    const int hx = f.hx[1], hy = f.hy[1];
    c.rect(hx - 4, hy, hx + 1, hy + 4); c.line(hx + 1, hy + 1, hx + 6, hy - 2); c.line(hx - 4, hy, hx - 4, hy - 3); c.line(hx - 4, hy - 3, hx - 1, hy - 3);
    if (k.a >= 0 && a > 50 && a < 230)
        for (int i = 0; i < 4; i++) { int q = (int)((k.t / 55 + i * 3) % 12); c.set(hx + 6 + q / 2, hy - 2 + q * q / 6); }
}
// -- night
void sky(const Cv& c, uint32_t t, uint32_t seed)
{
    c.disc(110, 13, 7); c.disc(114, 11, 6, false);   // crescent
    for (int i = 0; i < 9; i++) {
        const int x = 4 + (int)(hs(seed, i) % 100), y = 2 + (int)(hs(seed, i + 30) % 26);
        star(c, x, y, ((t / 400 + i) % 4) == 0);
    }
}
void nightYawn(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); sky(c, k.t, k.seed);
    Fig f = mk(k); f.step = (int)(k.t / 190);
    const bool yawn = k.p > 400 && k.p < 620;
    if (yawn) { f.autoArms = false; f.hl[0] = -1; f.hl[1] = 8; f.hr[0] = 3; f.hr[1] = -9; }
    figure(c, f);
    if (yawn) { c.set(f.headX + 3, f.headY + 2, false); c.set(f.headX + 3, f.headY + 3, false); c.set(f.headX + 2, f.headY + 3, false); }
}
void nightSleepwalk(const Ctx& k)
{
    const Cv& c = *k.c; ground(c); sky(c, k.t, k.seed);
    Fig f = mk(k); f.step = (int)(k.t / 200); f.autoArms = false; f.hl[0] = 8; f.hl[1] = 1; f.hr[0] = 8; f.hr[1] = 3;
    figure(c, f);
    c.set(f.headX + 2, f.headY - 1);   // eyes closed
    c.line(f.headX - 4, f.headY - 4, f.headX + 4, f.headY - 4);   // nightcap with a pompom
    c.line(f.headX - 3, f.headY - 5, f.headX - 7, f.headY - 10); c.line(f.headX + 3, f.headY - 5, f.headX - 7, f.headY - 10); c.rect(f.headX - 8, f.headY - 11, f.headX - 6, f.headY - 9);
    for (int i = 0; i < 3; i++) { int q = (int)((k.t / 110 + i * 5) % 15); zed(c, f.headX + 5 + q / 2, f.headY - 6 - q, 2 + i); }
}
// -- surprises (any condition)
void surpriseDance(const Ctx& k)
{
    const Cv& c = *k.c; ground(c);
    for (int i = 0; i < 14; i++) c.set((int)(hs(k.seed, i) % W), (int)((hs(k.seed, i + 50) % 64 + k.t / (12 + i % 4 * 4)) % 62));   // confetti
    Fig f = mk(k);
    const int a = act(k);
    if (k.a >= 0) {
        const bool ph = (k.t / 130) & 1;
        f.walk = false; f.autoArms = false;
        f.hl[0] = ph ? -4 : -3; f.hl[1] = ph ? -10 : 3; f.hr[0] = ph ? 4 : 5; f.hr[1] = ph ? 3 : -10;
        f.dy = -(int)(fabsf(sinf((float)a * 6.2831853f * 2.5f / 255.0f)) * 5.0f);
    }
    figure(c, f);
    if (k.a >= 0) { note(c, f.x - 14, GY - 30 - (int)(k.t / 90 % 6)); note(c, f.x + 14, GY - 26 - (int)((k.t / 90 + 3) % 6)); }
}
void surpriseUfo(const Ctx& k)
{
    const Cv& c = *k.c; ground(c);
    for (int i = 0; i < 6; i++) star(c, 5 + (int)(hs(k.seed, i) % 120), 2 + (int)(hs(k.seed, i + 9) % 12), (k.t / 300 + i) % 3 == 0);
    Fig f = mk(k);
    const int a = act(k);
    const int ux = k.a >= 0 ? f.x : f.x + 28;
    int rise = 0;
    if (k.a >= 0) { rise = a < 50 ? 0 : a < 200 ? (a - 50) * 17 / 150 : 17 - (a - 200) * 17 / 55; if (rise < 0) rise = 0; }
    f.dy = -rise;
    if (k.a >= 0) { f.walk = false; f.autoArms = false; f.hl[0] = -4; f.hl[1] = -8; f.hr[0] = 5; f.hr[1] = -8; }
    figure(c, f);
    const int uy = 8 + sw(k.t, 600, 1);
    c.disc(ux + 1, uy - 3, 3); c.rect(ux - 9, uy - 1, ux + 10, uy + 1); c.rect(ux - 5, uy + 2, ux + 6, uy + 2);
    for (int i = -6; i <= 6; i += 4) c.set(ux + 1 + i, uy, false);
    if (k.a >= 0 && a >= 30 && a <= 225)   // the tractor beam
        for (int y = uy + 4; y < GY; y += 2) { const int half = 3 + (y - uy) / 5; c.set(ux + 1 - half, y); c.set(ux + 1 + half, y); if ((y / 2 + (int)(k.t / 100)) % 3 == 0) c.set(ux + 1, y); }
}

// ---- tables: condition -> animations (add a line to extend) ---------------------
const AnimDef A_FROST[] = { { frostScarf, AF_STOP, 50, "scarf_shiver" }, { frostSnowball, AF_STOP, 50, "snowball" } };
const AnimDef A_THAW[]  = { { thawSlush, AF_STOP, 50, "slush_splash" }, { thawSnowman, AF_STOP, 80, "melting_snowman" } };
const AnimDef A_MILD[]  = { { mildWhistle, 0, 0, "whistle" }, { mildWave, AF_STOP, 50, "wave_flower" } };
const AnimDef A_HOT[]   = { { hotFan, AF_STOP, 50, "fan_sweat" }, { hotCocktail, AF_STOP, 50, "cocktail" } };
const AnimDef A_UV[]    = { { uvSunscreen, AF_STOP, 60, "sunscreen" } };
const AnimDef A_RAIN[]  = { { rainUmbrella, 0, 0, "umbrella" }, { rainPalm, AF_STOP, 50, "catch_drops" } };
const AnimDef A_WIND[]  = { { windLean, 0, 0, "lean_scarf" }, { windHat, AF_STOP, 50, "hat_flies" } };
const AnimDef A_LIGHT[] = { { lightningBolt, 0, 0, "bolt_run" } };
const AnimDef A_FOG[]   = { { fogFlashlight, 0, 0, "flashlight" } };
const AnimDef A_AIR[]   = { { airCough, AF_STOP, 50, "cough" }, { airMask, AF_STOP, 50, "mask" } };
const AnimDef A_SOIL[]  = { { soilWater, AF_STOP, 82, "watering_can" } };
const AnimDef A_NIGHT[] = { { nightYawn, 0, 0, "moon_yawn" }, { nightSleepwalk, 0, 0, "sleepwalk" } };
const AnimDef A_SURPRISE[] = { { surpriseDance, AF_STOP, 64, "dance" }, { surpriseUfo, AF_STOP, 64, "ufo" } };

struct Pool { const AnimDef* list; uint8_t n; };
#define POOL(a) { a, (uint8_t)(sizeof(a) / sizeof(a[0])) }
const Pool POOLS[SC_COUNT] = { POOL(A_FROST), POOL(A_THAW), POOL(A_MILD), POOL(A_HOT), POOL(A_UV), POOL(A_RAIN), POOL(A_WIND),
                               POOL(A_LIGHT), POOL(A_FOG), POOL(A_AIR), POOL(A_SOIL), POOL(A_NIGHT) };
const Pool SURPRISES = POOL(A_SURPRISE);

const AnimDef& defOf(uint8_t cond, uint8_t anim, bool surprise)
{
    const Pool& p = surprise ? SURPRISES : POOLS[cond < SC_COUNT ? cond : SC_MILD];
    return p.list[anim % p.n];
}

// ---- the other modes ------------------------------------------------------------
int tri(uint32_t pos, int range)   // triangle wave 0..range, pos advances 1 per step
{
    const int period = 2 * range, q = (int)(pos % (uint32_t)period);
    return q <= range ? q : period - q;
}

void renderDot(const Cv& c, uint32_t t)
{
    const int x = tri(t * 30 / 1000 + 20, W - 4), y = tri(t * 17 / 1000 + 9, H - 4);
    c.rect(x, y, x + 3, y + 3);
}

void renderIcons(const Cv& c, uint32_t t, int p)
{
    const int cx = 34 + (p * 56) / 1000, third = p * 3 / 1000 > 2 ? 2 : p * 3 / 1000;   // sun -> cloud -> rain, drifting right
    if (third == 0) sun(c, cx, 32, 10, t, 8);
    else if (third == 1) { cloud(c, cx, 30); }
    else {
        cloud(c, cx, 20);
        for (int i = 0; i < 6; i++) { int q = (int)((t / 70 + i * 3) % 14); c.line(cx - 12 + i * 5, 34 + q * 2, cx - 13 + i * 5, 36 + q * 2); }
        for (int i = 0; i < 6; i++) { int q = (int)((t / 70 + i * 3) % 14); if (34 + q * 2 > 60) for (int x = -1; x <= 1; x++) c.set(cx - 12 + i * 5 + x * 2, 62); }
    }
}

} // namespace

// ---- conditions -------------------------------------------------------------------
uint16_t saverConditions(const SaverInput& in)
{
    uint16_t m = 0;
    const bool t = !isnan(in.tempC);
    if (t && in.tempC < 0) m |= 1u << SC_FROST;
    if (t && in.thawed && in.tempC >= 0 && in.tempC <= 8) m |= 1u << SC_THAW;
    if (t && in.tempC >= 27) m |= 1u << SC_HOT;
    if (!t || (in.tempC >= 0 && in.tempC < 27)) m |= 1u << SC_MILD;
    if (!isnan(in.uvIndex) && in.uvIndex >= 6) m |= 1u << SC_UV;
    if ((!isnan(in.rainRateMmH) && in.rainRateMmH >= 0.1f) || (!isnan(in.rain1hMm) && in.rain1hMm >= 0.2f)) m |= 1u << SC_RAIN;
    if ((!isnan(in.gustMs) && in.gustMs >= 10) || (!isnan(in.windMs) && in.windMs >= 7)) m |= 1u << SC_WIND;
    if (in.lightning) m |= 1u << SC_LIGHTNING;
    if (!isnan(in.humidity) && in.humidity >= 93) m |= 1u << SC_FOG;
    if ((!isnan(in.pm25) && in.pm25 > 35) || (!isnan(in.co2Ppm) && in.co2Ppm > 1500)) m |= 1u << SC_AIR;
    if (in.soilPct >= 0 && in.soilPct <= 25) m |= 1u << SC_SOIL;
    if (in.night) m |= 1u << SC_NIGHT;
    return m;
}

uint8_t saverPickCondition(const SaverInput& in, uint32_t rnd)
{
    const uint16_t m = saverConditions(in);
    if (m & (1u << SC_LIGHTNING)) return SC_LIGHTNING;
    const uint16_t pick = m & ~((1u << SC_MILD) | (1u << SC_NIGHT));
    int n = 0;
    for (int i = 0; i < SC_COUNT; i++) if (pick & (1u << i)) n++;
    if (n) {
        int k = (int)(rnd % (uint32_t)n);
        for (int i = 0; i < SC_COUNT; i++) if ((pick & (1u << i)) && k-- == 0) return (uint8_t)i;
    }
    return (m & (1u << SC_NIGHT)) ? SC_NIGHT : SC_MILD;
}

uint8_t saverAnimCount(uint8_t cond) { return cond < SC_COUNT ? POOLS[cond].n : 0; }
uint8_t saverSurpriseCount() { return SURPRISES.n; }
const char* saverAnimName(uint8_t cond, uint8_t anim, bool surprise) { return defOf(cond, anim, surprise).name; }

SaverScene saverStart(uint8_t mode, const SaverInput& in, uint32_t rnd)
{
    SaverScene s;
    s.mode = mode; s.seed = rnd;
    s.cond = saverPickCondition(in, rnd >> 4);
    s.surprise = ((rnd >> 12) % 10) == 0 && s.cond != SC_LIGHTNING;
    s.anim = (uint8_t)((rnd >> 16) % (s.surprise ? SURPRISES.n : POOLS[s.cond].n));
    return s;
}

void saverRender(const SaverScene& sc, uint32_t elapsedMs, uint32_t durationMs, uint8_t out[1024])
{
    memset(out, 0, 1024);
    if (!durationMs) durationMs = 1;
    if (elapsedMs > durationMs) elapsedMs = durationMs;
    const Cv c = { out };
    const int p = (int)((uint64_t)elapsedMs * 1000 / durationMs);
    switch (sc.mode) {
        case SV_DOT: renderDot(c, elapsedMs); break;
        case SV_ICONS: renderIcons(c, elapsedMs, p); break;
        case SV_FIGURE: {
            const AnimDef& d = defOf(sc.cond, sc.anim, sc.surprise);
            Ctx k = { &c, 0, (int)(elapsedMs / 120), true, -1, p, elapsedMs, sc.seed };
            if (d.flags & AF_STOP) {
                if (p < 350) k.x = -14 + (d.stopX + 14) * p / 350;
                else if (p < 650) { k.x = d.stopX; k.walking = false; k.a = (p - 350) * 255 / 300; }
                else k.x = d.stopX + (142 - d.stopX) * (p - 650) / 350;
            } else {
                k.x = -14 + 156 * p / 1000; k.a = p * 255 / 1000;
            }
            d.fn(k);
            break;
        }
        default: break;   // SV_BLANK / SV_OFF: dark
    }
}
